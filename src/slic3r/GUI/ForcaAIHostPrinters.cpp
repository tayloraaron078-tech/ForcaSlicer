#include "ForcaAIHostPrinters.hpp"
#include "ForcaHostStatus.hpp"

#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI_App.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <wx/app.h>
#include <wx/timer.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* HOST_PREFIX = "host:";
constexpr int         POLL_MS     = 3000;

std::map<std::string, ForcaAIHostSeen>                s_seen;      // id -> latest read
std::set<std::string>                                 s_in_flight; // ids being read now
std::map<std::string, std::set<std::string>>          s_watchers;  // id -> owners that want it read
std::vector<std::function<void(const std::string&)>>  s_listeners;

const PhysicalPrinter* find_printer(const std::string& id)
{
    const std::string name = forca_ai_host_printer_name(id);
    PresetBundle*     bundle = wxGetApp().preset_bundle;
    if (name.empty() || !bundle)
        return nullptr;
    for (const PhysicalPrinter& p : bundle->physical_printers)
        if (p.name == name)
            return &p;
    return nullptr;
}

// The agent machine whose address `host` is, or nullptr.
MachineObject* agent_machine(const std::string& host)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev || host.empty())
        return nullptr;
    for (const auto& [id, obj] : dev->get_my_machine_list())
        if (forca_ai_is_agent_machine(obj) && forca_same_print_host(obj->get_dev_ip(), host))
            return obj;
    return nullptr;
}

void store(const std::string& id, ForcaAIHostSeen seen)
{
    s_in_flight.erase(id);
    s_seen[id] = std::move(seen);
    for (const auto& fn : std::vector<std::function<void(const std::string&)>>(s_listeners)) // a listener may add one
        fn(id);
}

// Reads every watched printer, one request per printer at a time, on worker threads.
class Poller : public wxTimer
{
public:
    void Notify() override
    {
        if (s_watchers.empty()) {
            Stop();
            return;
        }
        std::vector<std::string> ids; // a copy: a listener run by store() may stop watching
        for (const auto& w : s_watchers)
            ids.push_back(w.first);
        for (const std::string& id : ids) {
            if (s_in_flight.count(id))
                continue;
            DynamicPrintConfig config;
            if (!forca_ai_host_config(id, config)) {
                ForcaAIHostSeen gone;
                gone.error = "Forca no longer has this printer in its printer settings";
                gone.at    = std::chrono::steady_clock::now();
                store(id, gone);
                continue;
            }
            s_in_flight.insert(id);
            std::thread([id, config]() {
                ForcaAIHostSeen seen;
                seen.ok = forca_fetch_host_status(config, seen.status, seen.error);
                seen.at = std::chrono::steady_clock::now();
                if (!seen.ok)
                    BOOST_LOG_TRIVIAL(debug) << "Forca AI: status of " << id << " not read (" << seen.error << ")";
                if (wxTheApp)
                    wxTheApp->CallAfter([id, seen]() { store(id, seen); });
            }).detach();
        }
    }
};

Poller& poller()
{
    static Poller* p = new Poller(); // lives for the app (a wxTimer must not outlive wx at exit)
    return *p;
}

} // namespace

bool forca_ai_is_host_id(const std::string& id) { return id.rfind(HOST_PREFIX, 0) == 0 && id.size() > std::string(HOST_PREFIX).size(); }

std::string forca_ai_host_id(const std::string& physical_printer) { return HOST_PREFIX + physical_printer; }

std::string forca_ai_host_printer_name(const std::string& id) { return forca_ai_is_host_id(id) ? id.substr(std::string(HOST_PREFIX).size()) : std::string(); }

std::string forca_ai_host_type_label(const std::string& host_type)
{
    static const std::map<std::string, std::string> names = {
        { "moonraker", "Klipper" },        { "elegoolink", "Elegoo" },       { "octoprint", "OctoPrint" },
        { "flashforge", "Flashforge" },    { "prusalink", "PrusaLink" },     { "prusaconnect", "Prusa Connect" },
        { "crealityprint", "Creality" },   { "duet", "Duet" },               { "repetier", "Repetier" },
        { "simplyprint", "SimplyPrint" },  { "obico", "Obico" },             { "mks", "MKS" },
        { "esp3d", "ESP3D" },              { "astrobox", "AstroBox" },       { "flashair", "FlashAir" },
        { "ultimaker", "UltiMaker" },      { "3dprinteros", "3DPrinterOS" } };
    const auto it = names.find(host_type);
    return it != names.end() ? it->second : std::string("print host");
}

bool forca_ai_is_agent_machine(const MachineObject* obj)
{
    return obj && !obj->printer_agent_id.empty() && obj->printer_agent_id != "bbl" && !obj->get_dev_ip().empty();
}

MachineObject* forca_ai_bambu_machine(const std::string& dev_id)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* obj = dev ? dev->get_my_machine(dev_id) : nullptr;
    return forca_ai_is_agent_machine(obj) ? nullptr : obj;
}

std::vector<std::string> forca_ai_host_printers()
{
    std::vector<std::string> ids, hosts;
    if (PresetBundle* bundle = wxGetApp().preset_bundle)
        for (const PhysicalPrinter& p : bundle->physical_printers)
            if (!p.config.opt_string("print_host").empty()) {
                ids.push_back(forca_ai_host_id(p.name));
                hosts.push_back(p.config.opt_string("print_host"));
            }
    if (DeviceManager* dev = wxGetApp().getDeviceManager())
        for (const auto& [id, obj] : dev->get_my_machine_list()) {
            DynamicPrintConfig config;
            if (!forca_ai_is_agent_machine(obj) || !forca_find_host_config(obj->get_dev_ip(), config))
                continue;
            const bool listed = std::any_of(hosts.begin(), hosts.end(), [&](const std::string& h) { return forca_same_print_host(h, obj->get_dev_ip()); });
            if (!listed)
                ids.push_back(forca_ai_host_id(obj->get_dev_ip()));
        }
    return ids;
}

bool forca_ai_host_config(const std::string& id, DynamicPrintConfig& config)
{
    if (const PhysicalPrinter* p = find_printer(id)) {
        if (p->config.opt_string("print_host").empty())
            return false;
        config = p->config;
        return true;
    }
    // An agent machine: the saved printer settings that have its address.
    const std::string host = forca_ai_host_printer_name(id);
    return agent_machine(host) && forca_find_host_config(host, config);
}

std::string forca_ai_host_label(const std::string& id)
{
    DynamicPrintConfig config;
    if (!forca_ai_host_config(id, config))
        return forca_ai_host_printer_name(id);
    const auto* type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    std::string model;
    if (const MachineObject* obj = find_printer(id) ? nullptr : agent_machine(forca_ai_host_printer_name(id)))
        model = obj->printer_type;
    return forca_ai_host_printer_name(id) + " (" + (model.empty() ? forca_ai_host_type_label(type ? type->serialize() : std::string()) : model) + ")";
}

bool forca_ai_host_is_send_target(const std::string& id)
{
    DynamicPrintConfig config;
    PresetBundle*      bundle = wxGetApp().preset_bundle;
    if (!bundle || !forca_ai_host_config(id, config))
        return false;
    const PhysicalPrinter* p = find_printer(id);
    if (bundle->physical_printers.has_selection() && (!p || bundle->physical_printers.get_selected_printer_name() != p->name))
        return false;
    // The Print button uploads to the selected printer preset's host (Plater::send_gcode_legacy).
    return forca_same_print_host(bundle->printers.get_edited_preset().config.opt_string("print_host"), config.opt_string("print_host"));
}

const ForcaAIHostSeen* forca_ai_host_seen(const std::string& id, int max_age_s)
{
    const auto it = s_seen.find(id);
    if (it == s_seen.end() || std::chrono::steady_clock::now() - it->second.at > std::chrono::seconds(max_age_s))
        return nullptr;
    return &it->second;
}

void forca_ai_host_watch(const std::string& owner, const std::string& id, bool on)
{
    if (on) {
        s_watchers[id].insert(owner);
        if (!poller().IsRunning()) {
            poller().Start(POLL_MS);
            poller().Notify(); // a first read now, not in 3 s
        }
        return;
    }
    const auto it = s_watchers.find(id);
    if (it == s_watchers.end())
        return;
    it->second.erase(owner);
    if (it->second.empty())
        s_watchers.erase(it);
}

void forca_ai_host_on_update(std::function<void(const std::string& id)> fn) { s_listeners.push_back(std::move(fn)); }

}} // namespace Slic3r::GUI
