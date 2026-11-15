#pragma once

// Forca: the Plate report. One page about the plate: Forca's fixed-rule checks (libslic3r ForcaPlateReport) and the
// slicer's own warnings, the Print Risks count, print time / weight / layers, the plate's size and position, the presets
// and key settings, and the settings changed and not saved. No AI judgement: every line is a value or a rule.
// Opened from the Print Risks window, the View menu and "Before you print".

#include "libslic3r/ForcaPlateReport.hpp"

#include <cstddef>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI {

// The fixed-rule checks of a plate (-1: the current plate). GUI thread. Empty when there is no such plate.
std::vector<ForcaPlateCheck> forca_plate_report_checks(int plate_idx);

// What "Before you print" shows of the report.
struct ForcaPlateReportSummary
{
    bool   ready    = false; // the plate exists and has objects
    size_t warnings = 0;     // failed checks that are warnings, plus the slicer's own warnings once it is sliced
};
ForcaPlateReportSummary forca_plate_report_summary(int plate_idx);

// Opens the report of a plate (-1: the current plate) as a modal window.
void forca_show_plate_report(wxWindow* parent, int plate_idx);

}} // namespace Slic3r::GUI
