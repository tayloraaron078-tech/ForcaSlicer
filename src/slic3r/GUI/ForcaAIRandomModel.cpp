// Forca AI: "let the AI pick a random model" (HQ PLAN_forca_ai.md §10a, Aaron 2026-11-08: a folder the user chooses,
// else Forca's built-in test models; Forca itself never downloads anything). forca_pick_random_model names one model
// file; the AI imports it with forca_import_models like any other file.

#include "ForcaAI.hpp"
#include "GUI.hpp" // from_path / into_u8 / into_path / from_u8
#include "GUI_App.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp" // resources_dir()

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

constexpr const char* FOLDER_KEY = "forca_ai_model_folder";

} // namespace

bool forca_ai_is_model_file(const fs::path& file)
{
    static const std::set<std::string> extensions = { ".stl", ".3mf", ".obj", ".step", ".stp", ".amf", ".drc" };
    return extensions.count(boost::algorithm::to_lower_copy(file.extension().string())) > 0;
}

std::vector<fs::path> forca_ai_model_files(const fs::path& folder, size_t limit)
{
    std::vector<fs::path>     out;
    boost::system::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec))
        return out;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end && out.size() < limit; it.increment(ec)) {
        if (it.depth() >= FORCA_AI_MODEL_DEPTH)
            it.disable_recursion_pending();
        if (fs::is_regular_file(it->path(), ec) && forca_ai_is_model_file(it->path()))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path forca_ai_model_folder()
{
    const std::string v = wxGetApp().app_config->get(FOLDER_KEY);
    return v.empty() ? fs::path() : into_path(from_u8(v));
}

void forca_ai_set_model_folder(const fs::path& folder)
{
    wxGetApp().app_config->set(FOLDER_KEY, folder.empty() ? std::string() : into_u8(from_path(folder)));
}

namespace {

ForcaAIResult tool_pick_random_model(const json& args)
{
    std::set<std::string> exclude;
    if (args.contains("exclude") && args["exclude"].is_array())
        for (const json& e : args["exclude"])
            if (e.is_string())
                exclude.insert(e.get<std::string>());

    const fs::path        folder = forca_ai_model_folder();
    std::vector<fs::path> files  = forca_ai_model_files(folder, FORCA_AI_MODEL_LIMIT);
    std::string           source = "folder";
    if (files.empty()) {
        files  = forca_ai_model_files(fs::path(resources_dir()) / "handy_models", FORCA_AI_MODEL_LIMIT);
        source = "built-in";
    }
    files.erase(std::remove_if(files.begin(), files.end(), [&](const fs::path& f) { return exclude.count(into_u8(from_path(f))) > 0; }),
                files.end());
    if (files.empty())
        return ForcaAIResult::error(exclude.empty() ? "No model files found." : "Every model file has been excluded.");

    static std::mt19937 rng{ std::random_device{}() };
    const fs::path      pick = files[std::uniform_int_distribution<size_t>(0, files.size() - 1)(rng)];
    json j = { { "path", into_u8(from_path(pick)) },
               { "name", into_u8(from_path(pick.stem())) },
               { "source", source },
               { "candidates", files.size() },
               { "hint", "Import it with forca_import_models. Pass this path in 'exclude' to pick another." } };
    if (source == "built-in")
        j["note"] = folder.empty() ? "The user has not chosen a model folder (Forca AI window), so this is one of Forca's "
                                     "built-in test models."
                                   : "The user's model folder has no model files, so this is one of Forca's built-in "
                                     "test models.";
    return ForcaAIResult::json(j);
}

} // namespace

void register_forca_ai_random_model_tools(ForcaAI& ai)
{
    ai.register_tool(
        { "forca_pick_random_model", "Pick a random model",
          "Pick one model file at random from the folder the user chose for this in the Forca AI window (STL, 3MF, OBJ, "
          "STEP, AMF, DRC; subfolders included), or, without one, from Forca's built-in test models (3DBenchy, Voron "
          "cube, Cali Cat, ...). Returns its path; nothing is loaded or downloaded. Use it when the user asks you to "
          "pick something to print.",
          { { "type", "object" },
            { "properties",
              { { "exclude",
                  { { "type", "array" }, { "items", { { "type", "string" } } },
                    { "description", "Paths not to pick (e.g. ones already tried)." } } } } } },
          tool_pick_random_model });
}

}} // namespace Slic3r::GUI
