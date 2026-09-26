#ifndef slic3r_ForcaAIFiles_hpp_
#define slic3r_ForcaAIFiles_hpp_

#include <boost/filesystem/path.hpp>

#include <string>

namespace Slic3r { namespace GUI {

// Forca AI rule R1 -- never save over the user's files (HQ PLAN_forca_ai.md §2).
//
// Every file an AI tool writes (projects, G-code, presets) gets its path from here. A new file is always given a
// " (Claude YYYY-MM-DD)" name that does not exist yet (" 2", " 3", ... when taken). The AI may write to an existing
// path only when the ledger says the AI created that file and it is unchanged since the AI last wrote it; once the
// user edits it, it is theirs. The ledger lives in the data folder (forca/ai/ledger.json). The AI cannot delete files.

// " (Claude 2026-09-23)" for today.
std::string forca_ai_suffix();

// `name` with any trailing " (Claude <date>)[ N]" removed, so an AI copy of an AI copy doesn't stack suffixes.
std::string forca_ai_strip_suffix(const std::string& name);

// Makes a user-supplied name safe as a file name (drops path separators and characters Windows forbids).
std::string forca_ai_clean_name(const std::string& name);

// A path in `dir` that does not exist: "<stem> (Claude <date>)<ext>", then " 2", " 3", ...
// `ext` includes the dot and may be compound (".gcode.3mf"). Empty if no free name can be found (unreadable folder).
boost::filesystem::path forca_ai_new_path(const boost::filesystem::path& dir, const std::string& stem, const std::string& ext);

// True when the AI may write `path`: it does not exist, or it is an unchanged file the AI created.
bool forca_ai_may_write(const boost::filesystem::path& path);

// True when the ledger lists `path` as the AI's (whether or not it was changed since).
bool forca_ai_created(const boost::filesystem::path& path);

// Records that the AI just wrote `path` (call after every successful AI write).
void forca_ai_record_write(const boost::filesystem::path& path);

// Where AI saves go when the AI does not name a folder: <Documents>/Forca AI (created on demand).
boost::filesystem::path forca_ai_default_dir();

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaAIFiles_hpp_
