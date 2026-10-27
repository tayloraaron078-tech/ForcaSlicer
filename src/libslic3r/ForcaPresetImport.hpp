#pragma once

#include "Preset.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class PresetBundle;

// Forca: what importing one preset file (an OrcaSlicer, Bambu Studio or Forca user preset .json) would do, worked out
// without changing anything, for the import review (docs/HLSD/preset-import.md). The file format is the same for all
// three slicers: a name, "inherits" (the parent preset), "version", and only the settings that differ from the parent.
struct PresetImportItem
{
    enum class Source { OrcaOrForca, BambuStudio };
    enum class Parent {
        Found,   // the parent the file names, as Forca knows it
        Matched, // Forca has it under another name (renamed, or a "Generic ..." filament of the filament library)
        Missing, // Forca doesn't have it: best_parent is a suggestion, empty when there is none
        None,    // the file names no parent: a full preset of its own
    };

    std::string  file;     // the file analysed
    bool         readable = false;
    Preset::Type type     = Preset::TYPE_INVALID;
    std::string  name;
    Source       source = Source::OrcaOrForca;

    Parent      parent = Parent::None;
    std::string inherits;    // the parent's name as the file writes it
    std::string best_parent; // the parent the import would use (Found / Matched) or suggests (Missing)
    std::string filament_type; // a filament's type: the file's, or found in its name when it relies on its parent

    std::vector<std::string> printers;         // the file's compatible_printers (empty: those of its parent)
    std::vector<std::string> missing_printers; // of those, the ones that are not installed in Forca

    // Settings of the file, by what the import does with them. Values as the file writes them (vectors joined by ",").
    std::vector<std::pair<std::string, std::string>> unsupported; // key, value: Forca has no such setting
    std::vector<std::pair<std::string, std::string>> renamed;     // the file's key, Forca's key for the same setting
    struct Override
    {
        std::string key;
        std::string value;        // the file's value
        std::string parent_value; // the parent's value; empty without a parent
    };
    std::vector<Override> overrides; // settings the preset changes compared with its parent (the inheritance view)

    bool clashes_with_user_preset   = false; // a user preset already has this name
    bool clashes_with_system_preset = false; // a system preset has this name: it can't be imported under it
};

// Analyses one preset .json file against the presets loaded in bundle. Never throws for a bad file: readable is false.
PresetImportItem forca_analyse_preset_file(PresetBundle &bundle, const std::string &file);

// What the user chose in the import review for one analysed file.
struct PresetImportChoice
{
    std::string              name;     // the name to import under (the file's, or one the user typed)
    std::string              parent;   // the parent to use; empty keeps the file's own (or none)
    std::vector<std::string> printers; // replaces the file's compatible_printers when not empty
    bool                     replace_existing = false; // a user preset of that name is replaced (else it's kept)
    std::string              notes_heading; // the (translated) line above the unsupported settings in the notes
};

// Imports one analysed file with the user's choices, through PresetBundle::import_json_presets() (so it loads as at
// the next start): a copy of the file gets the chosen name, parent and printers, and, per DECISIONS 2026-10-25 (D4),
// the settings Forca doesn't have are added as text to the preset's notes (filament_notes for a filament), so nothing
// is lost silently. Returns false when nothing was imported; PresetBundle::import_failures() then says why.
bool forca_import_preset(PresetBundle &bundle, const PresetImportItem &item, const PresetImportChoice &choice);

// name when no preset of the collection has it, else pattern applied to it (a translated "%1% (imported)"), then
// with " 2", " 3", ... added until the name is free.
std::string forca_free_preset_name(const PresetCollection &collection, const std::string &name, const std::string &pattern);

// The system filament presets a filament of this type could be based on: of the type (any type when empty) and
// compatible with one of the printers (any printer when empty), sorted by name.
std::vector<std::string> forca_filament_parent_candidates(PresetBundle &bundle, const std::string &type,
                                                          const std::vector<std::string> &printers);

// Whether file is a Bambu Studio preset bundle: .bbsflmt (filament presets) or .bbscfg (a printer with its process
// and filament presets). Both are zip files with a bundle_structure.json and one .json per preset.
bool forca_is_bambu_bundle(const std::string &file);

// Writes the presets of a Bambu Studio bundle as .json files into dir/printer, dir/process and dir/filament (taken
// over as ordinary user presets, not as an OrcaSlicer local bundle: DECISIONS 2026-10-25, D3). Entries outside those
// three folders and anything that isn't a .json are skipped. Returns the files written; empty when the zip can't be read.
std::vector<std::string> forca_extract_bambu_bundle(const std::string &bundle, const std::string &dir);

// The visible filament preset that fits a preset whose parent is missing best: the same name before " @" as the
// missing parent for one of the printers, then the same vendor and type, then "Generic <type>", each compatible with
// one of the printers (any printer when printers is empty). Empty when no filament preset of that type exists.
std::string forca_best_filament_parent(PresetBundle &bundle, const std::string &missing_parent,
                                       const std::string &vendor, const std::string &type,
                                       const std::vector<std::string> &printers);

} // namespace Slic3r
