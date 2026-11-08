#pragma once

// Forca AI -- printers Forca reaches through a print host (Klipper/Moonraker, stock Flashforge, OctoPrint, ...), next to
// the Bambu printers of ForcaAIPrinters.cpp (HQ PLAN_forca_ai_print_hosts.md). Such a printer is one of Forca's physical
// printers, and its AI id is "host:<physical printer name>": it is shared, paused and given print grants through the
// same opt-in lists and rules as a Bambu printer. Its address and keys never leave Forca.

#include "ForcaHostStatus.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
class MachineObject;
namespace GUI {

bool        forca_ai_is_host_id(const std::string& id);
std::string forca_ai_host_id(const std::string& physical_printer);
// The physical printer's name, or empty when `id` is not a host printer's id.
std::string forca_ai_host_printer_name(const std::string& id);
// "Klipper", "Flashforge", "OctoPrint" ... from a host_type key ("moonraker", "flashforge", "octoprint").
std::string forca_ai_host_type_label(const std::string& host_type);

// GUI thread from here on.
// A Device-tab printer that a non-Bambu printer agent made from a printer preset's print host (dev_id = its address):
// Forca AI reaches it as a print-host printer, "host:<address>", not through the Bambu connection.
bool forca_ai_is_agent_machine(const MachineObject* obj);
// The Bambu printer bound to Forca with this dev_id, or nullptr (also for an agent machine).
MachineObject* forca_ai_bambu_machine(const std::string& dev_id);
// The ids of the print-host printers: Forca's physical printers that have a print host, then the agent machines whose
// address a saved printer preset has (forca_find_host_config).
std::vector<std::string> forca_ai_host_printers();
// A copy of a host printer's settings (address and keys included: for Forca's own requests only). False when it is gone.
bool        forca_ai_host_config(const std::string& id, DynamicPrintConfig& config);
// "Name (Klipper)".
std::string forca_ai_host_label(const std::string& id);
// Whether Forca's Print button sends to this printer now (its print host is the selected printer's).
bool        forca_ai_host_is_send_target(const std::string& id);

// The latest status read of a host printer.
struct ForcaAIHostSeen
{
    bool                                  ok = false; // false: `error` says why the read failed
    ForcaHostStatus                       status;
    std::string                           error;
    std::chrono::steady_clock::time_point at;
};
// Nullptr when it was never read, or not within the last `max_age_s` seconds.
const ForcaAIHostSeen* forca_ai_host_seen(const std::string& id, int max_age_s);
// Keeps reading `id`'s status every few seconds (off the GUI thread) while any owner ("pause", "autonomy") wants it.
void forca_ai_host_watch(const std::string& owner, const std::string& id, bool on);
// `fn` runs on the GUI thread after every read of a watched printer.
void forca_ai_host_on_update(std::function<void(const std::string& id)> fn);

}} // namespace Slic3r::GUI
