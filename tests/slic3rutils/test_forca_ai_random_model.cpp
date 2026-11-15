// Forca: the model files Forca AI's forca_pick_random_model picks from (src/slic3r/GUI/ForcaAIRandomModel.cpp).
#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/ForcaAI.hpp"
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <fstream>
#include <string>
#include <vector>

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

namespace {

void touch(const fs::path& p)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p.string()) << "x";
}

} // namespace

TEST_CASE("Model files are found by extension in any case, in subfolders, and nothing else is", "[ForcaAIRandomModel]")
{
    ScopedTemporaryDir dir("forca-random-model");
    const fs::path     root = dir.path();
    touch(root / "cube.stl");
    touch(root / "Benchy.3MF");
    touch(root / "parts" / "bracket.step");
    touch(root / "notes.txt");
    touch(root / "parts" / "readme.md");

    const std::vector<fs::path> files = forca_ai_model_files(root, FORCA_AI_MODEL_LIMIT);
    REQUIRE(files.size() == 3);
    CHECK(forca_ai_is_model_file("x.Drc"));
    CHECK_FALSE(forca_ai_is_model_file("x.gcode"));

    // At most `limit`, and nothing from a folder that is not there.
    CHECK(forca_ai_model_files(root, 2).size() == 2);
    CHECK(forca_ai_model_files(root / "missing", FORCA_AI_MODEL_LIMIT).empty());
}

TEST_CASE("Model files deeper than the search depth are left out", "[ForcaAIRandomModel]")
{
    ScopedTemporaryDir dir("forca-random-model-deep");
    fs::path           deep = dir.path();
    for (int i = 0; i <= FORCA_AI_MODEL_DEPTH + 1; ++i)
        deep /= "d" + std::to_string(i);
    touch(deep / "too_deep.stl");
    touch(dir.path() / "d0" / "shallow.stl");

    const std::vector<fs::path> files = forca_ai_model_files(dir.path(), FORCA_AI_MODEL_LIMIT);
    REQUIRE(files.size() == 1);
    CHECK(files.front().filename() == "shallow.stl");
}
