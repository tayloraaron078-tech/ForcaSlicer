#pragma once

#include <string>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI {

// Forca: the import review (DECISIONS 2026-10-25, docs/HLSD/preset-import.md). For the preset .json files given, shows
// what Forca takes over from each one (its parent: found, matched or missing with a suggestion; its printers; the
// settings it changes, the ones Forca names differently and the ones Forca doesn't have) and lets the user choose the
// name, the parent and the printer before anything is saved; then imports the files the user kept ticked
// (libslic3r/ForcaPresetImport). Returns the number of presets imported (-1: the user cancelled the review);
// PresetBundle::import_failures() lists the ones that failed. Shown for every import (D2), also when there is nothing
// to decide.
// With repair, the files are the user's own presets whose parent is missing (PresetCollection::presets_missing_parent):
// the same review, worded as a repair.
int forca_review_preset_import(wxWindow *parent, const std::vector<std::string> &files, bool repair = false);

// At start-up (DECISIONS 2026-10-25, D7): when user filament presets were not loaded because the preset they are
// based on is missing, a notification offers to repair them in the review. Nothing changes unless the user repairs.
void forca_offer_preset_repair();

}} // namespace Slic3r::GUI
