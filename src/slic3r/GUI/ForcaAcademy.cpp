#include "ForcaAcademy.hpp"

#include "ForcaAI.hpp" // forca_ai_plate_report
#include "ForcaAIFiles.hpp" // forca_ai_may_write / forca_ai_record_write (INDEX.md facts)
#include "ForcaAIKeys.hpp"  // forca_ai_is_secret_key (settings.json keeps no secrets)
#include "ForcaHostStatus.hpp" // Klipper / Flashforge print status
#include "GUI.hpp"     // from_u8 / into_path
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ProjectTask.hpp" // FilamentInfo (the AMS mapping)
#include "libslic3r/Utils.hpp"       // data_dir()
#include "DeviceManager.hpp"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevHMS.h"
#include "DeviceCore/DevManager.h"
#include "HMS.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

bool forca_academy_enabled() { return wxGetApp().app_config && wxGetApp().app_config->get_bool("forca_academy_enabled"); }

fs::path forca_academy_default_dir() { return into_path(wxStandardPaths::Get().GetDocumentsDir()) / "Forca Academy"; }

fs::path forca_academy_dir()
{
    const std::string dir = wxGetApp().app_config ? wxGetApp().app_config->get("forca_academy_dir") : std::string();
    return dir.empty() ? forca_academy_default_dir() : forca_academy_folder_in(into_path(from_u8(dir)));
}

// ---- file side --------------------------------------------------------------------------------

namespace {

// Punctuation outside ASCII (general, CJK and full-width) is a separator too; other non-ASCII characters are letters.
bool slug_keeps(wchar_t c)
{
    if (c < 0x80)
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9');
    return !((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFF00 && c <= 0xFF20) ||
             (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) || c == 0x00A0);
}

bool write_text(const fs::path& path, const std::string& text, std::string& err)
{
    boost::nowide::ofstream f(path.string(), std::ios::binary | std::ios::trunc);
    if (f)
        f << text;
    if (!f) {
        err = "Could not write " + path.string();
        return false;
    }
    return true;
}

const char* const AGENTS_MD = R"(# Forca Academy -- read this first

This folder is a 3D-printing journal and knowledge base, kept by Forca Slicer and by whoever prints here (people and
AI assistants). Everything is plain Markdown and JSON, so any AI can read it without special tools.

## Read order
1. `INDEX.md` -- printers, materials, top lessons and open problems, on one short page.
2. `printers/<printer>.md` for the printer in question, then `materials/<material>.md`.
3. `playbooks/<symptom>.md` when diagnosing a problem.
4. `prints/` for the evidence behind a lesson or playbook.

## Layout
- `prints/YYYY/YYYY-MM-DD_HHMM_<name>/` -- one folder per print that Forca sent: `record.json` (written by Forca:
  printer, presets, filaments and the AMS trays they printed from, settings changed from the presets, estimates,
  source, and -- for Bambu printers -- "print_end": how the printer said it ended), `notes.md`, `thumbnail.png`,
  `photos/` (the user's) and `camera/` (pictures Forca took at the end or during the print). The outcome goes in
  `record.json` "outcome" (Forca's Print Journal writes it) or in `notes.md`.
- `printers/` -- one page per physical printer: Forca keeps its facts block (model, firmware, AMS...) current; add
  quirks, maintenance and calibration notes below it.
- `materials/` -- one page per material (brand and type): tuned values per printer and nozzle, handling notes.
- `playbooks/` -- symptom -> likely causes (most likely first) -> checks -> the fixes that worked here, with links
  to the prints that prove them.
- `lessons.md` -- distilled rules, each with a confidence and links to its evidence prints.
- `experiments/` -- A/B setting trials and their results.

## Recording
- A lesson is promoted when a pattern repeats, and corrected when a later print contradicts it. Say how sure it is
  (low / medium / high) and link the prints it rests on.
- Keep `INDEX.md` to two screens at most; move detail into the other pages. Leave its `<!-- forca:facts -->` block
  in place: Forca rewrites it after every print (counts, success rates, open patterns, latest lessons).
- Don't change the fields Forca wrote in `record.json`; add to `outcome` and `notes.md` instead.

## Rules for AI assistants
- This folder belongs to the user. Add and update your own notes; don't delete or rewrite what the user wrote.
- Never store passwords, access codes, API keys or tokens here.
- An AI connected through Forca AI can only request a print; the user approves every print in Forca.
- Through Forca AI, the forca_academy_* tools read, search and write here with these rules enforced; any other AI
  reads and writes the files directly and follows the same rules.
)";

const char* const INDEX_MD = R"(# Forca Academy -- index

Keep this page to two screens at most. Details belong in printers/, materials/, playbooks/ and lessons.md.

## Printers
(none yet)

## Materials
(none yet)

## Top lessons
(none yet)

## Open problems
(none yet)
)";

const char* const LESSONS_MD = R"(# Lessons

One entry per rule: the rule, its confidence (low / medium / high), the evidence (links to prints/...) and the date
it was last confirmed or corrected.
)";

// notes.md: Forca's outcome block (set by the Print Journal) and the placeholder it replaces.
const char* const OUTCOME_OPEN      = "<!-- forca:outcome -->";
const char* const OUTCOME_CLOSE     = "<!-- /forca:outcome -->";
const char* const OUTCOME_UNWRITTEN = "Not recorded yet. Use the Print Journal in Forca, or write it here: Success, "
                                      "Partial or Failed, and what you saw.";

// "YYYY-MM-DDTHH:MM:SS" -> ("YYYY", "YYYY-MM-DD_HHMM"); anything else -> today's.
std::pair<std::string, std::string> folder_date(const std::string& t)
{
    if (t.size() >= 16 && t[4] == '-' && t[7] == '-' && t[10] == 'T' && t[13] == ':')
        return { t.substr(0, 4), t.substr(0, 10) + "_" + t.substr(11, 2) + t.substr(14, 2) };
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M", &tm);
    return { std::string(buf, 4), buf };
}

} // namespace

std::string forca_academy_slug(const std::string& name)
{
    const std::wstring w = boost::nowide::widen(name);
    std::wstring       out;
    size_t             kept = 0;
    for (wchar_t c : w) {
        if (kept >= 48)
            break;
        if (slug_keeps(c)) {
            out += (c >= L'A' && c <= L'Z') ? wchar_t(c - L'A' + L'a') : c;
            ++kept;
        } else if (!out.empty() && out.back() != L'-') {
            out += L'-';
            ++kept;
        }
    }
    while (!out.empty() && out.back() == L'-')
        out.pop_back();
    return out.empty() ? std::string("print") : boost::nowide::narrow(out);
}

fs::path forca_academy_folder_in(const fs::path& chosen)
{
    if (chosen.filename() == "Forca Academy")
        return chosen;
    boost::system::error_code ec;
    if (fs::is_regular_file(chosen / "AGENTS.md", ec) && fs::is_directory(chosen / "prints", ec))
        return chosen; // a journal kept here directly, from before this rule: keep using it
    return chosen / "Forca Academy";
}

const std::vector<std::string>& forca_academy_own_entries()
{
    static const std::vector<std::string> entries = { "AGENTS.md", "INDEX.md",    "lessons.md",  "prints", "printers",
                                                      "materials", "playbooks",   "experiments", "forca" };
    return entries;
}

bool forca_academy_ensure_skeleton(const fs::path& dir, std::string& err)
{
    boost::system::error_code ec;
    for (const char* sub : { "prints", "printers", "materials", "playbooks", "experiments" }) {
        fs::create_directories(dir / sub, ec);
        if (ec) {
            err = "Could not create " + (dir / sub).string() + ": " + ec.message();
            return false;
        }
    }
    const std::pair<const char*, const char*> files[] = { { "AGENTS.md", AGENTS_MD }, { "INDEX.md", INDEX_MD },
                                                          { "lessons.md", LESSONS_MD } };
    for (const auto& [name, text] : files)
        if (!fs::exists(dir / name) && !write_text(dir / name, text, err))
            return false;
    return forca_academy_write_starter_playbooks(dir, err);
}

namespace {
json without_private_settings(json settings); // settings.json sections without secrets (below)
} // namespace

fs::path forca_academy_write_record(const fs::path& dir, const json& record, const std::string& thumbnail_png, std::string& err)
{
    if (!forca_academy_ensure_skeleton(dir, err))
        return {};
    const std::string title          = record.value("title", std::string("print"));
    const auto [year, stamp]         = folder_date(record.value("time", std::string()));
    const fs::path    base           = dir / "prints" / year / (stamp + "_" + forca_academy_slug(title));
    fs::path          folder         = base;
    for (int n = 2; fs::exists(folder); ++n)
        folder = fs::path(base.string() + "-" + std::to_string(n));

    boost::system::error_code ec;
    fs::create_directories(folder / "photos", ec);
    if (ec) {
        err = "Could not create " + folder.string() + ": " + ec.message();
        return {};
    }
    if (record.contains("settings")) {
        json rest = record;
        rest.erase("settings");
        if (!write_text(folder / "record.json", rest.dump(2) + "\n", err) ||
            !write_text(folder / "settings.json", without_private_settings(record["settings"]).dump(1) + "\n", err))
            return {};
    } else if (!write_text(folder / "record.json", record.dump(2) + "\n", err))
        return {};

    std::string printer = record.contains("printer") ? record["printer"].value("device", std::string()) : std::string();
    if (printer.empty() && record.contains("printer"))
        printer = record["printer"].value("physical_printer", std::string());
    if (printer.empty() && record.contains("printer"))
        printer = record["printer"].value("preset", std::string());
    std::string when = record.value("time", std::string());
    if (when.size() >= 16)
        when = when.substr(0, 10) + " " + when.substr(11, 5);
    const std::string notes = "# " + title + "\n\n" + when + (printer.empty() ? "" : " -- " + printer) +
                              "\n\n## Outcome\n\n" + OUTCOME_UNWRITTEN + "\n\n## Notes\n\n";
    if (!write_text(folder / "notes.md", notes, err))
        return {};
    if (!thumbnail_png.empty() && !write_text(folder / "thumbnail.png", thumbnail_png, err))
        return {};
    return folder;
}

namespace {

bool read_json(const fs::path& p, json& out)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    if (!f)
        return false;
    try {
        out = json::parse(f);
        return out.is_object();
    } catch (...) {
        return false;
    }
}

std::string read_all(const fs::path& p)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool is_untouched_playbook(const fs::path& file); // a starter playbook still exactly as Forca wrote it (below)

std::string outcome_block(const json& o)
{
    std::string result = o.value("result", std::string());
    if (!result.empty())
        result[0] = char(std::toupper(static_cast<unsigned char>(result[0])));
    std::string text = std::string(OUTCOME_OPEN) + "\n**" + (result.empty() ? std::string("No result") : result) + "**";
    std::string tags;
    for (const auto& t : o.value("tags", json::array()))
        tags += (tags.empty() ? "" : ", ") + t.get<std::string>();
    if (!tags.empty())
        text += " -- " + tags;
    if (const std::string note = o.value("note", std::string()); !note.empty())
        text += "\n\n" + note;
    return text + "\n" + OUTCOME_CLOSE;
}

} // namespace

bool forca_academy_read_record(const fs::path& folder, json& record) { return read_json(folder / "record.json", record); }

std::string forca_academy_tray_label(int ams_id, int slot_id)
{
    if (ams_id == 255) // VIRTUAL_TRAY_MAIN_ID
        return "Ext";
    if (ams_id == 254) // VIRTUAL_TRAY_DEPUTY_ID (second external holder, e.g. H2D)
        return "Ext 2";
    if (ams_id >= 128 && ams_id < 128 + 26) // AMS HT (single slot each)
        return std::string("HT-") + char('A' + (ams_id - 128));
    if (ams_id >= 0 && ams_id < 26 && slot_id >= 0)
        return std::string(1, char('A' + ams_id)) + std::to_string(slot_id + 1);
    return "AMS " + std::to_string(ams_id) + " slot " + std::to_string(slot_id + 1);
}

std::string forca_academy_tray_colour(const std::string& printer_colour)
{
    const std::string hex = !printer_colour.empty() && printer_colour[0] == '#' ? printer_colour.substr(1) : printer_colour;
    return hex.empty() ? std::string() : "#" + hex.substr(0, 6);
}

bool forca_academy_append_history(const fs::path& page, const std::string& title, const std::string& line, std::string& err)
{
    boost::system::error_code ec;
    fs::create_directories(page.parent_path(), ec);
    std::string text;
    if (!fs::exists(page))
        text = "# " + title + "\n\n## Calibration history\n\nWritten by Forca's Calibration Wizard.\n\n";
    text += "- " + line + "\n";
    boost::nowide::ofstream f(page.string(), std::ios::binary | std::ios::app);
    if (f)
        f << text;
    if (!f) {
        err = "Could not write " + page.string();
        return false;
    }
    return true;
}

bool forca_academy_resolve(const fs::path& dir, const std::string& rel, fs::path& out, std::string& err)
{
    const fs::path p(boost::nowide::widen(rel));
    if (rel.empty() || p.is_absolute() || p.has_root_name() || p.has_root_directory()) {
        err = "Give a path relative to the Forca Academy folder, such as \"lessons.md\" or \"prints/2026/<print>\".";
        return false;
    }
    fs::path clean;
    for (const fs::path& part : p) {
        if (part == "..") {
            err = "Paths may not leave the Forca Academy folder (\"..\").";
            return false;
        }
        if (part != ".")
            clean /= part;
    }
    if (clean.empty()) {
        err = "Give a path relative to the Forca Academy folder.";
        return false;
    }
    out = dir / clean;
    return true;
}

std::string forca_academy_relative(const fs::path& dir, const fs::path& path)
{
    std::string rel = path.lexically_relative(dir).generic_string();
    return rel.empty() || rel == "." ? std::string(".") : rel;
}

bool forca_academy_is_untouched_starter(const fs::path& file)
{
    const std::string name = file.filename().string();
    if (file.parent_path().filename() == "playbooks")
        return is_untouched_playbook(file);
    const char*       text = name == "INDEX.md" ? INDEX_MD : name == "lessons.md" ? LESSONS_MD : nullptr;
    if (!text || !fs::exists(file))
        return false;
    std::string content = read_all(file);
    // Forca's own INDEX.md facts block (forca_academy_write_index_facts) doesn't make the page the user's.
    const size_t open = content.find("\n<!-- forca:facts -->"), close = content.find("<!-- /forca:facts -->\n");
    if (open != std::string::npos && close != std::string::npos && close > open)
        content.erase(open, close + std::string("<!-- /forca:facts -->\n").size() - open);
    return content == text;
}

std::vector<ForcaAcademyPrint> forca_academy_list_prints(const fs::path& dir, size_t max)
{
    std::vector<ForcaAcademyPrint> out;
    boost::system::error_code      ec;
    const fs::path                 prints = dir / "prints";
    if (!fs::is_directory(prints, ec))
        return out;
    for (fs::directory_iterator year(prints, ec), end; !ec && year != end; year.increment(ec)) {
        if (!fs::is_directory(year->path(), ec))
            continue;
        for (fs::directory_iterator it(year->path(), ec); !ec && it != end; it.increment(ec)) {
            json r;
            if (!fs::is_directory(it->path(), ec) || !read_json(it->path() / "record.json", r))
                continue;
            ForcaAcademyPrint p;
            p.folder = it->path();
            p.time   = r.value("time", std::string());
            p.title  = r.value("title", it->path().filename().string());
            if (r.contains("printer") && r["printer"].is_object()) {
                for (const char* k : { "device", "physical_printer", "preset" })
                    if (p.printer.empty())
                        p.printer = r["printer"].value(k, std::string());
            }
            if (r.contains("outcome") && r["outcome"].is_object()) {
                p.result = r["outcome"].value("result", std::string());
                if (r["outcome"].contains("tags") && r["outcome"]["tags"].is_array())
                    for (const json& t : r["outcome"]["tags"])
                        if (t.is_string())
                            p.tags.push_back(t.get<std::string>());
            }
            if (r.contains("print_end") && r["print_end"].is_object())
                p.ended = r["print_end"].value("state", std::string());
            p.process = r.value("process", std::string());
            if (r.contains("filaments") && r["filaments"].is_array())
                for (const json& f : r["filaments"]) {
                    // A slot with a recorded use of 0 g didn't print; older records have no use figures.
                    if (!f.is_object() || (f.contains("used_g") && f["used_g"].is_number() && f["used_g"].get<double>() <= 0.))
                        continue;
                    const std::string preset = f.value("preset", std::string());
                    if (!preset.empty() && std::find(p.filaments.begin(), p.filaments.end(), preset) == p.filaments.end())
                        p.filaments.push_back(preset);
                }
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const ForcaAcademyPrint& a, const ForcaAcademyPrint& b) {
        return a.time != b.time ? a.time > b.time : a.folder.string() > b.folder.string();
    });
    if (out.size() > max)
        out.resize(max);
    return out;
}

namespace {

// Counts one print into a history; `tag_counts` collects the tags of every print with an outcome (a successful print
// with a tag still had that problem, so it counts; h.tags_on_success says how many of them were successful).
void count_print(ForcaAcademyHistory& h, std::map<std::string, int>& tag_counts, const ForcaAcademyPrint& p)
{
    h.folders.push_back(p.folder);
    if (p.result == "success" || p.result == "partial" || p.result == "failed") {
        ++(p.result == "success" ? h.success : p.result == "partial" ? h.partial : h.failed);
        for (const std::string& t : p.tags) {
            ++tag_counts[t];
            if (p.result == "success")
                ++h.tags_on_success[t];
        }
    } else
        ++h.not_recorded;
}

// Most common first; equal counts keep the tags' alphabetical order.
void set_tags(ForcaAcademyHistory& h, const std::map<std::string, int>& tag_counts)
{
    h.tags.assign(tag_counts.begin(), tag_counts.end());
    std::stable_sort(h.tags.begin(), h.tags.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
}

} // namespace

ForcaAcademyHistory forca_academy_history(const std::vector<ForcaAcademyPrint>& prints, const std::string& printer,
                                          const std::string& filament)
{
    ForcaAcademyHistory        h;
    std::map<std::string, int> tag_counts;
    for (const ForcaAcademyPrint& p : prints) {
        if (!printer.empty() && p.printer != printer)
            continue;
        if (std::find(p.filaments.begin(), p.filaments.end(), filament) == p.filaments.end())
            continue;
        count_print(h, tag_counts, p);
    }
    set_tags(h, tag_counts);
    return h;
}

std::vector<ForcaAcademyInsight> forca_academy_insights(const std::vector<ForcaAcademyPrint>& prints, ForcaAcademyGroup by)
{
    std::map<std::string, ForcaAcademyInsight>        groups;
    std::map<std::string, std::map<std::string, int>> tag_counts;
    auto add = [&](const std::string& name, const ForcaAcademyPrint& p) {
        if (name.empty())
            return;
        ForcaAcademyInsight& g = groups[name];
        g.name                 = name;
        count_print(g.history, tag_counts[name], p);
        if ((p.result == "partial" || p.result == "failed") && p.time > g.last_failure)
            g.last_failure = p.time;
    };
    for (const ForcaAcademyPrint& p : prints) {
        switch (by) {
        case ForcaAcademyGroup::Printer: add(p.printer, p); break;
        case ForcaAcademyGroup::Process: add(p.process, p); break;
        case ForcaAcademyGroup::Filament:
            for (const std::string& f : p.filaments)
                add(f, p);
            break;
        }
    }
    std::vector<ForcaAcademyInsight> out;
    for (auto& [name, g] : groups) {
        set_tags(g.history, tag_counts[name]);
        out.push_back(std::move(g));
    }
    std::stable_sort(out.begin(), out.end(), [](const ForcaAcademyInsight& a, const ForcaAcademyInsight& b) {
        return a.history.folders.size() > b.history.folders.size(); // the map already ordered them by name
    });
    return out;
}

bool forca_academy_set_outcome(const fs::path& folder, const json& outcome, std::string& err)
{
    json record;
    if (!read_json(folder / "record.json", record)) {
        err = "Could not read " + (folder / "record.json").string();
        return false;
    }
    record["outcome"] = outcome;
    if (!write_text(folder / "record.json", record.dump(2) + "\n", err))
        return false;

    const fs::path notes_path = folder / "notes.md";
    std::string    notes      = read_all(notes_path);
    const size_t   open       = notes.find(OUTCOME_OPEN);
    const size_t   close      = open == std::string::npos ? std::string::npos : notes.find(OUTCOME_CLOSE, open);
    if (close != std::string::npos)
        notes.replace(open, close + std::string(OUTCOME_CLOSE).size() - open, outcome_block(outcome));
    else if (const size_t ph = notes.find(OUTCOME_UNWRITTEN); ph != std::string::npos)
        notes.replace(ph, std::string(OUTCOME_UNWRITTEN).size(), outcome_block(outcome));
    else
        return true; // the user rewrote the section: record.json has the outcome
    return write_text(notes_path, notes, err);
}

namespace {

// Replaces the text between `open` and `close` (markers included) with `block`; false if the markers aren't there.
bool replace_block(std::string& text, const std::string& open, const std::string& close, const std::string& block)
{
    const size_t a = text.find(open);
    const size_t b = a == std::string::npos ? std::string::npos : text.find(close, a);
    if (b == std::string::npos)
        return false;
    text.replace(a, b + close.size() - a, block);
    return true;
}

} // namespace

bool forca_academy_set_print_end(const fs::path& folder, const json& end, std::string& err)
{
    json record;
    if (!read_json(folder / "record.json", record)) {
        err = "Could not read " + (folder / "record.json").string();
        return false;
    }
    record["print_end"] = end;
    if (!write_text(folder / "record.json", record.dump(2) + "\n", err))
        return false;

    static const std::string open = "<!-- forca:print_end -->", close = "<!-- /forca:print_end -->";
    const std::string        state = end.value("state", std::string("unknown"));
    std::string              line  = "**Printer reported:** " + state;
    if (end.contains("time_from_send_s"))
        line += ", " + std::to_string(end.value("time_from_send_s", 0) / 60) + " min after Forca sent it";
    std::string block = open + "\n" + line + ".";
    if (const std::string e = end.value("print_error", std::string()); !e.empty())
        block += "\nPrinter error: " + e;
    if (end.contains("health_messages"))
        for (const json& h : end["health_messages"])
            block += "\n- " + h.value("level", std::string()) + ": " + h.value("message", std::string()) + " (" + h.value("code", std::string()) + ")";
    if (const std::string n = end.value("note", std::string()); !n.empty())
        block += "\n" + n;
    block += "\n" + close;

    std::string notes = read_all(folder / "notes.md");
    if (!replace_block(notes, open, close, block))
        notes += (notes.empty() || notes.back() == '\n' ? "" : "\n") + std::string("\n") + block + "\n";
    return write_text(folder / "notes.md", notes, err);
}

bool forca_academy_write_printer_facts(const fs::path& page, const std::string& title, const std::string& facts_md, std::string& err)
{
    static const std::string open = "<!-- forca:facts -->", close = "<!-- /forca:facts -->";
    const std::string        block = open + "\n" + facts_md + (facts_md.empty() || facts_md.back() == '\n' ? "" : "\n") + close;
    boost::system::error_code ec;
    fs::create_directories(page.parent_path(), ec);
    if (!fs::exists(page))
        return write_text(page, "# " + title + "\n\n" + block + "\n\n## Notes\n\n", err);
    std::string text = read_all(page);
    if (text.find(open) == std::string::npos) {
        // A page Forca created for calibration history only: put the facts under its title. Otherwise the user's.
        if (text.find("Written by Forca's Calibration Wizard.") == std::string::npos)
            return true;
        const size_t eol = text.find('\n');
        text.insert(eol == std::string::npos ? text.size() : eol + 1, "\n" + block + "\n");
        return write_text(page, text, err);
    }
    if (!replace_block(text, open, close, block))
        return true; // an opening marker without its end: the user edited it -- leave it
    return write_text(page, text, err);
}

// ---- part 2: compare, experiments, patterns, lessons, INDEX facts, playbooks ----

namespace {

const json& member(const json& j, const char* key)
{
    static const json empty = json::object();
    return j.is_object() && j.contains(key) && j[key].is_object() ? j[key] : empty;
}

std::string as_text(const json& obj, const std::string& key)
{
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return {};
    return obj[key].is_string() ? obj[key].get<std::string>() : obj[key].dump();
}

std::string record_printer(const json& r)
{
    for (const char* k : { "device", "physical_printer", "preset" })
        if (std::string v = as_text(member(r, "printer"), k); !v.empty())
            return v;
    return {};
}

// The filament presets a print used, as the journal lists them (slots with a recorded use of 0 g skipped).
std::string record_filaments(const json& r)
{
    std::vector<std::string> names;
    if (r.contains("filaments") && r["filaments"].is_array())
        for (const json& f : r["filaments"]) {
            if (!f.is_object() || (f.contains("used_g") && f["used_g"].is_number() && f["used_g"].get<double>() <= 0.))
                continue;
            const std::string preset = as_text(f, "preset");
            if (!preset.empty() && std::find(names.begin(), names.end(), preset) == names.end())
                names.push_back(preset);
        }
    std::string out;
    for (const std::string& n : names)
        out += (out.empty() ? "" : ", ") + n;
    return out;
}

std::string record_grams(const json& r)
{
    const json& e = member(r, "estimate");
    if (!e.contains("filament_total_g") || !e["filament_total_g"].is_number())
        return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f g", e["filament_total_g"].get<double>());
    return buf;
}

std::string record_tags(const json& r)
{
    std::string out;
    const json& o = member(r, "outcome");
    if (o.contains("tags") && o["tags"].is_array())
        for (const json& t : o["tags"])
            if (t.is_string())
                out += (out.empty() ? "" : ", ") + t.get<std::string>();
    return out;
}

fs::path insights_file(const fs::path& dir) { return dir / "forca" / "insights.json"; }

json load_insights(const fs::path& dir)
{
    json j;
    return read_json(insights_file(dir), j) ? j : json::object();
}

bool save_insights(const fs::path& dir, const json& j, std::string& err)
{
    boost::system::error_code ec;
    fs::create_directories(insights_file(dir).parent_path(), ec);
    return write_text(insights_file(dir), j.dump(2) + "\n", err);
}

std::string when_text(const std::string& iso) { return iso.size() >= 16 ? iso.substr(0, 10) + " " + iso.substr(11, 5) : iso; }

// "layer_shift" -> "Layer shift"
std::string tag_text(std::string tag)
{
    std::replace(tag.begin(), tag.end(), '_', ' ');
    if (!tag.empty())
        tag[0] = char(std::toupper(static_cast<unsigned char>(tag[0])));
    return tag;
}

const char* const EXPERIMENT_RESULT_UNWRITTEN = "Not written yet: which print came out better, and what you will keep.";

} // namespace

bool forca_academy_private_setting(const std::string& key)
{
    return forca_ai_is_secret_key(key) || key.rfind("printhost_", 0) == 0 || key == "print_host" || key == "print_host_webui" ||
           key.find("serial_number") != std::string::npos;
}

namespace {

// settings.json sections without private settings.
json without_private_settings(json settings)
{
    if (!settings.is_object())
        return settings;
    for (auto& [section, values] : settings.items())
        if (values.is_object())
            for (auto it = values.begin(); it != values.end();)
                it = forca_academy_private_setting(it.key()) ? values.erase(it) : std::next(it);
    return settings;
}

} // namespace

bool forca_academy_read_settings(const fs::path& folder, json& settings)
{
    if (!read_json(folder / "settings.json", settings))
        return false;
    settings = without_private_settings(std::move(settings));
    return true;
}

ForcaAcademyComparison forca_academy_compare(const json& ra, const json& sa, const json& rb, const json& sb)
{
    ForcaAcademyComparison c;
    auto add = [&c](const char* section, const std::string& key, const std::string& a, const std::string& b) {
        c.rows.push_back({ section, key, a, b, a != b });
    };
    add("print", "printer", record_printer(ra), record_printer(rb));
    add("print", "process", as_text(ra, "process"), as_text(rb, "process"));
    add("print", "filaments", record_filaments(ra), record_filaments(rb));
    add("print", "print_time", as_text(member(ra, "estimate"), "print_time"), as_text(member(rb, "estimate"), "print_time"));
    add("print", "filament_g", record_grams(ra), record_grams(rb));
    add("print", "result", as_text(member(ra, "outcome"), "result"), as_text(member(rb, "outcome"), "result"));
    add("print", "tags", record_tags(ra), record_tags(rb));

    c.full = !member(sa, "process").empty() && !member(sb, "process").empty();
    for (const char* section : { "process", "filament", "printer" }) {
        const json& a = c.full ? member(sa, section) : member(member(ra, "unsaved_changes"), section);
        const json& b = c.full ? member(sb, section) : member(member(rb, "unsaved_changes"), section);
        std::set<std::string> keys;
        for (const auto& [k, v] : a.items())
            keys.insert(k);
        for (const auto& [k, v] : b.items())
            keys.insert(k);
        for (const std::string& k : keys)
            if (const std::string va = as_text(a, k), vb = as_text(b, k); va != vb)
                add(section, k, va, vb);
    }
    return c;
}

fs::path forca_academy_write_experiment(const fs::path& dir, const std::string& title, const std::string& date,
                                        const ForcaAcademyPrint& a, const ForcaAcademyPrint& b,
                                        const std::vector<std::string>& differences, std::string& err)
{
    if (!forca_academy_ensure_skeleton(dir, err))
        return {};
    const std::string base = (dir / "experiments" / (date + "_" + forca_academy_slug(title))).string();
    fs::path          file = base + ".md";
    for (int n = 2; fs::exists(file); ++n)
        file = base + "-" + std::to_string(n) + ".md";

    auto print_line = [&dir](const char* label, const ForcaAcademyPrint& p) {
        std::string line = std::string("- ") + label + ": [" + p.title + "](../" + forca_academy_relative(dir, p.folder) + ") -- " +
                           when_text(p.time) + ", " + (p.result.empty() ? std::string("no outcome recorded") : p.result);
        std::string tags;
        for (const std::string& t : p.tags)
            tags += (tags.empty() ? "" : ", ") + t;
        return line + (tags.empty() ? "" : " (" + tags + ")") + "\n";
    };
    std::string text = "# " + title + "\n\nStarted: " + date + "\n\n## Prints\n\n" + print_line("A", a) + print_line("B", b) +
                       "\n## What differed\n\n";
    for (const std::string& d : differences)
        text += "- " + d + "\n";
    if (differences.empty())
        text += "- (no setting differences were recorded)\n";
    text += "\n## Result\n\n" + std::string(EXPERIMENT_RESULT_UNWRITTEN) + "\n";
    return write_text(file, text, err) ? file : fs::path();
}

std::vector<ForcaAcademyExperiment> forca_academy_list_experiments(const fs::path& dir)
{
    std::vector<ForcaAcademyExperiment> out;
    boost::system::error_code           ec;
    for (fs::directory_iterator it(dir / "experiments", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".md" || !fs::is_regular_file(it->path(), ec))
            continue;
        ForcaAcademyExperiment e;
        e.file                 = it->path();
        const std::string stem = it->path().stem().string();
        e.date                 = stem.size() >= 10 && stem[4] == '-' && stem[7] == '-' ? stem.substr(0, 10) : std::string();
        e.title                = stem;
        const std::string text = read_all(it->path());
        if (text.rfind("# ", 0) == 0)
            e.title = text.substr(2, text.find_first_of("\r\n") - 2);
        if (const size_t r = text.find("## Result"); r != std::string::npos) {
            std::string rest = text.substr(r + 9);
            if (const size_t next = rest.find("\n## "); next != std::string::npos)
                rest.resize(next);
            rest.erase(0, rest.find_first_not_of(" \t\r\n"));
            rest.erase(rest.find_last_not_of(" \t\r\n") + 1);
            e.has_result = !rest.empty() && rest != EXPERIMENT_RESULT_UNWRITTEN;
        }
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.file.stem().string() > y.file.stem().string(); });
    return out;
}

std::vector<ForcaAcademyPattern> forca_academy_patterns(const std::vector<ForcaAcademyPrint>& prints, int count, int window)
{
    count  = std::max(1, count);
    window = std::max(count, window);
    // The newest `window` prints with an outcome for each printer + filament preset (`prints` is newest first).
    std::map<std::pair<std::string, std::string>, std::vector<const ForcaAcademyPrint*>> groups;
    for (const ForcaAcademyPrint& p : prints) {
        if (p.result.empty() || p.printer.empty())
            continue;
        for (const std::string& f : p.filaments)
            if (auto& g = groups[{ p.printer, f }]; int(g.size()) < window)
                g.push_back(&p);
    }
    std::vector<ForcaAcademyPattern> out;
    for (const auto& [key, group] : groups) {
        std::map<std::string, std::vector<fs::path>> tagged;
        std::map<std::string, int>                   on_success;
        for (const ForcaAcademyPrint* p : group)
            for (const std::string& t : std::set<std::string>(p->tags.begin(), p->tags.end())) {
                tagged[t].push_back(p->folder);
                if (p->result == "success")
                    ++on_success[t];
            }
        for (auto& [tag, folders] : tagged)
            if (int(folders.size()) >= count)
                out.push_back({ key.first, key.second, tag, key.first + "|" + key.second + "|" + tag, int(folders.size()),
                                int(group.size()), on_success[tag], std::move(folders) });
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
    return out;
}

std::vector<std::string> forca_academy_dismissed(const fs::path& dir)
{
    std::vector<std::string> out;
    const json               j = load_insights(dir);
    if (j.contains("dismissed") && j["dismissed"].is_array())
        for (const json& id : j["dismissed"])
            if (id.is_string())
                out.push_back(id.get<std::string>());
    return out;
}

bool forca_academy_dismiss(const fs::path& dir, const std::string& pattern_id, std::string& err)
{
    json j = load_insights(dir);
    if (!j.contains("dismissed") || !j["dismissed"].is_array())
        j["dismissed"] = json::array();
    for (const json& id : j["dismissed"])
        if (id == pattern_id)
            return true;
    j["dismissed"].push_back(pattern_id);
    return save_insights(dir, j, err);
}

std::string forca_academy_lesson_draft(const fs::path& dir, const ForcaAcademyPattern& p, const std::string& confidence,
                                       const std::string& date)
{
    std::string evidence;
    for (const fs::path& f : p.folders)
        evidence += std::string(evidence.empty() ? "" : ", ") + "[" + f.filename().string() + "](" + forca_academy_relative(dir, f) + ")";
    return "## " + tag_text(p.tag) + " with " + p.filament + " on " + p.printer + "\n\n" +
           "- Rule: (what to change or check next time)\n" +
           "- Confidence: " + confidence + "\n" +
           "- Evidence: " + evidence + "\n" +
           "- Seen: " + std::to_string(p.count) + " of the last " + std::to_string(p.window) + " prints with an outcome" +
           (p.on_success > 0 ? ", " + std::to_string(p.on_success) + " of them successful" : std::string()) + " (" + date + ")\n" +
           "- Playbook: [playbooks/" + p.tag + ".md](playbooks/" + p.tag + ".md)\n";
}

bool forca_academy_append_lesson(const fs::path& dir, const std::string& entry, std::string& err)
{
    const fs::path file = dir / "lessons.md";
    std::string    text = fs::exists(file) ? read_all(file) : std::string(LESSONS_MD);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    text += "\n\n" + entry;
    if (text.back() != '\n')
        text += "\n";
    return write_text(file, text, err);
}

std::string forca_academy_index_facts(const fs::path& dir, const std::vector<ForcaAcademyPrint>& prints,
                                      const std::vector<ForcaAcademyPattern>& open_patterns)
{
    int success = 0, partial = 0, failed = 0;
    for (const ForcaAcademyPrint& p : prints) {
        if (p.result == "success")
            ++success;
        else if (p.result == "partial")
            ++partial;
        else if (p.result == "failed")
            ++failed;
    }
    std::string md = "Kept by Forca from the print journal and refreshed after each print and outcome; write your own notes "
                     "outside this block.\n\n" +
                     std::string("**Prints:** ") + std::to_string(prints.size()) + " recorded, " +
                     std::to_string(success + partial + failed) + " with an outcome: " + std::to_string(success) + " success, " +
                     std::to_string(partial) + " partial, " + std::to_string(failed) + " failed.\n";

    auto group_lines = [&](ForcaAcademyGroup by, const char* heading, bool printer_pages) {
        const auto groups = forca_academy_insights(prints, by);
        if (groups.empty())
            return;
        md += std::string("\n**") + heading + ":**\n";
        for (size_t i = 0; i < groups.size() && i < 10; ++i) {
            const ForcaAcademyHistory& h = groups[i].history;
            md += "- " + groups[i].name + " -- " + std::to_string(h.folders.size()) + " prints";
            if (h.recorded() > 0)
                md += ", " + std::to_string(h.success) + " of " + std::to_string(h.recorded()) + " with an outcome succeeded";
            if (!h.tags.empty()) {
                const auto& [tag, n] = h.tags.front();
                const auto  ok       = h.tags_on_success.find(tag);
                md += ", most often " + tag + " (" + std::to_string(n) + " prints" +
                      (ok == h.tags_on_success.end() ? std::string() : ", " + std::to_string(ok->second) + " of them successful") + ")";
            }
            const std::string page = "printers/" + forca_academy_slug(groups[i].name) + ".md";
            if (printer_pages && fs::exists(dir / page))
                md += " -- [" + page + "](" + page + ")";
            md += "\n";
        }
    };
    group_lines(ForcaAcademyGroup::Printer, "Printers", true);
    group_lines(ForcaAcademyGroup::Filament, "Filaments", false);

    if (!open_patterns.empty()) {
        md += "\n**Open patterns** (the same problem on several recent prints):\n";
        for (size_t i = 0; i < open_patterns.size() && i < 5; ++i) {
            const ForcaAcademyPattern& p = open_patterns[i];
            md += "- " + tag_text(p.tag) + " with " + p.filament + " on " + p.printer + " (" + std::to_string(p.count) + " of the last " +
                  std::to_string(p.window) + (p.on_success > 0 ? ", " + std::to_string(p.on_success) + " of them successful" : std::string()) +
                  ") -- [playbooks/" + p.tag + ".md](playbooks/" + p.tag + ".md)\n";
        }
    }

    std::vector<std::string> lessons;
    std::string              text = read_all(dir / "lessons.md");
    for (size_t at = 0; (at = text.find("\n## ", at)) != std::string::npos; ++at)
        lessons.push_back(text.substr(at + 4, text.find_first_of("\r\n", at + 4) - (at + 4)));
    if (!lessons.empty()) {
        md += "\n**Latest lessons** (lessons.md):\n";
        for (size_t i = 0; i < lessons.size() && i < 5; ++i)
            md += "- " + lessons[lessons.size() - 1 - i] + "\n";
    }
    return md;
}

bool forca_academy_write_index_facts(const fs::path& dir, const std::string& facts_md, std::string& err)
{
    static const std::string open = "<!-- forca:facts -->", close = "<!-- /forca:facts -->";
    const fs::path           page = dir / "INDEX.md";
    if (!fs::exists(page))
        return true; // nothing to write into (ensure_skeleton creates it)
    const std::string block = open + "\n" + facts_md + (facts_md.empty() || facts_md.back() == '\n' ? "" : "\n") + close;
    std::string       text  = read_all(page);
    if (replace_block(text, open, close, block))
        return write_text(page, text, err);
    json insights = load_insights(dir);
    if (insights.value("index_facts_added", false) || text.find(open) != std::string::npos)
        return true; // the user removed the block (or its end marker): leave the page alone
    // First time: under the title line (or at the top when there is none).
    const size_t eol = text.rfind("# ", 0) == 0 ? text.find('\n') : std::string::npos;
    if (eol == std::string::npos)
        text = block + "\n\n" + text;
    else
        text.insert(eol + 1, "\n" + block + "\n");
    insights["index_facts_added"] = true;
    return write_text(page, text, err) && save_insights(dir, insights, err);
}

const std::vector<std::string>& forca_academy_tag_keys()
{
    static const std::vector<std::string> keys = { "stringing",       "warping",        "layer_shift", "spaghetti",
                                                   "bed_adhesion",    "supports",       "under_extrusion",
                                                   "over_extrusion",  "ringing",        "layer_separation" };
    return keys;
}

namespace {

struct Playbook
{
    const char* tag;
    const char* title;
    const char* causes;
    const char* checks;
    const char* in_forca;
};

// Generic starters: the usual causes, quick checks, and where Forca can help. Written once; the user owns them after.
const Playbook PLAYBOOKS[] = {
    { "stringing", "Stringing",
      "1. Wet filament (PETG, TPU and nylon above all).\n2. Nozzle temperature too high for this filament.\n"
      "3. Retraction too short or too slow.\n4. Long travel moves over open space.\n",
      "- Does a freshly dried spool string less?\n- Did a temperature tower show less stringing at a lower temperature?\n"
      "- Is the stringing between parts (travel) or on one part (oozing)?\n",
      "- Calibration Wizard: Temperature, then Retraction.\n- Filament settings: nozzle temperature, retraction length and "
      "speed (filament overrides).\n- Process settings: Avoid crossing walls, Wipe while retracting.\n" },
    { "warping", "Warping",
      "1. Weak first-layer grip (dirty plate, wrong plate type or bed temperature).\n2. Drafts or a cold room (ABS, ASA, "
      "nylon need an enclosure).\n3. Too much cooling on the first layers.\n4. Large flat parts with sharp corners.\n",
      "- Clean the plate (soap and water for PEI) and check the plate type in Forca matches the plate on the printer.\n"
      "- Does it lift at the corners only, or along whole edges?\n",
      "- Calibration Wizard: Shrinkage (dimensions) and Flow rate (first-layer squish).\n- Filament settings: bed "
      "temperature for your plate type, No cooling for the first N layers.\n- Process settings: Brim type and width.\n" },
    { "layer_shift", "Layer shift",
      "1. The nozzle hit a curled-up edge or a loose part.\n2. Speed or acceleration too high for the printer.\n"
      "3. Loose belts or pulleys, or a motor running hot.\n",
      "- Did the shift happen at a height where something curled up?\n- Is it always on the same axis?\n"
      "- Do the belts have even tension?\n",
      "- Process settings: Z hop when retracting, Avoid crossing walls, lower outer wall and travel accelerations.\n"
      "- Printer calibration: input shaping (shaper frequency and damping).\n" },
    { "spaghetti", "Spaghetti",
      "1. The part came loose from the plate.\n2. An overhang or island printed in the air (missing supports).\n"
      "3. A clog or filament tangle stopped extrusion part way.\n",
      "- Where did it start: at the plate, at an overhang, or mid-print with a good part below?\n"
      "- Was the spool tangled or the filament ground down?\n",
      "- The Bed adhesion and Supports playbooks.\n- Process settings: supports (type, threshold angle), brim.\n"
      "- Forca's support-interface regions modifier for one area that needs a different interface.\n" },
    { "bed_adhesion", "Bed adhesion",
      "1. Dirty or greasy plate.\n2. First layer too high (no squish) or too fast.\n3. Bed temperature wrong for this "
      "filament and plate.\n4. Plate type in Forca doesn't match the printer.\n",
      "- Wash the plate; don't touch the print surface.\n- Look at the first layer: round lines (too high) or "
      "flattened lines touching each other (good)?\n",
      "- Calibration Wizard: Flow rate.\n- Process settings: initial layer height and speed, brim.\n"
      "- Filament settings: bed temperature for your plate type.\n" },
    { "supports", "Supports",
      "1. Supports too hard to remove (top Z distance too small, too many interface layers).\n2. Surfaces above supports "
      "sag (gap too big, interface too sparse).\n3. Supports fell over (too thin, too tall).\n",
      "- Was the trouble removing them, or the surface above them?\n- Normal or tree supports?\n",
      "- Process settings: support type, top Z distance, top interface layers and spacing, threshold angle.\n"
      "- Forca's support-interface regions modifier: a different interface (or material) for one area only.\n" },
    { "under_extrusion", "Under-extrusion",
      "1. Asking for more flow than the hotend melts (max volumetric speed).\n2. Partial clog or wet filament.\n"
      "3. Flow ratio too low.\n4. Nozzle temperature too low.\n",
      "- Thin or gappy walls only at high speed? Then it's the volumetric limit.\n- Does it improve after a cold pull or a "
      "new nozzle?\n",
      "- Calibration Wizard: Max volumetric speed, Flow rate, Temperature.\n- Filament settings: Max volumetric speed, Flow "
      "ratio.\n" },
    { "over_extrusion", "Over-extrusion",
      "1. Flow ratio too high.\n2. Pressure advance too low (blobs at corners and seams).\n3. Filament diameter set "
      "wrong.\n",
      "- Rough top surfaces or blobs on every corner?\n- Do parts come out oversized?\n",
      "- Calibration Wizard: Flow rate, Pressure advance.\n- Filament settings: Flow ratio, pressure advance.\n" },
    { "ringing", "Ringing",
      "1. Outer walls printed too fast or with high acceleration.\n2. Input shaping not tuned for this printer.\n"
      "3. Loose frame, belts or a wobbly table.\n",
      "- Are the ripples after sharp corners, fading with distance?\n- Same on X and Y?\n",
      "- Printer calibration: input shaping (shaper frequency and damping), cornering.\n- Process settings: outer wall "
      "speed and acceleration.\n" },
    { "layer_separation", "Layer separation",
      "1. Nozzle temperature too low for this speed.\n2. Too much part cooling (ABS, ASA, PETG, nylon).\n"
      "3. Under-extrusion (see that playbook).\n4. Drafts on a large part.\n",
      "- Do the layers split at the same height every time?\n- Did it happen on thin walls or thick ones?\n",
      "- Calibration Wizard: Temperature, Max volumetric speed.\n- Filament settings: nozzle temperature, fan speeds.\n" },
};

std::string playbook_text(const Playbook& p)
{
    return std::string("# ") + p.title +
           "\n\nA starter playbook from Forca: general advice, not yet about your printers. Edit it freely; Forca never "
           "overwrites it.\n\n## Likely causes (most likely first)\n\n" +
           p.causes + "\n## Checks\n\n" + p.checks + "\n## In Forca\n\n" + p.in_forca +
           "\n## What worked here\n\n(the fixes that worked, with links to the prints that prove them)\n";
}

bool is_untouched_playbook(const fs::path& file)
{
    for (const Playbook& p : PLAYBOOKS)
        if (file.stem() == p.tag)
            return fs::exists(file) && read_all(file) == playbook_text(p);
    return false;
}

} // namespace

bool forca_academy_write_starter_playbooks(const fs::path& dir, std::string& err)
{
    json insights = load_insights(dir);
    json written  = insights.contains("playbooks_written") && insights["playbooks_written"].is_array() ? insights["playbooks_written"]
                                                                                                        : json::array();
    bool changed = false;
    for (const Playbook& p : PLAYBOOKS) {
        if (std::find(written.begin(), written.end(), json(p.tag)) != written.end())
            continue;
        const fs::path file = dir / "playbooks" / (std::string(p.tag) + ".md");
        if (!fs::exists(file) && !write_text(file, playbook_text(p), err))
            return false;
        written.push_back(p.tag);
        changed = true;
    }
    if (!changed)
        return true;
    insights["playbooks_written"] = written;
    return save_insights(dir, insights, err);
}

std::vector<fs::path> forca_academy_add_photos(const fs::path& folder, const std::vector<fs::path>& files, std::string& err)
{
    std::vector<fs::path>     added;
    boost::system::error_code ec;
    const fs::path            photos = folder / "photos";
    fs::create_directories(photos, ec);
    for (const fs::path& src : files) {
        fs::path dst = photos / src.filename();
        for (int n = 2; fs::exists(dst); ++n)
            dst = photos / (src.stem().string() + "-" + std::to_string(n) + src.extension().string());
        fs::copy_file(src, dst, ec);
        if (ec) {
            err = "Could not copy " + src.string() + ": " + ec.message();
            continue;
        }
        added.push_back(dst);
    }
    return added;
}

// ---- GUI side ---------------------------------------------------------------------------------

namespace {

std::string s_source;
std::time_t s_source_time = 0;
// Print-host uploads in flight. The upload is queued on the background slicing thread, so its snapshot is taken on
// the GUI thread just after (CallAfter); the upload may finish before or after that.
struct Upload
{
    bool        snapped  = false;
    bool        finished = false;
    bool        ok       = false;
    bool        follow   = false; // Forca can read this printer's status (Klipper, Flashforge)
    json        record;
    std::string png, host, file;
};
std::mutex                     s_uploads_mutex;
std::map<std::size_t, Upload> s_uploads; // job id -> upload

std::string now_iso()
{
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::string take_source()
{
    std::string s = (!s_source.empty() && std::time(nullptr) - s_source_time < 3600) ? s_source : std::string("user");
    s_source.clear();
    return s;
}

// A print host's address without any "user:password@" part.
std::string safe_host(std::string host)
{
    const size_t scheme = host.find("//");
    const size_t start  = scheme == std::string::npos ? 0 : scheme + 2;
    const size_t at     = host.find('@', start);
    if (at != std::string::npos && host.find('/', start) > at)
        host.erase(start, at + 1 - start);
    return host;
}

json unsaved_changes(PresetCollection& c)
{
    json j = json::object();
    for (const std::string& key : c.current_dirty_options()) {
        std::string v = c.get_edited_preset().config.opt_serialize(key);
        if (v.size() > 200)
            v = v.substr(0, 200) + "...";
        j[key] = v;
    }
    return j;
}

// Every setting the print used, per preset type, for "Compare two prints" (written to settings.json). Values come from
// the full config, so filament options hold one value per filament slot. Preset bookkeeping keys are left out.
json all_settings(PresetBundle& b, const DynamicPrintConfig& full)
{
    auto section = [&full](const DynamicPrintConfig& preset_config) {
        json j = json::object();
        for (const std::string& key : preset_config.keys()) {
            if (!full.has(key) || key == "inherits" || key == "version" || key == "from" || key == "setting_id" ||
                key.rfind("compatible_", 0) == 0 || boost::algorithm::ends_with(key, "_settings_id"))
                continue;
            std::string v = full.opt_serialize(key);
            if (v.size() > 200)
                v = v.substr(0, 200) + "...";
            j[key] = v;
        }
        return j;
    };
    return { { "process", section(b.prints.get_edited_preset().config) },
             { "printer", section(b.printers.get_edited_preset().config) },
             { "filament", section(b.filaments.get_edited_preset().config) } };
}

std::string plate_png(int plate_idx)
{
    ThumbnailData    data;
    ThumbnailsParams params{ {}, false, true, true, false, plate_idx };
    wxGetApp().plater()->get_view3D_canvas3D()->render_thumbnail(data, 512, 512, params, Camera::EType::Ortho,
                                                                  Camera::ViewAngleType::Iso);
    if (!data.is_valid())
        return {};
    size_t size = 0;
    void*  png  = tdefl_write_image_to_png_file_in_memory_ex(data.pixels.data(), data.width, data.height, 4, &size,
                                                             MZ_DEFAULT_LEVEL, 1); // OpenGL rows are bottom-up
    if (!png)
        return {};
    std::string bytes(static_cast<const char*>(png), size);
    mz_free(png);
    return bytes;
}

// Everything Forca knows about the plate being sent, taken on the GUI thread at send time.
json snapshot(int plate_idx, const std::string& route)
{
    Plater*        p      = wxGetApp().plater();
    PresetBundle&  b      = *wxGetApp().preset_bundle;
    PartPlateList& plates = p->get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        plate_idx = plates.get_curr_plate_index();
    PartPlate* plate = plates.get_plate(plate_idx);

    json objects = json::array();
    for (ModelObject* o : plate->get_objects_on_this_plate())
        objects.push_back(o->name);
    const std::string project_file = into_u8(p->get_project_filename(".3mf"));
    std::string       title        = project_file.empty() ? std::string() : into_u8(p->get_project_name());
    if (title.empty())
        title = objects.empty() ? std::string("print") : objects[0].get<std::string>();

    std::string err;
    json        report   = forca_ai_plate_report(plate_idx, err);
    json        estimate = err.empty() ? json{ { "print_time", report.value("print_time", std::string()) },
                                               { "print_time_s", report.value("print_time_s", 0) },
                                               { "filament_total_g", report.value("filament_total_g", 0.0) },
                                               { "filament_total_m", report.value("filament_total_m", 0.0) },
                                               { "cost", report.value("cost", 0.0) },
                                               { "layers", report.value("layers", 0) } }
                                         : json{ { "note", err } };
    std::map<int, json> used; // slot -> usage from the slice
    if (err.empty())
        for (const json& f : report["per_filament"])
            used[f.value("filament", 0)] = f;

    const DynamicPrintConfig full   = b.full_config();
    const auto*              types  = full.option<ConfigOptionStrings>("filament_type");
    const auto*              colors = full.option<ConfigOptionStrings>("filament_colour");
    json                     filaments = json::array();
    for (size_t i = 0; i < b.filament_presets.size(); ++i) {
        json f = { { "slot", int(i + 1) }, { "preset", b.filament_presets[i] } };
        if (types && i < types->values.size())
            f["type"] = types->values[i];
        if (colors && i < colors->values.size())
            f["colour"] = colors->values[i];
        if (auto it = used.find(int(i + 1)); it != used.end()) {
            f["used_g"] = it->second.value("weight_g", 0.0);
            f["used_m"] = it->second.value("length_m", 0.0);
        }
        filaments.push_back(f);
    }

    json nozzles = json::array();
    if (const auto* nd = b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter"))
        for (double d : nd->values)
            nozzles.push_back(d);

    return { { "forca_academy_record", 1 },
             { "time", now_iso() },
             { "title", title },
             { "source", take_source() },
             { "route", route },
             { "printer", { { "preset", b.printers.get_edited_preset().name },
                            { "physical_printer", b.physical_printers.has_selection() ? b.physical_printers.get_selected_printer_name() : "" },
                            { "nozzle_diameters", nozzles } } },
             { "process", b.prints.get_edited_preset().name },
             { "filaments", filaments },
             { "unsaved_changes", { { "process", unsaved_changes(b.prints) },
                                    { "printer", unsaved_changes(b.printers) },
                                    { "filament", unsaved_changes(b.filaments) } } },
             { "plate", { { "number", plate_idx + 1 }, { "name", plate->get_plate_name() }, { "objects", objects } } },
             { "estimate", estimate },
             { "project_file", project_file },
             { "outcome", nullptr },
             { "settings", all_settings(b, full) } }; // -> settings.json (forca_academy_write_record)
}

fs::path write(const json& record, const std::string& png)
{
    std::string    err;
    const fs::path folder = forca_academy_write_record(forca_academy_dir(), record, png, err);
    if (folder.empty())
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
    else {
        BOOST_LOG_TRIVIAL(info) << "Forca Academy: recorded " << folder.string();
        forca_academy_refresh_index();
    }
    return folder;
}

} // namespace

std::pair<int, int> forca_academy_pattern_rule()
{
    auto get = [](const char* key, int def) {
        try {
            return std::max(1, std::stoi(wxGetApp().app_config->get(key)));
        } catch (...) {
            return def;
        }
    };
    return { get("forca_academy_pattern_count", 3), get("forca_academy_pattern_window", 10) };
}

std::vector<ForcaAcademyPattern> forca_academy_open_patterns(const std::vector<ForcaAcademyPrint>& prints)
{
    const auto [count, window]  = forca_academy_pattern_rule();
    auto                     out = forca_academy_patterns(prints, count, window);
    const auto               dismissed = forca_academy_dismissed(forca_academy_dir());
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const ForcaAcademyPattern& p) {
                                 return std::find(dismissed.begin(), dismissed.end(), p.id) != dismissed.end();
                             }),
              out.end());
    return out;
}

void forca_academy_refresh_index()
{
    if (!forca_academy_enabled())
        return;
    const fs::path dir    = forca_academy_dir();
    const auto     prints = forca_academy_list_prints(dir);
    std::string    err;
    // An INDEX.md an AI wrote stays the AI's: Forca refreshing its own block doesn't count as the user's edit.
    const bool ai_owned = forca_ai_may_write(dir / "INDEX.md");
    if (!forca_academy_write_index_facts(dir, forca_academy_index_facts(dir, prints, forca_academy_open_patterns(prints)), err))
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
    else if (ai_owned)
        forca_ai_record_write(dir / "INDEX.md");
}

fs::path forca_academy_record_current_plate(std::string& err)
{
    try {
        json record     = snapshot(-1, "manual");
        record["source"] = "user: added in the Print Journal";
        const fs::path folder = forca_academy_write_record(forca_academy_dir(), record,
                                                           plate_png(record["plate"]["number"].get<int>() - 1), err);
        if (!folder.empty())
            forca_academy_refresh_index();
        return folder;
    } catch (const std::exception& e) {
        err = e.what();
        return {};
    }
}

void forca_academy_log_calibration(bool printer_page, const std::string& page, const std::string& line)
{
    if (!forca_academy_enabled() || page.empty())
        return;
    const fs::path dir = forca_academy_dir();
    std::string    err;
    if (!forca_academy_ensure_skeleton(dir, err) ||
        !forca_academy_append_history(dir / (printer_page ? "printers" : "materials") / (forca_academy_slug(page) + ".md"), page,
                                      now_iso().substr(0, 10) + " -- " + line, err))
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
}

namespace {

// ---- camera frames (never on the GUI thread: the grab can take up to ~25 s) ----

fs::path save_frame(const fs::path& folder, const std::string& jpeg, const std::string& printer_name, const std::string& suffix,
                    std::string& err)
{
    std::string stamp = now_iso(); // 2026-09-27T14:03:12 -> 20260927_140312
    stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
    stamp.erase(std::remove(stamp.begin(), stamp.end(), ':'), stamp.end());
    std::replace(stamp.begin(), stamp.end(), 'T', '_');
    boost::system::error_code ec;
    fs::create_directories(folder / "camera", ec);
    const std::string base = stamp + "_" + forca_academy_slug(printer_name) + (suffix.empty() ? "" : "_" + suffix);
    fs::path          file = folder / "camera" / (base + ".jpg");
    for (int n = 2; fs::exists(file); ++n)
        file = folder / "camera" / (base + "-" + std::to_string(n) + ".jpg");
    return write_text(file, jpeg, err) ? file : fs::path();
}

void grab_frame_async(const std::string& dev_id, const fs::path& folder, const std::string& printer_name, const std::string& suffix)
{
    std::thread([dev_id, folder, printer_name, suffix]() {
        std::string jpeg, err;
        if (!forca_printer_camera_jpeg(dev_id, 1280, jpeg, err)) {
            BOOST_LOG_TRIVIAL(info) << "Forca Academy: no camera picture (" << err << ")";
            return;
        }
        if (wxTheApp)
            wxTheApp->CallAfter([folder, jpeg, printer_name, suffix]() {
                std::string e;
                if (save_frame(folder, jpeg, printer_name, suffix, e).empty())
                    BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e;
            });
    }).detach();
}

std::set<std::string> s_no_host_camera; // print-host addresses that turned out to have no webcam (this session)

// A Klipper printer's webcam (Moonraker). `config` carries the printer's address and key for this one request.
void grab_host_frame_async(const DynamicPrintConfig& config, const fs::path& folder, const std::string& printer_name,
                           const std::string& suffix)
{
    const std::string host = config.opt_string("print_host");
    if (s_no_host_camera.count(host))
        return;
    std::thread([config, host, folder, printer_name, suffix]() {
        std::string jpeg, err;
        if (!forca_fetch_host_snapshot(config, jpeg, err)) {
            BOOST_LOG_TRIVIAL(info) << "Forca Academy: no camera picture (" << err << ")";
            if (err == "no camera" && wxTheApp)
                wxTheApp->CallAfter([host]() { s_no_host_camera.insert(host); });
            return;
        }
        if (wxTheApp)
            wxTheApp->CallAfter([folder, jpeg, printer_name, suffix]() {
                std::string e;
                if (save_frame(folder, jpeg, printer_name, suffix, e).empty())
                    BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e;
            });
    }).detach();
}

// ---- printer pages ----

std::string ams_type_name(DevAmsType t)
{
    switch (t) {
    case DevAmsType::AMS: return "AMS";
    case DevAmsType::AMS_LITE:
    case DevAmsType::AMS_LITE_MIXED: return "AMS lite";
    case DevAmsType::N3F: return "AMS 2 Pro";
    case DevAmsType::N3S: return "AMS HT";
    default: return "AMS";
    }
}

// Refreshes printers/<name>.md for the printer in `record` (with the live MachineObject when there is one) and returns
// the page's path relative to the Academy folder (for record.json), or "" on failure.
std::string update_printer_page(const fs::path& dir, const json& record, MachineObject* obj)
{
    const json&  pr     = record.at("printer");
    const std::string preset = pr.value("preset", std::string());
    std::string  title  = obj ? obj->get_dev_name() : pr.value("physical_printer", std::string());
    if (title.empty())
        title = preset;
    if (title.empty())
        return {};
    std::string facts;
    if (obj) {
        facts += "- Model: " + std::string(obj->get_printer_type_display_str().ToUTF8().data()) + "\n";
        facts += std::string("- Connection: ") + (obj->is_lan_mode_printer() ? "local network (LAN)" : "Bambu Cloud") + "\n";
        if (const std::string fw = obj->get_ota_version(); !fw.empty())
            facts += "- Firmware: " + fw + "\n";
        facts += std::string("- Camera: ") + (obj->has_ipcam ? "yes" : "no") + "\n";
        if (auto fila = obj->GetFilaSystem()) {
            std::string ams;
            for (auto& [id, unit] : fila->GetAmsList()) {
                if (!unit || unit->GetAmsType() == DevAmsType::EXT_SPOOL)
                    continue;
                int n = -1;
                try { n = std::stoi(id); } catch (...) {}
                std::string label = n >= 0 ? forca_academy_tray_label(n, 0) : id;
                if (n >= 0 && n < 128 && !label.empty())
                    label = label.substr(0, label.size() - 1); // "A1" -> "A"
                ams += (ams.empty() ? "" : "; ") + label + ": " + ams_type_name(unit->GetAmsType()) + " (" +
                       std::to_string(unit->GetSlotCount()) + (unit->GetSlotCount() == 1 ? " slot)" : " slots)");
            }
            facts += "- AMS: " + (ams.empty() ? std::string("none") : ams) + "\n";
        }
    } else if (const std::string host = pr.value("host", std::string()); !host.empty()) {
        facts += "- Connection: network print host at " + host + "\n";
    }
    if (!preset.empty()) {
        facts += "- Printer preset: " + preset;
        if (pr.contains("nozzle_diameters") && pr["nozzle_diameters"].is_array() && !pr["nozzle_diameters"].empty()) {
            char nz[32];
            std::snprintf(nz, sizeof(nz), " (nozzle %.2f mm)", pr["nozzle_diameters"][0].get<double>());
            facts += nz;
        }
        facts += "\n";
        const std::string preset_page = "printers/" + forca_academy_slug(preset) + ".md";
        if (forca_academy_slug(preset) != forca_academy_slug(title) && fs::exists(dir / preset_page))
            facts += "- Calibration history for that preset: " + preset_page + "\n";
    }
    facts += "- Updated by Forca: " + now_iso().substr(0, 10) + " (this block is rewritten on each print; write your own notes below)\n";

    const std::string rel = "printers/" + forca_academy_slug(title) + ".md";
    std::string       err;
    if (!forca_academy_write_printer_facts(dir / rel, title, facts, err)) {
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
        return {};
    }
    return rel;
}

// ---- following sent prints ----

struct Tracked
{
    std::string dev_id, folder, name, job; // folder: absolute, UTF-8
    std::string host, file; // print-host sends (Klipper, Flashforge): the printer's address (no credentials), the uploaded file
    long long   sent = 0, last_frame = 0;
    bool        running = false;
};
std::vector<Tracked>  s_tracked;
std::set<std::string> s_host_polls; // folders whose print-host status read is in flight
wxTimer*             s_timer = nullptr;

fs::path tracking_file() { return into_path(from_u8(data_dir())) / "forca" / "academy_tracking.json"; }

void save_tracking()
{
    json list = json::array();
    for (const Tracked& t : s_tracked)
        list.push_back({ { "dev_id", t.dev_id }, { "folder", t.folder }, { "name", t.name }, { "job", t.job }, { "host", t.host },
                         { "file", t.file }, { "sent", t.sent }, { "last_frame", t.last_frame }, { "running", t.running } });
    std::string err;
    boost::system::error_code ec;
    fs::create_directories(tracking_file().parent_path(), ec);
    if (!write_text(tracking_file(), list.dump(1), err))
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
}

void load_tracking()
{
    json list;
    boost::nowide::ifstream f(tracking_file().string(), std::ios::binary);
    try {
        list = json::parse(f);
    } catch (...) {
        return;
    }
    s_tracked.clear();
    for (const json& j : list) {
        Tracked t;
        t.dev_id     = j.value("dev_id", std::string());
        t.folder     = j.value("folder", std::string());
        t.name       = j.value("name", std::string());
        t.job        = j.value("job", std::string());
        t.host       = j.value("host", std::string());
        t.file       = j.value("file", std::string());
        t.sent       = j.value("sent", 0LL);
        t.last_frame = j.value("last_frame", 0LL);
        t.running    = j.value("running", false);
        if ((!t.dev_id.empty() || !t.host.empty()) && !t.folder.empty())
            s_tracked.push_back(t);
    }
}

int photo_minutes()
{
    try {
        return std::max(0, std::stoi(wxGetApp().app_config->get("forca_academy_photo_minutes")));
    } catch (...) {
        return 0;
    }
}

// `host_config`: a print-host printer's settings (its end photo comes from its webcam); `printer_message`: its error text.
void end_print(const Tracked& t, MachineObject* obj, const std::string& state, const std::string& note,
               const DynamicPrintConfig* host_config = nullptr, const std::string& printer_message = {})
{
    json end = { { "state", state }, { "ended", now_iso() }, { "time_from_send_s", (long long) std::time(nullptr) - t.sent } };
    if (!t.job.empty())
        end["job"] = t.job;
    if (!note.empty())
        end["note"] = note;
    if (!printer_message.empty())
        end["print_error"] = printer_message;
    if (obj && obj->print_error != 0)
        end["print_error"] = obj->get_print_error_str();
    json hms = json::array();
    if (obj)
        if (DevHMS* h = obj->GetHMS())
            for (const DevHMSItem& item : h->GetHMSItems()) {
                const std::string code = item.get_long_error_code();
                std::string       text;
                if (HMSQuery* q = wxGetApp().get_hms_query())
                    text = q->query_hms_msg(obj, code).ToUTF8().data();
                static const char* levels[] = { "unknown", "fatal", "serious", "common", "info" };
                const int          lvl      = int(item.get_level());
                hms.push_back({ { "code", code }, { "level", lvl >= 0 && lvl <= 4 ? levels[lvl] : "unknown" }, { "message", text } });
            }
    if (!hms.empty())
        end["health_messages"] = hms;
    std::string err;
    if (!forca_academy_set_print_end(into_path(from_u8(t.folder)), end, err))
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
    else
        BOOST_LOG_TRIVIAL(info) << "Forca Academy: " << t.folder << " ended (" << state << ")";
    if (state != "unknown" && wxGetApp().app_config->get_bool("forca_academy_end_photo")) {
        if (obj)
            grab_frame_async(t.dev_id, into_path(from_u8(t.folder)), t.name, "end");
        else if (host_config)
            grab_host_frame_async(*host_config, into_path(from_u8(t.folder)), t.name, "end");
    }
}

// A print-host printer's status arrived (GUI thread): the same rules as a Bambu printer's -- follow the print once the
// printer runs this job, record how it ended, give up after 6 h if it never started.
void apply_host_status(const std::string& folder, bool ok, const ForcaHostStatus& st, const DynamicPrintConfig& config)
{
    s_host_polls.erase(folder);
    auto it = std::find_if(s_tracked.begin(), s_tracked.end(), [&](const Tracked& t) { return t.folder == folder; });
    if (it == s_tracked.end())
        return;
    Tracked&        t    = *it;
    const long long now  = (long long) std::time(nullptr);
    const auto      drop = [&]() {
        s_tracked.erase(it);
        save_tracking();
    };
    if (!ok) { // printer off or unreachable: wait, like a Bambu printer without live data
        if (!t.running && now - t.sent > 6 * 3600)
            drop();
        return;
    }
    const bool active = st.state == ForcaHostState::Printing || st.state == ForcaHostState::Paused ||
                        st.state == ForcaHostState::Preparing;
    if (active) {
        const bool ours = t.file.empty() || st.file.empty() || forca_same_job_file(t.file, st.file);
        if (!t.running) {
            if (!ours) // still busy with an earlier job
                return;
            t.running = true;
            t.job     = st.file;
            save_tracking();
        } else if (!ours && !t.job.empty() && !forca_same_job_file(t.job, st.file)) {
            end_print(t, nullptr, "unknown", "Another print started before Forca saw this one end.");
            drop();
            return;
        }
        if (const int minutes = photo_minutes();
            minutes > 0 && st.state != ForcaHostState::Preparing && now - t.last_frame >= minutes * 60LL) {
            t.last_frame = now;
            save_tracking();
            grab_host_frame_async(config, into_path(from_u8(t.folder)), t.name,
                                  st.layer >= 0 ? "L" + std::to_string(st.layer) : std::string());
        }
        return;
    }
    if (t.running) {
        std::string state = st.state == ForcaHostState::Finished ? "finished" :
                            st.state == ForcaHostState::Failed   ? "failed" :
                            st.state == ForcaHostState::Stopped  ? "stopped" :
                                                                   "unknown";
        // Back to standby (a cancel from Fluidd or a printer macro does that): Moonraker's job history knows the ending.
        if (state == "unknown" && !st.last_job_end.empty() &&
            forca_same_job_file(st.last_job_file, t.job.empty() ? t.file : t.job))
            state = st.last_job_end;
        end_print(t, nullptr, state, state == "unknown" ? "The printer went idle without saying how the print ended." : "",
                  &config, st.message);
        drop();
        return;
    }
    if (now - t.sent > 6 * 3600) // never seen printing
        drop();
}

// Ask each followed print-host printer for its status, one request per print at a time, off the GUI thread.
void poll_hosts()
{
    for (const Tracked& t : s_tracked) {
        if (t.host.empty() || s_host_polls.count(t.folder))
            continue;
        DynamicPrintConfig config;
        if (!forca_find_host_config(t.host, config))
            continue; // the printer's settings are gone: the 6 h / 7 day rules drop it
        s_host_polls.insert(t.folder);
        std::thread([folder = t.folder, config]() {
            ForcaHostStatus st;
            std::string     err;
            const bool      ok = forca_fetch_host_status(config, st, err);
            if (!ok)
                BOOST_LOG_TRIVIAL(debug) << "Forca Academy: printer status not read (" << err << ")";
            if (wxTheApp)
                wxTheApp->CallAfter([folder, ok, st, config]() { apply_host_status(folder, ok, st, config); });
        }).detach();
    }
}

void tick()
{
    if (s_tracked.empty()) {
        if (s_timer)
            s_timer->Stop();
        return;
    }
    if (!forca_academy_enabled())
        return;
    DeviceManager*  dev     = wxGetApp().getDeviceManager();
    const long long now     = (long long) std::time(nullptr);
    bool            changed = false;
    for (auto it = s_tracked.begin(); it != s_tracked.end();) {
        Tracked& t = *it;
        if (now - t.sent > 7 * 24 * 3600 || !fs::exists(into_path(from_u8(t.folder)))) {
            it = s_tracked.erase(it); // given up (or the user moved the record)
            changed = true;
            continue;
        }
        if (!t.host.empty() || !dev) { // print hosts are polled below
            ++it;
            continue;
        }
        MachineObject* obj = dev->get_my_machine(t.dev_id);
        if (!obj || !obj->is_connected()) { // no live data (Forca follows the Device tab's printer): wait
            if (!t.running && now - t.sent > 6 * 3600) {
                it      = s_tracked.erase(it);
                changed = true;
            } else
                ++it;
            continue;
        }
        const bool        printing = obj->is_in_printing() || obj->is_in_printing_pause();
        const std::string job      = obj->subtask_name;
        if (printing) {
            if (!t.running) {
                t.running = true;
                t.job     = job;
                changed   = true;
            } else if (!t.job.empty() && !job.empty() && job != t.job) {
                end_print(t, nullptr, "unknown", "Another print started before Forca saw this one end.");
                it      = s_tracked.erase(it);
                changed = true;
                continue;
            }
            if (const int minutes = photo_minutes(); minutes > 0 && now - t.last_frame >= minutes * 60LL) {
                t.last_frame = now;
                changed      = true;
                grab_frame_async(t.dev_id, into_path(from_u8(t.folder)), t.name, "L" + std::to_string(obj->curr_layer));
            }
            ++it;
            continue;
        }
        if (t.running) {
            const std::string st    = obj->print_status;
            const std::string state = st == "FINISH" ? "finished" : st == "FAILED" ? "failed" : "stopped";
            end_print(t, obj, state, "");
            it      = s_tracked.erase(it);
            changed = true;
            continue;
        }
        if (now - t.sent > 6 * 3600) { // never seen printing
            it      = s_tracked.erase(it);
            changed = true;
            continue;
        }
        ++it;
    }
    if (changed)
        save_tracking();
    poll_hosts();
}

void start_timer()
{
    if (!s_timer) {
        s_timer = new wxTimer(); // lives for the app's lifetime
        s_timer->Bind(wxEVT_TIMER, [](wxTimerEvent&) { tick(); });
    }
    if (!s_timer->IsRunning())
        s_timer->Start(15000);
}

} // namespace

void forca_academy_start()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg && cfg->get("forca_academy_end_photo").empty())
        cfg->set_bool("forca_academy_end_photo", true);
    if (cfg && cfg->get("forca_academy_photo_minutes").empty())
        cfg->set("forca_academy_photo_minutes", "0");
    if (cfg && cfg->get("forca_academy_pattern_count").empty())
        cfg->set("forca_academy_pattern_count", "3");
    if (cfg && cfg->get("forca_academy_pattern_window").empty())
        cfg->set("forca_academy_pattern_window", "10");
    load_tracking();
    if (!s_tracked.empty())
        start_timer();
}

fs::path forca_academy_save_camera_frame(const std::string& print_rel, const std::string& jpeg, const std::string& printer_name,
                                         std::string& err)
{
    if (!forca_academy_enabled()) {
        err = "Forca Academy is off (Preferences > Forca Academy).";
        return {};
    }
    const fs::path dir = forca_academy_dir();
    fs::path       folder;
    json           record;
    if (!forca_academy_resolve(dir, print_rel, folder, err))
        return {};
    if (!read_json(folder / "record.json", record)) {
        err = "No recorded print at " + print_rel + ".";
        return {};
    }
    return save_frame(folder, jpeg, printer_name, "", err);
}

void forca_academy_mark_source(const std::string& source)
{
    s_source      = source;
    s_source_time = std::time(nullptr);
}

namespace {

// A Bambu send's record: the plate as it is now, the device, and the tray each filament slot prints from.
json sent_record(int plate_idx, const std::string& device_label, const std::vector<FilamentInfo>* trays)
{
    json record = snapshot(plate_idx, "printer");
    if (!device_label.empty())
        record["printer"]["device"] = device_label;
    // The tray each filament slot printed from (the print dialog's mapping), and what that tray holds.
    if (trays)
        for (const FilamentInfo& t : *trays) {
            if (t.tray_id < 0 || t.id < 0 || size_t(t.id) >= record["filaments"].size())
                continue; // not mapped
            int ams = -1, slot = -1;
            try {
                ams  = t.ams_id.empty() ? -1 : std::stoi(t.ams_id);
                slot = t.slot_id.empty() ? -1 : std::stoi(t.slot_id);
            } catch (...) {}
            json tray = { { "tray", ams >= 0 ? forca_academy_tray_label(ams, slot) : "tray " + std::to_string(t.tray_id) } };
            if (ams >= 0) {
                tray["ams_id"]  = ams;
                tray["slot_id"] = slot;
            }
            if (!t.type.empty())
                tray["tray_type"] = t.type;
            if (const std::string colour = forca_academy_tray_colour(t.color); !colour.empty())
                tray["tray_colour"] = colour;
            if (!t.filament_id.empty())
                tray["tray_filament_id"] = t.filament_id;
            record["filaments"][t.id]["printed_from"] = tray;
        }
    return record;
}

// Writes a Bambu send's record and follows the print until the printer reports it ended.
void commit_sent_record(json record, const std::string& png, const std::string& dev_id, const std::string& device_label)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev && !dev_id.empty() ? dev->get_my_machine(dev_id) : nullptr;
    if (const std::string page = update_printer_page(forca_academy_dir(), record, obj); !page.empty())
        record["printer"]["page"] = page;
    const fs::path folder = write(record, png);
    if (!folder.empty() && !dev_id.empty()) { // follow it until the printer reports it ended
        Tracked t;
        t.dev_id = dev_id;
        t.folder = into_u8(from_path(folder));
        t.name   = obj ? obj->get_dev_name() : device_label;
        t.sent   = (long long) std::time(nullptr);
        s_tracked.erase(std::remove_if(s_tracked.begin(), s_tracked.end(), [&](const Tracked& o) { return o.dev_id == dev_id; }),
                        s_tracked.end()); // one print per printer at a time
        s_tracked.push_back(t);
        save_tracking();
        start_timer();
    }
}

// Multi-machine sends: the plate is snapshotted when the job is queued and written once that printer's send succeeds.
struct HeldTask
{
    json        record;
    std::string png, dev_id, device_label;
};
std::mutex               s_tasks_mutex;
std::map<int, HeldTask> s_tasks; // TaskManager task id -> snapshot

} // namespace

void forca_academy_on_print_sent(int plate_idx, const std::string& dev_id, const std::string& device_label,
                                 const std::vector<FilamentInfo>* trays)
{
    if (!forca_academy_enabled())
        return;
    try { // never let the journal get in the way of printing
        json record = sent_record(plate_idx, device_label, trays);
        const std::string png = plate_png(record["plate"]["number"].get<int>() - 1);
        commit_sent_record(std::move(record), png, dev_id, device_label);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
    }
}

void forca_academy_hold_task(int task_id, int plate_idx, const std::string& dev_id, const std::string& device_label)
{
    if (!forca_academy_enabled())
        return;
    try {
        HeldTask held;
        held.record = sent_record(plate_idx, device_label, nullptr);
        held.record["source"] = "user: multi-machine send";
        held.png          = plate_png(held.record["plate"]["number"].get<int>() - 1);
        held.dev_id       = dev_id;
        held.device_label = device_label;
        std::lock_guard<std::mutex> lock(s_tasks_mutex);
        s_tasks[task_id] = std::move(held);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
    }
}

void forca_academy_task_done(int task_id, bool ok)
{
    HeldTask held;
    {
        std::lock_guard<std::mutex> lock(s_tasks_mutex);
        auto it = s_tasks.find(task_id);
        if (it == s_tasks.end())
            return;
        held = std::move(it->second);
        s_tasks.erase(it);
    }
    if (!ok)
        return;
    wxGetApp().CallAfter([held = std::move(held)]() mutable {
        try {
            commit_sent_record(std::move(held.record), held.png, held.dev_id, held.device_label);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
        }
    });
}

namespace {

// Writes a print-host send's record and, for printers Forca can read (Klipper, Flashforge), follows the print.
void commit_upload(json record, const std::string& png, const Upload& upload)
{
    const fs::path folder = write(record, png);
    if (folder.empty() || !upload.follow)
        return;
    Tracked t;
    t.folder = into_u8(from_path(folder));
    t.host   = upload.host;
    t.file   = upload.file;
    t.name   = record["printer"].value("physical_printer", std::string());
    if (t.name.empty())
        t.name = record["printer"].value("preset", std::string());
    t.sent = (long long) std::time(nullptr);
    s_tracked.erase(std::remove_if(s_tracked.begin(), s_tracked.end(), [&](const Tracked& o) { return !o.host.empty() && o.host == t.host; }),
                    s_tracked.end()); // one print per printer at a time
    s_tracked.push_back(t);
    save_tracking();
    start_timer();
}

} // namespace

void forca_academy_hold_upload(std::size_t job_id, const std::string& host, const std::string& file)
{
    {
        std::lock_guard<std::mutex> lock(s_uploads_mutex);
        Upload upload;
        upload.host       = safe_host(host);
        upload.file       = file;
        s_uploads[job_id] = std::move(upload);
    }
    wxGetApp().CallAfter([job_id, host]() {
        json        record;
        std::string png;
        bool        ok     = forca_academy_enabled();
        bool        follow = false;
        if (ok) {
            try {
                DynamicPrintConfig config;
                follow = forca_find_host_config(host, config) && !forca_host_status_kind(config).empty();
                record                    = snapshot(-1, "print_host");
                record["printer"]["host"] = safe_host(host);
                if (const std::string page = update_printer_page(forca_academy_dir(), record, nullptr); !page.empty())
                    record["printer"]["page"] = page;
                png                       = plate_png(record["plate"]["number"].get<int>() - 1);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
                ok = false;
            }
        }
        std::unique_lock<std::mutex> lock(s_uploads_mutex);
        auto                         it = s_uploads.find(job_id);
        if (it == s_uploads.end())
            return;
        if (!ok) {
            s_uploads.erase(it);
            return;
        }
        it->second.follow = follow;
        if (!it->second.finished) {
            it->second.record  = std::move(record);
            it->second.png     = std::move(png);
            it->second.snapped = true;
            return;
        }
        Upload upload = std::move(it->second); // the upload already ended
        s_uploads.erase(it);
        lock.unlock();
        if (upload.ok)
            commit_upload(std::move(record), png, upload);
    });
}

void forca_academy_upload_done(std::size_t job_id, bool ok)
{
    std::unique_lock<std::mutex> lock(s_uploads_mutex);
    auto                         it = s_uploads.find(job_id);
    if (it == s_uploads.end())
        return;
    if (!it->second.snapped) { // the snapshot is still on its way: it writes (or not) when it arrives
        it->second.finished = true;
        it->second.ok       = ok;
        return;
    }
    Upload upload = std::move(it->second);
    s_uploads.erase(it);
    lock.unlock();
    if (ok) {
        try {
            json record = std::move(upload.record);
            commit_upload(std::move(record), upload.png, upload);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
        }
    }
}

}} // namespace Slic3r::GUI
