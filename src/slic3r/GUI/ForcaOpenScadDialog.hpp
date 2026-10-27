#pragma once

#include "libslic3r/ForcaOpenScad.hpp"

#include <string>
#include <utility>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI {

// Forca: the OpenSCAD Customizer inside Forca (docs/HLSD/openscad.md; DECISIONS 2026-10-26, D3). Shows the .scad
// file's parameters by group, filled with values (name, value in OpenSCAD syntax; the file's default for any not
// listed). On Render or Edit source, values then holds every parameter whose value differs from the file's default, so
// the rest keep following the file.
enum class OpenScadParamsAction { Cancel, Render, EditSource };
OpenScadParamsAction forca_openscad_parameters_dialog(wxWindow *parent, const std::string &file_name, const std::vector<OpenScadParam> &params,
                                                      std::vector<std::pair<std::string, std::string>> &values);

}} // namespace Slic3r::GUI
