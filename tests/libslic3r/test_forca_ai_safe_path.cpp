// Forca AI rule R1: the AI never saves over the user's files (libslic3r/ForcaAISafePath).
#include <catch2/catch_all.hpp>

#include "libslic3r/ForcaAISafePath.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {
const std::string DATE = "2026-09-24";

void write_file(const fs::path& p, const std::string& content)
{
    fs::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}
} // namespace

TEST_CASE("AI file names carry the Claude date suffix", "[ForcaAISafePath]")
{
    CHECK(ForcaAI::suffix(DATE) == " (Claude 2026-09-24)");
}

TEST_CASE("Stripping removes stacked and numbered AI suffixes", "[ForcaAISafePath]")
{
    CHECK(ForcaAI::strip_suffix("Benchy (Claude 2026-09-23)") == "Benchy");
    CHECK(ForcaAI::strip_suffix("Benchy (Claude 2026-09-23) 3") == "Benchy");
    CHECK(ForcaAI::strip_suffix("Benchy (Claude 2026-09-23) (Claude 2026-09-24)") == "Benchy");
    CHECK(ForcaAI::strip_suffix("Benchy (calibrated 2026-09-23)") == "Benchy (calibrated 2026-09-23)");
    CHECK(ForcaAI::strip_suffix("Benchy") == "Benchy");
}

TEST_CASE("Cleaning a name removes path separators and characters Windows forbids", "[ForcaAISafePath]")
{
    CHECK(ForcaAI::clean_name("..\\..\\evil/name") == "evilname");
    CHECK(ForcaAI::clean_name("a<b>c:d\"e|f?g*h") == "abcdefgh");
    CHECK(ForcaAI::clean_name("  trailing dots... ") == "trailing dots");
    CHECK(ForcaAI::clean_name(std::string("tab\there")) == "tabhere");
    CHECK(ForcaAI::clean_name("") == "Untitled");
    CHECK(ForcaAI::clean_name("...") == "Untitled");
}

TEST_CASE("A path is free only when nothing exists there", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    CHECK(ForcaAI::path_is_free(dir.path() / "missing.3mf"));
    write_file(dir.path() / "there.3mf", "x");
    CHECK_FALSE(ForcaAI::path_is_free(dir.path() / "there.3mf"));
    fs::create_directories(dir.path() / "folder");
    CHECK_FALSE(ForcaAI::path_is_free(dir.path() / "folder"));
}

TEST_CASE("A new AI path never names an existing file", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");

    const fs::path first = ForcaAI::new_path(dir.path(), "chain", ".gcode", DATE);
    CHECK(first.filename().string() == "chain (Claude 2026-09-24).gcode");
    write_file(first, "1");

    const fs::path second = ForcaAI::new_path(dir.path(), "chain", ".gcode", DATE);
    CHECK(second.filename().string() == "chain (Claude 2026-09-24) 2.gcode");
    write_file(second, "2");

    // An AI copy of an AI file does not stack suffixes, and still skips the taken names.
    const fs::path third = ForcaAI::new_path(dir.path(), "chain (Claude 2026-09-24) 2", ".gcode", DATE);
    CHECK(third.filename().string() == "chain (Claude 2026-09-24) 3.gcode");

    // The user's own file with the plain name is never a candidate.
    write_file(dir.path() / "chain.gcode", "user");
    CHECK(ForcaAI::new_path(dir.path(), "chain", ".gcode", DATE).filename().string() != "chain.gcode");
}

TEST_CASE("A new AI path handles compound extensions", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    CHECK(ForcaAI::new_path(dir.path(), "plate", ".gcode.3mf", DATE).filename().string() == "plate (Claude 2026-09-24).gcode.3mf");
}

TEST_CASE("A new AI path gives up instead of looping when no name is free", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    write_file(dir.path() / "x (Claude 2026-09-24).txt", "");
    write_file(dir.path() / "x (Claude 2026-09-24) 2.txt", "");
    CHECK(ForcaAI::new_path(dir.path(), "x", ".txt", DATE, 2).empty());
}

TEST_CASE("The ledger lets the AI rewrite only its own unchanged files", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    const ForcaAI::Ledger ledger(dir.path() / "ai" / "ledger.json");
    const fs::path ai_file   = dir.path() / "ai (Claude 2026-09-24).3mf";
    const fs::path user_file = dir.path() / "mine.3mf";

    // Nothing there yet: writing is allowed.
    CHECK(ledger.may_write(ai_file));

    // The AI wrote it and nobody changed it: allowed again.
    write_file(ai_file, "ai version 1");
    ledger.record_write(ai_file, DATE);
    CHECK(ledger.created(ai_file));
    CHECK(ledger.may_write(ai_file));

    // The user edited it: it is theirs now.
    write_file(ai_file, "user edit");
    CHECK(ledger.created(ai_file));
    CHECK_FALSE(ledger.may_write(ai_file));

    // A file the AI never wrote is never writable.
    write_file(user_file, "user");
    CHECK_FALSE(ledger.created(user_file));
    CHECK_FALSE(ledger.may_write(user_file));
}

TEST_CASE("The ledger survives a missing or corrupt ledger file", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    const fs::path file = dir.path() / "ledger.json";
    const fs::path user = dir.path() / "mine.3mf";
    write_file(user, "user");

    CHECK_FALSE(ForcaAI::Ledger(file).may_write(user)); // no ledger yet
    write_file(file, "{ not json");
    CHECK_FALSE(ForcaAI::Ledger(file).may_write(user)); // corrupt ledger = empty ledger, never "allowed"
}

TEST_CASE("Ledger keys ignore how the path is spelled", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    const fs::path a = dir.path() / "sub" / ".." / "file.3mf";
    const fs::path b = dir.path() / "file.3mf";
    CHECK(ForcaAI::Ledger::key_for(a) == ForcaAI::Ledger::key_for(b));
}

namespace {
std::string read_file(const fs::path& p)
{
    fs::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("A backup copy keeps the original's bytes in a dated folder", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    const fs::path user = dir.path() / "Benchy.3mf";
    write_file(user, "the user's project");

    std::string    err;
    const fs::path copy = ForcaAI::backup_copy(user, dir.path() / "Backups", DATE, err);
    REQUIRE(err.empty());
    CHECK(copy == dir.path() / "Backups" / DATE / "Benchy.3mf");
    CHECK(read_file(copy) == "the user's project");
    CHECK(read_file(user) == "the user's project"); // the original is untouched
}

TEST_CASE("A second backup of the same name never replaces the first", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    const fs::path user = dir.path() / "Benchy.3mf";
    std::string    err;
    write_file(user, "version 1");
    const fs::path first = ForcaAI::backup_copy(user, dir.path() / "Backups", DATE, err);
    write_file(user, "version 2");
    const fs::path second = ForcaAI::backup_copy(user, dir.path() / "Backups", DATE, err);
    REQUIRE(err.empty());
    CHECK(second.filename() == fs::path("Benchy 2.3mf"));
    CHECK(read_file(first) == "version 1");
    CHECK(read_file(second) == "version 2");
}

TEST_CASE("A backup refuses what is not a file, so nothing gets overwritten", "[ForcaAISafePath]")
{
    ScopedTemporaryDir dir("forca-ai");
    std::string        err;
    CHECK(ForcaAI::backup_copy(dir.path() / "missing.3mf", dir.path() / "Backups", DATE, err).empty());
    CHECK_FALSE(err.empty());
    err.clear();
    CHECK(ForcaAI::backup_copy(dir.path(), dir.path() / "Backups", DATE, err).empty()); // a folder
    CHECK_FALSE(err.empty());
}
