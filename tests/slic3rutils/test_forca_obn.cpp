// Forca: Open Bamboo Networking as a network plug-in choice (src/slic3r/Utils/ForcaOBN.cpp) -- the parts that decide
// what Forca downloads, unpacks and trusts.
#include <catch2/catch_all.hpp>

#include "slic3r/Utils/ForcaOBN.hpp"

#include <string>

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
