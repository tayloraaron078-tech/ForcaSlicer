// Forca: Open Bamboo Networking as a network plug-in choice (src/slic3r/Utils/ForcaOBN.cpp) -- the parts that decide
// what Forca downloads, unpacks and trusts.
#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ForcaOBN.hpp"

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>

#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {
std::string release_json(const std::string& digest, bool prerelease = false, const std::string& asset = "obn-windows-x64.zip")
{
    return std::string(R"({"tag_name":"v2.2.0","draft":false,"prerelease":)") + (prerelease ? "true" : "false") +
           R"(,"assets":[{"name":"obn-linux-x64.tar.gz","digest":"sha256:)" + std::string(64, 'a') +
           R"(","browser_download_url":"https://github.com/x/obn-linux-x64.tar.gz"},{"name":")" + asset + R"(",)" +
           (digest.empty() ? "" : R"("digest":")" + digest + R"(",)") +
           R"("browser_download_url":"https://github.com/ClusterM/open-bamboo-networking/releases/download/v2.2.0/)" + asset +
           R"("}]})";
}
} // namespace

TEST_CASE("OBN uses its own plug-in version name next to Bambu's series", "[ForcaOBN]")
{
    CHECK(forca_obn_version("02.08.01") == "02.08.01-obn");
    CHECK(forca_obn_is_obn_version("02.08.01-obn"));
    CHECK_FALSE(forca_obn_is_obn_version("02.08.01"));
    CHECK_FALSE(forca_obn_is_obn_version("02.08.01.53"));
    CHECK_FALSE(forca_obn_is_obn_version("-obn"));
}

TEST_CASE("A stable OBN release gives the Windows zip and its SHA-256", "[ForcaOBN]")
{
    const std::string hex = "D53C4CE6DCA38CAD5EDD5E90D4937F86732BFA55550FB6E42E5F8BD62310333F";
    ForcaObnRelease   r;
    std::string       err;
    REQUIRE(forca_obn_parse_release(release_json("sha256:" + hex), r, err));
    CHECK(r.tag == "v2.2.0");
    CHECK(r.zip_url == "https://github.com/ClusterM/open-bamboo-networking/releases/download/v2.2.0/obn-windows-x64.zip");
    CHECK(r.sha256 == "d53c4ce6dca38cad5edd5e90d4937f86732bfa55550fb6e42e5f8bd62310333f");
}

TEST_CASE("Forca refuses an OBN release it cannot check", "[ForcaOBN]")
{
    ForcaObnRelease r;
    std::string     err;
    CHECK_FALSE(forca_obn_parse_release(release_json(""), r, err));                                  // no digest
    CHECK_FALSE(forca_obn_parse_release(release_json("md5:abc"), r, err));                           // not SHA-256
    CHECK_FALSE(forca_obn_parse_release(release_json("sha256:" + std::string(64, 'a'), true), r, err)); // pre-release
    CHECK_FALSE(forca_obn_parse_release(release_json("sha256:" + std::string(64, 'a'), false, "obn-windows-arm64.zip"), r, err));
    CHECK_FALSE(forca_obn_parse_release("not json", r, err));
}

TEST_CASE("Only the plug-in series' DLLs are taken from the OBN zip", "[ForcaOBN]")
{
    CHECK(forca_obn_zip_entry_matches("obn-windows-x64\\lib\\v02.08.01\\bambu_networking.dll", "02.08.01", "bambu_networking.dll"));
    CHECK(forca_obn_zip_entry_matches("obn-windows-x64/lib/v02.08.01/BambuSource.dll", "02.08.01", "BambuSource.dll"));
    CHECK(forca_obn_zip_entry_matches("lib/v02.08.01/BambuSource.dll", "02.08.01", "BambuSource.dll"));
    CHECK_FALSE(forca_obn_zip_entry_matches("obn-windows-x64/lib/v02.03.00/bambu_networking.dll", "02.08.01", "bambu_networking.dll"));
    CHECK_FALSE(forca_obn_zip_entry_matches("obn-windows-x64/lib/v02.08.01/ota/plugins/bambu_networking.dll", "02.08.01",
                                            "bambu_networking.dll"));
    CHECK_FALSE(forca_obn_zip_entry_matches("obn-windows-x64/xlib/v02.08.01/bambu_networking.dll", "02.08.01", "bambu_networking.dll"));
}

TEST_CASE("A crash inside Bambu's network plug-in is recognised from Forca's crash log", "[ForcaOBN]")
{
    const std::string plugin = "Exception Code :c0000005 ACCESS_VIOLATION\n"
                               "Fault address:  0x12BDC840 0x1:0xEB840 C:\\Users\\x\\AppData\\Roaming\\ForcaSlicer\\plugins\\bambu_networking_02.08.01.dll\n";
    const std::string forca  = "Exception Code :c0000005 ACCESS_VIOLATION\n"
                               "Fault address:  0xAAC397DD 0x1:0x24A87DD C:\\Program Files\\Forca Slicer\\ForcaSlicer.dll\n"
                               "Show CallStack:\n7ff9aac397dd: bambu_networking somewhere in the stack\n";
    CHECK(forca_obn_crash_in_network_plugin(plugin));
    CHECK_FALSE(forca_obn_crash_in_network_plugin(forca));
    CHECK_FALSE(forca_obn_crash_in_network_plugin(""));
}

TEST_CASE("The OBN download is checked with SHA-256", "[ForcaOBN]")
{
    CHECK(forca_obn_sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(forca_obn_sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Every plug-in series in an OBN zip is found, with both of its DLLs", "[ForcaOBN]")
{
    const std::vector<std::string> entries = {
        "obn-windows-x64/install.ps1",
        "obn-windows-x64\\lib\\v02.08.04\\bambu_networking.dll", "obn-windows-x64/lib/v02.08.04/BambuSource.dll",
        "obn-windows-x64/lib/v02.08.01/bambu_networking.dll", "obn-windows-x64/lib/v02.08.01/BambuSource.dll",
        "obn-windows-x64/lib/v02.03.00/bambu_networking.dll",                 // no camera DLL: not a usable build
        "obn-windows-x64/xlib/v02.09.00/bambu_networking.dll", "obn-windows-x64/xlib/v02.09.00/BambuSource.dll",
        "obn-windows-x64/lib/v02.08.01/ota/plugins/bambu_networking.dll", // nested: not the series folder
    };
    CHECK(forca_obn_zip_series(entries) == std::vector<std::string>{ "02.08.01", "02.08.04" });
    CHECK(forca_obn_zip_series({}).empty());
}

TEST_CASE("OBN follows Forca's plug-in series at start-up", "[ForcaOBN]")
{
    const std::vector<std::string> cached = { "02.08.01", "02.08.04" };
    // Not chosen: nothing to do, whatever is configured.
    CHECK(forca_obn_startup(false, "02.08.04", "02.08.04", cached, true) == ForcaObnStartup::NotChosen);
    // Chosen and already in place for this series.
    CHECK(forca_obn_startup(true, "02.08.04-obn", "02.08.04", cached, true) == ForcaObnStartup::Ready);
    // The series changed (an upstream merge): the cached release has the new series -> no download.
    CHECK(forca_obn_startup(true, "02.08.01-obn", "02.08.04", cached, false) == ForcaObnStartup::Activate);
    // Selected for this series but the plug-in file is gone: put it back from the cache.
    CHECK(forca_obn_startup(true, "02.08.04-obn", "02.08.04", cached, false) == ForcaObnStartup::Activate);
    // Nothing on disk for the new series -> ask to download.
    CHECK(forca_obn_startup(true, "02.08.01-obn", "02.08.04", { "02.08.01" }, false) == ForcaObnStartup::Download);
    CHECK(forca_obn_startup(true, "", "02.08.04", {}, false) == ForcaObnStartup::Download);
}

TEST_CASE("A cached OBN release counts only for the plug-in series it has files for", "[ForcaOBN]")
{
    ScopedDataDir data("forca-obn-cache");
    const boost::filesystem::path release = boost::filesystem::path(forca_obn_cache_dir("v2.2.0"));
    auto put = [](const boost::filesystem::path& dir) {
        boost::filesystem::create_directories(dir);
        for (const char* file : { "bambu_networking.dll", "BambuSource.dll" })
            boost::nowide::ofstream((dir / file).string()) << "dll";
    };

    // An older Forca kept one series flat in the release folder: after the move it is that series' folder.
    put(release);
    forca_obn_adopt_flat_cache("v2.2.0", "02.08.01");
    CHECK(forca_obn_cached_series("v2.2.0") == std::vector<std::string>{ "02.08.01" });
    CHECK(forca_obn_cached_tags() == std::vector<std::string>{ "v2.2.0" });

    // The release is on disk, but not for Forca's new plug-in series -- so it still has to be downloaded.
    CHECK(forca_obn_has_series("v2.2.0", "02.08.01"));
    CHECK_FALSE(forca_obn_has_series("v2.2.0", "02.08.04"));
    CHECK_FALSE(forca_obn_has_series("", "02.08.04"));

    put(release / "v02.08.04");
    CHECK(forca_obn_has_series("v2.2.0", "02.08.04"));
}

namespace {
// An OBN-style Windows zip in memory: folder entries first (as real archives have), then files.
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& entries)
{
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    REQUIRE(mz_zip_writer_init_heap(&zip, 0, 0));
    for (const auto& [name, data] : entries)
        REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION));
    void*  buf  = nullptr;
    size_t size = 0;
    REQUIRE(mz_zip_writer_finalize_heap_archive(&zip, &buf, &size));
    std::string out(static_cast<const char*>(buf), size);
    mz_zip_writer_end(&zip);
    mz_free(buf);
    return out;
}

std::string read_text(const boost::filesystem::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("Every plug-in series of an OBN zip is unpacked, and a failed unpack keeps the cached builds", "[ForcaOBN]")
{
    ScopedDataDir                 data("forca-obn-unpack");
    const boost::filesystem::path release = boost::filesystem::path(forca_obn_cache_dir("v2.2.0"));
    // What an older Forca left: one series, cached before.
    boost::filesystem::create_directories(release / "v02.08.01");
    for (const char* file : { "bambu_networking.dll", "BambuSource.dll" })
        boost::nowide::ofstream((release / "v02.08.01" / file).string()) << "old";

    std::string err;
    // A zip with no plug-in builds fails, says why, and leaves the cache alone.
    CHECK_FALSE(forca_obn_unpack(make_zip({ { "obn-windows-x64/", "" }, { "obn-windows-x64/install.ps1", "x" } }), "v2.2.0", err));
    CHECK_FALSE(err.empty());
    CHECK(read_text(release / "v02.08.01" / "bambu_networking.dll") == "old");

    err.clear();
    const std::string zip = make_zip({
        { "obn-windows-x64/", "" },
        { "obn-windows-x64/lib/", "" },
        { "obn-windows-x64/lib/v02.08.01/", "" },
        { "obn-windows-x64/lib/v02.08.04/", "" },
        { "obn-windows-x64/install.ps1", "x" },
        { "obn-windows-x64/lib/v02.08.01/bambu_networking.dll", "net-01" },
        { "obn-windows-x64/lib/v02.08.01/BambuSource.dll", "cam-01" },
        { "obn-windows-x64/lib/v02.08.04/bambu_networking.dll", "net-04" },
        { "obn-windows-x64/lib/v02.08.04/BambuSource.dll", "cam-04" },
    });
    REQUIRE(forca_obn_unpack(zip, "v2.2.0", err));
    CHECK(err.empty());
    CHECK(forca_obn_cached_series("v2.2.0") == std::vector<std::string>{ "02.08.01", "02.08.04" });
    CHECK(read_text(release / "v02.08.04" / "bambu_networking.dll") == "net-04");
    CHECK(read_text(release / "v02.08.01" / "BambuSource.dll") == "cam-01"); // the old copy was replaced
}
