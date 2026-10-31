#include "ForcaOpenScadImport.hpp"

#include "GUI.hpp" // show_error / from_u8 / into_u8
#include "GUI_App.hpp"
#include "ForcaOpenScadDialog.hpp"
#include "ForcaOpenScadEditor.hpp"
#include "GUI_ObjectList.hpp"
#include "GuiColor.hpp" // calc_color_distance
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ForcaOpenScad.hpp"
#include "libslic3r/Format/ProjectStrategy.hpp" // LoadStrategy
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/Utils/BoostProcessCompat.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <wx/busycursor.h>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/string.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <exception>
#include <functional>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
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

// D5: OpenSCAD's colours (read from its 3MF; OrcaSlicer's reader ignores per-triangle materials) become the project's
// filaments: each colour the closest filament colour, the faces painted with it, and the most common one the object's
// filament. Faces OpenSCAD left uncoloured keep the object's filament. The part must still have the file's triangles.
void apply_colors(ModelObject &object, const OpenScadColors &colors, size_t object_in_file)
{
    if (object_in_file >= colors.triangles.size() || wxGetApp().preset_bundle == nullptr)
        return;
    const std::vector<int> &triangles = colors.triangles[object_in_file];
    ModelVolume            *volume    = nullptr;
    for (ModelVolume *v : object.volumes)
        if (v->is_model_part() && v->mesh().its.indices.size() == triangles.size()) {
            volume = v;
            break;
        }
    const auto *filaments = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (volume == nullptr || filaments == nullptr || filaments->values.empty())
        return;

    std::map<int, int>  filament_of; // material -> 0-based filament
    std::vector<size_t> count(filaments->values.size(), 0);
    bool                uncoloured = false;
    for (int material : triangles) {
        if (material < 0 || material == colors.default_index || material >= int(colors.colors.size())) {
            uncoloured = true;
            continue;
        }
        auto it = filament_of.find(material);
        if (it == filament_of.end()) {
            int   best          = 0;
            float best_distance = std::numeric_limits<float>::max();
            for (size_t f = 0; f < filaments->values.size(); ++f) {
                const float distance = calc_color_distance(wxColour(from_u8(colors.colors[material])), wxColour(from_u8(filaments->values[f])));
                if (distance < best_distance) {
                    best          = int(f);
                    best_distance = distance;
                }
            }
            it = filament_of.emplace(material, best).first;
        }
        ++count[it->second];
    }
    if (filament_of.empty())
        return; // no color() in the file

    const int current = object.config.has("extruder") ? std::max(object.config.opt_int("extruder"), 1) - 1 : 0;
    const int base    = uncoloured ? current : int(std::max_element(count.begin(), count.end()) - count.begin());
    object.config.set_key_value("extruder", new ConfigOptionInt(base + 1));
    if (volume->config.has("extruder"))
        volume->config.set_key_value("extruder", new ConfigOptionInt(base + 1));

    TriangleSelector selector(volume->mesh());
    bool             painted = false;
    for (size_t i = 0; i < triangles.size(); ++i)
        if (auto it = filament_of.find(triangles[i]); it != filament_of.end() && it->second != base) {
            const auto state = static_cast<EnforcerBlockerType>(static_cast<int>(EnforcerBlockerType::Extruder1) + it->second);
            if (state <= EnforcerBlockerType::ExtruderMax) {
                selector.set_facet(int(i), state);
                painted = true;
            }
        }
    volume->mmu_segmentation_facets.reset();
    if (painted)
        volume->mmu_segmentation_facets.set(selector);
}

// Renders the job's one render in the background and puts the result in place of the object's model parts, as the undo
// step undo_name. A failed render shows OpenSCAD's messages and offers the editor.
void queue_rerender(Plater &plater, std::shared_ptr<RenderJob> job, const OpenScadLink &link, ObjectID object_id, std::string undo_name);

// The .scad files opened with "Edit in OpenSCAD" or "New OpenSCAD model", checked once a second on the UI thread. A
// change counts once the file has stayed the same for one more check, so a save isn't read half-written. A file no
// linked object uses any more is dropped. The watch lives as long as the Plater.
class OpenScadWatch : public wxTimer
{
public:
    static OpenScadWatch *instance() { return s_instance; }

    static OpenScadWatch &get(Plater &plater)
    {
        if (s_instance == nullptr) {
            s_instance = new OpenScadWatch(plater);
            plater.Bind(wxEVT_DESTROY, [&plater](wxWindowDestroyEvent &event) {
                if (event.GetEventObject() == &plater) {
                    delete s_instance;
                    s_instance = nullptr;
                }
                event.Skip();
            });
            s_instance->Start(1000);
        }
        return *s_instance;
    }

    void watch(const fs::path &file)
    {
        boost::system::error_code ec;
        for (const File &watched : m_files)
            if (fs::equivalent(watched.path, file, ec))
                return;
        m_files.push_back({file, stamp(file)});
    }

    // Forca's own OpenSCAD dialogs change the file themselves: no question while they are open, and what they saved
    // counts as seen.
    void pause() { ++m_paused; }
    void resume()
    {
        if (m_paused > 0 && --m_paused == 0)
            for (File &file : m_files) {
                file.seen        = stamp(file.path);
                file.has_pending = false;
            }
    }

    void Notify() override
    {
        if (m_paused > 0 || m_asking)
            return;
        std::vector<fs::path> changed;
        for (File &file : m_files) {
            const std::time_t now = stamp(file.path);
            if (now == file.seen)
                file.has_pending = false;
            else if (!file.has_pending || file.pending != now) {
                file.pending     = now;
                file.has_pending = true;
            } else {
                file.seen        = now;
                file.has_pending = false;
                changed.push_back(file.path);
            }
        }
        for (const fs::path &file : changed)
            ask_to_render(file);
    }

private:
    struct File
    {
        fs::path    path;
        std::time_t seen        = 0;
        std::time_t pending     = 0;
        bool        has_pending = false;
    };

    explicit OpenScadWatch(Plater &plater) : m_plater(plater) {}

    static std::time_t stamp(const fs::path &file)
    {
        boost::system::error_code ec;
        const std::time_t         time = fs::last_write_time(file, ec);
        return ec ? 0 : time;
    }

    void ask_to_render(const fs::path &file)
    {
        boost::system::error_code ec;
        std::vector<ObjectID>     linked;
        for (const ModelObject *object : m_plater.model().objects) {
            const OpenScadLink link = forca_openscad_link_from_json(object->forca_openscad);
            if (!link.empty() && !link.file.empty() && fs::equivalent(fs::path(boost::nowide::widen(link.file)), file, ec))
                linked.push_back(object->id());
        }
        if (linked.empty()) {
            m_files.erase(std::remove_if(m_files.begin(), m_files.end(), [&](const File &f) { return f.path == file; }), m_files.end());
            return;
        }
        m_asking = true;
        // TRN %1% is a file name.
        const bool render = MessageDialog(&m_plater, format_wxstr(_L("%1% has changed. Render the model again?"), file.filename().string()),
                                          _L("OpenSCAD"), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION)
                                .ShowModal() == wxID_YES;
        m_asking = false;
        if (!render)
            return;
        const OpenScadInfo info = forca_openscad_find();
        if (!info.usable) {
            show_error(&m_plater, no_openscad_message(info));
            return;
        }
        // Each object with its own values; the object may have gone while the question was open.
        for (const ObjectID &id : linked)
            for (const ModelObject *object : m_plater.model().objects)
                if (object->id() == id) {
                    const OpenScadLink link = forca_openscad_link_from_json(object->forca_openscad);
                    auto               job  = std::make_shared<RenderJob>();
                    job->exe                = info.path;
                    job->dir                = make_work_dir();
                    job->renders.push_back({file, job->dir / (file.stem().string() + ".3mf"), link.values});
                    queue_rerender(m_plater, job, link, id, _u8L("Render OpenSCAD model again"));
                }
    }

    static inline OpenScadWatch *s_instance = nullptr;
    Plater                      &m_plater;
    std::vector<File>            m_files;
    int                          m_paused = 0;
    bool                         m_asking = false;
};

// Keeps the watch from asking while one of Forca's own OpenSCAD dialogs is open.
struct WatchPause
{
    WatchPause()
    {
        if (OpenScadWatch *watch = OpenScadWatch::instance())
            watch->pause();
    }
    ~WatchPause()
    {
        if (OpenScadWatch *watch = OpenScadWatch::instance())
            watch->resume();
    }
    WatchPause(const WatchPause &)            = delete;
    WatchPause &operator=(const WatchPause &) = delete;
};

// Opens the file in OpenSCAD's own window and watches it.
bool open_in_openscad(Plater &plater, const fs::path &file)
{
    const OpenScadInfo info = forca_openscad_find();
    if (info.path.empty()) {
        show_error(&plater, no_openscad_message(info));
        return false;
    }
    const wxString command = "\"" + from_u8(info.path) + "\" \"" + from_path(file) + "\"";
    if (wxExecute(command, wxEXEC_ASYNC) == 0) {
        // TRN %1% is the path to openscad.exe.
        show_error(&plater, format_wxstr(_L("Couldn't start OpenSCAD (%1%)."), info.path));
        return false;
    }
    OpenScadWatch::get(plater).watch(file);
    return true;
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
                std::vector<size_t>        from_render_idx;
                for (size_t idx : objects)
                    if (idx < model_objects.size() &&
                        (fs::path(model_objects[idx]->input_file).stem() == render.out.stem() || rendered.size() == 1)) {
                        from_render.push_back(model_objects[idx]);
                        from_render_idx.push_back(idx);
                    }
                // The objects come in the file's order (D5: their colours to filaments).
                const OpenScadColors colors = forca_openscad_read_colors(render.out.string());
                for (size_t k = 0; k < from_render.size(); ++k) {
                    apply_colors(*from_render[k], colors, k);
                    wxGetApp().obj_list()->update_info_items(from_render_idx[k]);
                }
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
    const WatchPause  pause_watch;
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
    queue_rerender(plater, job, link, object->id(), _u8L("Change OpenSCAD parameters"));
}

void forca_openscad_edit_in_openscad(Plater &plater, size_t obj_idx)
{
    if (obj_idx >= plater.model().objects.size())
        return;
    const OpenScadLink link = forca_openscad_link_from_json(plater.model().objects[obj_idx]->forca_openscad);
    if (link.empty())
        return;
    const fs::path            file(boost::nowide::widen(link.file));
    boost::system::error_code ec;
    if (link.file.empty() || !fs::is_regular_file(file, ec)) {
        // TRN %1% is a file path.
        show_error(&plater, format_wxstr(_L("%1% isn't there any more. OpenSCAD parameters > Edit source... edits the copy kept in "
                                            "the project."),
                                         link.file));
        return;
    }
    open_in_openscad(plater, file);
}

void forca_openscad_new_model(Plater &plater)
{
    wxFileDialog dlg(&plater, _L("Save the new OpenSCAD model as"), from_u8(wxGetApp().app_config->get_last_dir()), "model.scad",
                     _L("OpenSCAD files") + " (*.scad)|*.scad", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    fs::path file = into_path(dlg.GetPath());
    if (!forca_is_scad_file(file))
        file += ".scad";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        out << forca_openscad_starter_model();
        if (!out) {
            // TRN %1% is a file path.
            show_error(&plater, format_wxstr(_L("Couldn't save %1%."), file.string()));
            return;
        }
    }
    forca_openscad_import(plater, {file});
    open_in_openscad(plater, file);
}

namespace {

void queue_rerender(Plater &plater, std::shared_ptr<RenderJob> job, const OpenScadLink &link, ObjectID object_id, std::string undo_name)
{
    auto finish = [job, link, object_id, undo_name, &plater](bool canceled, std::exception_ptr &eptr) {
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
                const WatchPause pause_watch;
                const Render    &render = job->renders.front();
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
        plater.take_snapshot(undo_name);
        ModelObject &object = *objects[obj_idx];
        replace_model_parts(object, *rendered.objects.front());
        apply_colors(object, forca_openscad_read_colors(render.out.string()), 0);
        wxGetApp().obj_list()->update_info_items(obj_idx);
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

} // namespace

OpenScadRun forca_openscad_render_file(const std::string &exe, const fs::path &scad,
                                       const std::vector<std::pair<std::string, std::string>> &values, int timeout_s)
{
    OpenScadRun run;
    run.work_dir  = make_work_dir();
    run.out_3mf   = run.work_dir / ((scad.stem().string().empty() ? std::string("model") : scad.stem().string()) + ".3mf");
    run.scad_text = read_file(scad);
    const RunResult result = run_openscad(exe, forca_openscad_render_args(scad.string(), run.out_3mf.string(), values), run.work_dir, nullptr,
                                          std::chrono::seconds(timeout_s));
    boost::system::error_code ec;
    run.output = result.output;
    run.ok     = result.error.empty() && !result.timed_out && result.exit_code == 0 && fs::file_size(run.out_3mf, ec) > 0 && !ec;
    if (!result.error.empty())
        run.error = "OpenSCAD couldn't be started: " + result.error;
    else if (result.timed_out)
        run.error = "OpenSCAD took longer than " + std::to_string(timeout_s) + " s and was stopped.";
    else if (!run.ok)
        run.error = "OpenSCAD couldn't render the file (exit " + std::to_string(result.exit_code) + ").";
    if (!run.ok)
        fs::remove_all(run.work_dir, ec);
    return run;
}

std::vector<OpenScadParam> forca_openscad_read_params(const std::string &exe, const fs::path &scad, std::string &output, std::string &error)
{
    const fs::path            dir       = make_work_dir();
    const fs::path            json_file = dir / "parameters.json";
    const RunResult           result    = run_openscad(exe, forca_openscad_param_args(scad.string(), json_file.string()), dir, nullptr,
                                                       std::chrono::seconds(60));
    std::vector<OpenScadParam> params;
    output = result.output;
    try {
        params = forca_openscad_parse_params(read_file(json_file));
    } catch (const std::exception &err) {
        error = err.what();
    }
    boost::system::error_code ec;
    fs::remove_all(dir, ec);
    return params;
}

int forca_openscad_place(Plater &plater, const OpenScadRun &run, const fs::path &scad,
                         const std::vector<std::pair<std::string, std::string>> &values, int replace_obj, const std::string &undo_name,
                         std::string &error)
{
    boost::system::error_code ec;
    struct Cleanup
    {
        const fs::path &dir;
        ~Cleanup()
        {
            boost::system::error_code ec;
            fs::remove_all(dir, ec);
        }
    } cleanup{run.work_dir};

    OpenScadLink link;
    link.file   = fs::absolute(scad, ec).string();
    link.scad   = run.scad_text;
    link.values = values;
    ModelObjectPtrs &objects = plater.model().objects;

    if (replace_obj >= 0) {
        if (replace_obj >= int(objects.size())) {
            error = "No object " + std::to_string(replace_obj) + ".";
            return -1;
        }
        Model rendered;
        try {
            rendered = Model::read_from_file(run.out_3mf.string(), nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        } catch (const std::exception &err) {
            error = err.what();
            return -1;
        }
        if (rendered.objects.size() != 1) {
            error = "The render has " + std::to_string(rendered.objects.size()) + " objects; an object can only be replaced by one.";
            return -1;
        }
        plater.take_snapshot(undo_name);
        ModelObject &object = *objects[replace_obj];
        replace_model_parts(object, *rendered.objects.front());
        apply_colors(object, forca_openscad_read_colors(run.out_3mf.string()), 0);
        object.forca_openscad = forca_openscad_link_to_json(link);
        wxGetApp().obj_list()->add_volumes_to_object_in_list(size_t(replace_obj));
        wxGetApp().obj_list()->update_info_items(size_t(replace_obj));
        plater.changed_object(replace_obj);
        return replace_obj;
    }

    const size_t before = objects.size();
    {
        Plater::TakeSnapshot snapshot(&plater, undo_name);
        plater.load_files({run.out_3mf}, LoadStrategy::LoadModel, false);
    }
    if (objects.size() <= before) {
        error = "Forca couldn't load the render.";
        return -1;
    }
    const OpenScadColors colors = forca_openscad_read_colors(run.out_3mf.string());
    for (size_t i = before; i < objects.size(); ++i) {
        ModelObject *object = objects[i];
        apply_colors(*object, colors, i - before);
        wxGetApp().obj_list()->update_info_items(i);
        if (openscad_default_name(object->name)) {
            object->name = scad.stem().string();
            for (ModelVolume *volume : object->volumes)
                if (openscad_default_name(volume->name))
                    volume->name = object->name;
        }
        if (objects.size() == before + 1) // linked only when the file gave one object, as an import
            object->forca_openscad = forca_openscad_link_to_json(link);
    }
    wxGetApp().obj_list()->update_name_for_items();
    return int(before);
}

}} // namespace Slic3r::GUI
