// Forca AI tools for OpenSCAD (docs/HLSD/openscad.md; DECISIONS 2026-10-26, D7): the AI reads a .scad file's Customizer
// parameters, and renders a .scad file -- one of the user's, or code it writes itself -- onto the plate, linked like an
// imported one, or in place of a linked object. OpenSCAD runs off the GUI thread; the plate is changed on it.
//
// R1 (never save over the user's files) applies to the code the AI writes: a new file goes into
// Documents\Forca AI\OpenSCAD with a "(Claude <date>)" name; an existing file is written only when the AI created it and
// nobody changed it since, or, at the Advanced level, after Forca has backed it up (ForcaAIFiles). The user's own
// .scad files are otherwise only read.
#include "ForcaAI.hpp"
#include "ForcaAIFiles.hpp"
#include "ForcaOpenScadImport.hpp"
#include "GUI.hpp" // from_path / into_path / from_u8 / into_u8
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "libslic3r/ForcaOpenScad.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <exception>
#include <ios>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

const size_t MAX_CODE      = 1024 * 1024; // longest .scad text the AI writes in one call
const int    RENDER_TIME_S = 300;         // the AI waits for its answer, so a render is stopped sooner than an import's
const size_t MAX_ECHO      = 60;          // ECHO lines returned

json object_schema(json properties = json::object(), json required = json::array())
{
    json s = { { "type", "object" }, { "properties", std::move(properties) } };
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}

json prop(const char* type, const char* description) { return { { "type", type }, { "description", description } }; }

// OpenSCAD's console for the AI: its warnings and errors (with the line of the file they name) and its ECHO lines.
json console_json(const std::string& output, const fs::path& scad)
{
    json problems = json::array();
    for (const std::string& line : forca_openscad_problems(output))
        problems.push_back(line);
    json lines = json::array();
    for (const OpenScadProblemLine& p : forca_openscad_problem_lines(output, scad.filename().string()))
        lines.push_back({ { "line", p.line }, { "error", p.error }, { "syntax_error", p.syntax } });
    json               echo = json::array();
    std::istringstream in(output);
    for (std::string line; std::getline(in, line) && echo.size() < MAX_ECHO;) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (boost::starts_with(line, "ECHO:"))
            echo.push_back(line);
    }
    return { { "warnings_and_errors", problems }, { "problem_lines", lines }, { "echo", echo } };
}

// The OpenSCAD Forca uses, from the GUI thread.
bool find_openscad(std::string& exe, std::string& err)
{
    OpenScadInfo info;
    if (!ForcaAI::instance().on_gui([&]() { info = forca_openscad_find(); }, 60000)) {
        err = "Forca did not respond in time (it may be showing a dialog that needs the user).";
        return false;
    }
    if (!info.usable) {
        err = info.path.empty() ? "No OpenSCAD found. The user needs an OpenSCAD development snapshot from 2025 or later "
                                  "(openscad.org, Development Snapshots), chosen in Preferences > OpenSCAD."
                                : "The OpenSCAD at " + info.path + " (version " + info.version + ") is too old: Forca needs a "
                                  "development snapshot from 2025 or later.";
        return false;
    }
    exe = info.path;
    return true;
}

fs::path existing_scad(const json& args, std::string& err)
{
    const std::string file = args.value("file", std::string());
    const fs::path    path = into_path(from_u8(file));
    boost::system::error_code ec;
    if (file.empty() || !fs::is_regular_file(path, ec))
        err = "No such file: " + file;
    else if (!forca_is_scad_file(path))
        err = "Not a .scad file: " + file;
    else
        return path;
    return {};
}

// Writes the AI's code (GUI thread, R1): to `file` when the AI may write it, else to a new file in its own folder.
fs::path write_code(const json& args, std::string& backup_note, std::string& err)
{
    const std::string code = args.value("code", std::string());
    if (code.size() > MAX_CODE) {
        err = "code is longer than 1 MB.";
        return {};
    }
    fs::path path;
    if (args.contains("file")) {
        path = into_path(from_u8(args.value("file", std::string())));
        boost::system::error_code ec;
        if (!forca_is_scad_file(path) || !fs::is_regular_file(path, ec)) {
            // New code always goes into the AI's own folder (D7).
            err = "file must be an existing .scad file. To write new code, leave out 'file'.";
            return {};
        }
        fs::path backup;
        if (!forca_ai_claim_write(path, backup, err)) {
            if (err.empty())
                err = "The AI may not change " + into_u8(from_path(path)) + " at the Guarded level: it isn't a file the AI wrote (or "
                      "the user has changed it since). Leave out 'file' to write the code to a new file in the AI's folder.";
            return {};
        }
        if (!backup.empty())
            backup_note = into_u8(from_path(backup));
    } else {
        boost::system::error_code ec;
        const fs::path            dir = forca_ai_default_dir() / "OpenSCAD";
        fs::create_directories(dir, ec);
        path = forca_ai_new_path(dir, forca_ai_clean_name(forca_ai_strip_suffix(args.value("name", std::string("model")))), ".scad");
        if (path.empty()) {
            err = "No free file name in " + into_u8(from_path(dir)) + ".";
            return {};
        }
    }
    {
        boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        out << code;
        if (!out) {
            err = "Could not write " + into_u8(from_path(path)) + ".";
            return {};
        }
    }
    if (backup_note.empty()) // a file the AI overwrote at the Advanced level stays the user's
        forca_ai_record_write(path);
    return path;
}

ForcaAIResult tool_params(const json& args)
{
    std::string    err;
    const fs::path scad = existing_scad(args, err);
    if (scad.empty())
        return ForcaAIResult::error(err);
    std::string exe;
    if (!find_openscad(exe, err))
        return ForcaAIResult::error(err);
    std::string                      output;
    const std::vector<OpenScadParam> params = forca_openscad_read_params(exe, scad, output, err);
    if (!err.empty())
        return ForcaAIResult::error("OpenSCAD couldn't read the parameters: " + err + "\n" + console_json(output, scad).dump(2));
    json list = json::array();
    for (const OpenScadParam& p : params) {
        json item = { { "name", p.name }, { "group", p.group }, { "caption", p.caption }, { "value", p.value } };
        item["type"] = p.type == OpenScadParam::Type::Number  ? "number" :
                       p.type == OpenScadParam::Type::Boolean ? "boolean" :
                       p.type == OpenScadParam::Type::String  ? "string" :
                                                                "vector";
        if (p.min)
            item["min"] = *p.min;
        if (p.max)
            item["max"] = *p.max;
        if (p.step)
            item["step"] = *p.step;
        if (!p.options.empty()) {
            json options = json::array();
            for (const auto& [label, value] : p.options)
                options.push_back({ { "label", label }, { "value", value } });
            item["options"] = options;
        }
        list.push_back(item);
    }
    return ForcaAIResult::json({ { "file", into_u8(from_path(scad)) },
                                 { "parameters", list },
                                 { "note", "Values are in OpenSCAD syntax (strings quoted). Pass changed ones to forca_openscad_render "
                                           "as JSON values, e.g. {\"width\": 42, \"part\": \"lid\"}." } });
}

ForcaAIResult tool_render(const json& args)
{
    if (!args.contains("code") && !args.contains("file"))
        return ForcaAIResult::error("Give 'file' (a .scad file to render) or 'code' (OpenSCAD code to write and render).");
    if (args.contains("code") && !args["code"].is_string())
        return ForcaAIResult::error("code must be a string.");

    // -D definitions from the JSON values.
    std::vector<std::pair<std::string, std::string>> values;
    if (args.contains("parameters")) {
        if (!args["parameters"].is_object())
            return ForcaAIResult::error("parameters must be an object: name -> value.");
        for (const auto& [name, value] : args["parameters"].items()) {
            try {
                values.emplace_back(name, forca_openscad_value_from_json(value.dump()));
            } catch (const std::exception& e) {
                return ForcaAIResult::error(std::string("parameters.") + name + ": " + e.what());
            }
        }
    }
    const int replace = args.value("object", -1);

    std::string err, exe, backup_note;
    if (!find_openscad(exe, err))
        return ForcaAIResult::error(err);

    // The file: the AI's code written through R1, or an existing file that is only read. A replaced object must be one
    // of Forca's OpenSCAD objects.
    fs::path scad;
    if (!ForcaAI::instance().on_gui([&]() {
            Plater* p = wxGetApp().plater();
            if (!p) {
                err = "Forca is still starting up.";
                return;
            }
            if (replace >= 0 && (replace >= int(p->model().objects.size()) || !forca_openscad_is_linked(*p->model().objects[replace]))) {
                err = "object " + std::to_string(replace) + " is not an OpenSCAD object (forca_scene shows which are).";
                return;
            }
            scad = args.contains("code") ? write_code(args, backup_note, err) : existing_scad(args, err);
        }, 30000))
        return ForcaAIResult::error("Forca did not respond in time (it may be showing a dialog that needs the user).");
    if (scad.empty())
        return ForcaAIResult::error(err);

    const OpenScadRun run     = forca_openscad_render_file(exe, scad, values, RENDER_TIME_S);
    json              console = console_json(run.output, scad);
    if (!run.ok)
        return ForcaAIResult::error(run.error + " File: " + into_u8(from_path(scad)) +
                                    ". Fix the code and call again with this file and the new code.\n" + console.dump(2));

    int  obj_idx = -1;
    json placed;
    if (!ForcaAI::instance().on_gui([&]() {
            Plater* p = wxGetApp().plater();
            if (!p) {
                err = "Forca is still starting up.";
                return;
            }
            obj_idx = forca_openscad_place(*p, run, scad, values, replace, "AI: render OpenSCAD model", err);
            if (obj_idx >= 0 && obj_idx < int(p->model().objects.size())) {
                const ModelObject* o = p->model().objects[obj_idx];
                const Vec3d        s = o->instances.empty() ? Vec3d::Zero() : o->instance_bounding_box(0).size();
                placed = { { "index", obj_idx }, { "name", o->name },
                           { "size_mm", { std::round(s.x() * 100) / 100, std::round(s.y() * 100) / 100, std::round(s.z() * 100) / 100 } },
                           { "linked", forca_openscad_is_linked(*o) } };
            }
        }, 60000))
        return ForcaAIResult::error("Forca did not respond in time (it may be showing a dialog that needs the user).");
    if (obj_idx < 0)
        return ForcaAIResult::error(err);

    json j = { { "file", into_u8(from_path(scad)) }, { "object", placed }, { "replaced", replace >= 0 }, { "console", console } };
    if (!backup_note.empty())
        j["backup_of_previous"] = backup_note;
    if (!placed.value("linked", true))
        j["note"] = "The file rendered as several objects (lazy union), so they aren't linked: OpenSCAD parameters can't "
                    "re-render them. Wrap the top level in union() to get one object.";
    return ForcaAIResult::json(j);
}

} // namespace

void register_forca_ai_openscad_tools(ForcaAI& ai)
{
    ForcaAITool params{ "forca_openscad_params", "OpenSCAD parameters",
        "The Customizer parameters of a .scad file, as OpenSCAD reads them: name, group, caption, type, current default "
        "value (OpenSCAD syntax), min / max / step and dropdown options. Read-only; needs the user's OpenSCAD.",
        object_schema({ { "file", prop("string", "Full path of the .scad file.") } }, { "file" }),
        tool_params };
    params.off_gui = true; // OpenSCAD runs on this thread
    ai.register_tool(params);

    ForcaAITool render{ "forca_openscad_render", "Render an OpenSCAD model",
        "Render OpenSCAD code onto the plate with the user's OpenSCAD (Manifold backend): either an existing .scad "
        "'file' (only read), or 'code' you write, saved first as a new file in Documents\\Forca AI\\OpenSCAD named "
        "'<name> (Claude <date>).scad' -- or, with 'file' too, into that file when it is one you wrote and the user hasn't "
        "changed (any file at the Advanced level, backed up first). Write Customizer comments (`width = 40; // [10:1:200]`, "
        "/* [Group] */) so the user can change the model under right-click > OpenSCAD parameters. 'parameters' sets "
        "Customizer values (JSON values). The result is a new object linked to the file (one 'AI: ...' undo step), or, "
        "with 'object', replaces that OpenSCAD object's model in place, keeping its position and settings. Returns the "
        "object and OpenSCAD's ECHO / WARNING / ERROR lines with the lines they name; when the render fails, fix the code "
        "and call again with the returned file and the new code. Look at the result with forca_render_plate.",
        object_schema({ { "file", prop("string", "Full path of a .scad file to render, or (with code) to write.") },
                        { "code", prop("string", "OpenSCAD code to save and render.") },
                        { "name", prop("string", "File name for new code (default 'model'); Forca adds ' (Claude <date>)'.") },
                        { "parameters", { { "type", "object" }, { "description", "Customizer values, name -> JSON value." } } },
                        { "object", { { "type", "integer" }, { "minimum", 0 },
                                      { "description", "Replace this OpenSCAD object (index from forca_scene) instead of adding one." } } } }),
        tool_render };
    render.off_gui = true;
    ai.register_tool(render);
}

}} // namespace Slic3r::GUI
