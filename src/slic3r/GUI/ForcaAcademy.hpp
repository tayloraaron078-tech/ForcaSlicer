#pragma once

#include <nlohmann/json.hpp>
#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <string>
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
boost::filesystem::path forca_academy_dir();         // the Preferences folder, else the default

// ---- file side (no GUI; unit-tested) ----
// A folder-name slug: keeps letters/digits (any script), turns everything else into '-', at most 48 characters.
std::string forca_academy_slug(const std::string& name);
// Creates the folder layout and starter files that are missing. Existing files are never touched.
bool forca_academy_ensure_skeleton(const boost::filesystem::path& dir, std::string& err);
// Writes one print's folder: record.json (the record as given), notes.md, thumbnail.png (if any) and photos/.
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
};
// The recorded prints under <dir>/prints, newest first, at most `max`. Folders without a readable record.json are
// skipped.
// A path an AI gave, relative to the Academy folder `dir` ("prints/2026/...", "lessons.md"), as a path inside it.
// Rejects empty, absolute and drive paths, and any ".." part. Does not require the path to exist.
bool forca_academy_resolve(const boost::filesystem::path& dir, const std::string& rel, boost::filesystem::path& out,
                           std::string& err);
// `path` relative to `dir`, with forward slashes (for showing an AI where a file is).
std::string forca_academy_relative(const boost::filesystem::path& dir, const boost::filesystem::path& path);
// True when `file` is one of the starter files (INDEX.md, lessons.md) and still exactly as Forca wrote it.
bool forca_academy_is_untouched_starter(const boost::filesystem::path& file);
// Appends "- <line>" to a page's calibration history (materials/ or printers/ pages). A missing page is created with
// "# <title>" and a "## Calibration history" heading; an existing page is only appended to (the user's text stays).
bool forca_academy_append_history(const boost::filesystem::path& page, const std::string& title, const std::string& line,
                                  std::string& err);
// The name Bambu's printer screen and Forca's Device tab give an AMS tray: "A4" (first AMS, slot 4), "B1", "HT-A"
// (AMS HT), "Ext" / "Ext 2" (external spool holders). `ams_id` / `slot_id` are the printer's ids (0-based).
std::string forca_academy_tray_label(int ams_id, int slot_id);
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
// Network print hosts: hold a snapshot of the plate when a "print" upload is queued, write it when that upload
// completes, drop it if it fails or is cancelled. `job_id` is the upload queue's row. hold_upload may be called on
// any thread (the upload is queued on the background slicing thread); upload_done on the GUI thread.
void forca_academy_hold_upload(std::size_t job_id, const std::string& host);
void forca_academy_upload_done(std::size_t job_id, bool ok);
// Print Journal "Add a print": records the current plate now (for prints Forca did not send, e.g. from an SD card).
// Returns the new folder, or an empty path with `err` set.
boost::filesystem::path forca_academy_record_current_plate(std::string& err);
// Saves a camera frame (JPEG) into a recorded print's camera/ folder (`print_rel` as forca_academy_list_prints gives
// it), named by time and printer, never overwriting. Returns the file, or an empty path with `err` set.
boost::filesystem::path forca_academy_save_camera_frame(const std::string& print_rel, const std::string& jpeg,
                                                        const std::string& printer_name, std::string& err);

}} // namespace Slic3r::GUI
