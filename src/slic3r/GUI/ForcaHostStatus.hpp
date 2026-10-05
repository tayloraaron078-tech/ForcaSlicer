#pragma once

// Forca: live status of network printers that are not Bambu printers -- Klipper printers through Moonraker (host types
// moonraker, elegoolink, octoprint) and stock-firmware Flashforge printers through their local API (port 8898).
// Used by Forca Academy to follow a sent print to its end, and by the Flashforge device panel.
// The fetch functions block on the network: call them off the GUI thread.

#include <nlohmann/json.hpp>

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

// Klipper web interfaces: "Fluidd" or "Mainsail" when `html` is that interface's page, else empty.
std::string forca_web_interface_name(const std::string& html);
// Blocking (a few seconds): the Fluidd / Mainsail pages a Klipper printer serves at the usual addresses, as
// (name, url) -- the printer's own address, ports 81 / 4408 / 4409 and the /fluidd, /mainsail paths.
std::vector<std::pair<std::string, std::string>> forca_find_web_interfaces(const std::string& host);

// The saved printer settings (current printer, physical printers, printer presets) whose print_host is `host`.
// GUI thread. False when none matches.
bool forca_find_host_config(const std::string& host, DynamicPrintConfig& config);

}} // namespace Slic3r::GUI
