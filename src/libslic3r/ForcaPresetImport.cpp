#include "ForcaPresetImport.hpp"

#include "Config.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "Semver.hpp"
#include "format.hpp"
#include "miniz_extension.hpp"
#include "Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <miniz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

using json = nlohmann::json;

// Keys of a preset file that describe the preset rather than set anything.
bool is_meta_key(const std::string &key)
{
    static const std::set<std::string> keys = {
        BBL_JSON_KEY_NAME, BBL_JSON_KEY_INHERITS, BBL_JSON_KEY_VERSION, BBL_JSON_KEY_FROM, BBL_JSON_KEY_TYPE,
        BBL_JSON_KEY_SETTING_ID, BBL_JSON_KEY_FILAMENT_ID, BBL_JSON_KEY_INSTANTIATION, BBL_JSON_KEY_DESCRIPTION,
        BBL_JSON_KEY_BASE_ID, BBL_JSON_KEY_IS_CUSTOM, BBL_JSON_KEY_INCLUDES, BBL_JSON_KEY_URL, BBL_JSON_KEY_USER_ID,
        "updated_time", "renamed_from"};
    return keys.count(key) != 0;
}

// Settings the review shows elsewhere (the preset's name and printers), or that only say how the others are laid out
// (the nozzle variants a per-variant value is for), not as overrides.
bool is_identity_key(const std::string &key)
{
    return key == "compatible_printers" || key == "compatible_prints" || boost::ends_with(key, "_settings_id") ||
           boost::ends_with(key, "_extruder_variant") || boost::ends_with(key, "_extruder_id");
}

// Settings only Bambu Studio writes: their presence tells a Bambu Studio file from an OrcaSlicer or Forca one.
bool is_bambu_studio_key(const std::string &key)
{
    static const std::set<std::string> keys = {
        "hole_coef_1", "hole_coef_2", "hole_coef_3", "hole_limit_min", "hole_limit_max", "counter_coef_1",
        "counter_coef_2", "counter_coef_3", "counter_limit_min", "counter_limit_max", "diameter_limit",
        "circle_compensation_speed", "filament_scarf_seam_type", "filament_scarf_gap", "filament_scarf_height",
        "filament_scarf_length", "filament_velocity_adaptation_factor", "impact_strength_z",
        "overhang_threshold_participating_cooling", "first_x_layer_part_fan_speed"};
    return keys.count(key) != 0;
}

// A value as the file writes it: an array's items joined by ",".
std::string file_value(const json &value)
{
    if (value.is_string())
        return value.get<std::string>();
    if (!value.is_array())
        return value.dump();
    std::string out;
    for (const json &item : value) {
        if (!out.empty())
            out += ',';
        out += file_value(item);
    }
    return out;
}

Preset::Type preset_type(const std::string &type, const DynamicPrintConfig &config)
{
    if (type == "filament" || config.has("filament_settings_id"))
        return Preset::TYPE_FILAMENT;
    if (type == "process" || config.has("print_settings_id"))
        return Preset::TYPE_PRINT;
    if (type == "machine" || config.has("printer_settings_id"))
        return Preset::TYPE_PRINTER;
    return Preset::TYPE_INVALID;
}

PresetCollection *collection_of(PresetBundle &bundle, Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_FILAMENT: return &bundle.filaments;
    case Preset::TYPE_PRINT: return &bundle.prints;
    case Preset::TYPE_PRINTER: return &bundle.printers;
    default: return nullptr;
    }
}

std::string first_string(const DynamicPrintConfig &config, const std::string &key)
{
    const auto *opt = config.option<ConfigOptionStrings>(key);
    return opt != nullptr && !opt->values.empty() ? opt->values.front() : std::string();
}

// The part of a preset name before its "@printer" suffix.
std::string name_stem(const std::string &name)
{
    const size_t at = name.find(" @");
    std::string  stem = at == std::string::npos ? name : name.substr(0, at);
    while (!stem.empty() && stem.back() == ' ')
        stem.pop_back();
    return stem;
}

bool fits_printers(const Preset &preset, const std::vector<std::string> &printers)
{
    const auto *compatible = preset.config.option<ConfigOptionStrings>("compatible_printers");
    if (printers.empty() || compatible == nullptr || compatible->values.empty())
        return true;
    return std::any_of(printers.begin(), printers.end(), [compatible](const std::string &printer) {
        return std::find(compatible->values.begin(), compatible->values.end(), printer) != compatible->values.end();
    });
}

// A file that relies on its missing parent for its filament type often still names it ("OVERTURE High Speed TPU"):
// the longest filament type of Forca's system filaments that the name has as a whole word.
std::string filament_type_in_name(const PresetBundle &bundle, const std::string &name)
{
    std::set<std::string> types;
    for (const Preset &preset : bundle.filaments)
        if (preset.is_system)
            if (std::string type = first_string(preset.config, "filament_type"); !type.empty())
                types.insert(type);
    auto is_word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    std::string best;
    for (const std::string &type : types)
        for (size_t at = name.find(type); at != std::string::npos; at = name.find(type, at + 1))
            if ((at == 0 || !is_word_char(name[at - 1])) && (at + type.size() == name.size() || !is_word_char(name[at + type.size()])) &&
                type.size() > best.size())
                best = type;
    return best;
}

} // namespace

std::string forca_best_filament_parent(PresetBundle &bundle, const std::string &missing_parent, const std::string &vendor,
                                       const std::string &type, const std::vector<std::string> &printers)
{
    if (type.empty())
        return {};
    const std::string missing_stem = name_stem(missing_parent);
    const Preset     *best         = nullptr;
    int               best_score   = 0;
    for (const Preset &preset : bundle.filaments) {
        if (!preset.is_system || !preset.is_visible || first_string(preset.config, "filament_type") != type ||
            !fits_printers(preset, printers))
            continue;
        const std::string stem  = name_stem(preset.name);
        int               score = 1;
        if (!missing_stem.empty() && stem == missing_stem)
            score = 4;
        else if (!vendor.empty() && boost::iequals(first_string(preset.config, "filament_vendor"), vendor))
            score = 3;
        else if (stem == "Generic " + type)
            score = 2;
        if (score > best_score) {
            best       = &preset;
            best_score = score;
        }
    }
    return best != nullptr ? best->name : std::string();
}

std::string forca_free_preset_name(const PresetCollection &collection, const std::string &name, const std::string &pattern)
{
    if (collection.find_preset(name, false) == nullptr)
        return name;
    const std::string renamed = format(pattern, name);
    for (int i = 1;; ++i) {
        std::string candidate = renamed + (i == 1 ? std::string() : " " + std::to_string(i));
        if (collection.find_preset(candidate, false) == nullptr)
            return candidate;
    }
}

bool forca_import_preset(PresetBundle &bundle, const PresetImportItem &item, const PresetImportChoice &choice)
{
    if (!item.readable)
        return false;
    json j;
    try {
        boost::nowide::ifstream in(item.file);
        in >> j;
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(warning) << "Forca preset import: can't read " << item.file << ": " << err.what();
        return false;
    }

    // The copy with the user's choices. A *_settings_id names the preset itself: an array in a filament file.
    const std::string name = choice.name.empty() ? item.name : choice.name;
    j[BBL_JSON_KEY_NAME]   = name;
    for (const char *id_key : {"filament_settings_id", "print_settings_id", "printer_settings_id"})
        if (j.contains(id_key))
            j[id_key] = j[id_key].is_array() ? json::array({name}) : json(name);
    if (!choice.parent.empty())
        j[BBL_JSON_KEY_INHERITS] = choice.parent;
    if (!choice.printers.empty())
        j["compatible_printers"] = choice.printers;
    if (item.type == Preset::TYPE_FILAMENT && !item.unsupported.empty()) {
        std::string notes = j.contains("filament_notes") ? file_value(j["filament_notes"]) : std::string();
        notes += (notes.empty() ? "" : "\n\n") + choice.notes_heading;
        for (const auto &[key, value] : item.unsupported)
            notes += "\n" + key + " = " + value;
        j["filament_notes"] = json::array({notes});
    }

    // In a folder of its own, under the file's own name (what import_failures() reports).
    boost::system::error_code ec;
    const boost::filesystem::path dir = boost::filesystem::temp_directory_path(ec) /
                                        boost::filesystem::unique_path("forca-import-%%%%-%%%%-%%%%", ec);
    boost::filesystem::create_directories(dir, ec);
    std::string copy = (dir / boost::filesystem::path(item.file).filename()).string();
    {
        boost::nowide::ofstream out(copy);
        out << j.dump(4);
    }

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    const bool replace = choice.replace_existing;
    // The confirmation's answer: 1 = replace this one, 0 = keep it (see MainFrame::load_config_file).
    const bool imported = bundle.import_json_presets(substitutions, copy, [replace](const std::string &) { return replace ? 1 : 0; },
                                                     ForwardCompatibilitySubstitutionRule::Enable, overwrite, result);
    boost::filesystem::remove_all(dir, ec);
    return imported && !result.empty();
}

std::vector<std::string> forca_filament_parent_candidates(PresetBundle &bundle, const std::string &type,
                                                          const std::vector<std::string> &printers)
{
    std::vector<std::string> names;
    for (const Preset &preset : bundle.filaments)
        if (preset.is_system && preset.is_visible && (type.empty() || first_string(preset.config, "filament_type") == type) &&
            fits_printers(preset, printers))
            names.push_back(preset.name);
    std::sort(names.begin(), names.end());
    return names;
}

bool forca_is_bambu_bundle(const std::string &file)
{
    return boost::iends_with(file, ".bbsflmt") || boost::iends_with(file, ".bbscfg");
}

std::vector<std::string> forca_extract_bambu_bundle(const std::string &bundle, const std::string &dir)
{
    std::vector<std::string> files;
    mz_zip_archive           zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, bundle)) {
        BOOST_LOG_TRIVIAL(warning) << "Forca preset import: can't open the bundle " << bundle;
        return files;
    }
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        // "filament/Brand PLA.json": the folder says what the preset is; only the file name is kept, so nothing is
        // written outside dir.
        const boost::filesystem::path entry(std::string(stat.m_filename));
        const std::string             kind = entry.has_parent_path() ? entry.begin()->string() : std::string();
        const boost::filesystem::path leaf = entry.filename();
        if ((kind != "printer" && kind != "process" && kind != "filament") || !boost::iequals(leaf.extension().string(), ".json") ||
            leaf.string() == ".." || leaf.string() == ".")
            continue;
        boost::system::error_code     ec;
        const boost::filesystem::path target = boost::filesystem::path(dir) / kind / leaf;
        boost::filesystem::create_directories(target.parent_path(), ec);
        if (mz_zip_reader_extract_to_file(&zip, i, encode_path(target.string().c_str()).c_str(), 0))
            files.push_back(target.string());
    }
    close_zip_reader(&zip);
    return files;
}

PresetImportItem forca_analyse_preset_file(PresetBundle &bundle, const std::string &file)
{
    PresetImportItem item;
    item.file = file;

    json j;
    try {
        boost::nowide::ifstream in(file);
        in >> j;
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(warning) << "Forca preset import: can't read " << file << ": " << err.what();
        return item;
    }
    if (!j.is_object())
        return item;

    // The settings as an import loads them (a Bambu Studio "nil" becomes a value, see ConfigSubstitutionContext).
    DynamicPrintConfig                                 config;
    std::map<std::string, std::string>                 key_values;
    std::string                                        reason;
    ConfigSubstitutionContext                          context(ForwardCompatibilitySubstitutionRule::EnableSilent);
    std::map<t_config_option_key, std::vector<size_t>> nil_items;
    context.nil_items = &nil_items;
    config.load_from_json(file, context, true, key_values, reason);

    item.name     = key_values[BBL_JSON_KEY_NAME];
    item.type     = preset_type(key_values[BBL_JSON_KEY_TYPE], config);
    item.readable = reason.empty() && !item.name.empty() && Semver::parse(key_values[BBL_JSON_KEY_VERSION]) &&
                    item.type != Preset::TYPE_INVALID;
    if (!item.readable)
        return item;
    PresetCollection &collection = *collection_of(bundle, item.type);

    if (const Preset *existing = collection.find_preset(item.name, false)) {
        item.clashes_with_system_preset = existing->is_system || existing->is_default;
        item.clashes_with_user_preset   = !item.clashes_with_system_preset;
    }

    // Parent.
    const Preset *parent = nullptr;
    if (j.contains(BBL_JSON_KEY_INHERITS) && j[BBL_JSON_KEY_INHERITS].is_string())
        item.inherits = j[BBL_JSON_KEY_INHERITS].get<std::string>();
    if (!item.inherits.empty()) {
        parent = collection.find_preset2(item.inherits, true);
        if (parent != nullptr) {
            item.parent      = parent->name == item.inherits ? PresetImportItem::Parent::Found : PresetImportItem::Parent::Matched;
            item.best_parent = parent->name;
        } else
            item.parent = PresetImportItem::Parent::Missing;
    }

    // Printers.
    if (const auto *printers = config.option<ConfigOptionStrings>("compatible_printers"); printers != nullptr && j.contains("compatible_printers"))
        item.printers = printers->values;
    else if (parent != nullptr)
        if (const auto *parent_printers = parent->config.option<ConfigOptionStrings>("compatible_printers"))
            item.printers = parent_printers->values;
    for (const std::string &printer : item.printers) {
        const Preset *installed = bundle.printers.find_preset(printer, false);
        if (installed == nullptr || !installed->is_visible)
            item.missing_printers.push_back(printer);
    }

    if (item.type == Preset::TYPE_FILAMENT) {
        item.filament_type = first_string(config, "filament_type");
        if (item.filament_type.empty() && parent != nullptr)
            item.filament_type = first_string(parent->config, "filament_type");
        if (item.filament_type.empty())
            item.filament_type = filament_type_in_name(bundle, item.name + " " + item.inherits);
    }
    if (item.parent == PresetImportItem::Parent::Missing && item.type == Preset::TYPE_FILAMENT) {
        item.best_parent = forca_best_filament_parent(bundle, item.inherits, first_string(config, "filament_vendor"),
                                                      item.filament_type, item.printers);
        parent           = item.best_parent.empty() ? nullptr : collection.find_preset(item.best_parent, false);
    }

    // Settings.
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (is_meta_key(it.key()))
            continue;
        const std::string value = file_value(it.value());
        // Bambu Studio's own settings, and its "nil" for a nozzle variant that keeps the parent's value.
        if (is_bambu_studio_key(it.key()) ||
            (it.value().is_array() && std::find(it.value().begin(), it.value().end(), json("nil")) != it.value().end()))
            item.source = PresetImportItem::Source::BambuStudio;
        t_config_option_key key        = it.key();
        std::string         legacy_val = value;
        PrintConfigDef::handle_legacy(key, legacy_val);
        if (key.empty()) {
            item.unsupported.emplace_back(it.key(), value);
            continue;
        }
        if (key != it.key())
            item.renamed.emplace_back(it.key(), key);
        if (is_identity_key(key))
            continue;
        const ConfigOption *own        = config.option(key);
        const ConfigOption *from_parent = parent != nullptr ? parent->config.option(key) : nullptr;
        if (own == nullptr || (from_parent != nullptr && *own == *from_parent))
            continue;
        item.overrides.push_back({key, value, from_parent != nullptr ? from_parent->serialize() : std::string()});
    }
    return item;
}

} // namespace Slic3r
