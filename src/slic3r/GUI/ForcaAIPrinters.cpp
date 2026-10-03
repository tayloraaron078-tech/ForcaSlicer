// Forca AI -- Phase 3 printer tools (read-only): which printers the user shares with the AI, and their live status.
//
// Per-printer opt-in (HQ PLAN_forca_ai.md §3a): the AI sees ONLY printers the user ticked in the Forca AI window
// (app config "forca_ai_printers", a list of device ids). The only printer commands are pause and resume-own-pause,
// and only on printers the user ALSO ticked "AI may pause" for ("forca_ai_pause_printers"). Secrets (access codes)
// and addresses are never returned.
#define BAMBU_DYNAMIC // the camera tunnel is loaded from the network plugin at runtime (as wxMediaCtrl3 does)
#include "ForcaAI.hpp"
#include "ForcaAcademy.hpp"

#include "AVVideoDecoder.hpp"
#include "GUI_App.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevBed.h"
#include "DeviceCore/DevChamber.h"
#include "DeviceCore/DevExtruderSystem.h"
#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevHMS.h"
#include "DeviceCore/DevManager.h"
#include "GUI.hpp"
#include "HMS.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "MainFrame.hpp"
#include "Monitor.hpp"
#include "StatusPanel.hpp"
#include "wxMediaCtrl3.h"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp" // data_dir()

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include "libslic3r_version.h"

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <algorithm>
#include <atomic>
#include <future>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <thread>

extern "C" BambuLib* bambulib_get(); // PrinterFileSystem.cpp: the loaded camera-tunnel functions

namespace Slic3r { namespace GUI {

using json = nlohmann::json;

static constexpr const char* CFG_PRINTERS       = "forca_ai_printers";
static constexpr const char* CFG_PAUSE_PRINTERS = "forca_ai_pause_printers";

static std::set<std::string> read_id_set(const char* key)
{
    std::set<std::string> ids;
    if (!wxGetApp().app_config)
        return ids;
    std::vector<std::string> parts;
    const std::string        v = wxGetApp().app_config->get(key);
    boost::split(parts, v, boost::is_any_of(","), boost::token_compress_on);
    for (const std::string& p : parts)
        if (!p.empty())
            ids.insert(p);
    return ids;
}

std::set<std::string> forca_ai_pause_optins()
{
    // "AI may pause" only counts on a printer that is also shared.
    std::set<std::string> shared = forca_ai_printer_optins(), out;
    for (const std::string& id : read_id_set(CFG_PAUSE_PRINTERS))
        if (shared.count(id))
            out.insert(id);
    return out;
}

void forca_ai_set_pause_optin(const std::string& dev_id, bool on)
{
    if (on && !forca_ai_printer_optins().count(dev_id))
        return; // only a printer the AI may use can also be paused by it
    std::set<std::string> ids = read_id_set(CFG_PAUSE_PRINTERS);
    const bool            was = ids.count(dev_id) > 0;
    if (on)
        ids.insert(dev_id);
    else
        ids.erase(dev_id);
    wxGetApp().app_config->set(CFG_PAUSE_PRINTERS, boost::algorithm::join(ids, ","));
    if (was != on)
        ForcaAI::instance().log("printers", std::string(on ? "The AI may now pause" : "The AI may no longer pause") + " printer " +
                                                forca_ai_printer_label(dev_id), true);
}

std::set<std::string> forca_ai_printer_optins()
{
    std::set<std::string> ids;
    if (!wxGetApp().app_config)
        return ids;
    std::vector<std::string> parts;
    const std::string        v = wxGetApp().app_config->get(CFG_PRINTERS);
    boost::split(parts, v, boost::is_any_of(","), boost::token_compress_on);
    for (const std::string& p : parts)
        if (!p.empty())
            ids.insert(p);
    return ids;
}

void forca_ai_set_printer_optin(const std::string& dev_id, bool on)
{
    std::set<std::string> ids = forca_ai_printer_optins();
    const bool            was = ids.count(dev_id) > 0;
    if (on)
        ids.insert(dev_id);
    else
        ids.erase(dev_id);
    wxGetApp().app_config->set(CFG_PRINTERS, boost::algorithm::join(ids, ","));
    if (!on)
        forca_ai_set_pause_optin(dev_id, false); // unsharing a printer also withdraws "AI may pause"
    if (was != on)
        ForcaAI::instance().log("printers", std::string(on ? "The AI may now use" : "The AI may no longer use") + " printer " +
                                                forca_ai_printer_label(dev_id), true);
}

std::vector<std::string> forca_ai_known_printers()
{
    std::vector<std::string> ids;
    if (DeviceManager* dev = wxGetApp().getDeviceManager())
        for (const auto& [id, obj] : dev->get_my_machine_list())
            if (obj)
                ids.push_back(id);
    return ids;
}

std::string forca_ai_printer_label(const std::string& dev_id)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_my_machine(dev_id) : nullptr;
    if (!obj)
        return dev_id;
    return obj->get_dev_name() + " (" + std::string(obj->get_printer_type_display_str().ToUTF8().data()) + ")";
}

namespace {

// The opted-in printer the AI named (device id or exact name), or nullptr + err.
MachineObject* shared_printer(const std::string& which, std::string& err)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev) {
        err = "Forca's printer connection is not available.";
        return nullptr;
    }
    const std::set<std::string> ids = forca_ai_printer_optins();
    MachineObject*              hit = nullptr;
    for (const std::string& id : ids) {
        MachineObject* obj = dev->get_my_machine(id);
        if (obj && (which.empty() || which == id || which == obj->get_dev_name())) {
            if (hit && which.empty()) {
                err = "More than one printer is shared; say which (see forca_list_printers).";
                return nullptr;
            }
            hit = obj;
        }
    }
    if (!hit)
        err = ids.empty() ? "The user has not shared any printer with the AI (Forca AI window -> printers)."
                          : "No shared printer called '" + which + "' (see forca_list_printers).";
    return hit;
}

json printer_summary(MachineObject* obj)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    return { { "printer", obj->get_dev_id() },
             { "name", obj->get_dev_name() },
             { "model", std::string(obj->get_printer_type_display_str().ToUTF8().data()) },
             { "connection", obj->is_lan_mode_printer() ? "lan" : "cloud" },
             { "online", obj->is_online() },
             { "selected_in_device_tab", dev && dev->get_selected_machine() == obj },
             { "ai_may_pause", forca_ai_pause_optins().count(obj->get_dev_id()) > 0 } };
}

struct AIPause
{
    std::string job;         // subtask name at the pause
    std::string reason;
    std::string frame_file;  // saved evidence picture (may be empty)
    std::chrono::steady_clock::time_point at;
    bool        confirmed = false; // the printer has reported PAUSE since
};
std::map<std::string, AIPause> s_ai_pauses; // dev_id -> the AI's own pause (GUI thread only)

ForcaAIResult tool_list_printers(const json&)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev)
        return ForcaAIResult::error("Forca's printer connection is not available.");
    const std::set<std::string> ids = forca_ai_printer_optins();
    json                        list = json::array();
    int                         hidden = 0;
    for (const std::string& id : forca_ai_known_printers()) {
        if (!ids.count(id)) {
            ++hidden;
            continue;
        }
        if (MachineObject* obj = dev->get_my_machine(id))
            list.push_back(printer_summary(obj));
    }
    json j = { { "printers", list } };
    if (hidden > 0)
        j["not_shared"] = std::to_string(hidden) + " other printer(s) Forca knows are not shared with the AI.";
    if (list.empty())
        j["hint"] = "The user shares printers in the Forca AI window (top bar) by ticking them.";
    return ForcaAIResult::json(j);
}

ForcaAIResult tool_printer_status(const json& args)
{
    std::string    err;
    MachineObject* obj = shared_printer(args.value("printer", std::string()), err);
    if (!obj)
        return ForcaAIResult::error(err);

    json j = printer_summary(obj);
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - obj->last_update_time).count();
    j["live"]       = obj->is_connected();
    j["data_age_s"] = age;
    if (!obj->is_connected())
        j["note"] = "No live data from this printer right now: Forca receives live status from the printer selected in "
                    "the Device tab. The values below may be old.";

    j["state"] = obj->print_status; // FINISH, RUNNING, PAUSE, PREPARE, FAILED, IDLE, ...
    if (obj->is_in_printing() || obj->is_in_printing_pause()) {
        j["job"]           = obj->subtask_name;
        j["progress_pct"]  = obj->mc_print_percent;
        j["remaining_min"] = obj->mc_left_time / 60;
        j["layer"]         = { { "current", obj->curr_layer }, { "total", obj->total_layers } };
    }

    json temps = json::object();
    if (DevExtderSystem* ex = obj->GetExtderSystem()) {
        json nozzles = json::array();
        for (int i = 0; i < ex->GetTotalExtderCount(); ++i)
            nozzles.push_back({ { "current", ex->GetNozzleTempCurrent(i) }, { "target", ex->GetNozzleTempTarget(i) } });
        temps["nozzles"] = nozzles;
    }
    if (DevBed* bed = obj->GetBed())
        temps["bed"] = { { "current", std::round(bed->GetBedTemp()) }, { "target", std::round(bed->GetBedTempTarget()) } };
    if (auto chamber = obj->GetChamber())
        temps["chamber"] = std::round(chamber->GetChamberTemp());
    j["temperatures_c"] = temps;

    if (obj->print_error != 0)
        j["print_error"] = obj->get_print_error_str();
    json hms = json::array();
    if (DevHMS* h = obj->GetHMS())
        for (const DevHMSItem& item : h->GetHMSItems()) {
            const std::string code = item.get_long_error_code();
            std::string       text;
            if (HMSQuery* q = wxGetApp().get_hms_query())
                text = q->query_hms_msg(obj, code).ToUTF8().data();
            static const char* levels[] = { "unknown", "fatal", "serious", "common", "info" };
            const int          lvl      = int(item.get_level());
            hms.push_back({ { "code", code }, { "level", lvl >= 0 && lvl <= 4 ? levels[lvl] : "unknown" }, { "message", text } });
        }
    if (!hms.empty())
        j["health_messages"] = hms;
    j["has_camera"] = obj->has_ipcam;

    // What is loaded where, by the slot names forca_request_print's 'trays' takes ("A4", "Ext").
    json sources = json::array();
    auto add     = [&sources](const std::string& slot, DevAmsTray& t) {
        json s = { { "slot", slot }, { "type", t.get_filament_type() }, { "name", t.get_display_filament_type() }, { "color", t.color } };
        if (!t.sub_brands.empty())
            s["brand"] = t.sub_brands;
        sources.push_back(s);
    };
    if (auto fila = obj->GetFilaSystem())
        for (const auto& [ams_id, ams] : fila->GetAmsList())
            for (const auto& [tray_id, tray] : ams->GetTrays())
                if (tray && tray->is_exists && !tray->get_filament_type().empty())
                    try {
                        add(forca_academy_tray_label(std::stoi(ams_id), std::stoi(tray_id)), *tray);
                    } catch (...) {}
    for (DevAmsTray& t : obj->vt_slot)
        if (!t.get_filament_type().empty())
            try {
                add(forca_academy_tray_label(std::stoi(t.id), 0), t);
            } catch (...) {}
    j["filament_sources"] = sources;

    if (auto it = s_ai_pauses.find(obj->get_dev_id()); it != s_ai_pauses.end())
        j["ai_pause"] = { { "reason", it->second.reason }, { "confirmed", it->second.confirmed },
                          { "note", "This pause is the AI's own; forca_resume_print may resume it while it stays paused." } };
    return ForcaAIResult::json(j);
}

// ---- camera ------------------------------------------------------------------------------------

// One frame from a Bambu printer's LAN camera, the way the Device tab's live view gets it (MediaPlayCtrl +
// wxMediaCtrl3): a "bambu:///" tunnel URL, the network plugin's Bambu_* stream functions and Forca's FFmpeg decoder.
// Runs on the server thread (slow I/O); the printer's address and access code stay inside Forca.
std::string camera_url(MachineObject* obj, std::string& err)
{
    if (!obj->has_ipcam) {
        err = "This printer has no camera.";
        return {};
    }
    if (obj->is_camera_busy_off()) {
        err = "The printer's camera is busy (for example while it downloads a file). Try again later.";
        return {};
    }
    const std::string ip = obj->get_dev_ip(), code = obj->get_access_code();
    if (ip.empty() || code.empty() || obj->liveview_local <= MachineObject::LVL_Disable) {
        err = "This printer's camera is not reachable over the local network from Forca (LAN live view is off or "
              "unsupported). Forca AI uses the LAN only.";
        return {};
    }
    std::string url;
    if (obj->liveview_local == MachineObject::LVL_Local)
        url = "bambu:///local/" + ip + ".?port=6000&user=bblp&passwd=" + code;
    else if (obj->liveview_local == MachineObject::LVL_Rtsps)
        url = "bambu:///rtsps___bblp:" + code + "@" + ip + "/streaming/live/1?proto=rtsps";
    else
        url = "bambu:///rtsp___bblp:" + code + "@" + ip + "/streaming/live/1?proto=rtsp";
    NetworkAgent* agent = wxGetApp().getAgent();
    url += "&device=" + obj->get_dev_id();
    url += "&net_ver=" + (agent ? agent->get_version() : std::string());
    url += "&dev_ver=" + obj->get_ota_version();
    url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid");
    url += "&cli_ver=" + std::string(SLIC3R_VERSION);
    return url;
}

std::string encode_jpeg(wxImage image, int max_width);

// Opens the tunnel, decodes the first complete frame, closes it. Empty + err on failure or timeout.
std::string grab_jpeg(const std::string& url, int max_width, std::string& err)
{
    BambuLib* lib = bambulib_get();
    if (!lib || !lib->Bambu_Create || !lib->Bambu_ReadSample) {
        err = "Forca's camera support (the Bambu network plugin) is not installed.";
        return {};
    }
    using clock          = std::chrono::steady_clock;
    const auto deadline  = clock::now() + std::chrono::seconds(20);
    auto       wait_more = [&]() {
        if (clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    };

    Bambu_Tunnel tunnel = nullptr;
    int          error  = lib->Bambu_Create(&tunnel, url.c_str());
    if (error == 0)
        error = lib->Bambu_Open(tunnel);
    if (error == 0)
        error = Bambu_would_block;
    while (error == int(Bambu_would_block) && wait_more())
        error = lib->Bambu_StartStream(tunnel, true);
    Bambu_StreamInfo info;
    if (error == 0)
        error = lib->Bambu_GetStreamInfo(tunnel, 0, &info);

    wxImage image;
    if (error == 0) {
        AVVideoDecoder decoder;
        decoder.open(info);
        const int    w     = info.format.video.width, h = info.format.video.height;
        const int    out_w = (w > 0 && w > max_width) ? max_width : (w > 0 ? w : max_width);
        const wxSize size(out_w, (h > 0 && w > 0) ? h * out_w / w : out_w * 9 / 16);
        Bambu_Sample sample;
        while (error == 0 || error == int(Bambu_would_block)) {
            error = lib->Bambu_ReadSample(tunnel, &sample);
            if (error == int(Bambu_would_block)) {
                if (!wait_more())
                    break;
                continue;
            }
            if (error != 0)
                break;
            decoder.decode(sample);
            if (decoder.toWxImage(image, size) && image.IsOk())
                break; // the first decodable frame (a video stream needs its first key frame)
            if (clock::now() > deadline)
                break;
        }
    }
    if (tunnel) {
        lib->Bambu_Close(tunnel);
        lib->Bambu_Destroy(tunnel);
    }
    if (!image.IsOk()) {
        err = (error == int(Bambu_would_block) || error == 0)
                  ? "The camera did not send a picture within 20 seconds (it may be busy with another viewer)."
                  : "Could not open the printer's camera (error " + std::to_string(error) + ").";
        return {};
    }
    const std::string bytes = encode_jpeg(image, max_width);
    if (bytes.empty())
        err = "Could not encode the camera picture.";
    return bytes;
}

std::string encode_jpeg(wxImage image, int max_width)
{
    if (image.GetWidth() > max_width)
        image.Rescale(max_width, std::max(1, image.GetHeight() * max_width / image.GetWidth()), wxIMAGE_QUALITY_HIGH);
    image.SetOption(wxIMAGE_OPTION_QUALITY, 85);
    wxMemoryOutputStream out;
    if (!image.SaveFile(out, wxBITMAP_TYPE_JPEG))
        return {};
    std::string bytes(out.GetSize(), '\0');
    out.CopyTo(&bytes[0], bytes.size());
    return bytes;
}

// One camera connection at a time. A printer may accept only one viewer, and a refused connection can block inside
// the Bambu plugin with no timeout of its own -- so it runs on its own thread, the tool stops waiting after a while,
// and no second connection starts until the stuck one has ended (2026-09-24: a blocked call on the server thread
// stalled every AI tool).
std::atomic<bool> s_camera_busy{ false };

struct CameraTarget
{
    std::string url, name, error, live_jpeg;
};

struct Frame
{
    std::string jpeg, error, name;
    bool        from_live_view = false;
};

// A camera frame of the printer `find` picks (on the GUI thread), from a non-GUI thread: the Device tab's live frame
// when it shows this printer, else a short own connection on a worker thread (25 s limit, one at a time).
Frame capture_frame_of(std::function<MachineObject*(std::string& err)> find, int max_width)
{
    Frame f;
    auto  target = std::make_shared<CameraTarget>(); // shared: on_gui may time out and run later
    if (!ForcaAI::instance().on_gui([target, find, max_width]() {
            MachineObject* obj = find(target->error);
            if (!obj)
                return;
            target->name = obj->get_dev_name();
            // Forca's Device tab is already showing this camera: take its current frame (no second connection).
            MainFrame* mf = wxGetApp().mainframe;
            // The Device tab is built lazily; get() is null until it has been opened (then no live view exists).
            MonitorPanel* monitor = mf && mf->m_monitor_page ? mf->m_monitor_page->get() : nullptr;
            if (monitor && monitor->get_status_panel())
                if (wxMediaCtrl3* live = monitor->get_status_panel()->forca_media_ctrl()) {
                    wxImage frame;
                    if (live->ForcaCurrentFrame(frame, obj->get_dev_id())) {
                        target->live_jpeg = encode_jpeg(frame, max_width);
                        if (!target->live_jpeg.empty())
                            return;
                    }
                }
            target->url = camera_url(obj, target->error);
        })) {
        f.error = "Forca did not respond (it may be showing a dialog).";
        return f;
    }
    f.name = target->name;
    if (!target->live_jpeg.empty()) {
        f.jpeg           = target->live_jpeg;
        f.from_live_view = true;
        return f;
    }
    if (target->url.empty()) {
        f.error = target->error;
        return f;
    }

    bool expected = false;
    if (!s_camera_busy.compare_exchange_strong(expected, true)) {
        f.error = "An earlier camera request is still waiting for the printer. Try again in a minute, or open the printer's "
                  "live view in Forca's Device tab and ask again (the AI then reads that picture).";
        return f;
    }
    struct Grab { std::string jpeg, err; };
    auto promise = std::make_shared<std::promise<Grab>>();
    auto future  = promise->get_future();
    std::thread([url = target->url, max_width, promise]() {
        Grab g;
        g.jpeg = grab_jpeg(url, max_width, g.err);
        promise->set_value(std::move(g));
        s_camera_busy = false;
    }).detach();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready) {
        f.error = "The printer's camera did not answer within 25 seconds. It probably allows one viewer at a time and "
                  "another is watching (Bambu Handy, Bambuddy, another slicer). If Forca's Device tab shows this printer's "
                  "live view, ask again and the AI reads that picture.";
        return f;
    }
    Grab g  = future.get();
    f.jpeg  = std::move(g.jpeg);
    f.error = std::move(g.err);
    return f;
}

// A camera frame of a printer shared with the AI (the AI tools' gate).
Frame capture_frame(const std::string& which, int max_width)
{
    return capture_frame_of([which](std::string& err) { return shared_printer(which, err); }, max_width);
}

ForcaAIResult tool_camera_snapshot(const json& args) // runs on the server thread
{
    const Frame f = capture_frame(args.value("printer", std::string()), std::clamp(args.value("max_width", 1280), 320, 1920));
    if (f.jpeg.empty())
        return ForcaAIResult::error(f.error);
    std::string text = "Camera of " + f.name + (f.from_live_view ? " (the frame Forca's Device tab live view is showing now)."
                                                                  : " (one frame, just now).");
    // Forca Academy: keep the frame with a recorded print (GUI thread: the Academy settings live there).
    if (const std::string print = args.value("save_to_print", std::string()); !print.empty()) {
        struct Saved { std::string file, err; };
        auto saved = std::make_shared<Saved>(); // shared: on_gui may time out and run later
        if (!ForcaAI::instance().on_gui([saved, print, jpeg = f.jpeg, name = f.name]() {
                const boost::filesystem::path p = forca_academy_save_camera_frame(print, jpeg, name, saved->err);
                if (!p.empty())
                    saved->file = forca_academy_relative(forca_academy_dir(), p);
            }))
            saved->err = "Forca did not respond (it may be showing a dialog).";
        text += saved->file.empty() ? " Not saved to Forca Academy: " + saved->err : " Saved to Forca Academy as " + saved->file + ".";
    }
    ForcaAIResult r = ForcaAIResult::text(text);
    r.add_image(f.jpeg, "image/jpeg");
    return r;
}

// ---- AI pause / resume-own-pause (HQ PLAN_forca_ai.md §2) --------------------------------------------------
//
// The AI MAY pause a print on its own for safety or quality (a separate per-printer "AI may pause" tick), with a
// reason; every AI pause is logged with its reason and a camera frame, and the user sees a card in Forca. The AI MAY
// resume ONLY a pause it made itself: its claim ends the moment Forca sees that print running again (resumed by the
// user, the printer or anyone), or the job changes. Pauses made by anyone else are never resumed by the AI.

std::map<std::string, wxDialog*> s_pause_cards;

void close_pause_card(const std::string& dev_id)
{
    auto it = s_pause_cards.find(dev_id);
    if (it != s_pause_cards.end()) {
        it->second->Destroy();
        s_pause_cards.erase(it);
    }
}

// Watches the AI's pauses (GUI thread, every 2 s while any exists): confirms them, and drops the AI's claim once the
// print is no longer paused -- so a later pause by someone else can never be taken for the AI's.
class PauseWatcher : public wxTimer
{
public:
    void Notify() override
    {
        DeviceManager* dev = wxGetApp().getDeviceManager();
        for (auto it = s_ai_pauses.begin(); it != s_ai_pauses.end();) {
            MachineObject*    obj   = dev ? dev->get_my_machine(it->first) : nullptr;
            const std::string label = forca_ai_printer_label(it->first);
            std::string       end;
            if (!obj)
                end = "the printer is no longer known to Forca";
            else if (!it->second.confirmed) {
                if (obj->print_status == "PAUSE")
                    it->second.confirmed = true;
                else if (std::chrono::steady_clock::now() - it->second.at > std::chrono::seconds(90))
                    end = "the printer never reported the pause (state " + obj->print_status + ")";
            } else if (obj->print_status != "PAUSE" || obj->subtask_name != it->second.job)
                end = "the print is no longer paused (state " + obj->print_status + ")";
            if (!end.empty()) {
                ForcaAI::instance().log("printer pause", "The AI's pause of " + label + " is closed: " + end + ".", true);
                close_pause_card(it->first);
                it = s_ai_pauses.erase(it);
            } else
                ++it;
        }
        if (s_ai_pauses.empty())
            Stop();
    }
};
PauseWatcher& pause_watcher()
{
    static PauseWatcher* w = new PauseWatcher(); // lives for the app's lifetime (a wxTimer must not outlive wx at exit)
    return *w;
}

std::string pause_error(MachineObject* obj)
{
    if (!forca_ai_pause_optins().count(obj->get_dev_id()))
        return "The user has not allowed the AI to pause " + obj->get_dev_name() + " (the \"AI may pause\" tick in the Forca AI window).";
    if (!obj->is_connected())
        return "Forca has no live connection to " + obj->get_dev_name() + " (select it in Forca's Device tab).";
    return {};
}

// The card the user sees in Forca after an AI pause (modeless). "Resume print" is the user's own action.
void show_pause_card(const std::string& dev_id, const std::string& jpeg)
{
    close_pause_card(dev_id);
    const auto it = s_ai_pauses.find(dev_id);
    if (it == s_ai_pauses.end() || !wxGetApp().mainframe)
        return;
    auto* dlg = new wxDialog(wxGetApp().mainframe, wxID_ANY, _L("Forca AI - print paused"), wxDefaultPosition, wxDefaultSize,
                             wxDEFAULT_DIALOG_STYLE);
    s_pause_cards[dev_id] = dlg;
    dlg->SetFont(Label::Body_14);
    const int pad  = dlg->FromDIP(12);
    auto*     root = new wxBoxSizer(wxVERTICAL);
    auto*     title = new wxStaticText(dlg, wxID_ANY, _L("The AI paused a print"));
    title->SetFont(Label::Head_16);
    root->Add(title, 0, wxALL, pad);
    if (!jpeg.empty()) {
        wxMemoryInputStream in(jpeg.data(), jpeg.size());
        wxImage             img(in, wxBITMAP_TYPE_JPEG);
        if (img.IsOk()) {
            const int w = dlg->FromDIP(480);
            if (img.GetWidth() > w)
                img.Rescale(w, img.GetHeight() * w / img.GetWidth(), wxIMAGE_QUALITY_HIGH);
            root->Add(new wxStaticBitmap(dlg, wxID_ANY, wxBitmap(img)), 0, wxLEFT | wxRIGHT, pad);
        }
    }
    wxString text = _L("Printer") + ": " + wxString::FromUTF8(forca_ai_printer_label(dev_id).c_str()) + "\n" +
                    _L("Job") + ": " + wxString::FromUTF8(it->second.job.c_str()) + "\n" + _L("Why") + ": " +
                    wxString::FromUTF8(it->second.reason.c_str());
    auto* facts = new wxStaticText(dlg, wxID_ANY, text);
    facts->Wrap(dlg->FromDIP(480));
    root->Add(facts, 0, wxALL, pad);
    auto* note = new wxStaticText(dlg, wxID_ANY,
        _L("Check the print. Resume it here or on the printer, or stop it on the printer. The AI may resume only this "
           "pause, and only while the print stays paused."));
    note->Wrap(dlg->FromDIP(480));
    note->SetForegroundColour(wxColour("#6B7488"));
    root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    auto* keep   = new wxButton(dlg, wxID_ANY, _L("Keep paused"));
    auto* resume = new wxButton(dlg, wxID_ANY, _L("Resume print"));
    buttons->Add(keep, 0, wxRIGHT, dlg->FromDIP(8));
    buttons->Add(resume, 0);
    root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);
    keep->Bind(wxEVT_BUTTON, [dev_id](wxCommandEvent&) { close_pause_card(dev_id); });
    dlg->Bind(wxEVT_CLOSE_WINDOW, [dev_id](wxCloseEvent&) { close_pause_card(dev_id); });
    resume->Bind(wxEVT_BUTTON, [dev_id](wxCommandEvent&) {
        DeviceManager* dev = wxGetApp().getDeviceManager();
        MachineObject* obj = dev ? dev->get_my_machine(dev_id) : nullptr;
        if (obj && obj->can_resume()) {
            obj->command_task_resume(); // the user's own click
            ForcaAI::instance().log("printer pause", "The user resumed " + forca_ai_printer_label(dev_id) + " from the AI pause card.", true);
        }
        s_ai_pauses.erase(dev_id);
        close_pause_card(dev_id);
    });
    dlg->SetSizerAndFit(root);
    dlg->CenterOnParent();
    dlg->Show();
    dlg->Raise();
}

ForcaAIResult tool_pause_print(const json& args) // runs on the server thread
{
    const std::string which  = args.value("printer", std::string());
    const std::string reason = args.value("reason", std::string());
    if (reason.size() < 10)
        return ForcaAIResult::error("Give the reason for pausing (what you saw), for the user and the log.");

    struct Outcome { std::string error, dev_id, name; };
    auto out = std::make_shared<Outcome>();
    if (!ForcaAI::instance().on_gui([out, which, reason]() {
            MachineObject* obj = shared_printer(which, out->error);
            if (!obj)
                return;
            if (std::string e = pause_error(obj); !e.empty()) {
                out->error = e;
                return;
            }
            if (!obj->can_pause()) {
                out->error = obj->get_dev_name() + " is not printing (state " + obj->print_status + "), so there is nothing to pause.";
                return;
            }
            if (obj->command_task_pause() != 0) {
                out->error = "Forca could not send the pause to " + obj->get_dev_name() + ".";
                return;
            }
            s_ai_pauses[obj->get_dev_id()] = AIPause{ obj->subtask_name, reason, {}, std::chrono::steady_clock::now(), false };
            pause_watcher().Start(2000);
            out->dev_id = obj->get_dev_id();
            out->name   = obj->get_dev_name();
            ForcaAI::instance().log("printer pause", "The AI paused " + forca_ai_printer_label(out->dev_id) + ": " + reason, true);
        }))
        return ForcaAIResult::error("Forca did not respond (it may be showing a dialog).");
    if (out->dev_id.empty())
        return ForcaAIResult::error(out->error);

    // Evidence: a frame right after the pause, saved in Forca's data folder and shown to the user.
    const Frame f    = capture_frame(out->dev_id, 1280);
    auto        file = std::make_shared<std::string>();
    ForcaAI::instance().on_gui([dev_id = out->dev_id, jpeg = f.jpeg, file]() {
        if (!jpeg.empty()) {
            char stamp[32];
            std::time_t t = std::time(nullptr);
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&t));
            const boost::filesystem::path dir = into_path(from_u8(data_dir())) / "forca" / "ai" / "pauses";
            boost::system::error_code     ec;
            boost::filesystem::create_directories(dir, ec);
            const boost::filesystem::path p = dir / (std::string(stamp) + "_" + dev_id + ".jpg");
            boost::filesystem::ofstream   o(p, std::ios::binary);
            o.write(jpeg.data(), std::streamsize(jpeg.size()));
            if (o) {
                *file = into_u8(from_path(p));
                if (auto it = s_ai_pauses.find(dev_id); it != s_ai_pauses.end())
                    it->second.frame_file = *file;
                ForcaAI::instance().log("printer pause", "Camera frame of the pause saved: " + *file, true);
            }
        }
        show_pause_card(dev_id, jpeg);
    });

    ForcaAIResult r = ForcaAIResult::text("Paused " + out->name + ". The user sees a card in Forca with your reason" +
                                          (f.jpeg.empty() ? std::string(" (no camera frame: ") + f.error + ")" : std::string(" and this frame")) +
                                          ". You may resume only this pause (forca_resume_print), and only while it stays paused.");
    if (!f.jpeg.empty())
        r.add_image(f.jpeg, "image/jpeg");
    return r;
}

ForcaAIResult tool_resume_print(const json& args)
{
    std::string    err;
    MachineObject* obj = shared_printer(args.value("printer", std::string()), err);
    if (!obj)
        return ForcaAIResult::error(err);
    const std::string reason = args.value("reason", std::string());
    if (reason.size() < 10)
        return ForcaAIResult::error("Give the reason why the pause was not needed after all.");
    if (std::string e = pause_error(obj); !e.empty())
        return ForcaAIResult::error(e);
    const auto it = s_ai_pauses.find(obj->get_dev_id());
    if (it == s_ai_pauses.end())
        return ForcaAIResult::error("The AI has no pause of its own on " + obj->get_dev_name() + " to resume. A pause by the user, "
                                    "the printer or anyone else is never resumed by the AI.");
    if (!it->second.confirmed || !obj->can_resume() || obj->subtask_name != it->second.job)
        return ForcaAIResult::error("The printer is not (or not yet) in the AI's pause (state " + obj->print_status +
                                    "). Check again in a few seconds.");
    if (obj->command_task_resume() != 0)
        return ForcaAIResult::error("Forca could not send the resume to " + obj->get_dev_name() + ".");
    const std::string label = forca_ai_printer_label(obj->get_dev_id());
    ForcaAI::instance().log("printer pause", "The AI resumed its own pause of " + label + ": " + reason, true);
    close_pause_card(obj->get_dev_id());
    s_ai_pauses.erase(it);
    return ForcaAIResult::text("Resumed " + obj->get_dev_name() + " (the AI's own pause).");
}

} // namespace

void register_forca_ai_printer_tools(ForcaAI& ai)
{
    ai.register_tool({ "forca_list_printers", "Shared printers",
        "The printers the user has shared with the AI (ticked in the Forca AI window): id, name, model, LAN/cloud, "
        "online, and whether it is the printer selected in Forca's Device tab. Other printers are not shown.",
        { { "type", "object" }, { "properties", json::object() } }, tool_list_printers });

    ai.register_tool({ "forca_printer_status", "Printer status",
        "Live status of a shared printer: state, job, progress, remaining time, layer, nozzle/bed/chamber temperatures, "
        "print errors and health (HMS) messages, the filament in each slot (filament_sources: AMS slots and the "
        "external spool), and whether it has a camera. Read-only -- this never sends anything "
        "to the printer. 'live' is false when Forca has no current data (Forca receives live data from the printer "
        "selected in its Device tab).",
        { { "type", "object" },
          { "properties", { { "printer", { { "type", "string" }, { "description", "Printer id or exact name (default: the only shared printer)." } } } } } },
        tool_printer_status });

    ForcaAITool camera{ "forca_camera_snapshot", "Printer camera",
        "One picture from a shared printer's camera over the local network (Bambu printers), to check a print. "
        "Opens a short camera connection, so it can take a few seconds; the printer may refuse while another viewer "
        "holds its camera. Read-only, except that save_to_print keeps the picture with a recorded print in Forca "
        "Academy (its camera/ folder).",
        { { "type", "object" },
          { "properties", { { "printer", { { "type", "string" }, { "description", "Printer id or exact name (default: the only shared printer)." } } },
                            { "max_width", { { "type", "integer" }, { "minimum", 320 }, { "maximum", 1920 } } },
                            { "save_to_print", { { "type", "string" }, { "description", "A print path from forca_academy_list_prints: also save the frame in that print's camera/ folder." } } } } } },
        tool_camera_snapshot };
    camera.off_gui = true;
    ai.register_tool(std::move(camera));

    ForcaAITool pause{ "forca_pause_print", "Pause a print (safety)",
        "Pause a running print on a shared printer the user ALSO allowed the AI to pause, when you see a safety or "
        "quality problem (spaghetti, a detached part, a layer shift, a blob on the nozzle...). Give the reason. Forca "
        "logs it with a camera frame and shows the user a card. Nothing else (stop, cancel, heat, move) is allowed.",
        { { "type", "object" },
          { "properties", { { "printer", { { "type", "string" } } },
                            { "reason", { { "type", "string" }, { "description", "What you saw and why it needs a pause." } } } } },
          { "required", { "reason" } } },
        tool_pause_print };
    pause.off_gui = true;
    ai.register_tool(std::move(pause));

    ai.register_tool({ "forca_resume_print", "Resume the AI's own pause",
        "Resume a print ONLY if the AI paused it itself and it is still in that pause (e.g. the pause turned out to "
        "be unnecessary). Pauses by the user, the printer or anyone else are never resumed by the AI. Give the reason.",
        { { "type", "object" },
          { "properties", { { "printer", { { "type", "string" } } },
                            { "reason", { { "type", "string" } } } } },
          { "required", { "reason" } } },
        tool_resume_print });
}

// Forca Academy's pictures of its own recorded prints (not gated by AI sharing: the user turned the journal on).
bool forca_printer_camera_jpeg(const std::string& dev_id, int max_width, std::string& jpeg, std::string& err)
{
    const Frame f = capture_frame_of(
        [dev_id](std::string& e) -> MachineObject* {
            DeviceManager* dev = wxGetApp().getDeviceManager();
            MachineObject* obj = dev ? dev->get_my_machine(dev_id) : nullptr;
            if (!obj)
                e = "Forca does not know this printer right now.";
            return obj;
        },
        max_width);
    jpeg = f.jpeg;
    err  = f.error;
    return !jpeg.empty();
}

}} // namespace Slic3r::GUI
