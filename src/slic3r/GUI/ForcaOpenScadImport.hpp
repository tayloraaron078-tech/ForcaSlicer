#pragma once

#include "libslic3r/ForcaOpenScad.hpp"

#include <boost/filesystem/path.hpp>

#include <cstddef>
#include <string>
#include <utility>
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

// "Edit in OpenSCAD": opens the linked file in OpenSCAD's own window and watches it. When it is saved there, Forca asks
// whether to render the objects linked to it again (D6), each with its own values (one undo step each).
void forca_openscad_edit_in_openscad(Plater &plater, size_t obj_idx);

// "New OpenSCAD model": saves a starter .scad file where the user chooses, imports it and opens it in OpenSCAD, watched
// as above.
void forca_openscad_new_model(Plater &plater);

// For Forca AI's OpenSCAD tools (ForcaAIOpenScad.cpp), which run OpenSCAD off the GUI thread and place the result on it.
// A render done here, waiting until OpenSCAD ends (any thread). work_dir holds the 3MF until forca_openscad_place
// removes it.
struct OpenScadRun
{
    bool                    ok = false;
    std::string             error;     // couldn't start, timed out, or OpenSCAD failed
    std::string             output;    // OpenSCAD's console
    std::string             scad_text; // the file as rendered
    boost::filesystem::path out_3mf;
    boost::filesystem::path work_dir;
};
OpenScadRun forca_openscad_render_file(const std::string &exe, const boost::filesystem::path &scad,
                                       const std::vector<std::pair<std::string, std::string>> &values, int timeout_s);
// The file's Customizer parameters (any thread); empty + error when OpenSCAD can't read them.
std::vector<OpenScadParam> forca_openscad_read_params(const std::string &exe, const boost::filesystem::path &scad, std::string &output,
                                                      std::string &error);
// GUI thread: puts a successful run on the plate as a new object linked to scad, or, with replace_obj >= 0, in place of
// that object's model parts (as "OpenSCAD parameters" does), as the undo step undo_name. Returns the object's index, or
// -1 and error. Removes run.work_dir.
int forca_openscad_place(Plater &plater, const OpenScadRun &run, const boost::filesystem::path &scad,
                         const std::vector<std::pair<std::string, std::string>> &values, int replace_obj, const std::string &undo_name,
                         std::string &error);

} // namespace GUI
} // namespace Slic3r
