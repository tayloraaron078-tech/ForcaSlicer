#include "ForcaRecipe.hpp"

#include "ForcaAcademy.hpp" // forca_academy_private_setting / forca_academy_slug / forca_academy_ensure_skeleton
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cstddef>
#include <ios>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

// ForcaAcademy.cpp all_settings / unsaved_changes keep 200 characters of a longer value and add "...".
constexpr size_t KEPT_CHARS = 200;

bool cut_value(const std::string& v) { return v.size() == KEPT_CHARS + 3 && boost::algorithm::ends_with(v, "..."); }

// Preset bookkeeping, never a recipe setting (all_settings leaves these out too; unsaved changes may hold them).
bool bookkeeping_key(const std::string& key)
{
    return key == "inherits" || key == "version" || key == "from" || key == "setting_id" || key == "filament_id" ||
           key.rfind("compatible_", 0) == 0 || boost::algorithm::ends_with(key, "_settings_id");
}

// `text` parsed as `like` (the parent's option), narrowed to one filament: a list with one entry per filament gives
// slot's entry when the parent holds one value. Null when it doesn't fit.
std::unique_ptr<ConfigOption> parse_for(const ConfigOption& like, const std::string& text, int slot)
{
    std::unique_ptr<ConfigOption> parsed(like.clone());
    try {
        if (!parsed->deserialize(text))
            return nullptr;
    } catch (...) {
        return nullptr;
    }
    if (!like.is_vector())
        return parsed;
    const auto&  want = static_cast<const ConfigOptionVectorBase&>(like);
    const auto&  got  = static_cast<const ConfigOptionVectorBase&>(*parsed);
    if (got.size() == want.size())
        return parsed;
    if (slot < 0 || want.size() != 1 || size_t(slot) >= got.size())
        return nullptr;
    std::unique_ptr<ConfigOption> one(like.clone());
    static_cast<ConfigOptionVectorBase&>(*one).set_at(&got, 0, size_t(slot));
    return one;
}

// A value in a Markdown table cell: on one line, its | escaped.
std::string cell(const std::string& v)
{
    if (v.empty())
        return "(empty)";
    std::string out = "`";
    for (char c : v)
        out += c == '|' ? std::string("\\|") : c == '\n' || c == '\r' ? std::string(" ") : std::string(1, c);
    return out + "`";
}

} // namespace

std::vector<ForcaRecipeRow> forca_recipe_rows(const DynamicPrintConfig& parent, const json& print_values, int slot, const json* only_keys)
{
    std::vector<ForcaRecipeRow> rows;
    if (!print_values.is_object())
        return rows;
    for (const auto& item : print_values.items()) {
        const std::string& key = item.key();
        if (!item.value().is_string() || bookkeeping_key(key) || forca_academy_private_setting(key))
            continue;
        if (only_keys != nullptr && !(only_keys->is_object() && only_keys->contains(key)))
            continue;
        const ConfigOption* now = parent.option(key);
        if (now == nullptr)
            continue;
        ForcaRecipeRow row{ key, now->serialize(), item.value().get<std::string>(), {} };
        if (cut_value(row.value)) {
            if (boost::algorithm::starts_with(row.parent, row.value.substr(0, KEPT_CHARS)))
                continue; // most likely the parent's value: nothing to tell
            row.problem = "cut";
        } else if (std::unique_ptr<ConfigOption> used = parse_for(*now, row.value, slot)) {
            if (*used == *now)
                continue;
            row.value = used->serialize();
        } else {
            row.problem = "invalid";
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool forca_recipe_apply(DynamicPrintConfig& config, const std::vector<ForcaRecipeRow>& rows, std::string& err)
{
    for (const ForcaRecipeRow& row : rows) {
        if (!row.problem.empty())
            continue;
        ConfigOption* opt = config.option(row.key);
        bool          ok  = opt != nullptr;
        try {
            ok = ok && opt->deserialize(row.value);
        } catch (...) {
            ok = false;
        }
        if (!ok) {
            err = row.key;
            return false;
        }
    }
    return true;
}

std::vector<int> forca_recipe_used_slots(const json& record)
{
    std::vector<int> all, weighed;
    const json       none      = json::array();
    const json&      filaments = record.contains("filaments") && record["filaments"].is_array() ? record["filaments"] : none;
    for (const json& f : filaments) {
        const int slot = f.value("slot", 0) - 1; // record.json counts from 1
        if (slot < 0)
            continue;
        all.push_back(slot);
        if (f.contains("used_g"))
            weighed.push_back(slot);
    }
    return weighed.empty() ? all : weighed;
}

fs::path forca_recipe_write_page(const fs::path& dir, const std::string& name, const std::string& date, const std::string& print_rel,
                                 const std::string& print_title, const std::string& source,
                                 const std::vector<ForcaRecipePreset>& presets, std::string& err)
{
    if (!forca_academy_ensure_skeleton(dir, err))
        return {};
    const std::string base = (dir / "recipes" / (date + "_" + forca_academy_slug(name))).string();
    fs::path          file = base + ".md";
    for (int n = 2; fs::exists(file); ++n)
        file = base + "-" + std::to_string(n) + ".md";

    std::string text = "# " + name + "\n\nRecipe saved " + date + ". " + source + "\n";
    if (!print_rel.empty())
        text += "From the print [" + print_title + "](../" + print_rel + ").\n";
    text += "Printer settings are not part of a recipe.\n";
    for (const ForcaRecipePreset& p : presets) {
        text += "\n## " + std::string(p.type == "filament" ? "Filament" : "Process") +
                (p.type == "filament" ? " (filament " + std::to_string(p.slot + 1) + ")" : std::string()) + ": " + p.name +
                "\n\nBased on " + p.parent + ".\n\n";
        if (p.rows.empty()) {
            text += "Same settings as " + p.parent + ".\n";
            continue;
        }
        text += "| Setting | " + p.parent + " | This recipe |\n|---|---|---|\n";
        for (const ForcaRecipeRow& r : p.rows)
            text += "| " + r.key + " | " + cell(r.parent) + " | " + cell(r.value) + " |\n";
    }
    text += "\n## Notes\n\nWhat this recipe is for and what to watch. Forca doesn't change this page after saving it.\n";

    boost::nowide::ofstream f(file.string(), std::ios::binary | std::ios::trunc);
    if (f)
        f << text;
    if (!f) {
        err = "Could not write " + file.string();
        return {};
    }
    return file;
}

std::vector<std::pair<std::string, std::string>> forca_recipe_list(const fs::path& dir)
{
    std::vector<std::pair<std::string, std::string>> out;
    boost::system::error_code                        ec;
    for (fs::directory_iterator it(dir / "recipes", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".md" || !fs::is_regular_file(it->path(), ec))
            continue;
        boost::nowide::ifstream f(it->path().string());
        std::string             first;
        std::getline(f, first);
        if (!first.empty() && first.back() == '\r')
            first.pop_back();
        out.emplace_back(it->path().stem().string(), boost::algorithm::starts_with(first, "# ") ? first.substr(2) : it->path().stem().string());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return out;
}

}} // namespace Slic3r::GUI
