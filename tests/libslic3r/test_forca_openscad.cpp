// Forca: OpenSCAD support without the GUI (libslic3r/ForcaOpenScad): what OpenSCAD can do, its command line, and a
// .scad file's Customizer parameters. tests/data/forca_openscad/box.param.json is the real output of
// `openscad --export-format param box.scad` (OpenSCAD 2026.10.05).
#include <catch2/catch_all.hpp>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "libslic3r/ForcaOpenScad.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <fstream>
#include <ios>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

std::string read_text(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

const OpenScadParam *find_param(const std::vector<OpenScadParam> &params, const std::string &name)
{
    for (const OpenScadParam &param : params)
        if (param.name == name)
            return &param;
    return nullptr;
}

} // namespace

TEST_CASE("A .scad file's Customizer parameters are read from OpenSCAD's param export", "[ForcaOpenScad]")
{
    const std::vector<OpenScadParam> params =
        forca_openscad_parse_params(read_text(std::string(TEST_DATA_DIR) + "/forca_openscad/box.param.json"));

    // Every parameter before the [Hidden] group, in file order.
    REQUIRE(params.size() == 7);
    CHECK(params.front().name == "width");
    CHECK(find_param(params, "wall") == nullptr);

    const OpenScadParam *width = find_param(params, "width");
    REQUIRE(width != nullptr);
    CHECK(width->group == "Size");
    CHECK(width->caption == "Outside width");
    CHECK(width->type == OpenScadParam::Type::Number);
    CHECK(width->value == "40.0");
    REQUIRE(width->min.has_value());
    CHECK_THAT(*width->min, Catch::Matchers::WithinAbs(10., 1e-9));
    CHECK_THAT(*width->max, Catch::Matchers::WithinAbs(100., 1e-9));
    CHECK_THAT(*width->step, Catch::Matchers::WithinAbs(0.5, 1e-9));

    // A number with no range comment has no min or max.
    const OpenScadParam *height = find_param(params, "height");
    REQUIRE(height != nullptr);
    CHECK_FALSE(height->min.has_value());
    CHECK_FALSE(height->max.has_value());

    const OpenScadParam *rounded = find_param(params, "rounded");
    REQUIRE(rounded != nullptr);
    CHECK(rounded->type == OpenScadParam::Type::Boolean);
    CHECK(rounded->value == "true");

    const OpenScadParam *part = find_param(params, "part");
    REQUIRE(part != nullptr);
    CHECK(part->type == OpenScadParam::Type::String);
    CHECK(part->value == "\"box\"");
    CHECK(part->options == std::vector<std::pair<std::string, std::string>>{{"Box", "\"box\""}, {"Lid", "\"lid\""}});

    const OpenScadParam *lid_size = find_param(params, "lid_size");
    REQUIRE(lid_size != nullptr);
    CHECK(lid_size->type == OpenScadParam::Type::Vector);
    CHECK(lid_size->value == "[40.0, 30.0]");
}

TEST_CASE("Text that isn't OpenSCAD's parameter JSON is refused", "[ForcaOpenScad]")
{
    CHECK_THROWS_AS(forca_openscad_parse_params("not json"), std::runtime_error);
    CHECK_THROWS_AS(forca_openscad_parse_params(R"({"title":"box"})"), std::runtime_error);
}

TEST_CASE("A string is quoted for OpenSCAD with its quotes and backslashes escaped", "[ForcaOpenScad]")
{
    // Raw strings outside the CHECK: MSVC can't stringify them inside the macro.
    const std::string text   = R"(say "hi" \ bye)";
    const std::string quoted = R"("say \"hi\" \\ bye")";
    CHECK(forca_openscad_quote("lid") == "\"lid\"");
    CHECK(forca_openscad_quote(text) == quoted);
    // And back, for the parameter dialog; a value that isn't a quoted string stays as it is.
    CHECK(forca_openscad_unquote(quoted) == text);
    CHECK(forca_openscad_unquote("\"lid\"") == "lid");
    CHECK(forca_openscad_unquote("42") == "42");
}

TEST_CASE("A render uses the Manifold backend, keeps colours and passes each parameter with -D", "[ForcaOpenScad]")
{
    CHECK(forca_openscad_render_args("in.scad", "out.3mf", {{"part", "\"lid\""}, {"width", "42"}}) ==
          std::vector<std::string>{"-o", "out.3mf", "--backend", "Manifold", "-O", "export-3mf/color-mode=model", "-D",
                                   "part=\"lid\"", "-D", "width=42", "in.scad"});
    CHECK(forca_openscad_param_args("in.scad", "p.json") ==
          std::vector<std::string>{"-o", "p.json", "--export-format", "param", "in.scad"});
}

TEST_CASE("Only an OpenSCAD with the Manifold backend and the param export is usable", "[ForcaOpenScad]")
{
    // From the --help text of the 2026.10.05 development snapshot and of the 2021.01 release.
    const std::string snapshot = "  --export-format arg               overrides format of exported scad file when\n"
                                 "                                    param, pov. May be used multiple times for \n"
                                 "  --backend arg                     3D rendering backend to use: 'CGAL' \n"
                                 "                                    (old/slow) or 'Manifold' (new/fast) \n";
    const std::string release  = "  --export-format arg          overrides format of exported scad file when \n"
                                 "  -p [ --p ] arg               customizer parameter file\n"
                                 "  --check-parameters arg       =true/false, configure the parameter check for \n";
    CHECK(forca_openscad_capabilities(snapshot).usable());
    const OpenScadCapabilities old = forca_openscad_capabilities(release);
    CHECK_FALSE(old.manifold);
    CHECK_FALSE(old.param_export);
    CHECK_FALSE(old.usable());
}

TEST_CASE("The version is read from OpenSCAD's --version output", "[ForcaOpenScad]")
{
    CHECK(forca_openscad_version("OpenSCAD version 2026.10.05\n") == "2026.10.05");
    CHECK(forca_openscad_version("OpenSCAD version 2021.01\r\n") == "2021.01");
    CHECK(forca_openscad_version("something else").empty());
}

TEST_CASE("Only OpenSCAD's warnings and errors are kept from its console output", "[ForcaOpenScad]")
{
    const std::string console = "ECHO: \"TPU pad: 208.4 x 268.9\"\r\n"
                                "WARNING: Ignoring unknown variable 'x' in file box.scad, line 3\r\n"
                                "Rendering Polygon Mesh using Manifold...\n"
                                "ERROR: Parser error in file \"box.scad\", line 9: syntax error\n";
    CHECK(forca_openscad_problems(console) ==
          std::vector<std::string>{"WARNING: Ignoring unknown variable 'x' in file box.scad, line 3",
                                   "ERROR: Parser error in file \"box.scad\", line 9: syntax error"});
}

TEST_CASE("The lines of the file that OpenSCAD's warnings and errors name are found", "[ForcaOpenScad]")
{
    const std::string console = "ECHO: \"line 2\"\n"
                                "WARNING: Ignoring unknown variable 'x' in file box.scad, line 3\n"
                                "WARNING: Too many unnamed arguments in file lib/util.scad, line 40\n"
                                "ERROR: Parser error in file \"C:/models/box.scad\", line 9: syntax error\n";
    const std::vector<OpenScadProblemLine> lines = forca_openscad_problem_lines(console, "box.scad");

    // util.scad is an included file, not the one being edited.
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].line == 3);
    CHECK_FALSE(lines[0].error);
    CHECK_FALSE(lines[0].syntax);
    CHECK(lines[1].line == 9);
    CHECK(lines[1].error);
    CHECK(lines[1].syntax);
    CHECK(forca_openscad_problem_lines(console, "other.scad").empty());

    // OpenSCAD 2026.10.05's own wording: a relative path with spaces, not quoted.
    const std::vector<OpenScadProblemLine> real = forca_openscad_problem_lines(
        "ERROR: Parser error: syntax error in file ../../../3D Objects/Cases/Dremel_Case_v3.scad, line 33\n", "Dremel_Case_v3.scad");
    REQUIRE(real.size() == 1);
    CHECK(real[0].line == 33);
    CHECK(real[0].syntax);
}

TEST_CASE("The colours of an OpenSCAD 3MF are read per triangle", "[ForcaOpenScad]")
{
    // colors.3mf is OpenSCAD 2026.10.05's render of colors.scad: a red block with a white label, one object.
    const OpenScadColors colors = forca_openscad_read_colors(std::string(TEST_DATA_DIR) + "/forca_openscad/colors.3mf");
    REQUIRE(colors.colors.size() == 3);
    CHECK(colors.default_index == 0);
    CHECK(colors.colors[1] == "#FF0000");
    CHECK(colors.colors[2] == "#FFFFFF");
    REQUIRE(colors.triangles.size() == 1);
    CHECK(colors.triangles[0].size() == 28);
    // Both colours are used, and nothing is left uncoloured.
    CHECK(std::count(colors.triangles[0].begin(), colors.triangles[0].end(), 1) > 0);
    CHECK(std::count(colors.triangles[0].begin(), colors.triangles[0].end(), 2) > 0);
    CHECK(std::count(colors.triangles[0].begin(), colors.triangles[0].end(), 0) == 0);
}

TEST_CASE("A triangle without its own colour takes its object's", "[ForcaOpenScad]")
{
    const std::string xml = "<model><resources><basematerials id=\"1\"><base name=\"Default\" displaycolor=\"#F9D72C00\" />"
                            "<base name=\"Color 1\" displaycolor=\"#0000FF\" /></basematerials>"
                            "<object id=\"2\" pid=\"1\" pindex=\"1\"><mesh><triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\" />"
                            "<triangle v1=\"0\" v2=\"2\" v3=\"3\" pid=\"1\" p1=\"0\" /></triangles></mesh></object>"
                            "<object id=\"3\"><mesh><triangles><triangle v1=\"0\" v2=\"1\" v3=\"2\" /></triangles></mesh></object>"
                            "</resources></model>";
    const OpenScadColors colors = forca_openscad_colors_from_model_xml(xml);
    CHECK(colors.colors == std::vector<std::string>{"#F9D72C", "#0000FF"});
    REQUIRE(colors.triangles.size() == 2);
    CHECK(colors.triangles[0] == std::vector<int>{1, 0});
    CHECK(colors.triangles[1] == std::vector<int>{-1});
    CHECK(forca_openscad_read_colors("no such file.3mf").triangles.empty());
}

TEST_CASE("A JSON value becomes an OpenSCAD value for -D", "[ForcaOpenScad]")
{
    CHECK(forca_openscad_value_from_json("42") == "42");
    CHECK(forca_openscad_value_from_json("2.5") == "2.5");
    CHECK(forca_openscad_value_from_json("true") == "true");
    CHECK(forca_openscad_value_from_json("\"lid\"") == "\"lid\"");
    CHECK(forca_openscad_value_from_json("[40, 30.5]") == "[40, 30.5]");
    CHECK_THROWS_AS(forca_openscad_value_from_json("{\"a\": 1}"), std::runtime_error);
    CHECK_THROWS_AS(forca_openscad_value_from_json("null"), std::runtime_error);
    CHECK_THROWS_AS(forca_openscad_value_from_json("not json"), std::runtime_error);
}

TEST_CASE("The starter model for a new OpenSCAD file has Customizer parameters in groups", "[ForcaOpenScad]")
{
    // Rendering it needs OpenSCAD; this checks the parts the parameters dialog relies on.
    const std::string scad = forca_openscad_starter_model();
    CHECK(scad.find("/* [Size] */") != std::string::npos);
    CHECK(scad.find("width = 40; // [10:1:200]") != std::string::npos);
    CHECK(scad.find("hollow = false;") != std::string::npos);
    CHECK(scad.find("/* [Hidden] */") != std::string::npos);
    CHECK(scad.find("rounded_box([width, depth, height], radius);") != std::string::npos);
}

TEST_CASE("An object's OpenSCAD link keeps its file, text and values through JSON", "[ForcaOpenScad]")
{
    OpenScadLink link;
    link.file   = "C:/models/box.scad";
    link.scad   = "label = \"Forca\";\ncube(10);\n";
    link.values = {{"width", "42"}, {"part", "\"lid\""}, {"lid_size", "[40, 30]"}};

    const OpenScadLink back = forca_openscad_link_from_json(forca_openscad_link_to_json(link));
    CHECK(back.file == link.file);
    CHECK(back.scad == link.scad);
    CHECK(back.values == link.values);
    CHECK_FALSE(back.empty());

    CHECK(forca_openscad_link_from_json("not json").empty());
    CHECK(forca_openscad_link_from_json("").empty());
}

TEST_CASE("The project's OpenSCAD links keep each link with its object's index", "[ForcaOpenScad]")
{
    OpenScadLink first, third;
    first.file = "a.scad";
    third.file = "c.scad";
    const std::map<int, std::string> back = forca_openscad_links_from_archive(
        forca_openscad_links_to_archive({{1, forca_openscad_link_to_json(first)}, {3, forca_openscad_link_to_json(third)}}));

    REQUIRE(back.size() == 2);
    CHECK(forca_openscad_link_from_json(back.at(1)).file == "a.scad");
    CHECK(forca_openscad_link_from_json(back.at(3)).file == "c.scad");
    CHECK(forca_openscad_links_from_archive("{}").empty());
    CHECK(forca_openscad_links_from_archive("garbage").empty());
}
