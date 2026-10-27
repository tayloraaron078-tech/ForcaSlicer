#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// Forca: OpenSCAD support (docs/HLSD/openscad.md). Forca runs the user's own openscad.exe as a separate program -- a
// .scad file in, a 3MF out -- and combines none of OpenSCAD's code with its own. This part is what can be done without
// the GUI: where OpenSCAD usually is, what the installed one can do, the command line, and the file's Customizer
// parameters.

// Where OpenSCAD is usually installed, the newest kind first: the development snapshot ("nightly"), then a release.
// Only paths that exist are returned; the PATH is not searched.
std::vector<std::string> forca_openscad_candidate_paths();

// What an installed OpenSCAD can do, read from its --help text. Forca needs both: the Manifold backend (renders in
// seconds where the old CGAL one can take minutes) and the "param" export (the file's Customizer as JSON). The 2021.01
// release has neither; development snapshots from 2025 on have both.
struct OpenScadCapabilities
{
    bool manifold     = false;
    bool param_export = false;
    bool usable() const { return manifold && param_export; }
};
OpenScadCapabilities forca_openscad_capabilities(const std::string &help_text);

// "2026.10.05" from OpenSCAD's --version output ("OpenSCAD version 2026.10.05"); empty when it isn't there.
std::string forca_openscad_version(const std::string &version_output);

// The arguments (without the program) to render scad_file into out_3mf with the Manifold backend, keeping OpenSCAD's
// colours, with each definition as "-D name=value" (value already in OpenSCAD syntax, see OpenScadParam::value).
std::vector<std::string> forca_openscad_render_args(const std::string &scad_file, const std::string &out_3mf,
                                                    const std::vector<std::pair<std::string, std::string>> &definitions);

// The arguments to write scad_file's Customizer parameters as JSON into out_json.
std::vector<std::string> forca_openscad_param_args(const std::string &scad_file, const std::string &out_json);

// One Customizer parameter of a .scad file.
struct OpenScadParam
{
    enum class Type { Number, Boolean, String, Vector };
    std::string name;
    std::string group;   // the file's /* [Group] */ section; empty for none
    std::string caption; // the comment above the variable
    Type        type = Type::Number;
    std::string value;   // the initial value in OpenSCAD syntax, ready for -D: 12.5, true, "lid", [300, 320]
    std::optional<double> min, max, step;
    // A dropdown: (label, value in OpenSCAD syntax) per choice; empty when the parameter is free.
    std::vector<std::pair<std::string, std::string>> options;
};

// The link between a model object and the .scad file it was rendered from (DECISIONS 2026-10-26, D4): the file's path
// and text at the last render, so the object re-renders even where the file is gone, and the parameter values the user
// chose (name, value in OpenSCAD syntax; the file's defaults for the rest). Kept as JSON in ModelObject::forca_openscad
// and in the project 3MF (Metadata/forca_openscad.json).
struct OpenScadLink
{
    std::string                                      file;
    std::string                                      scad;
    std::vector<std::pair<std::string, std::string>> values;
    bool empty() const { return file.empty() && scad.empty(); }
};
std::string  forca_openscad_link_to_json(const OpenScadLink &link);
// An empty link when the text isn't a link.
OpenScadLink forca_openscad_link_from_json(const std::string &json_text);

// The project 3MF's Metadata/forca_openscad.json: each linked object's link, by the object's 1-based index in the model
// (the index the 3MF's layer height profiles use too).
std::string                forca_openscad_links_to_archive(const std::vector<std::pair<int, std::string>> &links);
std::map<int, std::string> forca_openscad_links_from_archive(const std::string &json_text);

// Parses OpenSCAD's "param" export. Throws std::runtime_error when the text isn't that JSON.
std::vector<OpenScadParam> forca_openscad_parse_params(const std::string &json_text);

// A string in OpenSCAD syntax: quoted, with \ and " escaped.
std::string forca_openscad_quote(const std::string &text);
// The text of a quoted OpenSCAD string (the reverse of forca_openscad_quote); text that isn't quoted comes back as it is.
std::string forca_openscad_unquote(const std::string &value);

// The WARNING / ERROR lines of OpenSCAD's console output (stderr), without the ECHO and progress lines.
std::vector<std::string> forca_openscad_problems(const std::string &console_output);

// A line of a .scad file that one of OpenSCAD's warnings or errors names ("... in file box.scad, line 9").
struct OpenScadProblemLine
{
    int  line   = 0; // 1-based, as OpenSCAD counts
    bool error  = false;
    bool syntax = false; // a parser error: often caused by the end of the code line before (a missing ; or bracket)
};

// The lines of file_name (a file name, without folders) that OpenSCAD's warnings and errors name, in output order;
// lines of other files (an include) are left out.
std::vector<OpenScadProblemLine> forca_openscad_problem_lines(const std::string &console_output, const std::string &file_name);

} // namespace Slic3r
