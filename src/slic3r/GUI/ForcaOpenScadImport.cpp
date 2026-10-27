#include "ForcaOpenScadImport.hpp"

#include "GUI.hpp" // show_error / from_u8 / into_u8
#include "GUI_App.hpp"
#include "ForcaOpenScadDialog.hpp"
#include "ForcaOpenScadEditor.hpp"
#include "GUI_ObjectList.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ForcaOpenScad.hpp"
#include "libslic3r/Format/bbs_3mf.hpp" // LoadStrategy
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "slic3r/Utils/BoostProcessCompat.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/busycursor.h>
#include <wx/string.h>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

namespace fs = boost::filesystem;

// OpenSCAD renders in about a second with Manifold; a model that takes this long is stopped (Cancel is always there).
constexpr std::chrono::minutes RENDER_TIMEOUT{10};

struct RunResult
{
    int         exit_code = -1;
    std::string output; // stdout and stderr
    bool        canceled  = false;
    bool        timed_out = false;
    std::string error;  // the program couldn't be started
};

std::string read_file(const fs::path &path)
{
    boost::nowide::ifstream in(path.string(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Runs OpenSCAD without a console window, its output into files in work_dir, polling for cancel.
RunResult run_openscad(const std::string &exe, const std::vector<std::string> &args, const fs::path &work_dir,
                       const std::function<bool()> &canceled, std::chrono::milliseconds timeout)
{
    namespace bp = boost::process;
    RunResult                 result;
    boost::system::error_code ec;
    fs::create_directories(work_dir, ec);
    const fs::path out_file = work_dir / "openscad-out.txt";
    const fs::path err_file = work_dir / "openscad-err.txt";
    // boost.process opens a redirect file without truncating it (Windows: OPEN_ALWAYS), so a shorter output would
    // keep the end of the previous run's.
    fs::remove(out_file, ec);
    fs::remove(err_file, ec);
    try {
#ifdef _WIN32
        // Wide strings, so paths with any characters reach OpenSCAD intact.
        std::vector<std::wstring> wargs;
        for (const std::string &arg : args)
            wargs.push_back(boost::nowide::widen(arg));
        bp::child child(bp::exe = fs::path(boost::nowide::widen(exe)), bp::args = wargs, bp::std_in < bp::null,
                        bp::std_out > out_file, bp::std_err > err_file, bp::windows::create_no_window);
#else
        bp::child child(bp::exe = exe, bp::args = args, bp::std_in < bp::null, bp::std_out > out_file, bp::std_err > err_file);
#endif
        const auto start = std::chrono::steady_clock::now();
        while (!child.wait_for(std::chrono::milliseconds(100))) {
            if (canceled && canceled())
                result.canceled = true;
            else if (std::chrono::steady_clock::now() - start > timeout)
                result.timed_out = true;
            else
                continue;
            child.terminate();
            break;
        }
        if (!result.canceled && !result.timed_out)
            result.exit_code = child.exit_code();
    } catch (const std::exception &err) {
        result.error = err.what();
    }
    result.output = read_file(out_file) + read_file(err_file);
    return result;
}

fs::path make_work_dir()
{
    boost::system::error_code ec;
    return fs::temp_directory_path(ec) / fs::unique_path("forca-openscad-%%%%-%%%%-%%%%", ec);
}

// The editor's check: OpenSCAD renders the file with the chosen values, and its console output is what the editor shows.
std::string check_source(const std::string &exe, const fs::path &dir, const fs::path &scad,
                         const std::vector<std::pair<std::string, std::string>> &values)
{
    const RunResult result = run_openscad(exe, forca_openscad_render_args(scad.string(), (dir / "check.3mf").string(), values), dir,
                                          nullptr, std::chrono::seconds(60));
    std::string     output = result.output;
    if (!result.error.empty())
        output += result.error + "\n";
    if (result.timed_out)
        output += _u8L("OpenSCAD took longer than a minute and was stopped.") + "\n";
    return output;
}

// What to tell the user when Forca has no OpenSCAD it can use.
wxString no_openscad_message(const OpenScadInfo &info)
{
    if (info.path.empty())
        return _L("Opening a .scad file needs OpenSCAD, a development snapshot from 2025 or later. Install it from "
                  "openscad.org (Downloads, Development Snapshots), or choose it in Preferences > OpenSCAD.");
    // TRN %1% is the path to openscad.exe, %2% its version.
    return format_wxstr(_L("The OpenSCAD at %1% (version %2%) can't do what Forca needs. Forca needs a development "
                           "snapshot from 2025 or later: install one from openscad.org, or choose it in Preferences > OpenSCAD."),
                        info.path, info.version.empty() ? std::string("?") : info.version);
}

} // namespace

OpenScadInfo forca_openscad_check(const std::string &path)
{
    OpenScadInfo info;
    info.path = path;
    boost::system::error_code ec;
    if (path.empty() || !fs::is_regular_file(fs::path(boost::nowide::widen(path)), ec))
        return info;
    const fs::path   dir     = make_work_dir();
    const RunResult  help    = run_openscad(path, {"--help"}, dir, nullptr, std::chrono::seconds(20));
    const RunResult  version = run_openscad(path, {"--version"}, dir, nullptr, std::chrono::seconds(20));
    fs::remove_all(dir, ec);
    info.version = forca_openscad_version(version.output);
    info.usable  = forca_openscad_capabilities(help.output).usable();
    BOOST_LOG_TRIVIAL(info) << "Forca OpenSCAD: " << path << " version " << info.version << (info.usable ? " usable" : " not usable");
    return info;
}

OpenScadInfo forca_openscad_find()
{
    static std::string  cached_key;
    static OpenScadInfo cached;
    static bool         have_cached = false;
    const std::string   chosen      = wxGetApp().app_config->get("forca_openscad_path");
    if (have_cached && cached_key == chosen)
        return cached;

    OpenScadInfo found;
    if (!chosen.empty())
        found = forca_openscad_check(chosen);
    else
        for (const std::string &candidate : forca_openscad_candidate_paths()) {
            OpenScadInfo info = forca_openscad_check(candidate);
            if (found.path.empty() || info.usable)
                found = info; // the first usable one, else the first one found (to name it in the message)
            if (info.usable)
                break;
        }
    cached_key  = chosen;
    cached      = found;
    have_cached = true;
    return found;
}

bool forca_is_scad_file(const fs::path &path) { return boost::iequals(path.extension().string(), ".scad"); }


namespace {

// One .scad file to render, shared by the job's worker thread (render_all) and its result on the UI thread.
struct Render
{
    fs::path                                         scad;      // what OpenSCAD reads
    fs::path                                         out;       // <stem>.3mf: the stem becomes the object's name
    std::vector<std::pair<std::string, std::string>> values;    // -D definitions
    std::string                                      scad_text; // the file as rendered, kept in the object's link
    bool                                             ok = false;
    std::vector<std::string>                         problems;
    std::string                                      output;  // OpenSCAD's whole console, for the editor
    std::string                                      failure; // couldn't start, timed out
};

struct RenderJob
{
    std::string         exe;
    fs::path            dir;
    std::vector<Render> renders;
};

void render_all(RenderJob &job, Job::Ctl &ctl)
{
    for (size_t i = 0; i < job.renders.size() && !ctl.was_canceled(); ++i) {
        Render &render = job.renders[i];
        // TRN %1% is a file name.
        ctl.update_status(int(100 * i / job.renders.size()), format(_u8L("Rendering %1% with OpenSCAD..."), render.scad.filename().string()));
        render.scad_text = read_file(render.scad);
        const RunResult result = run_openscad(job.exe, forca_openscad_render_args(render.scad.string(), render.out.string(), render.values),
                                              job.dir, [&ctl] { return ctl.was_canceled(); }, RENDER_TIMEOUT);
        boost::system::error_code ec;
        render.problems = forca_openscad_problems(result.output);
        render.output   = result.output;
        render.ok = result.error.empty() && !result.canceled && !result.timed_out && result.exit_code == 0 && fs::file_size(render.out, ec) > 0 &&
                    !ec;
        if (!result.error.empty())
            render.failure = result.error;
        else if (result.timed_out)
            // TRN %1% is a number of minutes.
            render.failure = format(_u8L("OpenSCAD took longer than %1% minutes and was stopped."), RENDER_TIMEOUT.count());
        BOOST_LOG_TRIVIAL(info) << "Forca OpenSCAD: " << render.scad << " exit " << result.exit_code << (render.ok ? " ok" : " failed");
    }
    // 100 closes the progress notification, as every job does at its end.
    ctl.update_status(100, ctl.was_canceled() ? _u8L("Canceled") : _u8L("Done."));
}

wxString render_errors(const std::vector<Render> &renders)
{
    wxString errors;
    for (const Render &render : renders) {
        if (render.ok)
            continue;
        // TRN %1% is a file name; OpenSCAD's own (English) messages follow.
        errors += format_wxstr(_L("OpenSCAD couldn't render %1%:"), render.scad.filename().string()) + "\n";
        if (!render.failure.empty())
            errors += from_u8(render.failure) + "\n";
        for (const std::string &problem : render.problems)
            errors += from_u8(problem) + "\n";
        errors += "\n";
    }
    return errors.Trim();
}

// OpenSCAD calls every object and part "OpenSCAD Model".
bool openscad_default_name(const std::string &name) { return name.empty() || name == "OpenSCAD Model"; }

// Puts a new render's parts in place of the object's model parts: the object keeps its place on the plate, its
// modifiers and other non-part volumes, and each part's settings and name carry over to the new part in the same
// position.
void replace_model_parts(ModelObject &object, ModelObject &rendered)
{
    rendered.center_around_origin();
    const bool                 sinking = object.min_z() < SINKING_Z_THRESHOLD;
    std::vector<ModelVolume *> old_parts;
    for (ModelVolume *volume : object.volumes)
        if (volume->is_model_part())
            old_parts.push_back(volume);
    for (size_t i = 0; i < rendered.volumes.size(); ++i) {
        ModelVolume *volume = object.add_volume(*rendered.volumes[i]);
        volume->set_new_unique_id();
        if (i < old_parts.size()) {
            volume->config.apply(old_parts[i]->config);
            volume->name = old_parts[i]->name;
        } else if (openscad_default_name(volume->name))
            volume->name = object.name;
    }
    for (size_t i = object.volumes.size(); i-- > 0;)
        if (std::find(old_parts.begin(), old_parts.end(), object.volumes[i]) != old_parts.end())
            object.delete_volume(i);
    if (!sinking)
        object.ensure_on_bed();
    object.sort_volumes(true);
    object.invalidate_bounding_box();
}

} // namespace

void forca_openscad_import(Plater &plater, const std::vector<fs::path> &scad_files)
{
    if (scad_files.empty())
        return;
    const OpenScadInfo info = forca_openscad_find();
    if (!info.usable) {
        show_error(&plater, no_openscad_message(info));
        return;
    }
    auto job = std::make_shared<RenderJob>();
    job->exe = info.path;
    job->dir = make_work_dir();
    for (const fs::path &scad : scad_files)
        job->renders.push_back({scad, job->dir / (scad.stem().string() + ".3mf")});

    auto finish = [job, &plater](bool canceled, std::exception_ptr &eptr) {
        boost::system::error_code ec;
        if (eptr || canceled) {
            fs::remove_all(job->dir, ec);
            return; // the worker reports an exception itself
        }
        std::vector<fs::path> rendered;
        for (const Render &render : job->renders)
            if (render.ok)
                rendered.push_back(render.out);
        if (!rendered.empty()) {
            Plater::TakeSnapshot      snapshot(&plater, _u8L("Import OpenSCAD model"));
            const std::vector<size_t> objects = plater.load_files(rendered, LoadStrategy::LoadModel);
            // Each new object: named after its .scad file, and linked to it (D4) when the file gave just this object.
            ModelObjectPtrs &model_objects = plater.model().objects;
            for (const Render &render : job->renders) {
                if (!render.ok)
                    continue;
                std::vector<ModelObject *> from_render;
                for (size_t idx : objects)
                    if (idx < model_objects.size() &&
                        (fs::path(model_objects[idx]->input_file).stem() == render.out.stem() || rendered.size() == 1))
                        from_render.push_back(model_objects[idx]);
                for (ModelObject *object : from_render) {
                    if (openscad_default_name(object->name)) {
                        object->name = render.scad.stem().string();
                        for (ModelVolume *volume : object->volumes)
                            if (openscad_default_name(volume->name))
                                volume->name = object->name;
                    }
                    if (from_render.size() == 1) {
                        OpenScadLink              link;
                        boost::system::error_code abs_ec;
                        link.file              = fs::absolute(render.scad, abs_ec).string();
                        link.scad              = render.scad_text;
                        object->forca_openscad = forca_openscad_link_to_json(link);
                    }
                }
            }
            wxGetApp().obj_list()->update_name_for_items();
        }
        fs::remove_all(job->dir, ec);
        if (const wxString errors = render_errors(job->renders); !errors.empty())
            show_error(&plater, errors);
    };

    queue_job(plater.get_ui_job_worker(), [job](Job::Ctl &ctl) { render_all(*job, ctl); }, std::move(finish));
}

bool forca_openscad_is_linked(const ModelObject &object) { return !forca_openscad_link_from_json(object.forca_openscad).empty(); }

void forca_openscad_edit_parameters(Plater &plater, size_t obj_idx)
{
    if (obj_idx >= plater.model().objects.size())
        return;
    const ModelObject *object = plater.model().objects[obj_idx];
    OpenScadLink       link   = forca_openscad_link_from_json(object->forca_openscad);
    if (link.empty())
        return;
    const OpenScadInfo info = forca_openscad_find();
    if (!info.usable) {
        show_error(&plater, no_openscad_message(info));
        return;
    }

    // The file as it is now; when it's gone, the text of the last render (D4), saved next to the work files.
    auto job = std::make_shared<RenderJob>();
    job->exe = info.path;
    job->dir = make_work_dir();
    boost::system::error_code ec;
    fs::create_directories(job->dir, ec);
    const fs::path    original(boost::nowide::widen(link.file));
    const std::string stem   = original.stem().string().empty() ? std::string("model") : original.stem().string();
    fs::path          source = original;
    if (link.file.empty() || !fs::is_regular_file(original, ec)) {
        source = job->dir / (stem + ".scad");
        boost::nowide::ofstream(source.string(), std::ios::binary) << link.scad;
    }

    const std::string shown_name  = original.filename().string().empty() ? source.filename().string() : original.filename().string();
    auto              edit_source = [&](const std::string &output) {
        return forca_openscad_edit_source(&plater, source, output,
                                   [&](const fs::path &file) { return check_source(job->exe, job->dir, file, link.values); });
    };
    // The parameters, read by OpenSCAD itself, again after each edit of the file.
    for (;;) {
        std::vector<OpenScadParam> params;
        std::string                output;
        wxString                   failure;
        {
            wxBusyCursor   busy;
            const fs::path json_file = job->dir / "parameters.json";
            fs::remove(json_file, ec);
            const RunResult result = run_openscad(job->exe, forca_openscad_param_args(source.string(), json_file.string()), job->dir, nullptr,
                                                  std::chrono::seconds(60));
            output                 = result.output;
            try {
                params = forca_openscad_parse_params(read_file(json_file));
            } catch (const std::exception &err) {
                // TRN %1% is a file name; OpenSCAD's own (English) messages follow.
                failure = format_wxstr(_L("OpenSCAD couldn't read the parameters of %1%:"), source.filename().string()) + "\n" +
                          from_u8(err.what());
                for (const std::string &problem : forca_openscad_problems(result.output))
                    failure += "\n" + from_u8(problem);
            }
        }
        if (!failure.empty()) {
            if (MessageDialog(&plater, failure + "\n\n" + _L("Open the file in the editor to fix it?"), _L("OpenSCAD parameters"),
                              wxYES_NO | wxYES_DEFAULT | wxICON_ERROR)
                        .ShowModal() == wxID_YES &&
                edit_source(output))
                continue;
            fs::remove_all(job->dir, ec);
            return;
        }
        const OpenScadParamsAction action = forca_openscad_parameters_dialog(&plater, shown_name, params, link.values);
        if (action == OpenScadParamsAction::Render)
            break;
        if (action == OpenScadParamsAction::Cancel) {
            fs::remove_all(job->dir, ec);
            return;
        }
        edit_source(output);
    }

    job->renders.push_back({source, job->dir / (stem + ".3mf"), link.values});
    const ObjectID object_id = object->id();
    auto finish = [job, link, object_id, &plater](bool canceled, std::exception_ptr &eptr) {
        boost::system::error_code ec;
        const Render             &render  = job->renders.front();
        auto                      cleanup = [&] { fs::remove_all(job->dir, ec); };
        if (eptr || canceled)
            return cleanup();
        if (!render.ok && render.scad.parent_path() == job->dir) { // a copy of a file that's gone: nothing to edit
            show_error(&plater, render_errors(job->renders));
            return cleanup();
        }
        if (!render.ok) {
            // Offered after the job has finished, with OpenSCAD's whole console in the editor.
            wxGetApp().CallAfter([job, &plater] {
                const Render &render = job->renders.front();
                if (MessageDialog(&plater, render_errors(job->renders) + "\n\n" + _L("Open the file in the editor to fix it?"),
                                  _L("OpenSCAD parameters"), wxYES_NO | wxYES_DEFAULT | wxICON_ERROR)
                        .ShowModal() == wxID_YES)
                    forca_openscad_edit_source(&plater, render.scad, render.output, [job, &render](const fs::path &file) {
                        return check_source(job->exe, job->dir, file, render.values);
                    });
                boost::system::error_code ec;
                fs::remove_all(job->dir, ec);
            });
            return;
        }
        // The object may have moved in the list, or gone, while OpenSCAD rendered.
        ModelObjectPtrs &objects = plater.model().objects;
        size_t           obj_idx = 0;
        while (obj_idx < objects.size() && objects[obj_idx]->id() != object_id)
            ++obj_idx;
        if (obj_idx == objects.size())
            return cleanup();
        Model rendered;
        try {
            rendered = Model::read_from_file(render.out.string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        } catch (const std::exception &err) {
            show_error(&plater, from_u8(err.what()));
            return cleanup();
        }
        if (rendered.objects.size() != 1) {
            // TRN %1% is a file name.
            show_error(&plater, format_wxstr(_L("The new render of %1% has several objects; Forca can only replace an object with one."),
                                             render.scad.filename().string()));
            return cleanup();
        }
        plater.take_snapshot(_u8L("Change OpenSCAD parameters"));
        ModelObject &object = *objects[obj_idx];
        replace_model_parts(object, *rendered.objects.front());
        OpenScadLink updated  = link;
        updated.scad          = render.scad_text;
        updated.values        = render.values;
        object.forca_openscad = forca_openscad_link_to_json(updated);
        wxGetApp().obj_list()->add_volumes_to_object_in_list(obj_idx);
        plater.changed_object(int(obj_idx));
        cleanup();
    };
    queue_job(plater.get_ui_job_worker(), [job](Job::Ctl &ctl) { render_all(*job, ctl); }, std::move(finish));
}

}} // namespace Slic3r::GUI
