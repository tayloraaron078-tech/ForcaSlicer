// Forca: the Forca AI key (src/slic3r/GUI/ForcaAI.cpp) -- one key per user account, so an AI client set up once keeps
// working with the installed Forca and with dev builds, and keeps working after updating from a version that kept the
// key in its data folder.
// Same Windows include prologue as test_forca_brand.cpp (wx pulls in <windows.h>).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_all.hpp>

#include "slic3r/GUI/ForcaAI.hpp"
#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <string>

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {
const std::string OLD_KEY(64, 'a');
const std::string NEW_KEY(64, 'b');

void write_text(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream(p.string()) << text << "\n";
}

std::string read_text(const fs::path& p)
{
    std::string s;
    boost::nowide::ifstream in(p.string());
    std::getline(in, s);
    return s;
}
} // namespace

TEST_CASE("The Forca AI key is shared by every Forca of the user and survives an update", "[ForcaAI]")
{
    ScopedTemporaryDir dir("forca-ai-key");
    const fs::path     shared = dir.path() / "local" / "forca-ai-key.txt";
    const fs::path     legacy = dir.path() / "data" / "forca" / "ai" / "key.txt";
    int                made   = 0;
    const auto         make   = [&made] { ++made; return NEW_KEY; };

    SECTION("an update adopts the key the data folder already had, so AI clients set up before keep working")
    {
        write_text(legacy, OLD_KEY);
        CHECK(forca_ai_load_key(shared.string(), legacy.string(), make) == OLD_KEY);
        CHECK(read_text(shared) == OLD_KEY);
        CHECK(made == 0);
    }
    SECTION("another data folder (a dev build) uses the shared key, not its own")
    {
        write_text(shared, OLD_KEY);
        write_text(legacy, NEW_KEY);
        CHECK(forca_ai_load_key(shared.string(), legacy.string(), make) == OLD_KEY);
        CHECK(made == 0);
    }
    SECTION("the first Forca of the user makes a key and shares it")
    {
        CHECK(forca_ai_load_key(shared.string(), legacy.string(), make) == NEW_KEY);
        CHECK(read_text(shared) == NEW_KEY);
        CHECK(made == 1);
    }
    SECTION("a damaged shared key is replaced")
    {
        write_text(shared, "short");
        CHECK(forca_ai_load_key(shared.string(), legacy.string(), make) == NEW_KEY);
        CHECK(read_text(shared) == NEW_KEY);
    }
}
