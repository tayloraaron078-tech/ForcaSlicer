// Forca AI -- Phase 2 tools: "hands". Import and arrange models, presets, per-object settings, modifiers, slicing,
// saving. Every edit is one "AI: ..." undo step (Ctrl+Z undoes it). Every file write goes through ForcaAIFiles (R1).
// Nothing here talks to a printer (R2 is Phase 3).
#include "ForcaAI.hpp"
#include "ForcaAIFiles.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GLCanvas3D.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "Jobs/Worker.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>

namespace Slic3r { namespace GUI {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// ---- shared helpers ---------------------------------------------------------------------------

json object_schema(json properties = json::object(), json required = json::array())
{
    json s = { { "type", "object" }, { "properties", std::move(properties) } };
    if (!required.empty())
        s["required"] = std::move(required);
    return s;
}

json prop(const char* type, const char* description) { return { { "type", type }, { "description", description } }; }

json vec_prop(int min_items, int max_items, const char* description)
{
    return { { "type", "array" }, { "items", { { "type", "number" } } }, { "minItems", min_items }, { "maxItems", max_items },
             { "description", description } };
}

json round3(const Vec3d& v)
{
    return json::array({ std::round(v.x() * 100) / 100, std::round(v.y() * 100) / 100, std::round(v.z() * 100) / 100 });
}

// Same list as the read tools: printer secrets are never read or written by the AI.
bool is_secret_key(const std::string& key)
{
    std::string k = key;
    std::transform(k.begin(), k.end(), k.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* bad : { "password", "apikey", "api_key", "access_code", "token", "secret", "printhost_user", "cookie" })
        if (k.find(bad) != std::string::npos)
            return true;
    return false;
}

std::string json_to_setting(const json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "1" : "0";
    if (v.is_array()) { // ["a","b"] / [1,2] -> "a,b"
        std::string out;
        for (const auto& e : v)
            out += (out.empty() ? "" : ",") + json_to_setting(e);
        return out;
    }
    return v.dump();
}

// Parses `value` for `key` and sets it on `config`. A single value given for a per-extruder / per-variant vector is
// repeated to the vector's current length (H2S filament presets hold one value per extruder variant; a shorter vector
// makes Forca throw -- see the calibration wizard's note).
bool set_setting(DynamicPrintConfig& config, const std::string& key, const std::string& value, std::string& err)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (!def) {
        err = "unknown setting '" + key + "'";
        return false;
    }
    size_t old_size = 0;
    if (const auto* old = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key)))
        old_size = old->size();
    try {
        ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Disable);
        config.set_deserialize(key, value, ctx);
    } catch (const std::exception& e) {
        err = "'" + value + "' is not a valid value for '" + key + "' (" + e.what() + ")";
        return false;
    }
    if (auto* nv = dynamic_cast<ConfigOptionVectorBase*>(config.option(key)))
        if (!nv->empty() && nv->size() < old_size)
            nv->resize(old_size);
    return true;
}

PresetCollection* collection_for(const std::string& type)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (!b)
        return nullptr;
    if (type == "printer")  return &b->printers;
    if (type == "process")  return &b->prints;
    if (type == "filament") return &b->filaments;
    return nullptr;
}

Preset::Type preset_type_for(const std::string& type)
{
    if (type == "printer")
        return Preset::TYPE_PRINTER;
    if (type == "filament")
        return Preset::TYPE_FILAMENT;
    return Preset::TYPE_PRINT;
}

// The AI never discards the user's unsaved preset edits (switching or saving a preset would).
std::string unsaved_edits_error(PresetCollection& c)
{
    if (!c.current_is_dirty())
        return {};
    return "The preset '" + c.get_edited_preset().name + "' has unsaved changes in Forca. The AI does not discard the "
           "user's edits: ask them to save or discard those changes first.";
}

Plater* plater_ready(std::string& err)
{
    Plater* p = wxGetApp().plater();
    if (!p || !wxGetApp().preset_bundle) {
        err = "Forca is still starting up.";
        return nullptr;
    }
    if (!p->get_ui_job_worker().is_idle()) {
        err = "Forca is busy with another job (arrange / orient / ...). Try again in a moment.";
        return nullptr;
    }
    return p;
}

ModelObject* object_arg(Plater* p, const json& args, int& idx, std::string& err)
{
    idx = args.value("object", -1);
    if (idx < 0 || idx >= int(p->model().objects.size())) {
        err = "object must be an object index from forca_scene (0 to " + std::to_string(int(p->model().objects.size()) - 1) + ").";
        return nullptr;
    }
    return p->model().objects[idx];
}

void select_object(Plater* p, int obj_idx)
{
    Selection& sel = p->get_view3D_canvas3D()->get_selection();
    if (obj_idx < 0)
        sel.clear();
    else
        sel.add_object(unsigned(obj_idx), true);
    p->get_view3D_canvas3D()->set_as_dirty();
    if (ObjectList* list = wxGetApp().obj_list())
        list->update_selections();
}

// Refresh after changing an object's instances or volumes directly in the model.
void object_changed(Plater* p, int obj_idx)
{
    if (ObjectList* list = wxGetApp().obj_list())
        list->notify_instance_updated(obj_idx);
    p->changed_object(obj_idx); // reloads the 3D scene and schedules the background process
}

fs::path folder_arg(const json& args, std::string& err)
{
    const std::string folder = args.value("folder", std::string());
    if (folder.empty())
        return forca_ai_default_dir();
    const fs::path dir = into_path(from_u8(folder));
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        err = "folder '" + folder + "' does not exist (the AI saves into existing folders only).";
        return {};
    }
    return dir;
}

// The output path for an AI save: an explicit `path` the AI may write (its own unchanged file), else a new
// "(Claude <date>)" name in `folder`.
fs::path output_path(const json& args, const std::string& default_stem, const std::string& ext, std::string& err)
{
    const std::string explicit_path = args.value("path", std::string());
    if (!explicit_path.empty()) {
        const fs::path p = into_path(from_u8(explicit_path));
        if (!boost::algorithm::iends_with(explicit_path, ext)) {
            err = "path must end with " + ext;
            return {};
        }
        if (!forca_ai_may_write(p)) {
            err = "Forca does not let the AI write '" + explicit_path + "': it is the user's file, or the user changed "
                  "it since the AI saved it. Leave out 'path' to save under a new name.";
            return {};
        }
        return p;
    }
    const fs::path dir = folder_arg(args, err);
    if (dir.empty())
        return {};
    const std::string name = args.value("name", default_stem);
    fs::path          p    = forca_ai_new_path(dir, name, ext);
    if (p.empty())
        err = "Could not find a free file name in '" + into_u8(from_path(dir)) + "' (is the folder readable?).";
    return p;
}

std::string project_stem(Plater* p)
{
    const std::string n = into_u8(p->get_project_name());
    return n.empty() ? std::string("Untitled") : n;
}

// ---- forca_import_models ----------------------------------------------------------------------

// Moves objects [first, end) of the model, one by one, to the free spot on the current plate nearest its centre.
void place_new_objects(Plater* p, size_t first)
{
    const double      gap   = 5.0; // mm between footprints
    const BoundingBoxf3 bed = p->get_partplate_list().get_curr_plate()->get_build_volume();
    const Vec2d       centre(bed.center().x(), bed.center().y());
    std::vector<BoundingBoxf> taken;
    auto footprint = [](const ModelObject* o, size_t i) {
        const BoundingBoxf3 b = o->instance_bounding_box(i);
        return BoundingBoxf(Vec2d(b.min.x(), b.min.y()), Vec2d(b.max.x(), b.max.y()));
    };
    for (size_t i = 0; i < first; ++i)
        for (size_t k = 0; k < p->model().objects[i]->instances.size(); ++k)
            taken.push_back(footprint(p->model().objects[i], k));

    for (size_t i = first; i < p->model().objects.size(); ++i) {
        ModelObject* o = p->model().objects[i];
        for (size_t k = 0; k < o->instances.size(); ++k) {
            ModelInstance*     inst = o->instances[k];
            const BoundingBoxf fp   = footprint(o, k);
            const Vec2d        half = 0.5 * (fp.max - fp.min);
            const Vec2d        ref  = inst->get_offset().head<2>() - 0.5 * (fp.min + fp.max); // offset vs. footprint centre
            // Candidate centres on a 5 mm grid, nearest the plate centre first.
            std::vector<Vec2d> candidates;
            for (double x = bed.min.x() + half.x(); x <= bed.max.x() - half.x(); x += 5.0)
                for (double y = bed.min.y() + half.y(); y <= bed.max.y() - half.y(); y += 5.0)
                    candidates.emplace_back(x, y);
            std::sort(candidates.begin(), candidates.end(),
                      [&](const Vec2d& a, const Vec2d& b) { return (a - centre).squaredNorm() < (b - centre).squaredNorm(); });
            for (const Vec2d& c : candidates) {
                const BoundingBoxf box(c - half - Vec2d(gap, gap), c + half + Vec2d(gap, gap));
                if (std::none_of(taken.begin(), taken.end(), [&](const BoundingBoxf& t) { return t.overlap(box); })) {
                    inst->set_offset(Vec3d(c.x() + ref.x(), c.y() + ref.y(), inst->get_offset().z()));
                    break;
                } // no free spot: it stays where Forca put it (forca_arrange can use the other plates)
            }
            taken.push_back(footprint(o, k));
        }
        object_changed(p, int(i));
    }
}

ForcaAIResult tool_import(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    if (!args.contains("paths") || !args["paths"].is_array() || args["paths"].empty())
        return ForcaAIResult::error("paths must be a list of model files.");

    static const std::set<std::string> exts = { ".stl", ".3mf", ".obj", ".step", ".stp", ".amf", ".drc", ".svg" };
    std::vector<fs::path> files;
    for (const auto& v : args["paths"]) {
        if (!v.is_string())
            return ForcaAIResult::error("paths must be strings.");
        const fs::path f = into_path(from_u8(v.get<std::string>()));
        boost::system::error_code ec;
        if (!fs::is_regular_file(f, ec))
            return ForcaAIResult::error("No such file: " + v.get<std::string>());
        std::string ext = f.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (!exts.count(ext))
            return ForcaAIResult::error("Unsupported model type: " + v.get<std::string>() + " (stl, 3mf, obj, step, amf, drc, svg).");
        files.push_back(f);
    }

    PartPlateList& plates = p->get_partplate_list();
    if (args.contains("plate")) {
        const int plate = args.value("plate", 1) - 1;
        if (plate < 0 || plate >= plates.get_plate_count())
            return ForcaAIResult::error("plate must be between 1 and " + std::to_string(plates.get_plate_count()) + ".");
        p->select_plate(plate);
    }

    const size_t before = p->model().objects.size();
    {
        Plater::TakeSnapshot snapshot(p, "AI: import models");
        // Geometry only: a 3MF's printer/process/filament settings would replace the user's presets.
        p->load_files(files, LoadStrategy::LoadModel, false);
        // Forca's loader checks "is the plate empty?" once for the whole batch, so every model of a multi-file
        // load lands on the plate centre. Move each new model to the free spot nearest the centre (its footprint
        // plus a gap must clear everything already placed); existing objects never move.
        place_new_objects(p, before);
    }
    json added = json::array();
    for (size_t i = before; i < p->model().objects.size(); ++i) {
        const ModelObject* o = p->model().objects[i];
        added.push_back({ { "index", i }, { "name", o->name },
                          { "size_mm", o->instances.empty() ? json(nullptr) : round3(o->instance_bounding_box(0).size()) } });
    }
    if (added.empty())
        return ForcaAIResult::error("Nothing was imported (Forca may have shown a message; see forca_screenshot).");
    return ForcaAIResult::json({ { "imported", added } });
}

// ---- forca_transform_object -------------------------------------------------------------------

ForcaAIResult tool_transform(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    int          obj_idx = -1;
    ModelObject* o       = object_arg(p, args, obj_idx, err);
    if (!o)
        return ForcaAIResult::error(err);
    const int inst_idx = args.value("instance", 0);
    if (inst_idx < 0 || inst_idx >= int(o->instances.size()))
        return ForcaAIResult::error("instance must be between 0 and " + std::to_string(int(o->instances.size()) - 1) + ".");
    ModelInstance* inst = o->instances[inst_idx];

    auto read_vec = [&](const char* key, int n, Vec3d& out) -> bool {
        if (!args.contains(key))
            return false;
        const json& a = args[key];
        if (!a.is_array() || int(a.size()) < n || a.size() > 3)
            throw std::runtime_error(std::string(key) + " must be a list of " + std::to_string(n) + " to 3 numbers.");
        for (size_t i = 0; i < a.size(); ++i)
            out[i] = a[i].get<double>();
        return true;
    };

    Vec3d offset = inst->get_offset(), rotation = inst->get_rotation() * (180.0 / M_PI), scale = inst->get_scaling_factor();
    Vec3d move_by = Vec3d::Zero();
    bool  changed = false;
    try {
        changed |= read_vec("position", 2, offset);
        if (read_vec("move_by", 2, move_by)) {
            offset += move_by;
            changed = true;
        }
        changed |= read_vec("rotation_deg", 3, rotation);
        if (args.contains("scale")) {
            if (args["scale"].is_number()) {
                const double s = args["scale"].get<double>();
                scale          = Vec3d(s, s, s);
            } else
                read_vec("scale", 3, scale);
            changed = true;
        }
    } catch (const std::exception& e) {
        return ForcaAIResult::error(e.what());
    }
    if (!changed)
        return ForcaAIResult::error("Give at least one of: position, move_by, rotation_deg, scale.");
    if (scale.minCoeff() <= 0.0001 || scale.maxCoeff() > 100.0)
        return ForcaAIResult::error("scale factors must be between 0.0001 and 100 (1 = 100%).");

    {
        Plater::TakeSnapshot snapshot(p, "AI: transform " + o->name);
        inst->set_offset(offset);
        inst->set_rotation(rotation * (M_PI / 180.0));
        inst->set_scaling_factor(scale);
        if (args.value("drop_to_bed", true))
            o->ensure_on_bed();
        object_changed(p, obj_idx);
    }
    const int plate = p->get_partplate_list().find_instance(obj_idx, inst_idx);
    return ForcaAIResult::json({ { "object", obj_idx }, { "instance", inst_idx },
                                 { "plate", plate >= 0 ? json(plate + 1) : json("none -- the object is off every plate (check the bed size in forca_status)") },
                                 { "position_mm", round3(inst->get_offset()) },
                                 { "rotation_deg", round3(inst->get_rotation() * (180.0 / M_PI)) },
                                 { "scale", round3(inst->get_scaling_factor()) },
                                 { "size_mm", round3(o->instance_bounding_box(inst_idx).size()) } });
}

// ---- forca_add_copies / forca_delete_object ---------------------------------------------------

ForcaAIResult tool_add_copies(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    int          obj_idx = -1;
    ModelObject* o       = object_arg(p, args, obj_idx, err);
    if (!o)
        return ForcaAIResult::error(err);
    const int copies = args.value("copies", 1);
    if (copies < 1 || copies > 50)
        return ForcaAIResult::error("copies must be between 1 and 50.");
    const size_t before = o->instances.size();
    {
        Plater::TakeSnapshot snapshot(p, "AI: add copies of " + o->name);
        select_object(p, obj_idx);
        p->increase_instances(size_t(copies)); // Forca places them itself
    }
    if (o->instances.size() == before)
        return ForcaAIResult::error("Forca did not add copies (the object may not allow it).");
    return ForcaAIResult::text(o->name + " now has " + std::to_string(o->instances.size()) + " instances.");
}

ForcaAIResult tool_delete_object(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    int          obj_idx = -1;
    ModelObject* o       = object_arg(p, args, obj_idx, err);
    if (!o)
        return ForcaAIResult::error(err);
    if (o->is_cut())
        return ForcaAIResult::error("This object is part of a cut; Forca asks the user before breaking that, so delete it in Forca.");
    const std::string name = o->name;
    {
        Plater::TakeSnapshot snapshot(p, "AI: remove " + name);
        select_object(p, -1);
        p->remove(size_t(obj_idx));
    }
    return ForcaAIResult::text("Removed '" + name + "' from the plate (Ctrl+Z in Forca brings it back). Object indices "
                               "after it moved down by one.");
}

// ---- forca_arrange / forca_auto_orient --------------------------------------------------------

ForcaAIResult tool_arrange(const json&)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    if (p->model().objects.empty())
        return ForcaAIResult::error("There is nothing on the plates to arrange.");
    select_object(p, -1); // no selection = arrange everything
    {
        Plater::TakeSnapshot snapshot(p, "AI: arrange"); // replaces Forca's own "Arrange" step, so forca_undo can undo it
        p->arrange();                                   // runs as a job
    }
    return ForcaAIResult::text("Arranging all objects (a background job: call forca_scene in a moment for the new positions).");
}

ForcaAIResult tool_orient(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    if (p->model().objects.empty())
        return ForcaAIResult::error("There is nothing to orient.");
    int obj_idx = -1;
    if (args.contains("object")) {
        if (!object_arg(p, args, obj_idx, err))
            return ForcaAIResult::error(err);
    }
    select_object(p, obj_idx); // -1: all objects
    {
        Plater::TakeSnapshot snapshot(p, "AI: auto-orient"); // replaces Forca's own "Orient" step
        p->orient();                                        // runs as a job
    }
    return ForcaAIResult::text(std::string("Auto-orienting ") + (obj_idx < 0 ? "all objects" : "object " + std::to_string(obj_idx)) +
                               " (a background job: call forca_scene in a moment for the result).");
}

// ---- presets ----------------------------------------------------------------------------------

// Puts filament preset `name` into project slot `idx` (0-based), the way the sidebar's combo does. The slot the
// Filament tab is showing is switched through the tab (like the calibration wizard), so the tab stays in sync.
bool set_filament_slot(size_t idx, const std::string& name, std::string& err)
{
    PresetBundle* b      = wxGetApp().preset_bundle;
    Plater*       plater = wxGetApp().plater();
    if (idx >= b->filament_presets.size()) {
        err = "filament_slot must be between 1 and " + std::to_string(b->filament_presets.size()) + ".";
        return false;
    }
    const std::string old = b->filament_presets[idx];
    if (b->filaments.get_selected_preset_name() == old || b->filament_presets.size() == 1)
        if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
            tab->select_preset(name);
    b->set_filament_preset(idx, name);
    plater->update_project_dirty_from_presets();
    b->export_selections(*wxGetApp().app_config);
    plater->sidebar().update_dynamic_filament_list();
    plater->on_filament_change(idx);
    plater->sidebar().update_presets(Preset::TYPE_FILAMENT); // repaint the slot combos
    b->set_filament_preset(idx, name);                       // (that re-sync must not undo our slot)
    if (b->filament_presets[idx] != name) {
        err = "Forca did not accept the filament for that slot.";
        return false;
    }
    return true;
}

json filament_slots()
{
    json          slots = json::array();
    PresetBundle* b     = wxGetApp().preset_bundle;
    for (size_t i = 0; i < b->filament_presets.size(); ++i)
        slots.push_back({ { "slot", int(i + 1) }, { "preset", b->filament_presets[i] } });
    return slots;
}

ForcaAIResult tool_select_preset(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    const std::string type = args.value("type", std::string());
    PresetCollection* c    = collection_for(type);
    if (!c)
        return ForcaAIResult::error("type must be one of: printer, process, filament.");
    const std::string name   = args.value("name", std::string());
    const Preset*     preset = c->find_preset(name);
    if (!preset || preset->name != name)
        return ForcaAIResult::error("No " + type + " preset named '" + name + "' (see forca_list_presets).");
    if (!preset->is_compatible && type != "printer")
        return ForcaAIResult::error("'" + name + "' is not compatible with the selected printer.");

    if (type == "filament") {
        const int slot = args.value("filament_slot", 1);
        PresetBundle* b = wxGetApp().preset_bundle;
        // Only the slot the Filament tab shows is switched through the tab, which would discard unsaved edits.
        if (slot >= 1 && size_t(slot) <= b->filament_presets.size() &&
            (b->filaments.get_selected_preset_name() == b->filament_presets[slot - 1] || b->filament_presets.size() == 1))
            if (std::string e = unsaved_edits_error(*c); !e.empty())
                return ForcaAIResult::error(e);
        if (slot < 1 || !set_filament_slot(size_t(slot - 1), name, err))
            return ForcaAIResult::error(err.empty() ? "filament_slot must be 1 or more." : err);
        return ForcaAIResult::json({ { "filaments", filament_slots() } });
    }

    if (std::string e = unsaved_edits_error(*c); !e.empty())
        return ForcaAIResult::error(e);
    Tab* tab = wxGetApp().get_tab(preset_type_for(type));
    if (!tab)
        return ForcaAIResult::error("Forca's " + type + " settings tab is not available.");
    tab->select_preset(name);
    if (c->get_selected_preset_name() != name)
        return ForcaAIResult::error("Forca did not switch to '" + name + "' (it may be showing a message; see forca_screenshot).");
    return ForcaAIResult::text("Selected " + type + " preset '" + name + "'.");
}

// Creates a new "(Claude <date>)" user preset (or updates one the AI created and the user has not changed since),
// from a base preset plus changed settings, and selects it. The user's own presets are never edited (R1).
ForcaAIResult tool_create_preset(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    const std::string type = args.value("type", std::string());
    PresetCollection* c    = collection_for(type);
    if (!c)
        return ForcaAIResult::error("type must be one of: printer, process, filament.");
    if (!args.contains("settings") || !args["settings"].is_object() || args["settings"].empty())
        return ForcaAIResult::error("settings must be an object of setting key -> new value.");
    if (std::string e = unsaved_edits_error(*c); !e.empty())
        return ForcaAIResult::error(e);

    PresetBundle*     b         = wxGetApp().preset_bundle;
    const int         slot      = args.value("filament_slot", 1);
    if (type == "filament" && (slot < 1 || size_t(slot) > b->filament_presets.size()))
        return ForcaAIResult::error("filament_slot must be between 1 and " + std::to_string(b->filament_presets.size()) + ".");

    // The preset to start from: an explicit base, else the one in use (for filaments: the slot's).
    std::string base_name = args.value("base", std::string());
    if (base_name.empty())
        base_name = type == "filament" ? b->filament_presets[slot - 1] : c->get_selected_preset_name();
    const Preset* base = c->find_preset(base_name);
    if (!base || base->name != base_name)
        return ForcaAIResult::error("No " + type + " preset named '" + base_name + "'.");

    // The name: updating one of the AI's own presets, or a new "(Claude <date>)" name.
    std::string target = args.value("update", std::string());
    const bool  update = !target.empty();
    if (update) {
        const Preset* own = c->find_preset(target);
        if (!own || own->name != target || own->is_system || own->is_default)
            return ForcaAIResult::error("No user preset named '" + target + "' to update.");
        if (!forca_ai_may_write(into_path(from_u8(own->file))) || !forca_ai_created(into_path(from_u8(own->file))))
            return ForcaAIResult::error("'" + target + "' is not a preset the AI created, or the user changed it since. "
                                        "Leave out 'update' to create a new preset instead.");
        base = own; // an update starts from the AI preset itself
    } else {
        const std::string stem = forca_ai_strip_suffix(args.value("name", base->name)) + forca_ai_suffix();
        target = stem;
        for (int n = 2; c->find_preset(target) && c->find_preset(target)->name == target; ++n)
            target = stem + " " + std::to_string(n);
    }

    Preset temp = *base;
    json   applied = json::object();
    static const std::set<std::string> identity = { "inherits", "print_settings_id", "filament_settings_id", "printer_settings_id",
                                                    "compatible_printers", "compatible_prints", "setting_id", "name" };
    for (const auto& item : args["settings"].items()) {
        const std::string& key = item.key();
        if (is_secret_key(key) || identity.count(key))
            return ForcaAIResult::error("The AI may not set '" + key + "'.");
        if (!temp.config.has(key))
            return ForcaAIResult::error("'" + key + "' is not a " + type + " setting.");
        if (!set_setting(temp.config, key, json_to_setting(item.value()), err))
            return ForcaAIResult::error(err);
        applied[key] = temp.config.opt_serialize(key);
    }

    const std::string prev_selected = c->get_selected_preset_name();
    const std::string slot_old      = type == "filament" ? b->filament_presets[slot - 1] : std::string();
    c->save_current_preset(target, false, false, &temp); // saves to disk and selects it in the collection
    Preset* saved = c->find_preset(target, false, true);
    if (!saved)
        return ForcaAIResult::error("Forca could not save the preset.");
    saved->sync_info = update ? "update" : "create";
    if (!update && wxGetApp().is_user_login())
        saved->user_id = wxGetApp().getAgent()->get_user_id();
    saved->save_info();
    forca_ai_record_write(into_path(from_u8(saved->file)));
    b->update_compatible(PresetSelectCompatibleType::Never);

    // Put it in use.
    if (type == "filament") {
        if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT)) {
            if (prev_selected == slot_old || b->filament_presets.size() == 1) {
                tab->select_preset(target); // the Filament tab shows this slot: show the new preset (as the wizard does)
            } else {
                c->select_preset_by_name(prev_selected, true); // it shows another slot: keep showing that one
                tab->select_preset(prev_selected);
            }
        }
        if (!set_filament_slot(size_t(slot - 1), target, err))
            return ForcaAIResult::error("Saved '" + target + "' but could not put it in slot " + std::to_string(slot) + ": " + err);
    } else if (Tab* tab = wxGetApp().get_tab(preset_type_for(type))) {
        tab->select_preset(target);
    }

    json j = { { "preset", target }, { "type", type }, { "based_on", base_name }, { "updated", update }, { "changed", applied } };
    if (type == "filament")
        j["filaments"] = filament_slots();
    return ForcaAIResult::json(j);
}

// ---- per-object settings + modifiers ----------------------------------------------------------

// The keys an object (or a modifier volume) may override: print-object and print-region settings.
bool is_object_setting(const std::string& key)
{
    static const PrintObjectConfig object_defaults;
    static const PrintRegionConfig region_defaults;
    return object_defaults.has(key) || region_defaults.has(key) || key == "extruder";
}

bool apply_settings(ModelConfig& config, const json& settings, json& applied, std::string& err,
                    const std::function<bool(const std::string&)>& allowed)
{
    DynamicPrintConfig tmp = config.get();
    for (const auto& item : settings.items()) {
        const std::string& key = item.key();
        if (is_secret_key(key) || !allowed(key)) {
            err = "'" + key + "' cannot be set here.";
            return false;
        }
        if (!set_setting(tmp, key, json_to_setting(item.value()), err))
            return false;
        applied[key] = tmp.opt_serialize(key);
    }
    config.assign_config(std::move(tmp));
    return true;
}

ForcaAIResult tool_object_settings(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    int          obj_idx = -1;
    ModelObject* o       = object_arg(p, args, obj_idx, err);
    if (!o)
        return ForcaAIResult::error(err);
    const json settings = args.contains("settings") && args["settings"].is_object() ? args["settings"] : json::object();
    const json remove   = args.contains("remove") && args["remove"].is_array() ? args["remove"] : json::array();
    if (settings.empty() && remove.empty())
        return ForcaAIResult::error("Give 'settings' to override and/or 'remove' (keys to go back to the process preset).");

    json        applied = json::object(), removed = json::array();
    ModelConfig trial; // checked first, so a rejected call leaves no undo step
    trial.assign_config(o->config.get());
    if (!apply_settings(trial, settings, applied, err, is_object_setting))
        return ForcaAIResult::error(err);
    for (const auto& k : remove)
        if (k.is_string() && trial.erase(k.get<std::string>()))
            removed.push_back(k);
    {
        Plater::TakeSnapshot snapshot(p, "AI: settings of " + o->name);
        o->config.assign_config(trial.get());
        if (ObjectList* list = wxGetApp().obj_list()) {
            list->object_config_options_changed({ o, nullptr });
            list->changed_object(obj_idx);
        } else
            p->changed_object(obj_idx);
    }
    return ForcaAIResult::json({ { "object", obj_idx }, { "set", applied }, { "removed", removed } });
}

ForcaAIResult tool_add_modifier(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    int          obj_idx = -1;
    ModelObject* o       = object_arg(p, args, obj_idx, err);
    if (!o)
        return ForcaAIResult::error(err);
    if (o->instances.empty())
        return ForcaAIResult::error("The object has no instances.");

    static const std::map<std::string, ModelVolumeType> types = {
        { "modifier", ModelVolumeType::PARAMETER_MODIFIER },
        { "support_blocker", ModelVolumeType::SUPPORT_BLOCKER },
        { "support_enforcer", ModelVolumeType::SUPPORT_ENFORCER },
        { "support_interface_modifier", ModelVolumeType::SUPPORT_INTERFACE_MODIFIER },
        { "negative_volume", ModelVolumeType::NEGATIVE_VOLUME },
        { "part", ModelVolumeType::MODEL_PART },
    };
    const auto type_it = types.find(args.value("type", std::string("modifier")));
    if (type_it == types.end())
        return ForcaAIResult::error("type must be one of: modifier, support_blocker, support_enforcer, "
                                    "support_interface_modifier, negative_volume, part.");
    const ModelVolumeType vtype = type_it->second;

    // World-space box of the first instance: the default size/centre.
    const BoundingBoxf3 bb = o->instance_bounding_box(0);
    Vec3d size = bb.size(), center = bb.center();
    try {
        if (args.contains("size")) {
            const json& a = args["size"];
            if (a.is_number())
                size = Vec3d(a.get<double>(), a.get<double>(), a.get<double>());
            else if (a.is_array() && a.size() == 3)
                size = Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
            else
                return ForcaAIResult::error("size must be a number or [x, y, z] in mm.");
        }
        if (args.contains("center")) {
            const json& a = args["center"];
            if (!a.is_array() || a.size() != 3)
                return ForcaAIResult::error("center must be [x, y, z] in mm (plate coordinates, like forca_scene).");
            center = Vec3d(a[0].get<double>(), a[1].get<double>(), a[2].get<double>());
        }
    } catch (const std::exception&) {
        return ForcaAIResult::error("size and center must be numbers.");
    }
    if (size.minCoeff() < 0.1)
        return ForcaAIResult::error("Each size must be at least 0.1 mm.");

    const std::string shape = args.value("shape", std::string("box"));
    TriangleMesh      mesh;
    if (shape == "box")
        mesh = make_cube(size.x(), size.y(), size.z());
    else if (shape == "cylinder") // along Z; the diameter is the smaller of x / y
        mesh = make_cylinder(0.5 * std::min(size.x(), size.y()), size.z());
    else if (shape == "sphere")
        mesh = make_sphere(0.5 * size.minCoeff(), 2 * PI / 90);
    else
        return ForcaAIResult::error("shape must be box, cylinder or sphere.");
    mesh.translate(-mesh.bounding_box().center().cast<float>()); // centred on its origin

    // Check the settings first, so a rejected call leaves no undo step. Modifiers carry region settings; the regional
    // interface box also carries its two own keys.
    json        applied = json::object();
    ModelConfig checked;
    if (args.contains("settings") && args["settings"].is_object()) {
        const bool sim = vtype == ModelVolumeType::SUPPORT_INTERFACE_MODIFIER;
        static const PrintRegionConfig region_defaults;
        auto allowed = [sim](const std::string& k) {
            return region_defaults.has(k) || k == "extruder" ||
                   (sim && (k == "support_interface_filament" || k == "support_top_z_distance"));
        };
        if (!apply_settings(checked, args["settings"], applied, err, allowed))
            return ForcaAIResult::error(err);
    }
    {
        Plater::TakeSnapshot snapshot(p, "AI: add " + type_it->first + " to " + o->name);
        ModelVolume* v = o->add_volume(std::move(mesh), vtype);
        // Aligned with the plate like Forca's own "Add modifier": undo the instance rotation/scale, then place the
        // centre in plate coordinates.
        const Geometry::Transformation& it = o->instances[0]->get_transformation();
        const Transform3d inv = it.get_matrix_no_offset().inverse();
        v->set_transformation(Geometry::Transformation(inv));
        v->set_offset(inv * (center - it.get_offset()));
        v->name = args.value("name", std::string("AI ") + type_it->first);
        int extruder = 0;
        if (vtype == ModelVolumeType::MODEL_PART && o->config.has("extruder"))
            extruder = o->config.opt_int("extruder");
        v->config.set_key_value("extruder", new ConfigOptionInt(extruder));
        if (vtype == ModelVolumeType::SUPPORT_INTERFACE_MODIFIER) // Forca regional supports: 0 = object default
            v->config.set_key_value("support_interface_filament", new ConfigOptionInt(0));
        v->source.is_from_builtin_objects = true;

        v->config.apply(checked);
        Slic3r::save_object_mesh(*o);
        if (ObjectList* list = wxGetApp().obj_list()) {
            list->reorder_volumes_and_get_selection(obj_idx);
            list->object_config_options_changed({ o, v });
            list->notify_instance_updated(obj_idx);
        }
        if (vtype == ModelVolumeType::MODEL_PART)
            p->get_view3D_canvas3D()->update_instance_printable_state_for_object(size_t(obj_idx));
        p->changed_object(obj_idx);
    }
    return ForcaAIResult::json({ { "object", obj_idx }, { "added", type_it->first }, { "shape", shape },
                                 { "size_mm", round3(size) }, { "center_mm", round3(center) }, { "settings", applied } });
}

// ---- slicing ----------------------------------------------------------------------------------

struct SliceJob
{
    bool        active  = false;
    int         plate   = -1; // 0-based
    int         status  = -1; // -1 running, 0 finished, 1 cancelled, 2 error
    std::string message;
    std::chrono::steady_clock::time_point started;
    double      seconds = 0;
    int         retries = 0;  // automatic re-slices after Forca discarded a finished result
};
SliceJob s_slice;

ForcaAIResult tool_slice(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    PartPlateList& plates = p->get_partplate_list();
    const int      plate  = args.value("plate", plates.get_curr_plate_index() + 1) - 1;
    if (plate < 0 || plate >= plates.get_plate_count())
        return ForcaAIResult::error("plate must be between 1 and " + std::to_string(plates.get_plate_count()) + ".");
    if (!plates.get_plate(plate)->has_printable_instances())
        return ForcaAIResult::error("Plate " + std::to_string(plate + 1) + " has nothing printable on it.");

    static bool listening = false;
    if (!listening) {
        p->set_forca_ai_slice_listener([](int status, const std::string& message) {
            if (!s_slice.active || s_slice.status != -1)
                return;
            // A "finished" that left the plate unsliced is some other background run (e.g. the one an import
            // schedules) completing -- keep waiting for ours. forca_slice_result notices if nothing follows.
            PartPlateList& plates = wxGetApp().plater()->get_partplate_list();
            if (status == 0 && !plates.get_plate(s_slice.plate)->is_slice_result_valid())
                return;
            s_slice.status  = status;
            s_slice.message = message;
            s_slice.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_slice.started).count();
        });
        listening = true;
    }

    if (plate != plates.get_curr_plate_index())
        p->select_plate(plate);
    s_slice = SliceJob{ true, plate, -1, {}, std::chrono::steady_clock::now(), 0, 0 };
    // Exactly what the "Slice plate" button does (Plater::priv::on_action_slice_plate).
    {
        const DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        auto print_config = plates.get_current_fff_print().config();
        Model::setExtruderParams(config, int(wxGetApp().preset_bundle->filament_presets.size()));
        Model::setPrintSpeedTable(config, print_config);
    }
    p->reslice();
    // Show the Preview like the Slice button (MainFrame m_slice_btn): the Preview tab, or inside the wizard tab.
    if (MainFrame* mf = wxGetApp().mainframe)
        mf->select_tab(TAB_ID_PREVIEW);

    if (!p->is_background_process_slicing() && s_slice.status == -1) {
        if (plates.get_plate(plate)->is_slice_result_valid()) {
            s_slice.status = 0; // nothing changed since the last slice
            return ForcaAIResult::text("Plate " + std::to_string(plate + 1) + " was already sliced and is up to date. Call forca_slice_result.");
        }
        s_slice.active = false;
        return ForcaAIResult::error("Forca did not start slicing. It usually shows why (for example an object outside the "
                                    "plate, or a settings conflict): see forca_screenshot.");
    }
    return ForcaAIResult::text("Slicing plate " + std::to_string(plate + 1) + ". Call forca_slice_result to wait for the result.");
}

json plate_report(int plate_idx, std::string& err);

ForcaAIResult tool_slice_result(const json& args)
{
    Plater* p = wxGetApp().plater();
    if (!p)
        return ForcaAIResult::error("Forca is still starting up.");
    if (!s_slice.active)
        return ForcaAIResult::error("No slice was started by the AI. Call forca_slice first.");
    if (s_slice.status == -1) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_slice.started).count();
        if (!p->is_background_process_slicing() && t > 5) {
            PartPlate* plate = p->get_partplate_list().get_plate(s_slice.plate);
            if (plate && plate->is_slice_result_valid()) {
                s_slice.status  = 0; // finished without us hearing about it
                s_slice.seconds = t;
            } else {
                s_slice.active = false;
                return ForcaAIResult::error("Forca stopped without slicing plate " + std::to_string(s_slice.plate + 1) +
                                            " (it can happen right after an import, or Forca shows why: see "
                                            "forca_screenshot). Call forca_slice again.");
            }
        }
    }
    if (s_slice.status == -1) {
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_slice.started).count();
        return ForcaAIResult::json({ { "state", "slicing" }, { "plate", s_slice.plate + 1 }, { "elapsed_s", int(t) },
                                     { "hint", "Still slicing; call forca_slice_result again in a few seconds." } });
    }
    if (s_slice.status == 0 && !p->get_partplate_list().get_plate(s_slice.plate)->is_slice_result_valid() &&
        !p->is_background_process_slicing() && s_slice.retries < 1) {
        // Forca sometimes discards the first slice after an import (a follow-up config change, e.g. the prime
        // tower's, marks the print as changed). Slice once more, as the user would by pressing Slice again.
        ++s_slice.retries;
        s_slice.status  = -1;
        s_slice.started = std::chrono::steady_clock::now();
        p->reslice();
        return ForcaAIResult::json({ { "state", "slicing" }, { "plate", s_slice.plate + 1 },
                                     { "hint", "Forca discarded the first result; slicing again. Call forca_slice_result again shortly." } });
    }
    if (s_slice.status != 0)
        return ForcaAIResult::json({ { "state", s_slice.status == 1 ? "cancelled" : "error" }, { "plate", s_slice.plate + 1 },
                                     { "message", s_slice.message } });

    std::string err;
    json        j = plate_report(s_slice.plate, err);
    if (j.empty())
        return ForcaAIResult::error(err);
    j["state"]        = "done";
    j["slice_time_s"] = std::round(s_slice.seconds * 10) / 10;
    return ForcaAIResult::json(j);
}

// The slice report of a sliced plate (0-based); also shown on the print approval card (ForcaAIPrint.cpp).
json plate_report(int plate_idx, std::string& err)
{
    Plater*        p      = wxGetApp().plater();
    PartPlateList& plates = p->get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count()) {
        err = "No such plate.";
        return {};
    }
    PartPlate* plate = plates.get_plate(plate_idx);
    PrintBase* base  = nullptr;
    GCodeProcessorResult* result = nullptr;
    int        index = -1;
    plate->get_print(&base, &result, &index);
    const Print* print = dynamic_cast<const Print*>(base);
    if (!print || !result || !plate->is_slice_result_valid()) {
        err = "Plate " + std::to_string(plate_idx + 1) + " is not sliced, or changed since it was sliced. Slice again.";
        return {};
    }

    const PrintStatistics& ps = print->print_statistics();
    size_t layers = 0;
    for (const PrintObject* po : print->objects())
        layers = std::max(layers, po->layers().size());
    json per_filament = json::array();
    // Volumes (mm^3, incl. supports + prime tower) -> length and weight with each filament's diameter and density.
    const PrintConfig& pc = print->config();
    for (const auto& [ext, volume] : result->print_statistics.total_volumes_per_extruder) {
        const double d       = ext < pc.filament_diameter.size() ? pc.filament_diameter.get_at(ext) : 1.75;
        const double density = ext < pc.filament_density.size() ? pc.filament_density.get_at(ext) : 0.;
        const double length  = volume / (M_PI * 0.25 * d * d); // mm
        per_filament.push_back({ { "filament", int(ext) + 1 },
                                 { "length_m", std::round(length / 10.0) / 100.0 },
                                 { "weight_g", std::round(volume * density / 10.0) / 100.0 } });
    }
    json warnings = json::array();
    for (const auto& w : result->warnings)
        // Forca's levels: 0 tip, 1 and 3 warning (3 = e.g. bed too hot for the filament), 2 error.
        warnings.push_back({ { "level", w.level == 2 ? "error" : (w.level == 0 ? "tip" : "warning") },
                             { "message", w.msg }, { "text", into_u8(p->get_slice_warning_string(const_cast<GCodeProcessorResult::SliceWarning&>(w))) } });

    const float seconds = result->print_statistics.modes[0].time;
    char        hms[32];
    std::snprintf(hms, sizeof(hms), "%dh %02dm %02ds", int(seconds) / 3600, (int(seconds) % 3600) / 60, int(seconds) % 60);
    json j = { { "plate", plate_idx + 1 },
               { "print_time", hms },
               { "print_time_s", int(seconds) },
               { "filament_total_m", std::round(ps.total_used_filament / 10.0) / 100.0 },
               { "filament_total_g", std::round(ps.total_weight * 100) / 100 },
               { "cost", std::round(ps.total_cost * 100) / 100 },
               { "filament_changes", ps.total_toolchanges },
               { "per_filament", per_filament },
               { "layers", layers },
               { "toolpath_outside_plate", result->toolpath_outside },
               { "warnings", warnings } };
    return j;
}

// ---- view -------------------------------------------------------------------------------------

ForcaAIResult tool_view(const json& args)
{
    Plater* p = wxGetApp().plater();
    if (!p)
        return ForcaAIResult::error("Forca is still starting up.");
    const std::string mode = args.value("mode", std::string());
    if (!mode.empty()) {
        if (mode != "3d" && mode != "preview")
            return ForcaAIResult::error("mode must be 3d or preview.");
        // Like clicking the Prepare / Preview tab (opening the Preview slices the plate if it is not sliced yet).
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->select_tab(mode == "3d" ? TAB_ID_PREPARE : TAB_ID_PREVIEW);
    }
    const std::string camera = args.value("camera", std::string());
    if (!camera.empty()) {
        static const std::set<std::string> cams = { "iso", "top", "bottom", "front", "rear", "left", "right" };
        if (!cams.count(camera))
            return ForcaAIResult::error("camera must be one of: iso, top, bottom, front, rear, left, right.");
        p->select_view(camera);
    }
    if (mode.empty() && camera.empty())
        return ForcaAIResult::error("Give mode (3d / preview) and/or camera.");
    return ForcaAIResult::text("View changed. Use forca_screenshot to see it.");
}

// ---- saving (R1) ------------------------------------------------------------------------------

ForcaAIResult tool_save_project(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    const fs::path path = output_path(args, project_stem(p), ".3mf", err);
    if (path.empty())
        return ForcaAIResult::error(err);
    // Silence: saves a copy -- the user's open project keeps its own file name, so their next Ctrl+S is unaffected.
    if (p->export_3mf(path, SaveStrategy::Silence) < 0)
        return ForcaAIResult::error("Forca could not save the project copy.");
    forca_ai_record_write(path);
    return ForcaAIResult::json({ { "saved", into_u8(from_path(path)) },
                                 { "note", "A copy of the current project; the project open in Forca keeps its own file." } });
}

ForcaAIResult tool_export_gcode(const json& args)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    PartPlateList& plates = p->get_partplate_list();
    const int      idx    = args.value("plate", plates.get_curr_plate_index() + 1) - 1;
    if (idx < 0 || idx >= plates.get_plate_count())
        return ForcaAIResult::error("plate must be between 1 and " + std::to_string(plates.get_plate_count()) + ".");
    PartPlate* plate = plates.get_plate(idx);
    if (!plate->is_slice_result_valid())
        return ForcaAIResult::error("Plate " + std::to_string(idx + 1) + " is not sliced (or changed since). Call forca_slice first.");
    const fs::path src = into_path(from_u8(plate->get_tmp_gcode_path()));
    boost::system::error_code ec;
    if (!fs::is_regular_file(src, ec))
        return ForcaAIResult::error("Forca's sliced G-code for that plate is missing. Slice again.");

    const std::string stem = project_stem(p) + (plates.get_plate_count() > 1 ? " plate " + std::to_string(idx + 1) : std::string());
    const fs::path    dst  = output_path(args, stem, ".gcode", err);
    if (dst.empty())
        return ForcaAIResult::error(err);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec); // dst is new, or the AI's own unchanged file
    if (ec)
        return ForcaAIResult::error("Could not write the G-code: " + ec.message());
    forca_ai_record_write(dst);
    return ForcaAIResult::json({ { "saved", into_u8(from_path(dst)) }, { "plate", idx + 1 },
                                 { "note", "Saved to disk only; nothing was sent to a printer." } });
}

// ---- undo -------------------------------------------------------------------------------------

ForcaAIResult tool_undo(const json&)
{
    std::string err;
    Plater*     p = plater_ready(err);
    if (!p)
        return ForcaAIResult::error(err);
    std::string top;
    p->undo_redo_topmost_string_getter(true, top);
    if (!boost::starts_with(top, "AI: "))
        return ForcaAIResult::error(top.empty() ? "There is nothing to undo."
                                                : "The last step ('" + top + "') was not made by the AI, so the AI does not undo it.");
    p->undo();
    return ForcaAIResult::text("Undid '" + top + "'.");
}

} // namespace

json forca_ai_plate_report(int plate, std::string& err) { return plate_report(plate, err); }

void register_forca_ai_hand_tools(ForcaAI& ai)
{
    const json preset_type = { { "type", "string" }, { "enum", { "printer", "process", "filament" } } };
    const json object_idx  = { { "type", "integer" }, { "minimum", 0 }, { "description", "Object index from forca_scene." } };
    const json settings    = { { "type", "object" }, { "description", "Setting key -> value, e.g. {\"enable_support\": true, \"support_type\": \"tree(auto)\"}." } };
    const json save_args   = {
        { "name", prop("string", "File name without extension (default: the project name). Forca adds ' (Claude <date>)'.") },
        { "folder", prop("string", "An existing folder (default: Documents\\Forca AI).") },
        { "path", prop("string", "Overwrite this exact file -- only allowed for a file the AI saved earlier that the user has not changed.") } };

    ai.register_tool({ "forca_import_models", "Import models",
        "Load model files (STL, 3MF, OBJ, STEP, AMF, DRC, SVG) onto a plate. Geometry only: a 3MF's settings are not "
        "applied, so the user's presets stay as they are.",
        object_schema({ { "paths", { { "type", "array" }, { "items", { { "type", "string" } } }, { "description", "Full paths of the files." } } },
                        { "plate", { { "type", "integer" }, { "minimum", 1 }, { "description", "Plate to load onto (default: current)." } } } },
                      { "paths" }),
        tool_import });

    ai.register_tool({ "forca_transform_object", "Move / rotate / scale",
        "Move, rotate or scale one instance of an object. position is [x, y] (or [x, y, z]) in plate mm, like "
        "forca_scene's position_mm; move_by is relative; rotation_deg is absolute [x, y, z]; scale is a factor (1 = "
        "100%) or [x, y, z]. The object is dropped onto the bed afterwards unless drop_to_bed is false. One undo step.",
        object_schema({ { "object", object_idx },
                        { "instance", { { "type", "integer" }, { "minimum", 0 }, { "description", "Default 0." } } },
                        { "position", vec_prop(2, 3, "Absolute [x, y] or [x, y, z] in mm.") },
                        { "move_by", vec_prop(2, 3, "Relative [dx, dy] or [dx, dy, dz] in mm.") },
                        { "rotation_deg", vec_prop(3, 3, "Absolute rotation [x, y, z] in degrees.") },
                        { "scale", { { "description", "Uniform factor, or [x, y, z] factors." } } },
                        { "drop_to_bed", prop("boolean", "Default true.") } },
                      { "object" }),
        tool_transform });

    ai.register_tool({ "forca_add_copies", "Add copies",
        "Add copies (instances) of an object; Forca places them on the plate. One undo step.",
        object_schema({ { "object", object_idx }, { "copies", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 50 } } } },
                      { "object" }),
        tool_add_copies });

    ai.register_tool({ "forca_delete_object", "Remove an object",
        "Remove an object (all its instances) from the plates. Never touches files; Ctrl+Z in Forca brings it back.",
        object_schema({ { "object", object_idx } }, { "object" }), tool_delete_object });

    ai.register_tool({ "forca_arrange", "Arrange plates",
        "Auto-arrange all objects on the plates (Forca's Arrange). Runs as a background job; check forca_scene after.",
        object_schema(), tool_arrange });

    ai.register_tool({ "forca_auto_orient", "Auto-orient",
        "Auto-orient one object (or all objects) for printing (Forca's Orient). Runs as a background job.",
        object_schema({ { "object", object_idx } }), tool_orient });

    ai.register_tool({ "forca_select_preset", "Select a preset",
        "Switch the printer or process preset, or the filament preset in one project slot. Refused if the user has "
        "unsaved edits in that preset (the AI never discards them).",
        object_schema({ { "type", preset_type },
                        { "name", prop("string", "Exact preset name (see forca_list_presets).") },
                        { "filament_slot", { { "type", "integer" }, { "minimum", 1 }, { "description", "Filament only: project slot (default 1)." } } } },
                      { "type", "name" }),
        tool_select_preset });

    ai.register_tool({ "forca_create_preset", "Create a preset",
        "Create a new user preset named '<name> (Claude <date>)' from a base preset plus changed settings, save it and "
        "put it in use (for filaments: in filament_slot). The AI never edits the user's presets; 'update' changes a "
        "preset the AI created earlier, if the user has not edited it since. Values use Forca's text format "
        "(see forca_get_settings); one value for a per-extruder setting is applied to every extruder variant.",
        object_schema({ { "type", preset_type },
                        { "settings", settings },
                        { "base", prop("string", "Preset to start from (default: the one in use / in the slot).") },
                        { "name", prop("string", "Name for the new preset (default: the base's name).") },
                        { "update", prop("string", "Name of an AI-created preset to change instead of creating a new one.") },
                        { "filament_slot", { { "type", "integer" }, { "minimum", 1 }, { "description", "Filament only: project slot (default 1)." } } } },
                      { "type", "settings" }),
        tool_create_preset });

    ai.register_tool({ "forca_set_object_settings", "Per-object settings",
        "Override process settings for one object (e.g. supports, infill, walls), or remove overrides so the object "
        "follows the process preset again. Does not change any preset. One undo step.",
        object_schema({ { "object", object_idx }, { "settings", settings },
                        { "remove", { { "type", "array" }, { "items", { { "type", "string" } } } } } },
                      { "object" }),
        tool_object_settings });

    ai.register_tool({ "forca_add_modifier", "Add a modifier / support region",
        "Add a box, cylinder or sphere to an object as a modifier (own settings for the region inside), support "
        "blocker or enforcer, negative volume, extra part, or a Forca support-interface modifier (regional interface "
        "material: settings {\"support_interface_filament\": <slot>, \"support_top_z_distance\": 0}). size and "
        "center are in plate mm (default: the object's bounding box); the shape is aligned with the plate. One undo step.",
        object_schema({ { "object", object_idx },
                        { "type", { { "type", "string" }, { "enum", { "modifier", "support_blocker", "support_enforcer",
                                                                     "support_interface_modifier", "negative_volume", "part" } } } },
                        { "shape", { { "type", "string" }, { "enum", { "box", "cylinder", "sphere" } } } },
                        { "size", { { "description", "[x, y, z] mm, or one number for all three." } } },
                        { "center", vec_prop(3, 3, "Centre [x, y, z] in plate mm.") },
                        { "settings", settings },
                        { "name", prop("string", "Name shown in the object list.") } },
                      { "object", "type" }),
        tool_add_modifier });

    ai.register_tool({ "forca_slice", "Slice a plate",
        "Slice a plate (default: current) and show the Preview. Returns at once; call forca_slice_result for the result.",
        object_schema({ { "plate", { { "type", "integer" }, { "minimum", 1 } } } }), tool_slice });

    ai.register_tool({ "forca_slice_result", "Slice result",
        "The state of the slice started by forca_slice. When done: print time, filament length/weight/cost, filament "
        "changes, layers, and Forca's slicing warnings.",
        object_schema(), tool_slice_result });

    ai.register_tool({ "forca_view", "Change the view",
        "Show the 3D editor or the sliced Preview, and/or point the camera (iso, top, bottom, front, rear, left, "
        "right). Use forca_screenshot afterwards to see it. Like the Preview tab, opening the Preview slices the plate "
        "if it is not sliced yet.",
        object_schema({ { "mode", { { "type", "string" }, { "enum", { "3d", "preview" } } } },
                        { "camera", { { "type", "string" }, { "enum", { "iso", "top", "bottom", "front", "rear", "left", "right" } } } } }),
        tool_view });

    ai.register_tool({ "forca_save_project", "Save a project copy",
        "Save the current project (plates, objects, settings) as a new .3mf file named '<name> (Claude <date>).3mf'. "
        "Never overwrites the user's files; the project open in Forca keeps its own file name.",
        object_schema(save_args), tool_save_project });

    ai.register_tool({ "forca_export_gcode", "Export G-code",
        "Save a sliced plate's G-code as a new file named '<name> (Claude <date>).gcode'. Disk only -- this never sends "
        "anything to a printer.",
        [&] { json a = save_args; a["plate"] = { { "type", "integer" }, { "minimum", 1 } }; return object_schema(a); }(),
        tool_export_gcode });

    ai.register_tool({ "forca_undo", "Undo the AI's last step",
        "Undo the most recent change if the AI made it (its undo steps are named 'AI: ...'). Never undoes the user's work.",
        object_schema(), tool_undo });
}

}} // namespace Slic3r::GUI
