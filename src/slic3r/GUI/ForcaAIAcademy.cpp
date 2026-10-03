// Forca AI tools for Forca Academy, the print journal and knowledge base (HQ PLAN_forca_academy.md §5): an AI reads
// the journal, searches it, records a print's outcome and writes its own notes, lessons and playbooks.
//
// R1 (never save over the user's files) applies here as everywhere: the AI may create new Markdown files, replace
// only files it created that nobody has changed since (the ledger, ForcaAIFiles) or a starter file Forca wrote that
// is still untouched, and APPEND to any other note (appending never removes the user's text). It never deletes, never
// edits AGENTS.md or a print's record.json, and never overwrites an outcome the user recorded.
#include "ForcaAI.hpp"
#include "ForcaAIFiles.hpp"
#include "ForcaAcademy.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <iterator>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

const size_t MAX_TEXT  = 256 * 1024;      // longest text an AI reads in one call
const size_t MAX_IMAGE = 8 * 1024 * 1024; // largest image returned
const size_t MAX_WRITE = 200 * 1024;      // longest note the AI writes in one call

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string ext_of(const fs::path& p) { return lower(p.extension().string()); }

std::string read_file(const fs::path& p, size_t max, bool& truncated)
{
    boost::nowide::ifstream f(p.string(), std::ios::binary);
    std::string             s;
    s.resize(max + 1);
    f.read(&s[0], std::streamsize(max + 1));
    s.resize(size_t(f.gcount()));
    truncated = s.size() > max;
    if (truncated)
        s.resize(max);
    return s;
}

bool write_file(const fs::path& p, const std::string& text, bool append, std::string& err)
{
    boost::nowide::ofstream f(p.string(), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    if (f)
        f << text;
    if (!f) {
        err = "Could not write " + p.string();
        return false;
    }
    return true;
}

std::string today_time()
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

// The Academy folder, or an error result when the user hasn't turned the journal on.
bool academy(fs::path& dir, ForcaAIResult& err)
{
    if (!forca_academy_enabled()) {
        err = ForcaAIResult::error("The user has not turned on Forca Academy (Preferences > Forca Academy, or File > Print "
                                   "Journal). Ask them if they want it; you cannot turn it on.");
        return false;
    }
    dir = forca_academy_dir();
    return true;
}

// A print folder given as "prints/2026/2026-09-27_1403_benchy" (from forca_academy_list_prints).
bool print_folder(const fs::path& dir, const json& args, fs::path& folder, ForcaAIResult& err)
{
    std::string e;
    if (!forca_academy_resolve(dir, args.value("print", std::string()), folder, e)) {
        err = ForcaAIResult::error("print: " + e);
        return false;
    }
    json record;
    if (!forca_academy_read_record(folder, record)) {
        err = ForcaAIResult::error("No recorded print at " + forca_academy_relative(dir, folder) + ". Use forca_academy_list_prints.");
        return false;
    }
    return true;
}

// ---- tools ------------------------------------------------------------------------------------

ForcaAIResult tool_status(const json&)
{
    const fs::path dir = forca_academy_dir();
    json           j   = { { "enabled", forca_academy_enabled() }, { "folder", dir.string() } };
    if (!forca_academy_enabled()) {
        j["note"] = "The user has not turned on Forca Academy (Preferences > Forca Academy, or File > Print Journal). "
                    "Ask them if they want a print journal; you cannot turn it on.";
        return ForcaAIResult::json(j);
    }
    std::string err;
    forca_academy_ensure_skeleton(dir, err);
    const auto prints = forca_academy_list_prints(dir, 100000);
    j["prints"]       = prints.size();
    j["prints_with_outcome"] = std::count_if(prints.begin(), prints.end(), [](const ForcaAcademyPrint& p) { return !p.result.empty(); });
    bool truncated    = false;
    j["index_md"]     = fs::exists(dir / "INDEX.md") ? read_file(dir / "INDEX.md", 16 * 1024, truncated) : std::string();
    j["next"] = "Read AGENTS.md (forca_academy_read) for how this journal is organised and how to record in it. Then "
                "forca_academy_list_prints for recent prints, forca_academy_search to find anything.";
    return ForcaAIResult::json(j);
}

ForcaAIResult tool_list_prints(const json& args)
{
    fs::path      dir;
    ForcaAIResult err;
    if (!academy(dir, err))
        return err;
    const int         limit  = std::clamp(args.value("limit", 20), 1, 500);
    const std::string filter = args.value("result", std::string());
    json              list   = json::array();
    for (const ForcaAcademyPrint& p : forca_academy_list_prints(dir, 100000)) {
        if (!filter.empty() && (filter == "none" ? !p.result.empty() : p.result != filter))
            continue;
        list.push_back({ { "print", forca_academy_relative(dir, p.folder) },
                         { "time", p.time },
                         { "title", p.title },
                         { "printer", p.printer },
                         { "result", p.result.empty() ? json(nullptr) : json(p.result) },
                         { "printer_reported", p.ended.empty() ? json(nullptr) : json(p.ended) } });
        if (int(list.size()) >= limit)
            break;
    }
    return ForcaAIResult::json({ { "prints", list },
                                 { "hint", "forca_academy_read {\"path\": \"<print>/record.json\"} for the details; "
                                           "<print>/notes.md, thumbnail.png and photos/ are next to it." } });
}

ForcaAIResult tool_read(const json& args)
{
    fs::path      dir;
    ForcaAIResult err;
    if (!academy(dir, err))
        return err;
    const std::string rel = args.value("path", std::string("."));
    fs::path          p   = dir;
    std::string       e;
    if (rel != "." && !rel.empty() && !forca_academy_resolve(dir, rel, p, e))
        return ForcaAIResult::error(e);
    boost::system::error_code ec;
    if (fs::is_directory(p, ec)) {
        json entries = json::array();
        for (fs::directory_iterator it(p, ec), end; !ec && it != end; it.increment(ec)) {
            const bool is_dir = fs::is_directory(it->path(), ec);
            json       entry  = { { "name", it->path().filename().string() }, { "type", is_dir ? "folder" : "file" } };
            if (!is_dir)
                entry["bytes"] = fs::file_size(it->path(), ec);
            entries.push_back(entry);
        }
        return ForcaAIResult::json({ { "folder", forca_academy_relative(dir, p) }, { "entries", entries } });
    }
    if (!fs::is_regular_file(p, ec))
        return ForcaAIResult::error("Nothing at " + forca_academy_relative(dir, p) + ".");
    const std::string ext  = ext_of(p);
    const uintmax_t   size = fs::file_size(p, ec);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg") {
        if (size > MAX_IMAGE)
            return ForcaAIResult::error("That image is " + std::to_string(size / (1024 * 1024)) + " MB; the limit is 8 MB.");
        bool              truncated = false;
        const std::string bytes     = read_file(p, MAX_IMAGE, truncated);
        ForcaAIResult     r         = ForcaAIResult::text(forca_academy_relative(dir, p));
        r.add_image(bytes, ext == ".png" ? "image/png" : "image/jpeg");
        return r;
    }
    if (ext == ".md" || ext == ".json" || ext == ".txt" || ext == ".csv") {
        bool        truncated = false;
        std::string text      = read_file(p, MAX_TEXT, truncated);
        if (truncated)
            text += "\n\n[... cut at 256 KB]";
        return ForcaAIResult::text(text);
    }
    return ForcaAIResult::text(forca_academy_relative(dir, p) + " is a " + (ext.empty() ? std::string("binary") : ext) +
                               " file of " + std::to_string(size) + " bytes; only text (.md, .json, .txt, .csv) and "
                               "images (.png, .jpg) can be read here.");
}

ForcaAIResult tool_search(const json& args)
{
    fs::path      dir;
    ForcaAIResult err;
    if (!academy(dir, err))
        return err;
    const std::string query = lower(args.value("query", std::string()));
    if (query.empty())
        return ForcaAIResult::error("Give a query (case-insensitive text).");
    const int limit   = std::clamp(args.value("limit", 30), 1, 200);
    json      matches = json::array();
    size_t    files   = 0;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end && int(matches.size()) < limit; it.increment(ec)) {
        const std::string ext = ext_of(it->path());
        if (!fs::is_regular_file(it->path(), ec) || (ext != ".md" && ext != ".json" && ext != ".txt"))
            continue;
        if (fs::file_size(it->path(), ec) > 1024 * 1024 || ++files > 5000)
            continue;
        bool              truncated = false;
        const std::string text      = read_file(it->path(), 1024 * 1024, truncated);
        size_t            line_no   = 1, start = 0;
        while (start <= text.size() && int(matches.size()) < limit) {
            size_t      end_line = text.find('\n', start);
            std::string line     = text.substr(start, end_line == std::string::npos ? std::string::npos : end_line - start);
            if (lower(line).find(query) != std::string::npos) {
                if (line.size() > 240)
                    line = line.substr(0, 240) + "...";
                matches.push_back({ { "path", forca_academy_relative(dir, it->path()) }, { "line", line_no }, { "text", line } });
            }
            if (end_line == std::string::npos)
                break;
            start = end_line + 1;
            ++line_no;
        }
    }
    return ForcaAIResult::json({ { "query", args.value("query", std::string()) }, { "matches", matches } });
}

ForcaAIResult tool_record_outcome(const json& args)
{
    fs::path      dir, folder;
    ForcaAIResult err;
    if (!academy(dir, err) || !print_folder(dir, args, folder, err))
        return err;
    const std::string result = args.value("result", std::string());
    if (result != "success" && result != "partial" && result != "failed")
        return ForcaAIResult::error("result must be success, partial or failed.");
    json record;
    forca_academy_read_record(folder, record);
    if (record.contains("outcome") && record["outcome"].is_object() && record["outcome"].value("by", std::string()) != "ai" &&
        !record["outcome"].value("result", std::string()).empty())
        return ForcaAIResult::error("The user already recorded this print's outcome (" + record["outcome"].value("result", std::string()) +
                                    "); you can't change it. Add what you found to the print's notes with forca_academy_write_note "
                                    "(mode append).");
    json tags = json::array();
    if (args.contains("tags") && args["tags"].is_array())
        for (const json& t : args["tags"])
            if (t.is_string())
                tags.push_back(t);
    const json  outcome = { { "result", result }, { "tags", tags }, { "note", args.value("note", std::string()) },
                            { "recorded", today_time() }, { "by", "ai" } };
    std::string e;
    if (!forca_academy_set_outcome(folder, outcome, e))
        return ForcaAIResult::error(e);
    return ForcaAIResult::text("Recorded " + result + " for " + forca_academy_relative(dir, folder) + ". The user can change it in "
                               "File > Print Journal.");
}

ForcaAIResult tool_write_note(const json& args)
{
    fs::path      dir;
    ForcaAIResult err;
    if (!academy(dir, err))
        return err;
    fs::path    p;
    std::string e;
    if (!forca_academy_resolve(dir, args.value("path", std::string()), p, e))
        return ForcaAIResult::error(e);
    const std::string rel = forca_academy_relative(dir, p);
    if (ext_of(p) != ".md")
        return ForcaAIResult::error("Notes are Markdown files: the path must end in .md.");
    if (lower(rel) == "agents.md")
        return ForcaAIResult::error("AGENTS.md is the user's instructions for AIs; you can't change it.");
    const std::string mode    = args.value("mode", std::string("create"));
    std::string       content = args.value("content", std::string());
    if (content.empty())
        return ForcaAIResult::error("content is empty.");
    if (content.size() > MAX_WRITE)
        return ForcaAIResult::error("content is over 200 KB; split it.");
    if (content.back() != '\n')
        content += '\n';

    boost::system::error_code ec;
    const bool                exists = fs::exists(p, ec);
    if (exists && !fs::is_regular_file(p, ec))
        return ForcaAIResult::error(rel + " is a folder.");
    if (mode == "create" && exists)
        return ForcaAIResult::error(rel + " already exists. Use mode append to add to it, or replace if you wrote it.");
    if (mode == "replace" && exists && !forca_ai_may_write(p) && !forca_academy_is_untouched_starter(p))
        return ForcaAIResult::error(rel + " was written or changed by the user, so you can't replace it (Forca's rule: "
                                          "never save over the user's files). Use mode append to add to it.");
    if (mode != "create" && mode != "append" && mode != "replace")
        return ForcaAIResult::error("mode must be create, append or replace.");

    fs::create_directories(p.parent_path(), ec);
    // An appended note keeps the AI's ownership only if the file was the AI's and unchanged before.
    const bool ai_owned_before = exists && forca_ai_created(p) && forca_ai_may_write(p);
    const bool append          = mode == "append" && exists;
    if (!write_file(p, append ? "\n" + content : content, append, e))
        return ForcaAIResult::error(e);
    if (!append || ai_owned_before)
        forca_ai_record_write(p);
    return ForcaAIResult::text((append ? "Added to " : exists ? "Replaced " : "Created ") + rel + ".");
}

} // namespace

void register_forca_ai_academy_tools(ForcaAI& ai)
{
    ai.register_tool({ "forca_academy_status", "Forca Academy",
        "Forca Academy is the user's print journal and 3D-printing knowledge base: a folder of Markdown and JSON that "
        "Forca fills with a record of every print it sends, plus outcomes, photos, notes, lessons and playbooks. This "
        "tells you whether the user turned it on, where it is, how many prints it holds, and its INDEX.md. Read-only.",
        { { "type", "object" }, { "properties", json::object() } },
        tool_status });
    ai.register_tool({ "forca_academy_list_prints", "Recorded prints",
        "The recorded prints, newest first: path (for the other academy tools), time, title, printer, result "
        "(the user's or AI's verdict: success / partial / failed / null = not recorded) and printer_reported (how the "
        "printer said it ended, Bambu only: finished / failed / stopped / unknown / null = not known). Read-only.",
        { { "type", "object" },
          { "properties", { { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } },
                            { "result", { { "type", "string" }, { "enum", { "success", "partial", "failed", "none" } },
                                          { "description", "Only prints with this result (none = not recorded yet)." } } } } } },
        tool_list_prints });
    ai.register_tool({ "forca_academy_read", "Read from Forca Academy",
        "Read a file in the Forca Academy folder (Markdown, JSON, text; PNG/JPEG images come back as images, e.g. a "
        "print's thumbnail.png or photos/*.jpg), or list a folder. Paths are relative to the Academy folder; '.' lists "
        "the top. Start with AGENTS.md. Read-only.",
        { { "type", "object" },
          { "properties", { { "path", { { "type", "string" }, { "description", "e.g. AGENTS.md, prints/2026/<print>/record.json, playbooks" } } } } },
          { "required", { "path" } } },
        tool_read });
    ai.register_tool({ "forca_academy_search", "Search Forca Academy",
        "Case-insensitive text search over the Academy's Markdown, JSON and text files. Returns matching lines with "
        "their file and line number. Read-only.",
        { { "type", "object" },
          { "properties", { { "query", { { "type", "string" } } },
                            { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } } } } },
          { "required", { "query" } } },
        tool_search });
    ai.register_tool({ "forca_academy_record_outcome", "Record a print's outcome",
        "Record how a print turned out, when the user told you or you saw it (camera, photos): result, quick tags "
        "(stringing, warping, layer_shift, spaghetti, bed_adhesion, supports, under_extrusion, over_extrusion, ringing, "
        "layer_separation, or your own) and a short note. You can't change an outcome the user recorded; add to the "
        "print's notes.md instead.",
        { { "type", "object" },
          { "properties", { { "print", { { "type", "string" }, { "description", "The print's path from forca_academy_list_prints." } } },
                            { "result", { { "type", "string" }, { "enum", { "success", "partial", "failed" } } } },
                            { "tags", { { "type", "array" }, { "items", { { "type", "string" } } } } },
                            { "note", { { "type", "string" } } } } },
          { "required", { "print", "result" } } },
        tool_record_outcome });
    ai.register_tool({ "forca_academy_write_note", "Write a note in Forca Academy",
        "Write Markdown in the Academy: a lesson, a playbook (playbooks/<symptom>.md), a printer or material page, "
        "an experiment, or notes on a print (<print>/notes.md). mode create: a new file. mode append: add to the end of "
        "any note (the user's text is kept). mode replace: rewrite a file you created that nobody changed since, or an "
        "untouched starter file (INDEX.md, lessons.md). You can't delete files, edit AGENTS.md or a record.json. Follow "
        "AGENTS.md: link evidence prints, give lessons a confidence, keep INDEX.md short.",
        { { "type", "object" },
          { "properties", { { "path", { { "type", "string" }, { "description", "Relative .md path, e.g. playbooks/stringing.md" } } },
                            { "content", { { "type", "string" } } },
                            { "mode", { { "type", "string" }, { "enum", { "create", "append", "replace" } } } } } },
          { "required", { "path", "content" } } },
        tool_write_note });
}

}} // namespace Slic3r::GUI
