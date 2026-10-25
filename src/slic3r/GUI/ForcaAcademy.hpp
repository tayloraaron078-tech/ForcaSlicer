#pragma once

#include <nlohmann/json.hpp>
#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
struct FilamentInfo; // libslic3r/ProjectTask.hpp
namespace GUI {

// Forca Academy -- a print journal and knowledge base in a plain folder of Markdown + JSON (HQ
// PLAN_forca_academy.md), readable by any AI or person without special tools. Off until the user turns it on in
// Preferences. When on, every print Forca SENDS gets a record: a Bambu send that succeeded, or a network print-host
// upload that finished with "print" as its post-action. Nothing is ever uploaded; the folder is the user's.
//
//   <folder>/AGENTS.md, INDEX.md, lessons.md            written once, never overwritten (the user owns them)
//   <folder>/printers/ materials/ playbooks/ experiments/
//   <folder>/prints/YYYY/YYYY-MM-DD_HHMM_<slug>/        record.json, notes.md, thumbnail.png, photos/

bool                    forca_academy_enabled();
boost::filesystem::path forca_academy_default_dir(); // Documents/Forca Academy
boost::filesystem::path forca_academy_dir();         // the folder for the Preferences choice, else the default

// ---- file side (no GUI; unit-tested) ----
// The Academy folder for the folder the user chose: a "Forca Academy" folder inside it, so Forca never fills a folder
// that holds other things (and a reset never moves one). A chosen folder already named so, or one that already holds a
// journal at its top (chosen before this rule), is the Academy folder itself.
boost::filesystem::path forca_academy_folder_in(const boost::filesystem::path& chosen);
// The names Forca creates at the top of an Academy folder.
const std::vector<std::string>& forca_academy_own_entries();
// A folder-name slug: keeps letters/digits (any script), turns everything else into '-', at most 48 characters.
std::string forca_academy_slug(const std::string& name);
// Creates the folder layout and starter files that are missing. Existing files are never touched.
bool forca_academy_ensure_skeleton(const boost::filesystem::path& dir, std::string& err);
// Writes one print's folder: record.json (the record as given; its "settings", if any, goes to settings.json
// instead), notes.md, thumbnail.png (if any) and photos/.
// record["time"] ("YYYY-MM-DDTHH:MM:SS", local) and record["title"] name the folder; a clash gets "-2", "-3", ...
// Returns the new folder, or an empty path with `err` set.
boost::filesystem::path forca_academy_write_record(const boost::filesystem::path& dir, const nlohmann::json& record,
                                                   const std::string& thumbnail_png, std::string& err);

// One recorded print, for the Print Journal's list.
struct ForcaAcademyPrint
{
    boost::filesystem::path folder;
    std::string             time;    // "YYYY-MM-DDTHH:MM:SS"
    std::string             title;
    std::string             printer; // device name, else physical printer, else printer preset
    std::string             result;  // "success" | "partial" | "failed" | "" (not recorded)
    std::string             ended;   // how the printer said it ended: "finished" | "failed" | "stopped" | "unknown" | ""
    std::string              process;   // process preset
    std::vector<std::string> filaments; // filament presets the print used (slots with no recorded use are skipped)
    std::vector<std::string> tags;      // outcome tags ("stringing", ...)
};

// How earlier prints went with one printer and filament preset (the send dialogs' "Before you print" line and the
// journal's Insights). Only prints with an outcome count toward success / partial / failed.
struct ForcaAcademyHistory
{
    int                                      success      = 0;
    int                                      partial      = 0;
    int                                      failed       = 0;
    int                                      not_recorded = 0;
    std::vector<std::pair<std::string, int>> tags;    // on prints with an outcome, most common first
    std::map<std::string, int>               tags_on_success; // how many of those prints were otherwise successful
    std::vector<boost::filesystem::path>     folders; // the matching prints, newest first
    int recorded() const { return success + partial + failed; }
};
// `prints` as forca_academy_list_prints returns them (newest first). `printer` is matched against
// ForcaAcademyPrint::printer, `filament` against its filament presets; an empty `printer` matches any printer.
ForcaAcademyHistory forca_academy_history(const std::vector<ForcaAcademyPrint>& prints, const std::string& printer,
                                          const std::string& filament);

// "Before you print" suggests Thick external bridges when any of these histories has a print tagged "bridging" and
// the process has it off (it fixed sagging and torn bridges in Forca's tests).
bool forca_academy_suggest_thick_bridges(const std::vector<ForcaAcademyHistory>& histories, bool thick_bridges);

// The Print Journal's Insights: the prints grouped by printer, filament preset (a print with several filaments counts
// once under each) or process preset, most prints first (then by name). Prints with an empty group value are skipped.
enum class ForcaAcademyGroup { Printer, Filament, Process };
struct ForcaAcademyInsight
{
    std::string         name;
    ForcaAcademyHistory history;
    std::string         last_failure; // time of the newest partial or failed print ("" if none)
};
std::vector<ForcaAcademyInsight> forca_academy_insights(const std::vector<ForcaAcademyPrint>& prints, ForcaAcademyGroup by);

// ---- part 2: compare, experiments, patterns, lessons, INDEX facts, playbooks (no GUI; unit-tested) ----

// True for a setting the Academy never stores or shows: secrets (passwords, API keys, access codes, tokens -- the Forca
// AI rule), the printer's connection (address, web UI, every printhost_* value) and serial numbers.
bool forca_academy_private_setting(const std::string& key);
// Reads <folder>/settings.json: every setting the print used (private settings left out, even if an older file has them) ({"process": {key: value}, "printer": {...},
// "filament": {...}}, values as the config serializes them). Prints recorded before it existed have none (false).
bool forca_academy_read_settings(const boost::filesystem::path& folder, nlohmann::json& settings);

// One row of "Compare two prints". Section "print" holds the record's facts (keys: printer, process, filaments,
// print_time, filament_g, result, tags), always listed; "process" / "printer" / "filament" hold config keys, listed
// only when they differ. a / b are empty when a print has no value (record-only compare: the preset's value).
struct ForcaAcademyDiff
{
    std::string section, key, a, b;
    bool        differs = false;
};
struct ForcaAcademyComparison
{
    bool                          full = false; // both prints had settings.json: every differing setting is listed
    std::vector<ForcaAcademyDiff> rows;
};
// Without both settings.json files only the settings changed from the presets (record "unsaved_changes") are compared.
ForcaAcademyComparison forca_academy_compare(const nlohmann::json& record_a, const nlohmann::json& settings_a,
                                             const nlohmann::json& record_b, const nlohmann::json& settings_b);

// experiments/<date>_<slug(title)>.md for two compared prints (`differences`: one readable line each, already
// labelled), with a Result section for the user. Never overwrites ("-2", ...). `date` = "YYYY-MM-DD".
boost::filesystem::path forca_academy_write_experiment(const boost::filesystem::path& dir, const std::string& title,
                                                       const std::string& date, const ForcaAcademyPrint& a,
                                                       const ForcaAcademyPrint& b, const std::vector<std::string>& differences,
                                                       std::string& err);
struct ForcaAcademyExperiment
{
    boost::filesystem::path file;
    std::string             title, date;
    bool                    has_result = false; // the user wrote something under "## Result"
    std::string             state; // a planned experiment's: "planned" / "printed" / "judged"; empty for one from Compare
};
// Single-file experiments and planned ones (a folder with experiment.md, ForcaExperiment.hpp).
std::vector<ForcaAcademyExperiment> forca_academy_list_experiments(const boost::filesystem::path& dir); // newest first
// What an experiment page says under "## Result" until the user writes there.
extern const char* const FORCA_EXPERIMENT_RESULT_UNWRITTEN;

// "Possible pattern": the same tag on at least `count` of the last `window` prints with an outcome for one printer +
// filament preset. Strongest first. `id` names it for dismissing.
struct ForcaAcademyPattern
{
    std::string                          printer, filament, tag, id;
    int                                  count = 0, window = 0; // window = prints looked at (may be < the setting)
    int                                  on_success = 0;        // of the `count` prints, the ones marked successful
    std::vector<boost::filesystem::path> folders;               // the prints with the tag, newest first
};
std::vector<ForcaAcademyPattern> forca_academy_patterns(const std::vector<ForcaAcademyPrint>& prints, int count, int window);
// Patterns the user dismissed or turned into a lesson (<dir>/forca/insights.json, Forca-owned).
std::vector<std::string> forca_academy_dismissed(const boost::filesystem::path& dir);
bool forca_academy_dismiss(const boost::filesystem::path& dir, const std::string& pattern_id, std::string& err);
// A lessons.md entry for a pattern, for the user to edit before saving. `confidence`: low / medium / high.
std::string forca_academy_lesson_draft(const boost::filesystem::path& dir, const ForcaAcademyPattern& p,
                                       const std::string& confidence, const std::string& date);
// Appends an entry to lessons.md (created if missing), after a blank line.
bool forca_academy_append_lesson(const boost::filesystem::path& dir, const std::string& entry, std::string& err);

// INDEX.md's Forca-owned <!-- forca:facts --> block: print counts, printers, filaments, open patterns and the latest
// lessons. The first time, the block goes under the page title (once; remembered in forca/insights.json); after
// that it is only replaced between its markers, so a page whose markers the user removed is left alone.
std::string forca_academy_index_facts(const boost::filesystem::path& dir, const std::vector<ForcaAcademyPrint>& prints,
                                      const std::vector<ForcaAcademyPattern>& open_patterns);
bool forca_academy_write_index_facts(const boost::filesystem::path& dir, const std::string& facts_md, std::string& err);

// The journal's outcome tags, as record.json stores them.
const std::vector<std::string>& forca_academy_tag_keys();
// playbooks/<tag>.md for each tag: a generic starter (causes, checks, Forca settings and Calibration Wizard tests),
// written once per tag (remembered in forca/insights.json) and never overwritten or recreated after a delete.
bool forca_academy_write_starter_playbooks(const boost::filesystem::path& dir, std::string& err);
// The recorded prints under <dir>/prints, newest first, at most `max`. Folders without a readable record.json are
// skipped.
// A path an AI gave, relative to the Academy folder `dir` ("prints/2026/...", "lessons.md"), as a path inside it.
// Rejects empty, absolute and drive paths, and any ".." part. Does not require the path to exist.
bool forca_academy_resolve(const boost::filesystem::path& dir, const std::string& rel, boost::filesystem::path& out,
                           std::string& err);
// `path` relative to `dir`, with forward slashes (for showing an AI where a file is).
std::string forca_academy_relative(const boost::filesystem::path& dir, const boost::filesystem::path& path);
// True when `file` is one of the starter files (INDEX.md, lessons.md, a starter playbook) and still exactly as Forca
// wrote it. INDEX.md's Forca facts block doesn't count as a change.
bool forca_academy_is_untouched_starter(const boost::filesystem::path& file);
// Appends "- <line>" to a page's calibration history (materials/ or printers/ pages). A missing page is created with
// "# <title>" and a "## Calibration history" heading; an existing page is only appended to (the user's text stays).
bool forca_academy_append_history(const boost::filesystem::path& page, const std::string& title, const std::string& line,
                                  std::string& err);
// The name Bambu's printer screen and Forca's Device tab give an AMS tray: "A4" (first AMS, slot 4), "B1", "HT-A"
// (AMS HT), "Ext" / "Ext 2" (external spool holders). `ams_id` / `slot_id` are the printer's ids (0-based).
std::string forca_academy_tray_label(int ams_id, int slot_id);
// A tray colour as "#RRGGBB". Printers report "RRGGBBAA" or "#RRGGBBAA"; empty when there is no colour.
std::string forca_academy_tray_colour(const std::string& printer_colour);
// How the printer said a print ended: record.json "print_end" = `end`, mirrored as Forca's <!-- forca:print_end -->
// block at the end of notes.md (replaced when already there).
bool forca_academy_set_print_end(const boost::filesystem::path& folder, const nlohmann::json& end, std::string& err);
// A printer page's Forca-owned facts: the <!-- forca:facts --> block is replaced; a missing page is created as
// "# <title>", the block and "## Notes"; a page whose markers the user removed is left alone.
bool forca_academy_write_printer_facts(const boost::filesystem::path& page, const std::string& title, const std::string& facts_md,
                                       std::string& err);
// Reads <folder>/record.json (false if missing or not a JSON object).
bool forca_academy_read_record(const boost::filesystem::path& folder, nlohmann::json& record);
std::vector<ForcaAcademyPrint> forca_academy_list_prints(const boost::filesystem::path& dir, size_t max = 500);
// Sets record.json "outcome" ({result, tags, note, recorded}) and mirrors it into notes.md: Forca's block between
// "<!-- forca:outcome -->" markers is replaced, or the untouched "Not recorded yet." placeholder; if the user has
// rewritten that section, notes.md is left alone.
bool forca_academy_set_outcome(const boost::filesystem::path& folder, const nlohmann::json& outcome, std::string& err);
// Copies photos into <folder>/photos (never overwriting: "name-2.jpg", ...). Returns the new files.
std::vector<boost::filesystem::path> forca_academy_add_photos(const boost::filesystem::path& folder,
                                                              const std::vector<boost::filesystem::path>& files,
                                                              std::string& err);

// ---- GUI side (GUI thread) ----
// Once at startup: settings defaults, and resumes following prints sent before a restart (to record how they ended).
void forca_academy_start();
// The pattern rule from Preferences: forca_academy_pattern_count (default 3) of the last
// forca_academy_pattern_window (default 10) prints with an outcome.
std::pair<int, int> forca_academy_pattern_rule();
// The journal's patterns by that rule, without the ones the user dismissed or turned into a lesson.
std::vector<ForcaAcademyPattern> forca_academy_open_patterns(const std::vector<ForcaAcademyPrint>& prints);
// Refreshes INDEX.md's facts block from the journal (no-op when off): after a print is recorded, an outcome changes
// or a lesson is saved.
void forca_academy_refresh_index();
// Tags the next print record with where the print came from ("calibration_wizard: temperature",
// "forca_ai: print request"); otherwise it is "user". A tag older than an hour is ignored.
void forca_academy_mark_source(const std::string& source);
// Calibration Wizard results -> the Academy (no-op when off): a filament result to materials/<filament preset>.md,
// a printer result to printers/<printer preset>.md, one dated line each.
void forca_academy_log_calibration(bool printer_page, const std::string& page, const std::string& line);
// Bambu (and printer-agent) sends: called when the send succeeded. `plate_idx` is the plate that was sent; `trays` is
// the print dialog's filament -> AMS tray mapping (may be null or empty), recorded per filament slot.
// The print is then followed until the printer reports it ended (record.json "print_end", an optional picture at the
// end and every N minutes), and the printer's page (printers/) gets its facts refreshed.
void forca_academy_on_print_sent(int plate_idx, const std::string& dev_id, const std::string& device_label,
                                 const std::vector<FilamentInfo>* trays);
// Multi-machine sends (TaskManager): hold a snapshot of the plate when a printer's job is queued (GUI thread), write
// the record when that printer's send succeeded, drop it when it failed or was cancelled. task_done may be called on
// any thread. `plate_idx` is 0-based.
void forca_academy_hold_task(int task_id, int plate_idx, const std::string& dev_id, const std::string& device_label);
void forca_academy_task_done(int task_id, bool ok);
// Network print hosts: hold a snapshot of the plate when a "print" upload is queued, write it when that upload
// completes, drop it if it fails or is cancelled. `job_id` is the upload queue's row. hold_upload may be called on
// any thread (the upload is queued on the background slicing thread); upload_done on the GUI thread.
void forca_academy_hold_upload(std::size_t job_id, const std::string& host, const std::string& file);
void forca_academy_upload_done(std::size_t job_id, bool ok);
// Print Journal "Add a print": records the current plate now (for prints Forca did not send, e.g. from an SD card).
// Returns the new folder, or an empty path with `err` set.
boost::filesystem::path forca_academy_record_current_plate(std::string& err);
// Saves a camera frame (JPEG) into a recorded print's camera/ folder (`print_rel` as forca_academy_list_prints gives
// it), named by time and printer, never overwriting. Returns the file, or an empty path with `err` set.
boost::filesystem::path forca_academy_save_camera_frame(const std::string& print_rel, const std::string& jpeg,
                                                        const std::string& printer_name, std::string& err);

}} // namespace Slic3r::GUI
