// Forca AI, P5 power tool (HQ PLAN_forca_ai.md): forca_run_python runs a Python script in Forca's embedded Python, with
// the plugin API (`import orca`: the model, presets, sliced print, a 3MF copy of the project) at hand. Only at the
// Advanced control level, which only the user turns on. The script runs under the plugin audit hook as
// kForcaAIScriptKey, whose limits no grant lifts (PluginAuditManager.hpp): it writes only in its own folder
// (<Documents>/Forca AI/Scripts/<time>, handed to it as WORK_DIR), never uses the network, starts programs or threads;
// reading elsewhere is asked of the user as for any plugin. The plugin API has nothing that prints, so R2 holds too.
// A script that runs past its time limit is stopped with TimeoutError (pure-Python loops; a long call into C cannot be
// interrupted until it returns).

#include "ForcaAI.hpp"
#include "ForcaAIFiles.hpp"
#include "GUI.hpp" // from_path / into_u8
#include "GUI_App.hpp"
#include "Plater.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <exception>
#include <ios>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The Python headers come last (PluginAuditManager pulls them in): they must follow the wx and standard headers.
#include "slic3r/plugin/PluginAuditManager.hpp"
#include "slic3r/plugin/PluginLoader.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
namespace py = pybind11;
using json   = nlohmann::json;

namespace {

constexpr int    DEFAULT_TIMEOUT_S = 30;
constexpr int    MAX_TIMEOUT_S     = 300;
constexpr size_t MAX_OUTPUT        = 20000; // characters of output returned to the AI

// <Documents>/Forca AI/Scripts/<date time>, a new folder for each run.
fs::path new_script_dir(std::string& err)
{
    const std::time_t now = std::time(nullptr);
    char              stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H%M%S", std::localtime(&now));
    const fs::path            base = forca_ai_default_dir() / "Scripts";
    fs::path                  dir  = base / stamp;
    boost::system::error_code ec;
    for (int n = 2; fs::exists(dir, ec); ++n)
        dir = base / (std::string(stamp) + " " + std::to_string(n));
    fs::create_directories(dir, ec);
    if (ec) {
        err = "Forca could not make the script folder " + into_u8(from_path(dir)) + ": " + ec.message();
        return {};
    }
    return dir;
}

struct ScriptRun
{
    std::string output;
    std::string error;
    bool        timed_out = false;
};

// Runs `code` on this (the GUI) thread with the GIL, as kForcaAIScriptKey, with `dir` as its only writable folder.
ScriptRun run_script(const std::string& code, const fs::path& dir, int timeout_s)
{
    ScriptRun                run;
    std::mutex               mutex;
    std::condition_variable  finished_cv;
    bool                     finished = false;
    std::atomic<bool>        fired{ false };
    std::atomic<bool>        done{ false };
    unsigned long            thread_id = 0;
    std::thread              watchdog;
    {
        PythonGILState gil;
        if (!gil) {
            run.error = "Forca's Python is shutting down.";
            return run;
        }
        try {
            py::module_ sys     = py::module_::import("sys");
            py::module_ io      = py::module_::import("io");
            py::object  old_out = sys.attr("stdout");
            py::object  old_err = sys.attr("stderr");
            py::object  old_bc  = sys.attr("dont_write_bytecode");
            py::object  buffer  = io.attr("StringIO")();
            py::dict    globals;
            globals["__builtins__"] = py::module_::import("builtins");
            globals["__name__"]     = "__forca_ai__";
            globals["WORK_DIR"]     = into_u8(from_path(dir));
            sys.attr("stdout")              = buffer;
            sys.attr("stderr")              = buffer;
            sys.attr("dont_write_bytecode") = true; // no __pycache__ writes outside WORK_DIR

            // Stops the script when it runs too long: an exception raised in this thread at the next bytecode.
            thread_id = PyThread_get_thread_ident();
            watchdog  = std::thread([&]() {
                std::unique_lock<std::mutex> lock(mutex);
                if (finished_cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return finished; }))
                    return;
                lock.unlock();
                PythonGILState watchdog_gil;
                if (watchdog_gil && !done.load()) {
                    PyThreadState_SetAsyncExc(thread_id, PyExc_TimeoutError);
                    fired = true;
                }
            });

            {
                ScopedPluginAuditContext audit(kForcaAIScriptKey, "forca_run_python");
                PluginAuditManager::instance().add_scoped_allowed_root(dir, /*allow_write=*/true);
                try {
                    py::exec(code, globals);
                } catch (py::error_already_set& e) {
                    run.timed_out = fired.load() && e.matches(PyExc_TimeoutError);
                    run.error     = e.what();
                }
                // Done while this thread still holds the GIL, so the watchdog can no longer raise; drop a raise it
                // queued between the end of the script and here.
                done = true;
                PyThreadState_SetAsyncExc(thread_id, nullptr);
            }
            run.output = buffer.attr("getvalue")().cast<std::string>();
            sys.attr("stdout")              = old_out;
            sys.attr("stderr")              = old_err;
            sys.attr("dont_write_bytecode") = old_bc;
        } catch (const std::exception& e) {
            done = true;
            run.error = e.what();
        }
        PluginAuditManager::instance().clear_audit_denial();
        PluginAuditManager::instance().clear_last_violation();
    }
    // The GIL is released: the watchdog may be waiting for it.
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
    }
    finished_cv.notify_all();
    if (watchdog.joinable())
        watchdog.join();
    return run;
}

std::string clip(const std::string& s)
{
    return s.size() <= MAX_OUTPUT ? s : s.substr(0, MAX_OUTPUT) + "\n... (" + std::to_string(s.size() - MAX_OUTPUT) + " more characters)";
}

ForcaAIResult tool_run_python(const json& args)
{
    if (ForcaAI::instance().level() != ForcaAI::Level::Advanced)
        return ForcaAIResult::error("forca_run_python works only at the Advanced control level, which only the user can "
                                    "turn on (Forca AI window). Ask them, or do this with the other tools.");
    const std::string code = args.value("code", std::string());
    if (code.empty())
        return ForcaAIResult::error("Give the Python code to run in 'code'.");
    const int timeout_s = std::clamp(args.value("timeout_s", DEFAULT_TIMEOUT_S), 1, MAX_TIMEOUT_S);
    if (!wxGetApp().plater())
        return ForcaAIResult::error("Forca is still starting up.");

    PythonInterpreter& python = PythonInterpreter::instance();
    if (!python.is_initialized() && !python.initialize())
        return ForcaAIResult::error("Forca's Python could not start: " + python.last_error());

    std::string    err;
    const fs::path dir = new_script_dir(err);
    if (dir.empty())
        return ForcaAIResult::error(err);
    // The script itself is kept beside what it writes, so the user can see what ran.
    const fs::path script = dir / "script.py";
    {
        boost::nowide::ofstream out(script.string(), std::ios::binary);
        out << code;
    }
    forca_ai_record_write(script);

    const auto      started = std::chrono::steady_clock::now();
    const ScriptRun run     = run_script(code, dir, timeout_s);
    const double    seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    // What the script wrote is the AI's own (R1 ledger).
    json                      files = json::array();
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->path(), ec) && it->path() != script) {
            forca_ai_record_write(it->path());
            files.push_back(into_u8(from_path(it->path())));
        }

    json j = { { "ok", run.error.empty() },
               { "seconds", std::round(seconds * 10) / 10 },
               { "work_dir", into_u8(from_path(dir)) },
               { "output", clip(run.output) },
               { "files", files } };
    if (!run.error.empty())
        j["error"] = clip(run.error);
    if (run.timed_out)
        j["hint"] = "The script ran past timeout_s (" + std::to_string(timeout_s) + " s) and was stopped.";
    else if (run.error.find("numpy") != std::string::npos)
        // The plugin API's own message tells plugin authors to declare numpy, which a script cannot do.
        j["hint"] = "numpy is not installed in Forca's Python, and a script cannot install it. Use mesh.vertex(i) and "
                    "mesh.triangle(i) (tuples) and plain Python maths instead, or ask the user to click Install NumPy "
                    "in the Forca AI window.";
    return ForcaAIResult::json(j);
}

} // namespace

bool forca_ai_numpy_installed()
{
    boost::system::error_code ec;
    return fs::exists(fs::path(PythonInterpreter::shared_packages_dir()) / "numpy" / "__init__.py", ec);
}

std::string forca_ai_install_numpy()
{
    std::string error;
    return plugin_loader::install_packages({ "numpy" }, error) ? std::string() : error;
}

void register_forca_ai_python_tools(ForcaAI& ai)
{
    ai.register_tool(
        { "forca_run_python", "Run a Python script",
          "ADVANCED LEVEL ONLY. Run Python 3 code inside Forca, in the same embedded Python plugins use, and return what "
          "it printed. `import orca` gives the plugin API: orca.host.model() (objects, volumes, instances, meshes), "
          "orca.host.preset_bundle() to read presets, orca.host.plater().export_3mf_copy(path), "
          "orca.host.app_info(). In this API nearly everything is a method call: model.objects(), obj.volumes(), "
          "obj.instances(), vol.mesh(), mesh.triangle_count(), obj.bounding_box(); only name, module_name, input_file "
          "and printable are plain attributes (obj.name). help(x) or dir(x) lists the rest. numpy is NOT part of "
          "Forca's Python (it is there only when the user installed it from the Forca AI window): read meshes with mesh.vertex(i) "
          "and mesh.triangle(i), which return tuples; mesh.vertices(), mesh.triangles(), mesh.face_normals() and "
          "matrix() need numpy. To change settings use forca_set_settings, not the preset "
          "API (its begin_update() is for plugin worker threads). WORK_DIR is a new "
          "folder for this run: the ONLY place the script may write (anything else, the network, programs and threads "
          "are refused, whatever the user has granted); reading other files asks the user. Nothing in the API prints. "
          "The script runs on Forca's main thread, so keep it short; it is stopped after timeout_s.",
          { { "type", "object" },
            { "properties",
              { { "code", { { "type", "string" }, { "description", "Python 3 source to run." } } },
                { "timeout_s",
                  { { "type", "integer" }, { "minimum", 1 }, { "maximum", MAX_TIMEOUT_S },
                    { "description", "Stop the script after this many seconds (default 30)." } } } } },
            { "required", { "code" } } },
          tool_run_python });
}

}} // namespace Slic3r::GUI
