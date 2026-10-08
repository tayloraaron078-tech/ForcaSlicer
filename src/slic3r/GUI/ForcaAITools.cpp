// Forca AI -- Phase 1 tools: read-only "eyes". None of these writes a file or talks to a printer.
#include "ForcaAI.hpp"
#include "ForcaAIKeys.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "Selection.hpp"
#include "GLCanvas3D.hpp"
#include "Notebook.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r_version.h"

#include <miniz.h>

#include <wx/dcmemory.h>
#include <wx/dcscreen.h>
#include <wx/image.h>
#include <wx/mstream.h>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

std::string to_utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

json vec3(const Vec3d& v, int decimals = 3)
{
    const double f = std::pow(10.0, decimals);
    return json::array({ std::round(v.x() * f) / f, std::round(v.y() * f) / f, std::round(v.z() * f) / f });
}


PresetCollection* collection_for(const std::string& type)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (!b)
        return nullptr;
    if (type == "printer")
        return &b->printers;
    if (type == "process")
        return &b->prints;
    if (type == "filament")
        return &b->filaments;
    return nullptr;
}

const char* volume_type_name(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART:                 return "part";
    case ModelVolumeType::NEGATIVE_VOLUME:            return "negative volume";
    case ModelVolumeType::PARAMETER_MODIFIER:         return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER:            return "support blocker";
    case ModelVolumeType::SUPPORT_ENFORCER:           return "support enforcer";
    case ModelVolumeType::SUPPORT_INTERFACE_MODIFIER: return "support-interface modifier";
    default:                                          return "unknown";
    }
}

std::string png_from_rgba(const ThumbnailData& data, bool flip_vertical)
{
    size_t png_size = 0;
    void*  png      = tdefl_write_image_to_png_file_in_memory_ex(data.pixels.data(), data.width, data.height, 4, &png_size,
                                                                 MZ_DEFAULT_LEVEL, flip_vertical ? 1 : 0);
    if (!png)
        return {};
    std::string bytes(static_cast<const char*>(png), png_size);
    mz_free(png);
    return bytes;
}

std::string png_from_image(const wxImage& image)
{
    wxMemoryOutputStream stream;
    if (!image.SaveFile(stream, wxBITMAP_TYPE_PNG))
        return {};
    std::string bytes(stream.GetSize(), '\0');
    stream.CopyTo(&bytes[0], bytes.size());
    return bytes;
}

json object_schema(json properties = json::object(), json required = json::array())
{
    json s = { { "type", "object" }, { "properties", std::move(properties) } };
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}

// ---- forca_status -----------------------------------------------------------------------------

ForcaAIResult tool_status(const json&)
{
    PresetBundle* b      = wxGetApp().preset_bundle;
    Plater*       plater = wxGetApp().plater();
    MainFrame*    mf     = wxGetApp().mainframe;
    if (!b || !plater)
        return ForcaAIResult::error("Forca is still starting up.");

    json j;
    j["app"] = { { "name", "Forca Slicer" }, { "version", FORCA_VERSION }, { "orca_base", SoftFever_VERSION } };
    if (mf && mf->m_tabpanel)
        j["active_tab"] = to_utf8(mf->m_tabpanel->GetSelectedPageName());
    j["view"] = plater->is_preview_shown() ? "preview" : "3d";

    const Preset& printer = b->printers.get_edited_preset();
    json          nozzles = json::array();
    if (const auto* nd = printer.config.option<ConfigOptionFloats>("nozzle_diameter"))
        for (double d : nd->values)
            nozzles.push_back(d);
    j["printer"] = { { "preset", printer.name }, { "unsaved_changes", printer.is_dirty }, { "nozzle_diameters", nozzles } };
    // Bed: the printable area of plate 1 (other plates sit beside it; forca_scene positions are in these coordinates).
    if (const auto* area = printer.config.option<ConfigOptionPoints>("printable_area"); area && !area->values.empty()) {
        BoundingBoxf bed;
        for (const Vec2d& pt : area->values)
            bed.merge(pt);
        j["printer"]["bed_mm"] = { { "x", { bed.min.x(), bed.max.x() } }, { "y", { bed.min.y(), bed.max.y() } },
                                   { "max_height", printer.config.opt_float("printable_height") } };
    }
    const Preset& process = b->prints.get_edited_preset();
    j["process"] = { { "preset", process.name }, { "unsaved_changes", process.is_dirty } };
    json slots = json::array();
    for (size_t i = 0; i < b->filament_presets.size(); ++i)
        slots.push_back({ { "slot", int(i + 1) }, { "preset", b->filament_presets[i] } });
    j["filaments"] = slots;

    j["project"] = { { "name", to_utf8(plater->get_project_name()) },
                     { "file", to_utf8(plater->get_project_filename(".3mf")) },
                     { "unsaved_changes", plater->is_project_dirty() } };
    PartPlateList& plates = plater->get_partplate_list();
    j["plates"]           = { { "count", plates.get_plate_count() }, { "current", plates.get_curr_plate_index() + 1 } };
    j["objects"]          = plater->model().objects.size();
    j["ai_phase"]         = "3 (see + edit + print requests + printer status, camera and safety pause for printers the user shared; printing needs the user's click on an approval card in Forca)";
    const bool advanced   = ForcaAI::instance().level() == ForcaAI::Level::Advanced;
    j["control_level"]    = { { "level", ForcaAI::level_name(ForcaAI::instance().level()) },
                              { "files", advanced ? "you may overwrite the user's files and edit their presets ('path' / 'update'); Forca backs each file up first"
                                                  : "new '(Claude <date>)' files and presets only; you may update only your own unchanged ones" },
                              { "printing", advanced ? "forca_request_print opens Forca's print dialog directly; the user presses Send"
                                                     : "forca_request_print shows the user an approval card; only their click opens the print dialog" },
                              { "changed_by", "the user only, in Forca's Forca AI window" } };
    j["print_grant"]      = forca_ai_grant_json();
    return ForcaAIResult::json(j);
}

// ---- forca_scene ------------------------------------------------------------------------------

ForcaAIResult tool_scene(const json& args)
{
    Plater* plater = wxGetApp().plater();
    if (!plater)
        return ForcaAIResult::error("Forca is still starting up.");
    const bool with_overrides = args.value("include_overrides", true);

    Model&         model  = plater->model();
    PartPlateList& plates = plater->get_partplate_list();
    json           objects = json::array();
    for (size_t oi = 0; oi < model.objects.size(); ++oi) {
        const ModelObject* o = model.objects[oi];
        json               oj;
        oj["index"]     = oi;
        oj["name"]      = o->name;
        oj["triangles"] = o->facets_count();
        if (!o->instances.empty())
            oj["size_mm"] = vec3(o->instance_bounding_box(0).size(), 2);

        json instances = json::array();
        for (size_t ii = 0; ii < o->instances.size(); ++ii) {
            const ModelInstance* inst  = o->instances[ii];
            const int            plate = plates.find_instance(int(oi), int(ii));
            instances.push_back({ { "plate", plate >= 0 ? plate + 1 : -1 },
                                  { "position_mm", vec3(inst->get_offset(), 2) },
                                  { "rotation_deg", vec3(inst->get_rotation() * (180.0 / M_PI), 2) },
                                  { "scale", vec3(inst->get_scaling_factor(), 4) },
                                  { "printable", inst->printable } });
        }
        oj["instances"] = instances;

        json volumes = json::array();
        for (const ModelVolume* v : o->volumes)
            volumes.push_back({ { "name", v->name }, { "type", volume_type_name(v->type()) }, { "filament", v->extruder_id() } });
        oj["parts"] = volumes;

        if (with_overrides) {
            json overrides = json::object();
            for (const std::string& key : o->config.keys())
                if (!forca_ai_is_secret_key(key))
                    overrides[key] = o->config.opt_serialize(key);
            oj["setting_overrides"] = overrides;
        }
        objects.push_back(oj);
    }

    json j;
    j["plates"]  = { { "count", plates.get_plate_count() }, { "current", plates.get_curr_plate_index() + 1 } };
    j["objects"] = objects;
    const Selection& sel = plater->get_selection();
    j["selection"] = sel.is_empty() ? json(nullptr) : json{ { "object_index", sel.get_object_idx() }, { "instance_index", sel.get_instance_idx() } };
    return ForcaAIResult::json(j);
}

// ---- forca_list_presets -----------------------------------------------------------------------

ForcaAIResult tool_list_presets(const json& args)
{
    const std::string type = args.value("type", std::string());
    PresetCollection* c    = collection_for(type);
    if (!c)
        return ForcaAIResult::error("type must be one of: printer, process, filament.");
    const bool  compatible_only = args.value("compatible_only", true);
    std::string search          = args.value("search", std::string());
    std::transform(search.begin(), search.end(), search.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });

    const std::string selected = c->get_selected_preset_name();
    json              list     = json::array();
    for (const Preset& p : *c) {
        if (!p.is_visible || (compatible_only && !p.is_compatible))
            continue;
        if (!search.empty()) {
            std::string n = p.name;
            std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
            if (n.find(search) == std::string::npos)
                continue;
        }
        list.push_back({ { "name", p.name },
                         { "kind", p.is_system ? "system" : (p.is_default ? "default" : "user") },
                         { "selected", p.name == selected },
                         { "compatible", p.is_compatible } });
    }
    return ForcaAIResult::json({ { "type", type }, { "count", list.size() }, { "presets", list } });
}

// ---- forca_get_settings -----------------------------------------------------------------------

ForcaAIResult tool_get_settings(const json& args)
{
    const std::string type = args.value("type", std::string());
    PresetCollection* c    = collection_for(type);
    if (!c)
        return ForcaAIResult::error("type must be one of: printer, process, filament.");
    const std::string name   = args.value("preset", std::string());
    const Preset*     preset = name.empty() ? &c->get_edited_preset() : c->find_preset(name);
    if (!preset)
        return ForcaAIResult::error("No " + type + " preset named '" + name + "'.");

    const bool     changed_only = args.value("changed_only", false);
    const bool     with_labels  = args.value("with_labels", false);
    const Preset*  parent       = changed_only ? c->get_preset_parent(*preset) : nullptr;
    std::vector<std::string> keys;
    if (args.contains("keys") && args["keys"].is_array()) {
        for (const auto& k : args["keys"])
            if (k.is_string())
                keys.push_back(k.get<std::string>());
    } else {
        keys = preset->config.keys();
    }

    json values  = json::object();
    json labels  = json::object();
    json missing = json::array();
    json hidden  = json::array();
    for (const std::string& key : keys) {
        if (forca_ai_is_secret_key(key)) {
            hidden.push_back(key);
            continue;
        }
        const ConfigOption* opt = preset->config.option(key);
        if (!opt) {
            missing.push_back(key);
            continue;
        }
        if (parent) {
            const ConfigOption* base = parent->config.option(key);
            if (base && *base == *opt)
                continue;
        }
        values[key] = opt->serialize();
        if (with_labels)
            if (const ConfigOptionDef* def = print_config_def.get(key))
                labels[key] = def->full_label.empty() ? def->label : def->full_label;
    }

    json j = { { "type", type },
               { "preset", preset->name },
               { "is_active_edited", name.empty() },
               { "unsaved_changes", preset->is_dirty },
               { "values", values } };
    if (changed_only)
        j["compared_to"] = parent ? json(parent->name) : json("(no parent preset: all values shown)");
    if (with_labels)
        j["labels"] = labels;
    if (!missing.empty())
        j["unknown_keys"] = missing;
    if (!hidden.empty())
        j["withheld_secret_keys"] = hidden;
    return ForcaAIResult::json(j);
}

// ---- forca_render_plate -----------------------------------------------------------------------

ForcaAIResult tool_render_plate(const json& args)
{
    Plater* plater = wxGetApp().plater();
    if (!plater || !plater->get_view3D_canvas3D())
        return ForcaAIResult::error("Forca is still starting up.");
    PartPlateList& plates    = plater->get_partplate_list();
    const int      plate_idx = args.value("plate", plates.get_curr_plate_index() + 1) - 1;
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        return ForcaAIResult::error("plate must be between 1 and " + std::to_string(plates.get_plate_count()) + ".");

    static const std::map<std::string, Camera::ViewAngleType> views = {
        { "iso", Camera::ViewAngleType::Iso },   { "top", Camera::ViewAngleType::Top },     { "bottom", Camera::ViewAngleType::Bottom },
        { "front", Camera::ViewAngleType::Front }, { "rear", Camera::ViewAngleType::Rear }, { "left", Camera::ViewAngleType::Left },
        { "right", Camera::ViewAngleType::Right },
    };
    const std::string view = args.value("view", std::string("iso"));
    const auto        it   = views.find(view);
    if (it == views.end())
        return ForcaAIResult::error("view must be one of: iso, top, bottom, front, rear, left, right.");
    const int size = std::clamp(args.value("size", 800), 256, 1600);

    ThumbnailData    data;
    ThumbnailsParams params{ {}, false, true, false, false, plate_idx }; // Forca's thumbnail renderer never draws the bed
    plater->get_view3D_canvas3D()->render_thumbnail(data, size, size, params, Camera::EType::Ortho, it->second);
    if (!data.is_valid())
        return ForcaAIResult::error("Forca could not render the plate (the 3D view may not be ready yet).");
    const std::string png = png_from_rgba(data, true); // OpenGL rows are bottom-up
    if (png.empty())
        return ForcaAIResult::error("Could not encode the image.");
    ForcaAIResult r = ForcaAIResult::text("Plate " + std::to_string(plate_idx + 1) + ", " + view + " view, " +
                                          std::to_string(size) + "x" + std::to_string(size) + " px.");
    r.add_png(png);
    return r;
}

// ---- forca_screenshot -------------------------------------------------------------------------

ForcaAIResult tool_screenshot(const json& args)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf || !mf->IsShown())
        return ForcaAIResult::error("The Forca window is not shown.");
    if (mf->IsIconized())
        return ForcaAIResult::error("The Forca window is minimized, so there is nothing to capture.");

    const wxRect rect = mf->GetScreenRect();
    wxBitmap     bitmap(rect.width, rect.height);
    {
        wxScreenDC screen;
        wxMemoryDC mem(bitmap);
        mem.Blit(0, 0, rect.width, rect.height, &screen, rect.x, rect.y);
        mem.SelectObject(wxNullBitmap);
    }
    wxImage   image   = bitmap.ConvertToImage();
    const int max_w   = std::clamp(args.value("max_width", 1600), 400, 2560);
    if (image.GetWidth() > max_w)
        image.Rescale(max_w, std::max(1, image.GetHeight() * max_w / image.GetWidth()), wxIMAGE_QUALITY_HIGH);
    const std::string png = png_from_image(image);
    if (png.empty())
        return ForcaAIResult::error("Could not encode the screenshot.");
    ForcaAIResult r = ForcaAIResult::text("The Forca window as it appears on screen (" + std::to_string(image.GetWidth()) +
                                          "x" + std::to_string(image.GetHeight()) + " px).");
    r.add_png(png);
    return r;
}

} // namespace

void register_forca_ai_tools(ForcaAI& ai)
{
    const json preset_type = { { "type", "string" }, { "enum", { "printer", "process", "filament" } } };

    ai.register_tool({ "forca_status", "Forca status",
        "What Forca is doing right now: app version, active tab and view, the selected printer (with nozzle sizes), "
        "process and filament presets (per project slot), the project name/file and whether it has unsaved changes, "
        "plate and object counts. Start here.",
        object_schema(), tool_status });

    ai.register_tool({ "forca_scene", "Plates and objects",
        "Everything on the plates: each object's name, size (mm), triangle count, instances (plate, position, "
        "rotation, scale, printable), parts (part/modifier/support blocker/enforcer/support-interface modifier and "
        "their filament) and the object's own setting overrides; plus the current selection.",
        object_schema({ { "include_overrides", { { "type", "boolean" }, { "description", "Include per-object setting overrides (default true)." } } } }),
        tool_scene });

    ai.register_tool({ "forca_list_presets", "List presets",
        "List printer, process or filament presets: name, kind (system/user/default), whether selected, and whether "
        "compatible with the active printer.",
        object_schema({ { "type", preset_type },
                        { "compatible_only", { { "type", "boolean" }, { "description", "Only presets compatible with the active printer (default true)." } } },
                        { "search", { { "type", "string" }, { "description", "Only names containing this text (case-insensitive)." } } } },
                      { "type" }),
        tool_list_presets });

    ai.register_tool({ "forca_get_settings", "Preset settings",
        "Read a preset's settings as key/value strings. Default is the active (edited, possibly unsaved) preset of that "
        "type; for filaments that is project slot 1 -- pass 'preset' for another. Use 'keys' for specific settings, "
        "'changed_only' for just what differs from the parent (system) preset, 'with_labels' for human-readable names. "
        "Printer secrets (passwords, access codes, API keys) are never returned.",
        object_schema({ { "type", preset_type },
                        { "preset", { { "type", "string" }, { "description", "Preset name (default: the active edited preset)." } } },
                        { "keys", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                        { "changed_only", { { "type", "boolean" } } },
                        { "with_labels", { { "type", "boolean" } } } },
                      { "type" }),
        tool_get_settings });

    ai.register_tool({ "forca_render_plate", "Render a plate",
        "A rendered image of a plate's objects (not the sliced toolpaths, modifiers or the bed; objects off the plate "
        "are left out), from a chosen view. Works whatever tab Forca is showing. For the bed, modifiers or the "
        "Preview, use forca_view + forca_screenshot.",
        object_schema({ { "plate", { { "type", "integer" }, { "minimum", 1 }, { "description", "Plate number (default: current)." } } },
                        { "view", { { "type", "string" }, { "enum", { "iso", "top", "bottom", "front", "rear", "left", "right" } } } },
                        { "size", { { "type", "integer" }, { "minimum", 256 }, { "maximum", 1600 }, { "description", "Image size in px (default 800)." } } } }),
        tool_render_plate });

    ai.register_tool({ "forca_screenshot", "Screenshot Forca",
        "A screenshot of the Forca window exactly as it is on screen (tabs, panels, dialogs). Anything covering the "
        "Forca window also shows.",
        object_schema({ { "max_width", { { "type", "integer" }, { "minimum", 400 }, { "maximum", 2560 } } } }),
        tool_screenshot });

    register_forca_ai_hand_tools(ai);
    register_forca_ai_print_tools(ai);
    register_forca_ai_printer_tools(ai);
    register_forca_ai_autonomy_tools(ai);
    register_forca_ai_academy_tools(ai);
}

}} // namespace Slic3r::GUI
