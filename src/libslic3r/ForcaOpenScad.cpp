#include "ForcaOpenScad.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

using json = nlohmann::json;

std::string env(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

// A Customizer value in OpenSCAD syntax.
std::string openscad_value(const json &value)
{
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_number())
        return value.dump(); // locale-independent, shortest form that reads back the same
    if (value.is_string())
        return forca_openscad_quote(value.get<std::string>());
    if (value.is_array()) {
        std::string out = "[";
        for (const json &item : value)
            out += (out.size() > 1 ? ", " : "") + openscad_value(item);
        return out + "]";
    }
    throw std::runtime_error("Unsupported OpenSCAD Customizer value: " + value.dump());
}

std::optional<double> number_of(const json &object, const char *key)
{
    if (object.contains(key) && object[key].is_number())
        return object[key].get<double>();
    return std::nullopt;
}

std::string string_of(const json &object, const char *key)
{
    return object.contains(key) && object[key].is_string() ? object[key].get<std::string>() : std::string();
}

} // namespace

std::vector<std::string> forca_openscad_candidate_paths()
{
    std::vector<std::string> candidates;
#ifdef _WIN32
    for (const std::string &root : {env("ProgramFiles"), env("ProgramW6432"), env("ProgramFiles(x86)"),
                                     env("LOCALAPPDATA").empty() ? std::string() : env("LOCALAPPDATA") + "\\Programs"})
        if (!root.empty())
            for (const char *dir : {"OpenSCAD (Nightly)", "OpenSCAD"})
                candidates.push_back(root + "\\" + dir + "\\openscad.exe");
#elif defined(__APPLE__)
    candidates = {"/Applications/OpenSCAD (Nightly).app/Contents/MacOS/OpenSCAD", "/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD"};
#else
    candidates = {"/usr/bin/openscad-nightly", "/usr/local/bin/openscad-nightly", "/usr/bin/openscad", "/usr/local/bin/openscad"};
#endif
    std::vector<std::string> found;
    for (const std::string &path : candidates) {
        boost::system::error_code ec;
        if (boost::filesystem::is_regular_file(path, ec) && std::find(found.begin(), found.end(), path) == found.end())
            found.push_back(path);
    }
    return found;
}

OpenScadCapabilities forca_openscad_capabilities(const std::string &help_text)
{
    OpenScadCapabilities caps;
    caps.manifold     = help_text.find("Manifold") != std::string::npos;
    // "param" as a word in the --export-format list (not "parameter", which every version mentions).
    caps.param_export = std::regex_search(help_text, std::regex(R"(\bparam\b)"));
    return caps;
}

std::string forca_openscad_version(const std::string &version_output)
{
    std::smatch match;
    if (std::regex_search(version_output, match, std::regex(R"(OpenSCAD version ([0-9][0-9A-Za-z.\-]*))")))
        return match[1].str();
    return {};
}

std::vector<std::string> forca_openscad_render_args(const std::string &scad_file, const std::string &out_3mf,
                                                    const std::vector<std::pair<std::string, std::string>> &definitions)
{
    std::vector<std::string> args = {"-o", out_3mf, "--backend", "Manifold", "-O", "export-3mf/color-mode=model"};
    for (const auto &[name, value] : definitions) {
        args.push_back("-D");
        args.push_back(name + "=" + value);
    }
    args.push_back(scad_file);
    return args;
}

std::vector<std::string> forca_openscad_param_args(const std::string &scad_file, const std::string &out_json)
{
    return {"-o", out_json, "--export-format", "param", scad_file};
}

std::vector<OpenScadParam> forca_openscad_parse_params(const std::string &json_text)
{
    json j;
    try {
        j = json::parse(json_text);
    } catch (const std::exception &err) {
        throw std::runtime_error(std::string("OpenSCAD's parameter list isn't valid JSON: ") + err.what());
    }
    if (!j.is_object() || !j.contains("parameters") || !j["parameters"].is_array())
        throw std::runtime_error("OpenSCAD's parameter list has no \"parameters\"");

    std::vector<OpenScadParam> params;
    for (const json &item : j["parameters"]) {
        if (!item.is_object() || !item.contains("name") || !item.contains("initial"))
            continue;
        OpenScadParam param;
        param.name    = string_of(item, "name");
        param.group   = string_of(item, "group");
        param.caption = string_of(item, "caption");
        const json &initial = item["initial"];
        param.value         = openscad_value(initial);
        param.type          = initial.is_array()   ? OpenScadParam::Type::Vector :
                              initial.is_boolean() ? OpenScadParam::Type::Boolean :
                              initial.is_string()  ? OpenScadParam::Type::String :
                                                     OpenScadParam::Type::Number;
        param.min  = number_of(item, "min");
        param.max  = number_of(item, "max");
        param.step = number_of(item, "step");
        if (item.contains("options") && item["options"].is_array())
            for (const json &option : item["options"])
                if (option.is_object() && option.contains("value"))
                    param.options.emplace_back(option.contains("name") ? string_of(option, "name") : option["value"].dump(),
                                               openscad_value(option["value"]));
        if (!param.name.empty())
            params.push_back(std::move(param));
    }
    return params;
}

std::string forca_openscad_link_to_json(const OpenScadLink &link)
{
    json values = json::array();
    for (const auto &[name, value] : link.values)
        values.push_back(json::array({name, value}));
    return json{{"file", link.file}, {"scad", link.scad}, {"values", values}}.dump();
}

OpenScadLink forca_openscad_link_from_json(const std::string &json_text)
{
    OpenScadLink link;
    try {
        const json j = json::parse(json_text);
        if (!j.is_object())
            return link;
        link.file = string_of(j, "file");
        link.scad = string_of(j, "scad");
        if (j.contains("values") && j["values"].is_array())
            for (const json &pair : j["values"])
                if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_string())
                    link.values.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
    } catch (const std::exception &) {
        return OpenScadLink();
    }
    return link;
}

std::string forca_openscad_links_to_archive(const std::vector<std::pair<int, std::string>> &links)
{
    json objects = json::array();
    for (const auto &[index, link] : links) {
        json parsed = json::parse(link, nullptr, false);
        if (!parsed.is_discarded())
            objects.push_back(json{{"object", index}, {"link", parsed}});
    }
    return json{{"version", 1}, {"objects", objects}}.dump();
}

std::map<int, std::string> forca_openscad_links_from_archive(const std::string &json_text)
{
    std::map<int, std::string> links;
    const json                 j = json::parse(json_text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("objects") || !j["objects"].is_array())
        return links;
    for (const json &item : j["objects"])
        if (item.is_object() && item.contains("object") && item["object"].is_number_integer() && item.contains("link") &&
            item["link"].is_object())
            links[item["object"].get<int>()] = item["link"].dump();
    return links;
}

std::string forca_openscad_quote(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string forca_openscad_unquote(const std::string &value)
{
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return value;
    std::string out;
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] == '\\' && i + 2 < value.size())
            ++i;
        out += value[i];
    }
    return out;
}

std::vector<std::string> forca_openscad_problems(const std::string &console_output)
{
    std::vector<std::string> problems;
    std::istringstream       in(console_output);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (boost::starts_with(line, "WARNING:") || boost::starts_with(line, "ERROR:"))
            problems.push_back(line);
    }
    return problems;
}

std::vector<OpenScadProblemLine> forca_openscad_problem_lines(const std::string &console_output, const std::string &file_name)
{
    std::vector<OpenScadProblemLine> lines;
    const std::regex                 where(R"re(in file "?([^",]*)"?, line ([0-9]+))re");
    for (const std::string &problem : forca_openscad_problems(console_output)) {
        std::smatch match;
        if (!std::regex_search(problem, match, where) ||
            boost::filesystem::path(match[1].str()).filename().string() != file_name)
            continue;
        lines.push_back({std::stoi(match[2].str()), boost::starts_with(problem, "ERROR:"), problem.find("Parser error") != std::string::npos});
    }
    return lines;
}

} // namespace Slic3r
