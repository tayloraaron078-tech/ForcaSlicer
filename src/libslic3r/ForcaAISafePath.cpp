#include "ForcaAISafePath.hpp"

#include <nlohmann/json.hpp>

#include <boost/filesystem/fstream.hpp> // wide-path streams on Windows
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <regex>
#include <sstream>

namespace Slic3r { namespace ForcaAI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

std::mutex s_ledger_mutex; // one ledger file per data folder; tool calls run on the GUI thread, this is belt + braces

fs::path path_from_utf8(const std::string& s)
{
#ifdef _WIN32
    return fs::path(boost::nowide::widen(s));
#else
    return fs::path(s);
#endif
}

std::string utf8_from_path(const fs::path& p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.wstring());
#else
    return p.string();
#endif
}

json load(const fs::path& file)
{
    try {
        fs::ifstream in(file);
        if (in) {
            json j = json::parse(in);
            if (j.is_object() && j.contains("files") && j["files"].is_object())
                return j;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "Forca AI: unreadable ledger, treating it as empty: " << e.what();
    }
    return json{ { "files", json::object() } };
}

} // namespace

std::string suffix(const std::string& date) { return " (Claude " + date + ")"; }

std::string strip_suffix(const std::string& name)
{
    static const std::regex re(R"(\s*\(Claude \d{4}-\d{2}-\d{2}\)(\s+\d+)?\s*$)");
    std::string out = name, prev;
    do {
        prev = out;
        out  = std::regex_replace(out, re, "");
    } while (out != prev);
    return out;
}

std::string clean_name(const std::string& name)
{
    std::string out;
    for (unsigned char c : name)
        if (c >= 32 && c != 127 && std::string("<>:\"/\\|?*").find(char(c)) == std::string::npos)
            out.push_back(char(c));
    const auto first = out.find_first_not_of(" .");
    const auto last  = out.find_last_not_of(" .");
    out = first == std::string::npos ? std::string() : out.substr(first, last - first + 1);
    return out.empty() ? std::string("Untitled") : out;
}

bool path_is_free(const fs::path& path)
{
    // Not exists(p, ec): Boost 1.84 sets ec for a plain "not found" too, so it can't tell "free" from "can't check".
    // A loop on that hung Forca once (2026-09-24).
    boost::system::error_code ec;
    return fs::symlink_status(path, ec).type() == fs::file_not_found;
}

fs::path new_path(const fs::path& dir, const std::string& stem, const std::string& ext, const std::string& date, int max_tries)
{
    const std::string base = strip_suffix(clean_name(stem)) + suffix(date);
    for (int n = 1; n <= max_tries; ++n) {
        const fs::path p = dir / path_from_utf8(base + (n == 1 ? std::string() : " " + std::to_string(n)) + ext);
        if (path_is_free(p))
            return p;
    }
    return {};
}

fs::path backup_copy(const fs::path& file, const fs::path& backup_root, const std::string& date, std::string& err, int max_tries)
{
    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        err = "'" + utf8_from_path(file) + "' is not a file Forca can back up";
        return {};
    }
    const fs::path dir = backup_root / path_from_utf8(date);
    fs::create_directories(dir, ec);
    const std::string stem = utf8_from_path(file.stem()), ext = utf8_from_path(file.extension());
    for (int n = 1; n <= max_tries; ++n) {
        const fs::path p = dir / path_from_utf8(stem + (n == 1 ? std::string() : " " + std::to_string(n)) + ext);
        if (!path_is_free(p))
            continue;
        fs::copy_file(file, p, ec); // never over an existing backup
        if (ec) {
            err = "Forca could not back up '" + utf8_from_path(file) + "': " + ec.message();
            return {};
        }
        return p;
    }
    err = "no free backup name in '" + utf8_from_path(dir) + "'";
    return {};
}

std::string Ledger::key_for(const fs::path& path)
{
    std::string k = utf8_from_path(fs::absolute(path).lexically_normal());
    std::replace(k.begin(), k.end(), '\\', '/');
#ifdef _WIN32
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });
#endif
    return k;
}

std::string Ledger::file_hash(const fs::path& path)
{
    fs::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    uint64_t h = 1469598103934665603ULL;
    char     buf[65536];
    while (in) {
        in.read(buf, sizeof(buf));
        for (std::streamsize i = 0; i < in.gcount(); ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= 1099511628211ULL;
        }
    }
    std::ostringstream ss;
    ss << std::hex << h;
    return ss.str();
}

bool Ledger::may_write(const fs::path& path) const
{
    if (path_is_free(path))
        return true;
    std::lock_guard<std::mutex> lock(s_ledger_mutex);
    const json  ledger = load(m_file);
    const auto& files  = ledger["files"];
    const auto  it     = files.find(key_for(path));
    if (it == files.end() || !it->is_object())
        return false; // not the AI's file
    const std::string hash = file_hash(path);
    return !hash.empty() && it->value("hash", std::string()) == hash;
}

bool Ledger::created(const fs::path& path) const
{
    std::lock_guard<std::mutex> lock(s_ledger_mutex);
    return load(m_file)["files"].contains(key_for(path));
}

void Ledger::record_write(const fs::path& path, const std::string& date) const
{
    std::lock_guard<std::mutex> lock(s_ledger_mutex);
    json ledger = load(m_file);
    ledger["files"][key_for(path)] = { { "hash", file_hash(path) }, { "written", date } };
    try {
        fs::create_directories(m_file.parent_path());
        fs::ofstream out(m_file, std::ios::trunc);
        out << ledger.dump(2) << "\n";
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Forca AI: could not write the ledger: " << e.what();
    }
}

}} // namespace Slic3r::ForcaAI
