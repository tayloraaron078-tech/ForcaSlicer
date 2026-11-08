#pragma once

// Forca: the printer cards of the Home tab (HQ PLAN_item12_risk_map_obico_home.md, A1). One card per Bambu printer
// bound to the account and per physical printer with a print host: its state, job, progress and time left. The cards
// carry names and states only -- never an address, serial number or key -- since the Home page is a web page.

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r { namespace GUI {

struct ForcaAIHostSeen;

struct ForcaHomeCard
{
    std::string id;    // a Bambu dev_id, or "host:<physical printer>" (ForcaAIHostPrinters.hpp)
    std::string name;
    std::string type;  // "Bambu Lab X1 Carbon", "Klipper", "Flashforge" ...
    // "printing", "paused", "preparing", "idle", "finished", "stopped", "failed", "offline", "connecting" (no read yet),
    // "online" (reachable, no live data: a Bambu printer not selected in the Device tab), "no_status" (Forca can't
    // read this kind of printer) or "unknown".
    std::string state = "unknown";
    std::string job;
    int         progress_pct  = -1;
    int         remaining_min = -1;
    std::string error;         // the printer's own error text
    bool        selected = false; // the printer Forca prints to now
    std::string open;          // what a click opens: "device" (the Device tab), "web" (its web page) or empty
};

// Pure helpers (tested).
nlohmann::json forca_home_card_json(const ForcaHomeCard& card);
// A Bambu printer's print_status (RUNNING, PAUSE, PREPARE, SLICING, FINISH, FAILED, IDLE ...) as a card state.
std::string forca_home_bambu_state(const std::string& print_status);
// Fills a host printer card's state and job from its latest read; `seen` is null before the first read.
void forca_home_fill_host_card(ForcaHomeCard& card, bool can_read_status, const ForcaAIHostSeen* seen);

// GUI thread from here on.
// Every printer's card, as {"command": "forca_printer_cards", "printers": [...]}. While the Home tab is shown, it also
// keeps the host printers' status read every few seconds (forca_ai_host_watch).
nlohmann::json forca_home_printer_cards(bool home_shown);
// The Home tab was hidden: stop reading the host printers for it.
void forca_home_printers_hidden();
// A card was clicked: opens the printer's Device tab or web page. False when it has nothing to open.
bool forca_home_open_printer(const std::string& id);

}} // namespace Slic3r::GUI
