#include "ForcaFlashforgePanel.hpp"

#include "ForcaHostStatus.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "PrinterWebView.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp" // resources_dir()

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

namespace {

constexpr int POLL_MS = 3000;

const char* state_key(ForcaHostState s)
{
    switch (s) {
    case ForcaHostState::Idle: return "idle";
    case ForcaHostState::Preparing: return "preparing";
    case ForcaHostState::Printing: return "printing";
    case ForcaHostState::Paused: return "paused";
    case ForcaHostState::Finished: return "finished";
    case ForcaHostState::Stopped: return "stopped";
    case ForcaHostState::Failed: return "failed";
    default: return "unknown";
    }
}

// The page's words, translated here so they go through Forca's catalogs.
json labels()
{
    return {
        { "idle", _u8L("Idle") },
        { "preparing", _u8L("Preparing") },
        { "printing", _u8L("Printing") },
        { "paused", _u8L("Paused") },
        { "finished", _u8L("Finished") },
        { "stopped", _u8L("Canceled") },
        { "failed", _u8L("Error") },
        { "unknown", _u8L("Unknown") },
        { "connecting", _u8L("Connecting to printer...") },
        { "no_job", _u8L("No print running") },
        { "layer", _u8L("Layer") },
        { "elapsed", _u8L("Elapsed") },
        { "remaining", _u8L("Remaining") },
        { "nozzle", _u8L("Nozzle") },
        { "bed", _u8L("Bed") },
        { "chamber", _u8L("Chamber") },
        { "filament", _u8L("Filament") },
        { "light", _u8L("Lamp") },
        { "pause", _u8L("Pause") },
        { "resume", _u8L("Resume") },
        { "stop", _u8L("Stop") },
        { "clear_plate", _u8L("Plate is clear") },
        { "offline", _u8L("Offline") },
        { "need_code", _u8L("To show this printer's status, enter its access code (shown on the printer's screen) in the "
                            "printer's network settings, under API Key / Password.") },
    };
}

class FlashforgeHandler final : public PrinterWebViewHandler
{
public:
    explicit FlashforgeHandler(PrinterWebView& owner) : PrinterWebViewHandler(owner), m_alive(std::make_shared<std::atomic<bool>>(true))
    {
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); });
    }

    ~FlashforgeHandler() override
    {
        *m_alive = false; // requests still in flight drop their answers
        m_timer.Stop();
    }

    void on_loaded(wxWebViewEvent&) override
    {
        run("forcaInit(" + json{ { "labels", labels() }, { "dark", wxGetApp().dark_mode() } }.dump() + ")");
        poll();
        m_timer.Start(POLL_MS);
    }

    void on_script_message(wxWebViewEvent& evt) override
    {
        const json msg = json::parse(evt.GetString().ToUTF8().data(), nullptr, false);
        if (!msg.is_object() || msg.value("command", std::string()) != "forca_flashforge")
            return;
        const std::string action = msg.value("action", std::string());
        DynamicPrintConfig* config = active_config();
        if (!config)
            return;
        std::string cmd = "jobCtl_cmd";
        json        args;
        if (action == "pause" || action == "resume")
            args = { { "jobID", "" }, { "action", action == "pause" ? "pause" : "continue" } };
        else if (action == "stop") {
            MessageDialog dlg(owner().GetParent(), _L("Are you sure you want to stop this print?"), _L("Stop"),
                              wxICON_WARNING | wxYES_NO | wxNO_DEFAULT);
            if (dlg.ShowModal() != wxID_YES)
                return;
            args = { { "jobID", "" }, { "action", "cancel" } };
        } else if (action == "clear_plate") { // after a print the printer refuses the next job until the plate is confirmed clear
            cmd  = "stateCtrl_cmd";
            args = { { "action", "setClearPlatform" } };
        } else if (action == "light") {
            cmd  = "lightControl_cmd";
            args = { { "status", msg.value("on", false) ? "open" : "close" } };
        } else
            return;
        BOOST_LOG_TRIVIAL(info) << "Forca Flashforge: " << cmd << " " << args.dump();
        std::thread([config = *config, cmd, args, alive = std::weak_ptr<std::atomic<bool>>(m_alive), this]() {
            std::string err;
            const bool  ok = forca_flashforge_control(config, cmd, args, err);
            if (!ok)
                BOOST_LOG_TRIVIAL(warning) << "Forca Flashforge: command failed: " << err;
            wxGetApp().CallAfter([alive, ok, this]() {
                if (auto a = alive.lock(); a && *a) {
                    if (!ok)
                        run("forcaCommandFailed()");
                    poll();
                }
            });
        }).detach();
    }

private:
    static DynamicPrintConfig* active_config()
    {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        return bundle ? &bundle->printers.get_edited_preset().config : nullptr;
    }

    void run(const std::string& script)
    {
        if (browser())
            WebView::RunScript(browser(), wxString::FromUTF8(script));
    }

    void poll()
    {
        if (m_polling || !owner().IsShownOnScreen())
            return;
        DynamicPrintConfig* config = active_config();
        if (!config)
            return;
        m_polling = true;
        std::thread([config = *config, alive = std::weak_ptr<std::atomic<bool>>(m_alive), this]() {
            ForcaHostStatus st;
            std::string     err;
            const bool      ok = forca_fetch_host_status(config, st, err);
            json            j  = { { "ok", ok } };
            if (!ok)
                j["problem"] = config.opt_string("printhost_apikey").empty() ? "need_code" : "offline";
            else
                j.update({ { "state", state_key(st.state) },     { "file", st.file },
                           { "progress", st.progress },          { "layer", st.layer },
                           { "total_layers", st.total_layers },  { "elapsed_s", st.elapsed_s },
                           { "remaining_s", st.remaining_s },    { "nozzle", st.nozzle_c },
                           { "nozzle_target", st.nozzle_target_c }, { "bed", st.bed_c },
                           { "bed_target", st.bed_target_c },    { "chamber", st.chamber_c },
                           { "filament", st.filament_type },     { "light", st.light_on },
                           { "message", st.message } });
            const std::string script = "forcaStatus(" + j.dump() + ")";
            wxGetApp().CallAfter([alive, script, this]() {
                if (auto a = alive.lock(); a && *a) {
                    m_polling = false;
                    run(script);
                }
            });
        }).detach();
    }

    std::shared_ptr<std::atomic<bool>> m_alive;
    wxTimer                            m_timer;
    bool                               m_polling = false;
};

} // namespace

std::string forca_flashforge_page_url()
{
    std::string path = resources_dir() + "/web/forca/flashforge/index.html";
    std::replace(path.begin(), path.end(), '\\', '/');
    return "file:///" + (path.front() == '/' ? path.substr(1) : path);
}

std::unique_ptr<PrinterWebViewHandler> forca_make_flashforge_handler(PrinterWebView& owner)
{
    return std::make_unique<FlashforgeHandler>(owner);
}

}} // namespace Slic3r::GUI
