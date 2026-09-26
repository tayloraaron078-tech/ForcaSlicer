// Forca: presets under an Orca Cloud account folder are merged into user/default once (libslic3r/ForcaPresetMerge).
#include <catch2/catch_all.hpp>

#include "libslic3r/ForcaPresetMerge.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <ctime>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {
const std::time_t OLD = 1700000000;
const std::time_t NEW = 1800000000;

void write_file(const fs::path& p, const std::string& content, std::time_t mtime)
{
    fs::create_directories(p.parent_path());
    {
        fs::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << content;
    }
    fs::last_write_time(p, mtime);
}

std::string read_file(const fs::path& p)
{
    fs::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("Account presets missing from default are copied there and the account folder is untouched", "[ForcaPresetMerge]")
{
    ScopedTemporaryDir dir("forca-merge");
    const fs::path user = dir.path() / "user";
    write_file(user / "acct" / "filament" / "PLA.json", "account PLA", OLD);

    CHECK(forca_merge_account_presets(user, dir.path() / "backup") == 1);
    CHECK(read_file(user / "default" / "filament" / "PLA.json") == "account PLA");
    CHECK(read_file(user / "acct" / "filament" / "PLA.json") == "account PLA");
}

TEST_CASE("On a name clash the newer preset wins", "[ForcaPresetMerge]")
{
    ScopedTemporaryDir dir("forca-merge");
    const fs::path user = dir.path() / "user";
    write_file(user / "default" / "process" / "newer_in_account.json", "old default", OLD);
    write_file(user / "acct" / "process" / "newer_in_account.json", "new account", NEW);
    write_file(user / "default" / "process" / "newer_in_default.json", "new default", NEW);
    write_file(user / "acct" / "process" / "newer_in_default.json", "old account", OLD);
    write_file(user / "default" / "process" / "same_age.json", "default", OLD);
    write_file(user / "acct" / "process" / "same_age.json", "account", OLD);

    CHECK(forca_merge_account_presets(user, dir.path() / "backup") == 1);
    CHECK(read_file(user / "default" / "process" / "newer_in_account.json") == "new account");
    CHECK(read_file(user / "default" / "process" / "newer_in_default.json") == "new default");
    CHECK(read_file(user / "default" / "process" / "same_age.json") == "default");
}

TEST_CASE("Cloud sync markers (.info) are not copied", "[ForcaPresetMerge]")
{
    ScopedTemporaryDir dir("forca-merge");
    const fs::path user = dir.path() / "user";
    write_file(user / "acct" / "machine" / "Printer.json", "printer", OLD);
    write_file(user / "acct" / "machine" / "Printer.info", "user_id = acct", OLD);

    forca_merge_account_presets(user, dir.path() / "backup");
    CHECK(fs::exists(user / "default" / "machine" / "Printer.json"));
    CHECK_FALSE(fs::exists(user / "default" / "machine" / "Printer.info"));
}

TEST_CASE("The whole user folder is backed up before merging, and an existing backup is kept", "[ForcaPresetMerge]")
{
    ScopedTemporaryDir dir("forca-merge");
    const fs::path user   = dir.path() / "user";
    const fs::path backup = dir.path() / "backup";
    write_file(user / "default" / "filament" / "PLA.json", "old default", OLD);
    write_file(user / "acct" / "filament" / "PLA.json", "new account", NEW);

    forca_merge_account_presets(user, backup);
    CHECK(read_file(backup / "default" / "filament" / "PLA.json") == "old default");
    CHECK(read_file(backup / "acct" / "filament" / "PLA.json") == "new account");

    write_file(user / "acct" / "filament" / "PETG.json", "later", NEW);
    forca_merge_account_presets(user, backup);
    CHECK_FALSE(fs::exists(backup / "acct" / "filament" / "PETG.json"));
}

TEST_CASE("Without account folders nothing is copied and no backup is made", "[ForcaPresetMerge]")
{
    ScopedTemporaryDir dir("forca-merge");
    const fs::path user = dir.path() / "user";
    write_file(user / "default" / "filament" / "PLA.json", "default", OLD);
    write_file(user / "hints.cereal", "x", OLD);

    CHECK(forca_merge_account_presets(user, dir.path() / "backup") == 0);
    CHECK_FALSE(fs::exists(dir.path() / "backup"));
    CHECK(forca_merge_account_presets(dir.path() / "missing", dir.path() / "backup") == 0);
}
