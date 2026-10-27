#pragma once

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
class ModelObject;
namespace GUI {

class Plater;

// Forca: OpenSCAD in Forca (docs/HLSD/openscad.md; DECISIONS 2026-10-26). Forca runs the user's own openscad.exe as a
// separate program (libslic3r/ForcaOpenScad for the command line and the parameters).

// An OpenSCAD program and what it can do.
struct OpenScadInfo
{
    std::string path;    // empty: none found
    std::string version; // "2026.10.05"
    bool        usable = false; // has the Manifold backend and the param export
};

// Runs path --help and --version and reads them.
OpenScadInfo forca_openscad_check(const std::string &path);

// The OpenSCAD Forca uses: the one chosen in Preferences (app config "forca_openscad_path"), or, when none is chosen,
// the first installed one that is usable. Remembered until the preference changes.
OpenScadInfo forca_openscad_find();

bool forca_is_scad_file(const boost::filesystem::path &path);

// Renders the .scad files with their default parameters in the background and adds the results to the plate, named
// after the files (one undo step). When no usable OpenSCAD is found, says what to install or choose instead.
void forca_openscad_import(Plater &plater, const std::vector<boost::filesystem::path> &scad_files);

// Whether an object is linked to the .scad file it was rendered from (stage 2; ModelObject::forca_openscad).
bool forca_openscad_is_linked(const ModelObject &object);

// "OpenSCAD parameters...": reads the linked file's parameters with OpenSCAD (the file as it is now, or the text of the
// last render when the file is gone), lets the user change them, renders in the background and replaces the object's
// model parts in place (one undo step), keeping where the object is, its modifiers and its parts' settings.
void forca_openscad_edit_parameters(Plater &plater, size_t obj_idx);

} // namespace GUI
} // namespace Slic3r
