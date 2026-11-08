#pragma once

// Forca: live status of network printers that are not Bambu printers -- Klipper printers through Moonraker (host types
// moonraker, elegoolink, octoprint) and stock-firmware Flashforge printers through their local API (port 8898).
// Used by Forca Academy to follow a sent print to its end, and by the Flashforge device panel.
// The fetch functions block on the network: call them off the GUI thread.

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
namespace GUI {

enum class ForcaHostState { Unknown, Idle, Preparing, Printing, Paused, Finished, Stopped, Failed };

struct ForcaHostStatus
{
    ForcaHostState state = ForcaHostState::Unknown;
    std::string    raw_state;       // the printer's own word for it
    std::string    file;            // the job's file name, as the printer reports it
    std::string    message;         // error text, when the printer gives one
    double         progress     = -1; // 0..1
    int            layer        = -1;
    int            total_layers = -1;
    int            elapsed_s    = -1;
    int            remaining_s  = -1;
    double         nozzle_c = -1, nozzle_target_c = -1, bed_c = -1, bed_target_c = -1, chamber_c = -1;
    std::string    filament_type;
    bool           light_on = false;
    std::string    camera_url; // Flashforge: the camera's mjpg stream as the printer reports it (cameraStreamUrl)
    // Klipper printers that went idle: Moonraker's history of the last job (a cancel from Fluidd or a printer macro
    // can reset the printer straight to standby, so the live state alone can't say how the print ended).
    std::string    last_job_file;
    std::string    last_job_end; // "finished" / "stopped" / "failed", empty while unknown
};

// "moonraker" or "flashforge" when Forca can read this printer's status, else empty.
std::string forca_host_status_kind(const DynamicPrintConfig& config);

// The reply parsers (pure, tested): Moonraker's /printer/objects/query result ({"status": {...}} or the status object
// itself) and a Flashforge 'detail' object.
ForcaHostStatus forca_parse_moonraker_status(const nlohmann::json& result);
ForcaHostStatus forca_parse_flashforge_detail(const nlohmann::json& detail);

// How Moonraker's job history says a job ended: "finished" (completed), "stopped" (cancelled), "failed" (error,
// klippy_shutdown, klippy_disconnect, server_exit, interrupted), empty for in_progress / anything else.
std::string forca_moonraker_job_end(const std::string& history_status);

// True when two job file names are the same print: compared without folders, case and the .gcode / .3mf /
// .gcode.3mf ending (printers rename the extension).
bool forca_same_job_file(const std::string& a, const std::string& b);

// Blocking reads (worker thread). `config` holds print_host, host_type, printhost_apikey and, for Flashforge,
// flashforge_serial_number; credentials are only used for the request.
bool forca_fetch_host_status(const DynamicPrintConfig& config, ForcaHostStatus& status, std::string& err);
// One camera picture: Moonraker's webcams, else the cameras Fluidd (or an older Mainsail) saved in Moonraker's
// database. False when the printer has no camera.
bool forca_fetch_host_snapshot(const DynamicPrintConfig& config, std::string& jpeg, std::string& err);

// Blocking: a command to a stock-firmware Flashforge printer's local API (POST /control), e.g. cmd "jobCtl_cmd" with
// args {"jobID": "", "action": "pause" | "continue" | "cancel"}, "lightControl_cmd" with {"status": "open" | "close"},
// or "stateCtrl_cmd" with {"action": "setClearPlatform"} (the plate was cleared after a print).
bool forca_flashforge_control(const DynamicPrintConfig& config, const std::string& cmd, const nlohmann::json& args, std::string& err);

// A camera's still-picture address from what Fluidd / Mainsail saved: a snapshot address as is, an mjpg-streamer
// stream ("?action=stream") turned into "?action=snapshot", a bare address given "?action=snapshot".
std::string forca_camera_snapshot_url(const std::string& camera_url);

// A Flashforge printer's camera stream: the one its status reports, else mjpg-streamer on port 8080 of the printer
// (where the Adventurer 5M's camera streams, also when the firmware reports none).
std::string forca_flashforge_camera_url(const std::string& print_host, const ForcaHostStatus& status);
// The first whole JPEG picture in `data` (an mjpg stream as received so far, or one picture). False until there is one.
bool forca_first_mjpeg_frame(const std::string& data, std::string& jpeg);

// The Snapmaker U1's camera on its stock firmware has no webcam stream: Snapmaker's camera service writes about one
// picture a second to monitor.jpg in Moonraker's "camera" file root while a websocket client keeps
// "camera.start_monitor" going (the firmware stops it 6 minutes after the last start).
// Whether Moonraker's file roots (a /server/files/roots reply) include that "camera" root.
bool forca_u1_camera_root_listed(const nlohmann::json& roots_reply);
// The modification time of monitor.jpg in a /server/files/list?root=camera reply; -1 when it isn't there.
double forca_u1_monitor_stamp(const nlohmann::json& list_reply);
// What the U1 camera keeper does at `now` (seconds on one clock): `wanted` is when a viewer last asked for the camera,
// `started` when it last sent start_monitor (< 0: not yet).
enum class ForcaU1CameraStep { Wait, Start, Stop };
ForcaU1CameraStep forca_u1_camera_step(double now, double wanted, double started);
// Blocking: whether the printer at print_host is a U1 with that camera (asked once per host and session).
bool forca_is_u1_camera_host(const std::string& print_host, const std::string& api_key);
// Keeps the U1 camera at print_host running for a minute from now (a background websocket; call it again to extend).
void forca_u1_camera_want(const std::string& print_host, const std::string& api_key);
// Where the U1's camera picture is, on the printer's own address: what a camera in Fluidd needs as its snapshot URL.
inline constexpr const char* FORCA_U1_SNAPSHOT_PATH = "/server/files/camera/monitor.jpg";
// Whether Moonraker's webcams (a /server/webcams/list reply, where Fluidd saves its cameras) show that picture.
bool forca_u1_snapshot_camera_listed(const nlohmann::json& webcams_reply);
// Blocking: sets `saved` to whether the U1 at print_host has such a camera. False when the printer can't be asked.
bool forca_fetch_u1_snapshot_camera(const std::string& print_host, const std::string& api_key, bool& saved);

// Forca's own live view of a camera that serves one viewer at a time (the Flashforge Device page). While Forca takes a
// picture from that camera, the view is called with true to let go of the stream and with false to take it back; it
// may be called from any thread and returns whether it was showing the stream.
using ForcaCameraViewer = std::function<bool(bool hold)>;
// Registers the view of the camera of the printer at `print_host`; an empty function removes it.
void forca_set_camera_viewer(const std::string& print_host, ForcaCameraViewer viewer);

// While it lives, the registered view of `print_host`'s camera has let go of the stream.
class ForcaCameraLease
{
public:
    explicit ForcaCameraLease(const std::string& print_host);
    ~ForcaCameraLease();
    ForcaCameraLease(const ForcaCameraLease&)            = delete;
    ForcaCameraLease& operator=(const ForcaCameraLease&) = delete;
    // The view was showing the stream, so the camera needs a moment before it takes another viewer.
    bool released() const { return m_released; }

private:
    ForcaCameraViewer m_viewer;
    bool              m_released = false;
};

// What Forca AI can do with a print-host printer (HQ PLAN_forca_ai_print_hosts.md): read its status (and so pause it
// and follow its bed) on Klipper printers and on stock Flashforge printers whose serial number and access code are set,
// and print on them under a grant (plain G-code or a .gcode.3mf upload, as the printer's use_3mf option says).
struct ForcaHostCaps
{
    bool status = false, camera = false, pause = false, grant = false;
};
ForcaHostCaps forca_host_caps(const DynamicPrintConfig& config);

// "idle", "preparing", "printing", "paused", "finished", "stopped", "failed" or "unknown".
std::string forca_host_state_name(ForcaHostState state);
// A print under way, paused or starting -- or a state Forca doesn't know: a grant never sends to it, and a bed marked
// clear stops counting.
bool forca_host_busy(ForcaHostState state);
// Ready for a grant print: idle, or a finished / stopped print waiting for the next one.
bool forca_host_idle(ForcaHostState state);
// The printer's current or last job, compared without folders, case and extension; a Klipper printer that went back to
// standby after a print still gives that print. It changes when the printer starts another print.
std::string forca_host_job_key(const ForcaHostStatus& status);
// Why the print can't be paused now, or empty: only a print under way (a Flashforge printer ignores Pause during its
// start routine, which forca_parse_flashforge_detail reports as Preparing).
std::string forca_host_pause_refusal(const ForcaHostStatus& status);
// Moonraker's address for "pause" or "resume" of the current print, from a print_host as the user typed it.
std::string forca_moonraker_print_url(const std::string& print_host, const std::string& action);
// Whether two print_host values as the user typed them are the same printer (scheme, path and case aside).
bool forca_same_print_host(const std::string& a, const std::string& b);
// Blocking: pauses (true) or resumes (false) the print on a Klipper or stock Flashforge printer.
bool forca_host_pause_print(const DynamicPrintConfig& config, bool pause, std::string& err);

// Klipper web interfaces: "Fluidd" or "Mainsail" when `html` is that interface's page, else empty.
std::string forca_web_interface_name(const std::string& html);
// Blocking (a few seconds): the Fluidd / Mainsail pages a Klipper printer serves at the usual addresses, as
// (name, url) -- the printer's own address, ports 81 / 4408 / 4409 and the /fluidd, /mainsail paths.
std::vector<std::pair<std::string, std::string>> forca_find_web_interfaces(const std::string& host);

// The saved printer settings (current printer, physical printers, printer presets) whose print_host is `host`.
// GUI thread. False when none matches.
bool forca_find_host_config(const std::string& host, DynamicPrintConfig& config);

}} // namespace Slic3r::GUI
