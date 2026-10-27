#pragma once

#include <boost/filesystem/path.hpp>

#include <functional>
#include <string>

class wxWindow;

namespace Slic3r { namespace GUI {

// Forca: a small code editor for an OpenSCAD file, for a quick fix without leaving Forca (docs/HLSD/openscad.md).
// Scintilla (wxStyledTextCtrl) with line numbers and OpenSCAD colouring, and OpenSCAD's console under it; the lines
// OpenSCAD names in a warning or error are marked, and the first error is selected. "Save and check" saves the file
// and runs check(file) -- OpenSCAD reading the file again -- whose console output replaces the shown one.
// Returns true when the file was saved at least once.
bool forca_openscad_edit_source(wxWindow *parent, const boost::filesystem::path &file, const std::string &console_output,
                                const std::function<std::string(const boost::filesystem::path &)> &check);

}} // namespace Slic3r::GUI
