#include "ForcaAcademy.hpp"

#include "ForcaAI.hpp" // forca_ai_plate_report
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

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <miniz.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

bool forca_academy_enabled() { return wxGetApp().app_config && wxGetApp().app_config->get_bool("forca_academy_enabled"); }

fs::path forca_academy_default_dir() { return into_path(wxStandardPaths::Get().GetDocumentsDir()) / "Forca Academy"; }

fs::path forca_academy_dir()
{
    const std::string dir = wxGetApp().app_config ? wxGetApp().app_config->get("forca_academy_dir") : std::string();
    return dir.empty() ? forca_academy_default_dir() : into_path(from_u8(dir));
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
- Keep `INDEX.md` to two screens at most; move detail into the other pages.
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
    return true;
}

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
    if (!write_text(folder / "record.json", record.dump(2) + "\n", err))
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
    const char*       text = name == "INDEX.md" ? INDEX_MD : name == "lessons.md" ? LESSONS_MD : nullptr;
    return text && fs::exists(file) && read_all(file) == text;
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
            if (r.contains("outcome") && r["outcome"].is_object())
                p.result = r["outcome"].value("result", std::string());
            if (r.contains("print_end") && r["print_end"].is_object())
                p.ended = r["print_end"].value("state", std::string());
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
    json        record;
    std::string png;
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
             { "outcome", nullptr } };
}

fs::path write(const json& record, const std::string& png)
{
    std::string    err;
    const fs::path folder = forca_academy_write_record(forca_academy_dir(), record, png, err);
    if (folder.empty())
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << err;
    else
        BOOST_LOG_TRIVIAL(info) << "Forca Academy: recorded " << folder.string();
    return folder;
}

} // namespace

fs::path forca_academy_record_current_plate(std::string& err)
{
    try {
        json record     = snapshot(-1, "manual");
        record["source"] = "user: added in the Print Journal";
        const fs::path folder = forca_academy_write_record(forca_academy_dir(), record,
                                                           plate_png(record["plate"]["number"].get<int>() - 1), err);
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
    long long   sent = 0, last_frame = 0;
    bool        running = false;
};
std::vector<Tracked> s_tracked;
wxTimer*             s_timer = nullptr;

fs::path tracking_file() { return into_path(from_u8(data_dir())) / "forca" / "academy_tracking.json"; }

void save_tracking()
{
    json list = json::array();
    for (const Tracked& t : s_tracked)
        list.push_back({ { "dev_id", t.dev_id }, { "folder", t.folder }, { "name", t.name }, { "job", t.job },
                         { "sent", t.sent }, { "last_frame", t.last_frame }, { "running", t.running } });
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
        t.sent       = j.value("sent", 0LL);
        t.last_frame = j.value("last_frame", 0LL);
        t.running    = j.value("running", false);
        if (!t.dev_id.empty() && !t.folder.empty())
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

void end_print(const Tracked& t, MachineObject* obj, const std::string& state, const std::string& note)
{
    json end = { { "state", state }, { "ended", now_iso() }, { "time_from_send_s", (long long) std::time(nullptr) - t.sent } };
    if (!t.job.empty())
        end["job"] = t.job;
    if (!note.empty())
        end["note"] = note;
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
    if (obj && state != "unknown" && wxGetApp().app_config->get_bool("forca_academy_end_photo"))
        grab_frame_async(t.dev_id, into_path(from_u8(t.folder)), t.name, "end");
}

void tick()
{
    if (s_tracked.empty()) {
        if (s_timer)
            s_timer->Stop();
        return;
    }
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev || !forca_academy_enabled())
        return;
    const long long now     = (long long) std::time(nullptr);
    bool            changed = false;
    for (auto it = s_tracked.begin(); it != s_tracked.end();) {
        Tracked& t = *it;
        if (now - t.sent > 7 * 24 * 3600 || !fs::exists(into_path(from_u8(t.folder)))) {
            it = s_tracked.erase(it); // given up (or the user moved the record)
            changed = true;
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

void forca_academy_on_print_sent(int plate_idx, const std::string& dev_id, const std::string& device_label,
                                 const std::vector<FilamentInfo>* trays)
{
    if (!forca_academy_enabled())
        return;
    try { // never let the journal get in the way of printing
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
                if (!t.color.empty())
                    tray["tray_colour"] = "#" + t.color.substr(0, 6);
                if (!t.filament_id.empty())
                    tray["tray_filament_id"] = t.filament_id;
                record["filaments"][t.id]["printed_from"] = tray;
            }
        DeviceManager* dev = wxGetApp().getDeviceManager();
        MachineObject* obj = dev && !dev_id.empty() ? dev->get_my_machine(dev_id) : nullptr;
        if (const std::string page = update_printer_page(forca_academy_dir(), record, obj); !page.empty())
            record["printer"]["page"] = page;
        const fs::path folder = write(record, plate_png(record["plate"]["number"].get<int>() - 1));
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
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
    }
}

void forca_academy_hold_upload(std::size_t job_id, const std::string& host)
{
    {
        std::lock_guard<std::mutex> lock(s_uploads_mutex);
        s_uploads[job_id] = Upload();
    }
    wxGetApp().CallAfter([job_id, host]() {
        json        record;
        std::string png;
        bool        ok = forca_academy_enabled();
        if (ok) {
            try {
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
        if (!it->second.finished) {
            it->second.record  = std::move(record);
            it->second.png     = std::move(png);
            it->second.snapped = true;
            return;
        }
        const bool finished_ok = it->second.ok; // the upload already ended
        s_uploads.erase(it);
        lock.unlock();
        if (finished_ok)
            write(record, png);
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
            write(upload.record, upload.png);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "Forca Academy: " << e.what();
        }
    }
}

}} // namespace Slic3r::GUI
