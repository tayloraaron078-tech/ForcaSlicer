#ifndef slic3r_ForcaAISafePath_hpp_
#define slic3r_ForcaAISafePath_hpp_

#include <boost/filesystem/path.hpp>

#include <string>

// Forca AI rule R1 -- never save over the user's files (HQ PLAN_forca_ai.md §2). The file-system side of it, kept in
// libslic3r (no GUI) so tests/libslic3r/test_forca_ai_safe_path.cpp can cover it; the GUI wrapper is
// src/slic3r/GUI/ForcaAIFiles.hpp. All names are UTF-8.
namespace Slic3r { namespace ForcaAI {

// " (Claude <date>)", date as YYYY-MM-DD.
std::string suffix(const std::string& date);

// `name` with any trailing " (Claude <date>)[ N]" removed, so an AI copy of an AI copy doesn't stack suffixes.
std::string strip_suffix(const std::string& name);

// A name safe as a file name: drops path separators, control characters and characters Windows forbids, and
// leading/trailing spaces and dots. Never empty ("Untitled").
std::string clean_name(const std::string& name);

// True when nothing (file, folder or link) exists at `path`.
bool path_is_free(const boost::filesystem::path& path);

// A path in `dir` where nothing exists: "<stem> (Claude <date>)<ext>", then " 2", " 3", ... `ext` includes the dot
// and may be compound (".gcode.3mf"). Empty if no free name is found within `max_tries`.
boost::filesystem::path new_path(const boost::filesystem::path& dir, const std::string& stem, const std::string& ext,
                                 const std::string& date, int max_tries = 10000);

// The Advanced control level (HQ PLAN_forca_ai.md §10a) lets the AI overwrite the user's files, but Forca copies each
// one first: to "<backup_root>/<date>/<file name>" ("<stem> 2<ext>", " 3", ... when that is taken). Only a regular file
// is copied. Returns the copy's path, or empty + `err` -- then the file must not be overwritten.
boost::filesystem::path backup_copy(const boost::filesystem::path& file, const boost::filesystem::path& backup_root,
                                    const std::string& date, std::string& err, int max_tries = 10000);

// The ledger of files the AI wrote (a JSON file): the AI may overwrite a file only when the ledger lists it and its
// content is unchanged since the AI's last write -- once the user edits it, it is theirs.
class Ledger
{
public:
    explicit Ledger(boost::filesystem::path ledger_file) : m_file(std::move(ledger_file)) {}

    // True when the AI may write `path`: nothing is there yet, or it is an unchanged file the AI wrote.
    bool may_write(const boost::filesystem::path& path) const;
    // True when the ledger lists `path` (changed since or not).
    bool created(const boost::filesystem::path& path) const;
    // Records that the AI just wrote `path`.
    void record_write(const boost::filesystem::path& path, const std::string& date) const;

    // Ledger key: absolute, normalized, forward slashes, case-folded on Windows.
    static std::string key_for(const boost::filesystem::path& path);
    // FNV-1a of the file's bytes (change detection, not security); empty if unreadable.
    static std::string file_hash(const boost::filesystem::path& path);

private:
    boost::filesystem::path m_file;
};

}} // namespace Slic3r::ForcaAI

#endif // slic3r_ForcaAISafePath_hpp_
