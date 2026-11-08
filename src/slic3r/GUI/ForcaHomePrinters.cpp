#include "ForcaHomePrinters.hpp"
#include "ForcaAIHostPrinters.hpp"
#include "ForcaHostStatus.hpp"

#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Notebook.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <wx/string.h>
#include <wx/utils.h>

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* WATCH_OWNER = "home";

std::set<std::string> s_watched; // host printers read for the Home tab

bool has_tab(const char* name) { return wxGetApp().mainframe && wxGetApp().mainframe->m_tabpanel->FindPageByName(name) != wxNOT_FOUND; }

std::string host_web_url(DynamicPrintConfig config)
{
    const std::string url = PrintHost::get_print_host_webui(&config);
    return boost::algorithm::istarts_with(url, "http://") || boost::algorithm::istarts_with(url, "https://") ? url : std::string();
}

ForcaHomeCard bambu_card(MachineObject* obj, const MachineObject* selected)
{
    ForcaHomeCard card;
    card.id       = obj->get_dev_id();
    card.name     = obj->get_dev_name();
    card.type     = obj->get_printer_type_display_str().ToUTF8().data();
    card.selected = obj == selected;
    card.open     = has_tab(TAB_ID_MONITOR) ? "device" : "";
    if (!obj->is_online()) {
        card.state = "offline";
    } else if (!obj->is_connected()) {
        card.state = "online"; // Forca has live data only for the printer selected in the Device tab
    } else {
        card.state = forca_home_bambu_state(obj->print_status);
        if (obj->is_in_printing() || obj->is_in_printing_pause()) {
            card.job           = obj->subtask_name;
            card.progress_pct  = obj->mc_print_percent;
            card.remaining_min = obj->mc_left_time / 60;
        }
        if (obj->print_error != 0)
            card.error = obj->get_print_error_str();
    }
    return card;
}

} // namespace

nlohmann::json forca_home_card_json(const ForcaHomeCard& card)
{
    nlohmann::json j = { { "id", card.id },     { "name", card.name },         { "type", card.type },
                         { "state", card.state }, { "selected", card.selected }, { "open", card.open } };
    if (!card.job.empty())
        j["job"] = card.job;
    if (card.progress_pct >= 0)
        j["progress_pct"] = card.progress_pct;
    if (card.remaining_min >= 0)
        j["remaining_min"] = card.remaining_min;
    if (!card.error.empty())
        j["error"] = card.error;
    return j;
}

std::string forca_home_bambu_state(const std::string& print_status)
{
    static const std::map<std::string, std::string> states = { { "RUNNING", "printing" }, { "PAUSE", "paused" },
                                                               { "PREPARE", "preparing" }, { "SLICING", "preparing" },
                                                               { "FINISH", "finished" },  { "FAILED", "failed" },
                                                               { "IDLE", "idle" } };
    const auto it = states.find(print_status);
    return it != states.end() ? it->second : std::string("unknown");
}

void forca_home_fill_host_card(ForcaHomeCard& card, bool can_read_status, const ForcaAIHostSeen* seen)
{
    if (!can_read_status) {
        card.state = "no_status";
        return;
    }
    if (!seen) {
        card.state = "connecting";
        return;
    }
    if (!seen->ok) {
        card.state = "offline";
        return;
    }
    const ForcaHostStatus& st = seen->status;
    card.state = forca_host_state_name(st.state);
    if (st.state == ForcaHostState::Printing || st.state == ForcaHostState::Paused || st.state == ForcaHostState::Preparing) {
        card.job = st.file;
        if (st.progress >= 0)
            card.progress_pct = int(std::round(st.progress * 100));
        if (st.remaining_s >= 0)
            card.remaining_min = st.remaining_s / 60;
    }
    card.error = st.message;
}

nlohmann::json forca_home_printer_cards(bool home_shown)
{
    nlohmann::json printers = nlohmann::json::array();

    if (DeviceManager* dev = wxGetApp().getDeviceManager()) {
        const MachineObject* selected = dev->get_selected_machine();
        for (const auto& [id, obj] : dev->get_my_machine_list())
            if (obj && !forca_ai_is_agent_machine(obj))
                printers.push_back(forca_home_card_json(bambu_card(obj, selected)));
    }

    std::set<std::string> watch;
    for (const std::string& id : forca_ai_host_printers()) {
        DynamicPrintConfig config;
        if (!forca_ai_host_config(id, config))
            continue;
        const auto*   type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
        ForcaHomeCard card;
        card.id       = id;
        card.name     = forca_ai_host_printer_name(id);
        card.type     = forca_ai_host_type_label(type ? type->serialize() : std::string());
        card.selected = forca_ai_host_is_send_target(id);
        card.open     = card.selected ? "device" : (host_web_url(config).empty() ? "" : "web");
        const bool can_read = forca_host_caps(config).status;
        forca_home_fill_host_card(card, can_read, forca_ai_host_seen(id, 15));
        if (can_read)
            watch.insert(id);
        printers.push_back(forca_home_card_json(card));
    }

    // Read the host printers only while the Home tab is shown; a hidden page's timer may still ask.
    if (!home_shown)
        watch.clear();
    for (const std::string& id : s_watched)
        if (!watch.count(id))
            forca_ai_host_watch(WATCH_OWNER, id, false);
    for (const std::string& id : watch)
        if (!s_watched.count(id))
            forca_ai_host_watch(WATCH_OWNER, id, true);
    s_watched = watch;

    return { { "command", "forca_printer_cards" }, { "printers", printers } };
}

void forca_home_printers_hidden()
{
    for (const std::string& id : s_watched)
        forca_ai_host_watch(WATCH_OWNER, id, false);
    s_watched.clear();
}

bool forca_home_open_printer(const std::string& id)
{
    MainFrame* frame = wxGetApp().mainframe;
    if (!frame)
        return false;

    if (!forca_ai_is_host_id(id)) {
        if (!forca_ai_bambu_machine(id) || !has_tab(TAB_ID_MONITOR))
            return false;
        frame->jump_to_monitor(id);
        return true;
    }

    DynamicPrintConfig config;
    if (!forca_ai_host_config(id, config))
        return false;
    if (forca_ai_host_is_send_target(id)) {
        // The Device tab shows the selected printer's page (as the Go to Device shortcut picks it).
        if (frame->m_tabpanel->SelectPageByName(TAB_ID_MONITOR) || frame->m_tabpanel->SelectPageByName(TAB_ID_MONITOR_WEB))
            return true;
    }
    const std::string url = host_web_url(config);
    return !url.empty() && wxLaunchDefaultBrowser(wxString::FromUTF8(url));
}

}} // namespace Slic3r::GUI
