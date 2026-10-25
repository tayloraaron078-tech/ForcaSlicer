#include "ForcaExperiment.hpp"

#include "ForcaAcademy.hpp" // forca_academy_slug / forca_academy_ensure_skeleton / FORCA_EXPERIMENT_RESULT_UNWRITTEN
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

// Forca-owned blocks of experiment.md; the user's text outside them is never touched.
const char* const PRINTS_OPEN   = "<!-- forca:prints -->";
const char* const PRINTS_CLOSE  = "<!-- /forca:prints -->";
const char* const RESULTS_OPEN  = "<!-- forca:results -->";
const char* const RESULTS_CLOSE = "<!-- /forca:results -->";

// The settings worth trying per problem tag (HQ PLAN_experiment_generator.md section 3), most likely first. Only keys
// that can differ per copy on one plate; plate-wide ones (bed temperature, first layer speed, travel speed) can't.
const std::map<std::string, std::vector<std::string>>& candidate_table()
{
    static const std::map<std::string, std::vector<std::string>> table = {
        { "stringing", { "object_retraction_length", "object_nozzle_temperature", "object_retraction_speed", "object_z_hop" } },
        { "bridging", { "thick_bridges", "bridge_speed", "bridge_flow", "object_overhang_fan_speed" } },
        { "warping", { "brim_width", "brim_type" } },
        { "ringing", { "outer_wall_speed", "outer_wall_acceleration" } },
        { "under_extrusion", { "print_flow_ratio", "object_max_volumetric_speed", "object_nozzle_temperature" } },
        { "over_extrusion", { "print_flow_ratio", "object_pressure_advance" } },
        { "layer_separation", { "object_nozzle_temperature", "object_fan_min_speed", "outer_wall_speed" } },
        { "supports", { "support_top_z_distance", "support_interface_top_layers", "support_interface_spacing", "support_type" } },
        { "bed_adhesion", { "brim_width", "brim_type" } },
        { "layer_shift", { "outer_wall_acceleration", "travel_acceleration" } },
        { "spaghetti", { "outer_wall_acceleration", "travel_acceleration" } },
    };
    return table;
}

// Forca's per-object filament overrides (docs/HLSD/per-object-filament-overrides.md) and the setting each replaces.
const std::map<std::string, std::string>& override_sources()
{
    static const std::map<std::string, std::string> sources = {
        { "object_retraction_length", "retraction_length" },
        { "object_retraction_speed", "retraction_speed" },
        { "object_deretraction_speed", "deretraction_speed" },
        { "object_z_hop", "z_hop" },
        { "object_pressure_advance", "pressure_advance" },
        { "object_max_volumetric_speed", "filament_max_volumetric_speed" },
        { "object_nozzle_temperature", "nozzle_temperature" },
        { "object_nozzle_temperature_initial_layer", "nozzle_temperature_initial_layer" },
        { "object_fan_min_speed", "fan_min_speed" },
        { "object_fan_max_speed", "fan_max_speed" },
        { "object_overhang_fan_speed", "overhang_fan_speed" },
    };
    return sources;
}

// The first value of a vector option, or the value of a scalar one, as text.
std::string option_text(const ConfigOption* opt)
{
    if (opt == nullptr)
        return {};
    if (opt->is_vector()) {
        const std::vector<std::string> values = static_cast<const ConfigOptionVectorBase*>(opt)->vserialize();
        return values.empty() ? std::string() : values.front();
    }
    return opt->serialize();
}

// A round step near `x`: 1, 2 or 5 times a power of ten.
double nice_step(double x)
{
    if (x <= 0.)
        return 0.;
    const double p = std::pow(10., std::floor(std::log10(x)));
    const double m = x / p;
    return (m < 1.5 ? 1. : m < 3.5 ? 2. : m < 7.5 ? 5. : 10.) * p;
}

std::string number_text(double v, bool integer)
{
    if (integer)
        return std::to_string(static_cast<long long>(std::lround(v)));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    std::string s = buf;
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.')
        s.pop_back();
    return s == "-0" ? std::string("0") : s;
}

std::string value_shown(const std::string& key, const std::string& value)
{
    const ConfigOptionDef* def = print_config_def.get(forca_experiment_source_key(key));
    if (def != nullptr && (def->type == coBool || def->type == coBools))
        return value == "1" ? "on" : "off";
    return value;
}

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool write_file(const fs::path& p, const std::string& text, std::string& err)
{
    boost::nowide::ofstream f(p.string(), std::ios::binary | std::ios::trunc);
    if (f)
        f << text;
    if (!f) {
        err = "Could not write " + p.string();
        return false;
    }
    return true;
}

} // namespace

std::vector<std::string> forca_experiment_candidates(const std::string& tag)
{
    const auto it = candidate_table().find(tag);
    return it == candidate_table().end() ? std::vector<std::string>() : it->second;
}

bool forca_experiment_varies_on_one_plate(const std::string& key)
{
    static const PrintObjectConfig object_defaults;
    static const PrintRegionConfig region_defaults;
    return object_defaults.has(key) || region_defaults.has(key);
}

std::string forca_experiment_source_key(const std::string& key)
{
    const auto it = override_sources().find(key);
    return it == override_sources().end() ? key : it->second;
}

std::string forca_experiment_current_value(const DynamicPrintConfig& config, const std::string& key)
{
    const std::string source = forca_experiment_source_key(key);
    // Retraction and z-hop are printer settings that a filament may override ("filament_retraction_length", nil = not).
    if (source != key && source.rfind("filament_", 0) != 0)
        if (const ConfigOption* f = config.option("filament_" + source); f != nullptr)
            if (std::string v = option_text(f); !v.empty() && v != "nil")
                return v;
    return option_text(config.option(source));
}

std::vector<std::string> forca_experiment_suggest_values(const std::string& key, const std::string& current, size_t count)
{
    std::vector<std::string> out;
    const ConfigOptionDef*   def = print_config_def.get(forca_experiment_source_key(key));
    if (def == nullptr || count == 0)
        return out;
    switch (def->type) {
    case coBool:
    case coBools: out.push_back(current == "1" ? "0" : "1"); break;
    case coEnum:
    case coEnums:
        for (const std::string& v : def->enum_values)
            if (v != current && out.size() < count)
                out.push_back(v);
        break;
    case coInt:
    case coInts:
    case coFloat:
    case coFloats:
    case coFloatOrPercent:
    case coFloatsOrPercents:
    case coPercent:
    case coPercents: {
        const bool   percent = !current.empty() && current.back() == '%';
        const bool   integer = def->type == coInt || def->type == coInts;
        double       value   = 0.;
        try {
            value = std::stod(percent ? current.substr(0, current.size() - 1) : current);
        } catch (...) {
            return out;
        }
        double step;
        if (key.find("temperature") != std::string::npos)
            step = 5.;
        else if (key.find("fan") != std::string::npos)
            step = 10.;
        else if (key.find("flow") != std::string::npos)
            step = nice_step(std::abs(value) * 0.05);
        else
            step = nice_step(std::abs(value) * 0.1);
        if (step == 0.)
            step = integer ? 1. : 0.1;
        if (integer)
            step = std::max(1., std::round(step));
        // Overrides allow -1 (= not set); the values to try are real ones, never below zero.
        const double lo = std::max(0., double(def->min));
        const double hi = double(def->max);
        for (int k = 1; out.size() < count && k <= 6; ++k) {
            const double v = value + (k % 2 == 1 ? 1 : -1) * ((k + 1) / 2) * step;
            if (v < lo - 1e-9 || v > hi + 1e-9)
                continue;
            const std::string text = number_text(v, integer) + (percent ? "%" : "");
            if (text != current && std::find(out.begin(), out.end(), text) == out.end())
                out.push_back(text);
        }
        break;
    }
    default: break;
    }
    if (out.size() > count)
        out.resize(count);
    return out;
}

ForcaExperimentLayout forca_experiment_layout(double width, double depth, double height, double bed_width, double bed_depth,
                                              double printable_height, double gap, double height_to_rod, double height_to_lid)
{
    if (height <= 0. || (printable_height > 0. && height > printable_height))
        return {};
    auto layout = [&](double w, double d, bool turned) -> ForcaExperimentLayout {
        if (w <= 0. || d <= 0. || w > bed_width || d > bed_depth)
            return {};
        const size_t cols = height > height_to_rod ? 1 : size_t((bed_width + gap) / (w + gap));
        const size_t rows = height > height_to_lid ? 1 : size_t((bed_depth + gap) / (d + gap));
        return { cols * rows, cols, turned };
    };
    const ForcaExperimentLayout straight = layout(width, depth, false);
    const ForcaExperimentLayout turned   = layout(depth, width, true);
    return turned.per_plate > straight.per_plate ? turned : straight;
}

std::vector<size_t> forca_experiment_plates(size_t copies, size_t per_plate)
{
    std::vector<size_t> out;
    if (per_plate == 0)
        return out;
    const size_t plates = (copies + per_plate - 1) / per_plate;
    const size_t each   = plates == 0 ? 0 : (copies + plates - 1) / plates; // evenly: 4 copies, 3 a plate -> 2 + 2
    for (size_t i = 0; i < copies; ++i)
        out.push_back(i / each);
    return out;
}

namespace {

// The baseline and the variants as the UI shows them.
std::vector<std::string> values_shown(const ForcaExperimentPlan& plan)
{
    if (plan.shown.size() == plan.variants.size() + 1)
        return plan.shown;
    std::vector<std::string> out{ value_shown(plan.key, plan.baseline) };
    for (const std::string& v : plan.variants)
        out.push_back(value_shown(plan.key, v));
    return out;
}

} // namespace

std::vector<std::string> forca_experiment_copy_names(const ForcaExperimentPlan& plan)
{
    const std::vector<std::string> shown = values_shown(plan);
    std::vector<std::string>       names{ "A baseline - " + shown[0] };
    for (size_t i = 1; i < shown.size(); ++i)
        names.push_back(std::string(1, char('A' + i)) + " - " + shown[i]);
    return names;
}

fs::path forca_experiment_create(const fs::path& dir, const ForcaExperimentPlan& plan, std::string& err)
{
    if (!forca_academy_ensure_skeleton(dir, err))
        return {};
    const fs::path base   = dir / "experiments" / (plan.date + "_" + forca_academy_slug(plan.title));
    fs::path       folder = base;
    for (int n = 2; fs::exists(folder); ++n)
        folder = base.string() + "-" + std::to_string(n);
    boost::system::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        err = "Could not create " + folder.string();
        return {};
    }

    const std::vector<std::string> names = forca_experiment_copy_names(plan);
    const std::vector<std::string> shown = values_shown(plan);
    const bool                     plates = plan.plates.size() == names.size() &&
                        std::any_of(plan.plates.begin(), plan.plates.end(), [](size_t p) { return p > 0; });
    json                           copies = json::array();
    copies.push_back({ { "name", names[0] }, { "value", plan.baseline }, { "shown", shown[0] }, { "baseline", true } });
    for (size_t i = 0; i < plan.variants.size(); ++i)
        copies.push_back({ { "name", names[i + 1] }, { "value", plan.variants[i] }, { "shown", shown[i + 1] } });
    if (plates)
        for (size_t i = 0; i < names.size(); ++i)
            copies[i]["plate"] = plan.plates[i] + 1;
    const json j = { { "version", 1 },           { "state", "planned" },        { "title", plan.title },
                     { "date", plan.date },       { "printer", plan.printer },   { "filament", plan.filament },
                     { "process", plan.process }, { "tag", plan.tag },           { "key", plan.key },
                     { "label", plan.label },     { "hypothesis", plan.hypothesis }, { "model", plan.model },
                     { "print_sequence", plan.by_object ? "by object" : "unchanged" }, { "copies", copies },
                     { "prints", json::array() } };
    if (!write_file(folder / "plan.json", j.dump(2) + "\n", err))
        return {};

    std::string md = "# " + plan.title + "\n\nStarted: " + plan.date + "\n\n## Plan\n\n";
    md += "- Setting: " + plan.label + " (`" + plan.key + "`), one setting only\n";
    md += "- Printer: " + plan.printer + "\n- Filament: " + plan.filament + "\n- Process: " + plan.process + "\n";
    if (!plan.tag.empty())
        md += "- Problem: " + plan.tag + "\n";
    md += "- Test model: " + plan.model + "\n";
    if (plan.by_object)
        md += "- Print sequence: by object (each copy prints on its own, start to finish)\n";
    if (plates)
        md += "- Plates: " + std::to_string(*std::max_element(plan.plates.begin(), plan.plates.end()) + 1) +
              " (not all copies fit on one plate printed by object; print each plate)\n";
    md += "\n## Hypothesis\n\n" + (plan.hypothesis.empty() ? std::string("(none written)") : plan.hypothesis) + "\n\n## Copies\n\n";
    md += "| Copy | " + plan.label + (plates ? " | Plate |\n|---|---|---|\n" : " |\n|---|---|\n");
    for (size_t i = 0; i < names.size(); ++i)
        md += "| " + names[i] + " | " + shown[i] + (plates ? " | " + std::to_string(plan.plates[i] + 1) : std::string()) + " |\n";
    md += "\n## Prints\n\n" + std::string(PRINTS_OPEN) + "\nNot printed yet.\n" + PRINTS_CLOSE + "\n";
    md += "\n## Comparison\n\n" + std::string(RESULTS_OPEN) + "\nNot judged yet: print the plate, then compare each copy with the baseline.\n" +
          RESULTS_CLOSE + "\n";
    md += "\n## Result\n\n" + std::string(FORCA_EXPERIMENT_RESULT_UNWRITTEN) + "\n";
    if (!write_file(folder / "experiment.md", md, err))
        return {};
    return folder;
}

bool forca_experiment_load(const fs::path& folder, ForcaExperimentPlan& plan)
{
    const json j = json::parse(read_file(folder / "plan.json"), nullptr, false);
    if (!j.is_object() || !j.contains("key"))
        return false;
    plan            = ForcaExperimentPlan();
    plan.title      = j.value("title", std::string());
    plan.date       = j.value("date", std::string());
    plan.printer    = j.value("printer", std::string());
    plan.filament   = j.value("filament", std::string());
    plan.process    = j.value("process", std::string());
    plan.tag        = j.value("tag", std::string());
    plan.key        = j.value("key", std::string());
    plan.label      = j.value("label", std::string());
    plan.hypothesis = j.value("hypothesis", std::string());
    plan.model      = j.value("model", std::string());
    plan.by_object  = j.value("print_sequence", std::string()) == "by object";
    plan.state      = j.value("state", std::string("planned"));
    plan.winner     = j.value("winner", -1);
    std::vector<std::string> shown;
    if (const auto c = j.find("copies"); c != j.end() && c->is_array())
        for (const json& copy : *c) {
            if (!copy.is_object())
                continue;
            const std::string value = copy.value("value", std::string());
            if (copy.value("baseline", false))
                plan.baseline = value;
            else {
                plan.variants.push_back(value);
                plan.ratings.push_back(copy.value("rating", std::string()));
            }
            plan.notes.push_back(copy.value("note", std::string()));
            shown.push_back(copy.value("shown", std::string()));
            plan.plates.push_back(size_t(std::max(1, copy.value("plate", 1)) - 1));
        }
    if (std::all_of(plan.plates.begin(), plan.plates.end(), [](size_t p) { return p == 0; }))
        plan.plates.clear();
    if (std::none_of(shown.begin(), shown.end(), [](const std::string& s) { return s.empty(); }))
        plan.shown = shown;
    if (const auto p = j.find("prints"); p != j.end() && p->is_array())
        for (const json& print : *p)
            if (print.is_object())
                plan.prints.push_back(print.value("folder", std::string()));
    return true;
}

namespace {

json read_plan(const fs::path& folder) { return json::parse(read_file(folder / "plan.json"), nullptr, false); }

// Replaces the text between `open` and `close` (markers included); false if the user removed the markers.
bool replace_block(std::string& text, const char* open, const char* close, const std::string& body)
{
    const size_t a = text.find(open);
    const size_t b = a == std::string::npos ? std::string::npos : text.find(close, a);
    if (b == std::string::npos)
        return false;
    text.replace(a, b + std::string(close).size() - a, std::string(open) + "\n" + body + close);
    return true;
}

} // namespace

std::string forca_experiment_for_plate(const fs::path& dir, const std::vector<std::string>& object_names)
{
    std::vector<fs::path>     folders;
    boost::system::error_code ec;
    for (fs::directory_iterator it(dir / "experiments", ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->path() / "plan.json", ec))
            folders.push_back(it->path());
    // Newest first (the folder names start with the date).
    std::sort(folders.begin(), folders.end(), [](const fs::path& a, const fs::path& b) { return a.filename().string() > b.filename().string(); });
    for (const fs::path& f : folders) {
        const json j = read_plan(f);
        if (!j.is_object() || !j.contains("copies") || !j["copies"].is_array() || j["copies"].empty())
            continue;
        // Per plate (1 when the plan has no "plate"): linked when all of that plate's copies are on the sent one.
        std::map<int, bool> plate_complete;
        for (const json& copy : j["copies"]) {
            if (!copy.is_object())
                continue;
            const bool on = std::find(object_names.begin(), object_names.end(), copy.value("name", std::string())) != object_names.end();
            const auto [it, added] = plate_complete.emplace(copy.value("plate", 1), on);
            if (!added)
                it->second = it->second && on;
        }
        if (std::any_of(plate_complete.begin(), plate_complete.end(), [](const auto& p) { return p.second; }))
            return "experiments/" + f.filename().string();
    }
    return {};
}

bool forca_experiment_add_print(const fs::path& folder, const std::string& print_rel, const std::string& when, std::string& err)
{
    json j = read_plan(folder);
    if (!j.is_object()) {
        err = "Could not read " + (folder / "plan.json").string();
        return false;
    }
    if (!j.contains("prints") || !j["prints"].is_array())
        j["prints"] = json::array();
    j["prints"].push_back({ { "folder", print_rel }, { "time", when } });
    if (j.value("state", std::string()) == "planned")
        j["state"] = "printed";
    if (!write_file(folder / "plan.json", j.dump(2) + "\n", err))
        return false;

    std::string lines;
    for (const json& p : j["prints"]) {
        std::string t = p.value("time", std::string());
        if (t.size() >= 16)
            t = t.substr(0, 10) + " " + t.substr(11, 5);
        lines += "- " + t + ": [" + p.value("folder", std::string()) + "](../../" + p.value("folder", std::string()) + ")\n";
    }
    std::string md = read_file(folder / "experiment.md");
    if (replace_block(md, PRINTS_OPEN, PRINTS_CLOSE, lines)) // the user's page without the block stays as it is
        return write_file(folder / "experiment.md", md, err);
    return true;
}

int forca_experiment_proposed_winner(const std::vector<std::string>& ratings)
{
    for (size_t i = 0; i < ratings.size(); ++i)
        if (ratings[i] == "better")
            return int(i) + 1;
    return 0;
}

bool forca_experiment_judge(const fs::path& folder, const std::vector<std::string>& ratings, const std::vector<std::string>& notes,
                            int winner, std::string& err)
{
    json j = read_plan(folder);
    ForcaExperimentPlan plan;
    if (!j.is_object() || !forca_experiment_load(folder, plan) || !j["copies"].is_array()) {
        err = "Could not read " + (folder / "plan.json").string();
        return false;
    }
    json&  copies = j["copies"];
    size_t v      = 0;
    for (size_t i = 0; i < copies.size(); ++i) {
        if (!copies[i].is_object())
            continue;
        copies[i]["note"] = i < notes.size() ? notes[i] : std::string();
        if (!copies[i].value("baseline", false)) {
            copies[i]["rating"] = v < ratings.size() ? ratings[v] : std::string("same");
            ++v;
        }
    }
    winner        = std::clamp(winner, 0, int(plan.variants.size()));
    j["winner"]   = winner;
    j["state"]    = "judged";
    if (!write_file(folder / "plan.json", j.dump(2) + "\n", err) || !forca_experiment_load(folder, plan))
        return false;

    const std::vector<std::string> names = forca_experiment_copy_names(plan);
    const std::vector<std::string> shown = values_shown(plan);
    auto rating_text = [](const std::string& r) { return r == "better" ? "better" : r == "worse" ? "worse" : "about the same"; };
    std::string body = "| Copy | " + plan.label + " | Compared with A | Note |\n|---|---|---|---|\n";
    for (size_t i = 0; i < names.size(); ++i)
        body += "| " + names[i] + " | " + shown[i] + " | " + (i == 0 ? std::string("(baseline)") : rating_text(plan.ratings[i - 1])) +
                " | " + (i < plan.notes.size() ? plan.notes[i] : std::string()) + " |\n";
    body += "\n**Verdict:** " + (winner == 0 ? "A, the baseline (" + shown[0] + "), stays best." :
                                               names[size_t(winner)] + " works best: " + plan.label + " " + shown[size_t(winner)] + ".") + "\n";
    std::string md = read_file(folder / "experiment.md");
    if (replace_block(md, RESULTS_OPEN, RESULTS_CLOSE, body))
        return write_file(folder / "experiment.md", md, err);
    return true;
}

std::string forca_experiment_lesson_draft(const ForcaExperimentPlan& plan, const std::string& folder_rel, const std::string& date)
{
    const std::vector<std::string> shown = values_shown(plan);
    const int                      w     = std::clamp(plan.winner, 0, int(plan.variants.size()));
    std::string rule = w == 0 ? "Keep " + plan.label + " at " + shown[0] + ": the other values were no better." :
                                "Use " + plan.label + " " + shown[size_t(w)] + " instead of " + shown[0] + ".";
    return "## " + plan.label + " with " + plan.filament + " on " + plan.printer + "\n\n" + "- Rule: " + rule + "\n" +
           "- Confidence: medium\n" + "- Evidence: [" + plan.title + "](" + folder_rel + "/experiment.md), copies side by side on one plate (" +
           date + ")\n" + (plan.tag.empty() ? std::string() : "- Playbook: [playbooks/" + plan.tag + ".md](playbooks/" + plan.tag + ".md)\n");
}

ForcaExperimentTarget forca_experiment_preset_target(const std::string& key)
{
    static const std::map<std::string, std::string> retraction = { { "object_retraction_length", "filament_retraction_length" },
                                                                    { "object_retraction_speed", "filament_retraction_speed" },
                                                                    { "object_deretraction_speed", "filament_deretraction_speed" },
                                                                    { "object_z_hop", "filament_z_hop" } };
    if (const auto it = retraction.find(key); it != retraction.end())
        return { ForcaExperimentTarget::Filament, it->second };
    if (key.rfind("object_", 0) == 0)
        return { ForcaExperimentTarget::Filament, forca_experiment_source_key(key) };
    return { ForcaExperimentTarget::Process, key };
}

bool forca_experiment_set_value(DynamicPrintConfig& config, const std::string& key, const std::string& value)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr)
        return false;
    // Orca's number parsers accept trailing junk ("hot" -> 0); a number must be a number.
    switch (def->type) {
    case coInt: case coInts: case coFloat: case coFloats: case coPercent: case coPercents: case coFloatOrPercent: case coFloatsOrPercents: {
        char* end = nullptr;
        std::strtod(value.c_str(), &end);
        if (end == value.c_str() || !(*end == '\0' || (*end == '%' && end[1] == '\0')))
            return false;
        break;
    }
    default: break;
    }
    const ConfigOption* opt = config.option(key, true); // a filament override may be missing until it is set
    if (opt == nullptr)
        return false;
    std::string text = value;
    if (opt->is_vector()) { // every slot / variant of the preset gets the value
        const size_t n = std::max<size_t>(1, static_cast<const ConfigOptionVectorBase*>(opt)->size());
        for (size_t i = 1; i < n; ++i)
            text += "," + value;
    }
    std::unique_ptr<ConfigOption> parsed(opt->clone());
    try {
        if (!parsed->deserialize(text))
            return false;
    } catch (...) {
        return false;
    }
    config.set_key_value(key, parsed.release());
    return true;
}

}} // namespace Slic3r::GUI
