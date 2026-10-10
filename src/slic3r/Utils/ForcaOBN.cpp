#include "ForcaOBN.hpp"

#include "Http.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace fs = boost::filesystem;
using nlohmann::json;

namespace Slic3r {

namespace {

constexpr const char* OBN_RELEASES_LATEST = "https://api.github.com/repos/ClusterM/open-bamboo-networking/releases/latest";
constexpr const char* OBN_ASSET           = "obn-windows-x64.zip";
constexpr const char* OBN_SUFFIX          = "-obn";
constexpr size_t      OBN_MAX_ZIP_BYTES   = 400u * 1024u * 1024u; // the 2.2.0 zip is 79 MB

fs::path plugins_dir() { return fs::path(data_dir()) / "plugins"; }
fs::path bambu_camera_backup() { return plugins_dir() / "obn" / "bambu-camera" / "BambuSource.dll"; }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

bool read_file(const fs::path& path, std::string& out)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const fs::path& path, const std::string& data, std::string& err)
{
    boost::system::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
    if (!out || !out.write(data.data(), std::streamsize(data.size()))) {
        err = "could not write " + path.string();
        return false;
    }
    return true;
}

// Replace `dst` with `src` even while this process still maps `dst` (a loaded DLL can't be overwritten or deleted
// on Windows, but it can be renamed aside; GUI_App sweeps plugins/*.old before the plug-in loads).
bool install_file(const fs::path& src, const fs::path& dst, std::string& err)
{
    boost::system::error_code ec;
    if (fs::exists(dst, ec)) {
        fs::remove(dst, ec);
        if (ec) {
            fs::path aside = dst;
            aside += ".old";
            boost::system::error_code ec2;
            fs::remove(aside, ec2);
            fs::rename(dst, aside, ec2);
            if (ec2) {
                err = "could not replace " + dst.filename().string() + " (" + ec2.message() + ")";
                return false;
            }
        }
    }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        err = "could not copy " + src.filename().string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool same_content(const fs::path& a, const fs::path& b)
{
    std::string da, db;
    return read_file(a, da) && read_file(b, db) && da == db;
}

} // namespace

std::string forca_obn_version(const std::string& series) { return series + OBN_SUFFIX; }

bool forca_obn_is_obn_version(const std::string& version)
{
    const std::string sfx = OBN_SUFFIX;
    return version.size() > sfx.size() && version.compare(version.size() - sfx.size(), sfx.size(), sfx) == 0;
}

bool forca_obn_parse_release(const std::string& github_json, ForcaObnRelease& out, std::string& err)
{
    const json j = json::parse(github_json, nullptr, false);
    if (!j.is_object() || !j.contains("tag_name")) {
        err = "GitHub did not send a release";
        return false;
    }
    if (j.value("draft", false) || j.value("prerelease", false)) {
        err = "the latest OBN release is not a stable release";
        return false;
    }
    out.tag = j.value("tag_name", std::string());
    for (const auto& asset : j.value("assets", json::array())) {
        if (asset.value("name", std::string()) != OBN_ASSET)
            continue;
        const std::string digest = asset.contains("digest") && asset["digest"].is_string() ? asset["digest"].get<std::string>() : "";
        if (digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64) {
            err = "the OBN release has no SHA-256 for " + std::string(OBN_ASSET);
            return false;
        }
        out.zip_url = asset.value("browser_download_url", std::string());
        out.sha256  = lower(digest.substr(7));
        if (out.tag.empty() || out.zip_url.rfind("https://", 0) != 0) {
            err = "the OBN release is incomplete";
            return false;
        }
        return true;
    }
    err = "the OBN release has no " + std::string(OBN_ASSET);
    return false;
}

bool forca_obn_zip_entry_matches(const std::string& entry, const std::string& series, const std::string& file)
{
    std::string e = entry;
    std::replace(e.begin(), e.end(), '\\', '/');
    const std::string tail = "lib/v" + series + "/" + file;
    return e == tail || (e.size() > tail.size() && e.compare(e.size() - tail.size(), tail.size(), tail) == 0 &&
                         e[e.size() - tail.size() - 1] == '/');
}

bool forca_obn_crash_in_network_plugin(const std::string& crash_log)
{
    // Forca's crash log names the faulting module on its "Fault address:" line.
    for (size_t pos = crash_log.find("Fault address:"); pos != std::string::npos; pos = crash_log.find("Fault address:", pos + 1)) {
        const std::string line = lower(crash_log.substr(pos, crash_log.find('\n', pos) - pos));
        if (line.find("bambu_networking") != std::string::npos)
            return true;
    }
    return false;
}

std::string forca_obn_sha256_hex(const std::string& data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int  len = 0;
    if (EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr) != 1)
        return {};
    std::string hex;
    char        buf[3];
    for (unsigned int i = 0; i < len; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", md[i]);
        hex += buf;
    }
    return hex;
}

std::string forca_obn_cache_dir(const std::string& tag) { return (plugins_dir() / "obn" / tag).string(); }

std::vector<std::string> forca_obn_cached_tags()
{
    std::vector<std::string> tags;
    boost::system::error_code ec;
    const fs::path root = plugins_dir() / "obn";
    if (!fs::is_directory(root, ec))
        return tags;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_directory(it->path()) && fs::exists(it->path() / "bambu_networking.dll") && fs::exists(it->path() / "BambuSource.dll"))
            tags.push_back(it->path().filename().string());
    // "v2.10.0" after "v2.9.1": compare the numbers, not the text.
    auto numbers = [](const std::string& tag) {
        std::vector<int> n;
        int cur = -1;
        for (char c : tag) {
            if (std::isdigit((unsigned char) c))
                cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else if (cur >= 0) {
                n.push_back(cur);
                cur = -1;
            }
        }
        if (cur >= 0)
            n.push_back(cur);
        return n;
    };
    std::sort(tags.begin(), tags.end(), [&](const std::string& a, const std::string& b) { return numbers(a) > numbers(b); });
    return tags;
}

bool forca_obn_fetch_latest(ForcaObnRelease& out, std::string& err)
{
    bool        ok = false;
    std::string body;
    Http::get(OBN_RELEASES_LATEST)
        .header("Accept", "application/vnd.github+json")
        .timeout_connect(10)
        .timeout_max(30)
        .on_complete([&](std::string b, unsigned) {
            body = std::move(b);
            ok   = true;
        })
        .on_error([&](std::string, std::string error, unsigned status) { err = error + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    return ok && forca_obn_parse_release(body, out, err);
}

bool forca_obn_download(const ForcaObnRelease& release, const std::string& series, std::string& err)
{
    bool        ok = false;
    std::string zip;
    Http::get(release.zip_url)
        .timeout_connect(10)
        .timeout_max(600)
        .size_limit(OBN_MAX_ZIP_BYTES)
        .on_complete([&](std::string b, unsigned) {
            zip = std::move(b);
            ok  = true;
        })
        .on_error([&](std::string, std::string error, unsigned status) { err = error + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    if (!ok)
        return false;
    if (forca_obn_sha256_hex(zip) != release.sha256) {
        err = "the downloaded OBN file does not match its published SHA-256";
        return false;
    }

    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) {
        err = "the OBN download is not a valid zip file";
        return false;
    }
    const fs::path dir = forca_obn_cache_dir(release.tag);
    bool           all = true;
    for (const char* file : { "bambu_networking.dll", "BambuSource.dll" }) {
        bool found = false;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive) && !found; ++i) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&archive, i, &stat) || stat.m_is_directory ||
                !forca_obn_zip_entry_matches(stat.m_filename, series, file))
                continue;
            size_t size = 0;
            void*  data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
            if (data) {
                found = write_file(dir / file, std::string(static_cast<const char*>(data), size), err);
                mz_free(data);
            }
        }
        if (!found) {
            if (err.empty())
                err = "the OBN release has no " + std::string(file) + " for plug-in " + series;
            all = false;
            break;
        }
    }
    mz_zip_reader_end(&archive);
    if (!all) {
        boost::system::error_code ec;
        fs::remove_all(dir, ec); // never leave half a build behind
    }
    return all;
}

bool forca_obn_activate(const std::string& tag, const std::string& series, std::string& err)
{
    const fs::path dir = forca_obn_cache_dir(tag);
    if (!fs::exists(dir / "bambu_networking.dll") || !fs::exists(dir / "BambuSource.dll")) {
        err = "OBN " + tag + " is not downloaded";
        return false;
    }
    // Keep Bambu's camera DLL the first time, unless what is there now is already an OBN one.
    const fs::path camera = plugins_dir() / "BambuSource.dll";
    boost::system::error_code ec;
    if (fs::exists(camera, ec) && !fs::exists(bambu_camera_backup(), ec)) {
        bool is_obn = false;
        for (const std::string& t : forca_obn_cached_tags())
            is_obn = is_obn || same_content(camera, fs::path(forca_obn_cache_dir(t)) / "BambuSource.dll");
        if (!is_obn) {
            fs::create_directories(bambu_camera_backup().parent_path(), ec);
            fs::copy_file(camera, bambu_camera_backup(), fs::copy_options::overwrite_existing, ec);
            if (ec) {
                err = "could not keep Bambu's camera DLL (" + ec.message() + ")";
                return false;
            }
        }
    }
    const fs::path plugin = plugins_dir() / ("bambu_networking_" + forca_obn_version(series) + ".dll");
    return install_file(dir / "bambu_networking.dll", plugin, err) && install_file(dir / "BambuSource.dll", camera, err);
}

bool forca_obn_restore_bambu_camera(std::string& err)
{
    // Forca's own copy first, then the camera DLL of Bambu's last downloaded plug-in.
    for (const fs::path& src : { bambu_camera_backup(), fs::path(data_dir()) / "ota" / "plugins" / "BambuSource.dll" })
        if (fs::exists(src))
            return install_file(src, plugins_dir() / "BambuSource.dll", err);
    err = "Bambu's camera DLL is not on this computer; reinstall Bambu's network plug-in to get the camera back";
    return false;
}

} // namespace Slic3r
