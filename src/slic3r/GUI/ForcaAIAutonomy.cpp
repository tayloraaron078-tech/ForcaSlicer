// Forca AI -- pre-approved autonomy (HQ PLAN_forca_ai.md §10a): the user gives the AI a print grant in the Forca AI
// window (Advanced level only): up to N prints, on printers they tick, until a time, each no longer than a limit.
// Within it forca_request_print(start: true) sends a print without the user's click -- but only:
//   - on a Bambu printer that is shared with the AI, selected in the Device tab, live and not busy;
//   - when that printer's bed is clear: the user clicked "Bed is clear" in Forca and the printer has run nothing
//     since, or (only if the grant allows it) the AI's own camera check (forca_bed_check) is fresh. One clearing is
//     good for one print; any print Forca sees on that printer ends it;
//   - from the printer slots the AI names for each filament ("A4", "Ext"), each holding that filament's material:
//     Forca sets the print dialog's mapping to them and checks it before the countdown and again before Send (the
//     dialog's own auto-mapping may pick another slot -- 2026-09-29 it chose an AMS tray over the loaded external spool);
//   - through Orca's own print dialog, with all its checks: Forca opens it, waits until it is ready with nothing for
//     the user to confirm, shows a 10 s countdown with Cancel, checks everything again and presses Send. Wherever
//     the dialog would ask the user to confirm a warning, the AI's send stops instead.
// A grant may also name the files to print (with copies and a note for the AI): then every object on a grant plate
// must come from those files (ModelObject::input_file), up to the copies still owed, and the grant ends when all are
// sent. The AI picks the settings, following the user's note.
// The grant lives in memory only: restarting Forca, choosing the Guarded level, expiry, using it up or Revoke end it.
#include "ForcaAI.hpp"
#include "ForcaAIFiles.hpp"
#include "ForcaAcademy.hpp"

#include "DeviceCore/DevDefs.h"
#include "DeviceCore/DevFilaSystem.h"
#include "libslic3r/ProjectTask.hpp"

#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp"
#include "libslic3r/ForcaAISafePath.hpp"
#include "libslic3r/Model.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/timer.h>

#include <chrono>
#include <ctime>
#include <map>
#include <memory>

namespace Slic3r { namespace GUI {

using json   = nlohmann::json;
namespace fs = boost::filesystem;

namespace {

constexpr int COUNTDOWN_S       = 10;  // the visible countdown before Forca presses Send
constexpr int READY_TIMEOUT_S   = 60;  // how long the print dialog may take to become ready
constexpr int BED_CHECK_VALID_S = 180; // a camera bed check is good for this long

ForcaAIGrant s_grant; // id 0 = none (GUI thread only)
int          s_next_grant = 1;

struct Bed
{
    bool        clear = false;
    std::string job_key; // the printer's job identity when it was cleared: a print since then changes it
    std::string how;     // "marked clear by the user" / "camera check #n"
    std::time_t at = 0;
};
std::map<std::string, Bed> s_beds; // dev_id -> bed state (GUI thread only)

struct BedCheck
{
    std::string dev_id, job_key, file;
    std::time_t at = 0;
};
std::map<int, BedCheck> s_checks; // the AI's camera bed checks (GUI thread only)
int                     s_next_check = 1;

std::string hhmm(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

// What identifies the printer's current / last job: it changes whenever the printer starts another print.
std::string job_key(MachineObject* o) { return o->task_id_ + "|" + o->subtask_id_ + "|" + o->subtask_name; }

bool is_busy(MachineObject* o)
{
    const std::string& s = o->print_status;
    return o->is_in_printing() || o->is_in_printing_pause() || s == "RUNNING" || s == "PAUSE" || s == "PREPARE" || s == "SLICING";
}

MachineObject* machine(const std::string& dev_id)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    return dev ? dev->get_my_machine(dev_id) : nullptr;
}

bool files_left(const ForcaAIGrant& g)
{
    return g.files.empty() || std::any_of(g.files.begin(), g.files.end(), [](const ForcaAIGrantFile& f) { return f.done < f.copies; });
}

bool grant_active()
{
    return s_grant.id != 0 && s_grant.prints_left > 0 && files_left(s_grant) && std::time(nullptr) < s_grant.expires &&
           ForcaAI::instance().level() == ForcaAI::Level::Advanced;
}

// Each printable object on the plate (0-based): its source file and how many of its instances are on that plate.
std::vector<std::pair<std::string, int>> plate_objects(int plate)
{
    std::vector<std::pair<std::string, int>> out;
    Plater*                                  p = wxGetApp().plater();
    if (!p || plate < 0 || plate >= p->get_partplate_list().get_plate_count())
        return out;
    PartPlate*   pp    = p->get_partplate_list().get_plate(plate);
    const Model& model = p->model();
    for (int i = 0; i < int(model.objects.size()); ++i) {
        const ModelObject* o = model.objects[i];
        int                n = 0;
        for (int k = 0; k < int(o->instances.size()); ++k)
            if (o->instances[k]->printable && pp->contain_instance(i, k))
                ++n;
        if (n > 0)
            out.emplace_back(o->input_file, n);
    }
    return out;
}

// Ends the grant when it expires, and a bed's "clear" as soon as Forca sees that printer busy (any print, anyone's).
class Watcher : public wxEvtHandler
{
public:
    Watcher()
    {
        m_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    }
    void ensure_running()
    {
        if (!m_timer.IsRunning())
            m_timer.Start(5000);
    }

private:
    void tick()
    {
        if (s_grant.id != 0 && !grant_active())
            forca_ai_revoke_grant(s_grant.prints_left <= 0 ? "it is used up"
                                  : !files_left(s_grant) ? "all its files are printed"
                                                         : "it expired");
        bool any_clear = false;
        for (auto& [dev_id, bed] : s_beds) {
            if (!bed.clear)
                continue;
            MachineObject* o = machine(dev_id);
            if (o && o->is_connected() && (is_busy(o) || job_key(o) != bed.job_key)) {
                bed.clear = false;
                ForcaAI::instance().log("bed", forca_ai_printer_label(dev_id) + " is printing, so its bed no longer counts as clear.", true);
            }
            any_clear |= bed.clear;
        }
        if (s_grant.id == 0 && !any_clear)
            m_timer.Stop();
    }
    wxTimer m_timer;
};

Watcher& watcher()
{
    static Watcher* w = new Watcher(); // lives for the app
    return *w;
}

std::string bed_reason(MachineObject* o, const ForcaAIGrant& g, int bed_check)
{
    const std::string id    = o->get_dev_id();
    const std::string label = forca_ai_printer_label(id);
    if (bed_check > 0) {
        if (!g.camera_check)
            return "The grant does not let the AI clear a bed by camera; the user has to click 'Bed is clear' in the Forca AI window.";
        auto it = s_checks.find(bed_check);
        if (it == s_checks.end())
            return "There is no bed check #" + std::to_string(bed_check) + " (take one with forca_bed_check).";
        if (it->second.dev_id != id)
            return "Bed check #" + std::to_string(bed_check) + " was of another printer.";
        if (std::time(nullptr) - it->second.at > BED_CHECK_VALID_S)
            return "Bed check #" + std::to_string(bed_check) + " is more than 3 minutes old; take a new one.";
        if (job_key(o) != it->second.job_key)
            return "The printer has run something since bed check #" + std::to_string(bed_check) + "; take a new one.";
        return {};
    }
    auto it = s_beds.find(id);
    if (it == s_beds.end() || !it->second.clear)
        return "The bed of " + label + " is not marked clear. Ask the user to clear it and click 'Bed is clear' in the Forca "
               "AI window" + (g.camera_check ? std::string(", or check it yourself with forca_bed_check and pass bed_check") : "") + ".";
    if (job_key(o) != it->second.job_key) {
        it->second.clear = false;
        return "The printer has run a print since its bed was marked clear; the user has to clear it again.";
    }
    return {};
}

std::string tray_label(const std::string& ams_id, const std::string& slot_id)
{
    try {
        return forca_academy_tray_label(std::stoi(ams_id), slot_id.empty() ? 0 : std::stoi(slot_id));
    } catch (...) {
        return ams_id + "/" + slot_id;
    }
}

// Every filament the plate uses must come from a named printer slot that holds that filament's material.
std::string tray_reason(MachineObject* o, const std::map<int, std::string>& trays, const std::vector<int>& used)
{
    const DynamicPrintConfig  config = wxGetApp().preset_bundle->full_config();
    const ConfigOptionStrings* types = config.option<ConfigOptionStrings>("filament_type");
    for (int slot : used) {
        const std::string n  = std::to_string(slot);
        auto              it = trays.find(slot);
        if (it == trays.end())
            return "Say which printer slot filament " + n + " prints from: trays {\"" + n + "\": \"A1\"} or {\"" + n +
                   "\": \"Ext\"} (forca_printer_status lists filament_sources).";
        std::string ams, tray;
        if (!forca_ai_parse_tray(it->second, ams, tray))
            return "'" + it->second + "' is not a printer slot name (A1..D4, HT-A, Ext, Ext 2).";
        if (!o->contains_tray(ams, tray))
            return "The printer has no slot " + it->second + ".";
        DevAmsTray        t    = o->get_tray(ams, tray);
        const std::string have = t.get_filament_type();
        const std::string want = types && size_t(slot) <= types->size() ? types->get_at(slot - 1) : std::string();
        if ((!devPrinterUtil::IsVirtualSlot(ams) && !t.is_exists) || have.empty())
            return "Forca does not know of any filament in slot " + it->second + " (empty, or not set on the printer).";
        if (!want.empty() && !boost::iequals(have, want))
            return "Slot " + it->second + " holds " + have + ", but filament " + n + " is " + want + ".";
    }
    return {};
}

// Whether the print dialog's mapping is exactly the slots the AI named.
std::string mapping_reason(SelectMachineDialog* dlg, const std::map<int, std::string>& trays, const std::vector<int>& used)
{
    const std::vector<FilamentInfo>& list = dlg->get_ams_mapping_list();
    for (int slot : used) {
        std::string ams, tray;
        forca_ai_parse_tray(trays.at(slot), ams, tray);
        auto f = std::find_if(list.begin(), list.end(), [slot](const FilamentInfo& i) { return i.id == slot - 1; });
        if (f == list.end())
            return "the print dialog has no slot for filament " + std::to_string(slot);
        if (f->ams_id != ams || (!devPrinterUtil::IsVirtualSlot(ams) && f->slot_id != tray))
            return "the print dialog maps filament " + std::to_string(slot) + " to " + tray_label(f->ams_id, f->slot_id) +
                   ", not " + trays.at(slot);
    }
    return {};
}

// The countdown card over the print dialog, and the steps from "dialog open" to "Send pressed".
class GrantSender : public wxEvtHandler
{
public:
    explicit GrantSender(ForcaAIGrantPrint job) : m_job(std::move(job))
    {
        m_timer.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { tick(); });
    }

    // Opens the print dialog (modal: returns when it closes) and drives it from the timer meanwhile.
    void run()
    {
        m_opened  = std::chrono::steady_clock::now();
        m_timer.Start(500);
        BOOST_LOG_TRIVIAL(info) << "[forca-grant] request " << m_job.request << ": opening the print dialog";
        wxGetApp().plater()->print_current_plate();
        BOOST_LOG_TRIVIAL(info) << "[forca-grant] request " << m_job.request << ": print dialog closed";
        m_timer.Stop();
        if (!m_finished)
            finish("cancelled", "the print dialog was closed before the AI sent it");
        close_card();
    }

private:
    int seconds_open() const
    {
        return int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_opened).count());
    }

    void finish(const std::string& state, const std::string& detail)
    {
        m_finished = true;
        BOOST_LOG_TRIVIAL(info) << "[forca-grant] request " << m_job.request << ": " << state << " -- " << detail;
        m_timer.Stop();
        m_job.done(state, detail);
    }

    // Stops the AI's send; the print dialog stays open for the user to send or close.
    void refuse(const std::string& why)
    {
        finish("void", why + ". Nothing was sent; the print dialog stays open for the user.");
        if (m_text) {
            m_text->SetLabel(_L("The AI did not send this print") + ": " + wxString::FromUTF8(why.c_str()));
            m_text->Wrap(m_card->FromDIP(420));
            m_cancel->SetLabel(_L("Close"));
            m_card->Fit();
        }
    }

    void close_card()
    {
        if (m_card) {
            m_card->Destroy();
            m_card = nullptr;
            m_text = nullptr;
        }
    }

    void show_card(wxWindow* over)
    {
        // A child of the modal print dialog, so it stays clickable while that dialog is up.
        m_card = new wxDialog(over, wxID_ANY, _L("Forca AI"), wxDefaultPosition, wxDefaultSize, wxCAPTION);
        m_card->SetFont(Label::Body_14);
        const int pad  = m_card->FromDIP(12);
        auto*     root = new wxBoxSizer(wxVERTICAL);
        m_text = new wxStaticText(m_card, wxID_ANY, "");
        root->Add(m_text, 0, wxALL, pad);
        m_cancel = new wxButton(m_card, wxID_ANY, _L("Cancel"));
        m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (!m_finished)
                finish("cancelled", "the user cancelled the AI's send; the print dialog stays open for them");
            close_card();
        });
        root->Add(m_cancel, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_RIGHT, pad);
        m_card->SetSizer(root);
        update_card();
        m_card->Show();
        m_card->Raise();
    }

    void update_card()
    {
        if (!m_text)
            return;
        m_text->SetLabel(wxString::Format(_L("The AI is sending this print in %d s (print grant #%d)."), (m_ticks + 1) / 2, s_grant.id));
        m_card->Fit();
        m_card->CentreOnParent();
    }

    // Logs the send's progress once per change (dialog shown, slots mapped, countdown running), not every tick.
    void log_state(bool shown)
    {
        const std::string state = std::string("dialog ") + (shown ? "shown" : "not shown") + ", slots " +
                                  (m_mapped ? "mapped" : "not mapped") + (m_ticks >= 0 ? ", counting down" : "");
        if (state != m_logged) {
            m_logged = state;
            BOOST_LOG_TRIVIAL(info) << "[forca-grant] request " << m_job.request << ": " << state;
        }
    }

    void tick()
    {
        if (m_finished)
            return;
        auto* dlg = dynamic_cast<SelectMachineDialog*>(wxGetApp().plater()->get_select_machine_dialog());
        log_state(dlg != nullptr && dlg->IsShown());
        if (!dlg || !dlg->IsShown()) {
            if (seconds_open() > 20)
                refuse("the print dialog did not open");
            return;
        }
        std::string why;
        // Once the dialog has made its own mapping (it maps only while it has none), set the slots the AI named.
        if (!m_mapped && !dlg->get_ams_mapping_list().empty()) {
            m_mapped = true;
            for (int slot : m_job.used) {
                std::string ams, tray;
                forca_ai_parse_tray(m_job.trays.at(slot), ams, tray);
                if (!dlg->forca_set_mapping(slot - 1, ams, tray, why)) {
                    show_card(dlg);
                    refuse(why);
                    return;
                }
            }
        }
        if (!m_mapped)
            why = "the print dialog has not mapped the filaments yet";
        else
            why = mapping_reason(dlg, m_job.trays, m_job.used);
        const bool ready = why.empty() && dlg->forca_clean_ready(m_job.dev_id, why);
        if (m_ticks < 0) { // waiting for the dialog to be ready
            if (ready) {
                m_ticks = COUNTDOWN_S * 2;
                show_card(dlg);
            } else if (seconds_open() > READY_TIMEOUT_S) {
                show_card(dlg);
                refuse(why);
            }
            return;
        }
        if (!ready) {
            refuse(why);
            return;
        }
        if (--m_ticks > 0) {
            update_card();
            return;
        }
        // Everything again, right before Send: the slice, the grant, the printer and its bed.
        std::string dev_id;
        why = m_job.stale();
        if (why.empty())
            why = forca_ai_grant_check(m_job, m_job.dev_id, dev_id);
        if (why.empty())
            why = mapping_reason(dlg, m_job.trays, m_job.used);
        if (why.empty() && dev_id != m_job.dev_id)
            why = "the printer changed";
        if (!why.empty()) {
            refuse(why);
            return;
        }
        const int grant_id = s_grant.id;
        forca_academy_mark_source("forca_ai: autonomous print (grant #" + std::to_string(grant_id) + ")");
        if (!dlg->forca_auto_send(why)) {
            forca_academy_mark_source({});
            refuse(why);
            return;
        }
        // Sent: this print uses up one print of the grant and the bed's clearing.
        const std::string how = m_job.bed_check > 0 ? "camera check #" + std::to_string(m_job.bed_check)
                                                    : s_beds[m_job.dev_id].how;
        std::vector<int> count; // the copies of each grant file this plate prints
        if (!s_grant.files.empty() && forca_ai_grant_files_reason(s_grant.files, plate_objects(m_job.plate), count).empty())
            for (size_t i = 0; i < count.size(); ++i)
                s_grant.files[i].done += count[i];
        --s_grant.prints_left;
        s_beds[m_job.dev_id].clear = false;
        s_checks.erase(m_job.bed_check);
        finish("sent", "Forca pressed Send under print grant #" + std::to_string(grant_id) + " (bed: " + how + "); " +
                           std::to_string(s_grant.prints_left) + " print(s) of the grant left");
        close_card();
    }

    ForcaAIGrantPrint                     m_job;
    wxTimer                               m_timer;
    std::chrono::steady_clock::time_point m_opened;
    int                                   m_ticks    = -1; // countdown half-seconds; -1 = waiting for the dialog
    bool                                  m_mapped   = false; // the AI's slots are set in the dialog
    bool                                  m_finished = false;
    std::string                           m_logged;           // the last state log_state wrote
    wxDialog*                             m_card     = nullptr;
    wxStaticText*                         m_text     = nullptr;
    wxButton*                             m_cancel   = nullptr;
};

// ---- tools ------------------------------------------------------------------------------------

// Runs on the server thread: the camera call blocks.
ForcaAIResult tool_bed_check(const json& args)
{
    std::string err, dev_id, label, key;
    if (!ForcaAI::instance().on_gui([&]() {
            const ForcaAIGrant g = forca_ai_grant();
            if (!g.id) {
                err = "There is no print grant, so there is nothing to check the bed for.";
                return;
            }
            if (!g.camera_check) {
                err = "The grant does not let the AI clear a bed by camera; the user clicks 'Bed is clear' in Forca.";
                return;
            }
            const std::string which = args.value("printer", std::string());
            for (const std::string& id : g.printers)
                if (MachineObject* o = machine(id); o && (which.empty() || which == id || which == o->get_dev_name())) {
                    if (!dev_id.empty() && which.empty()) {
                        err = "The grant covers more than one printer; say which.";
                        return;
                    }
                    dev_id = id;
                    label  = forca_ai_printer_label(id);
                    key    = job_key(o);
                    if (is_busy(o))
                        err = label + " is printing.";
                }
            if (dev_id.empty() && err.empty())
                err = "No printer '" + which + "' in the print grant.";
        }))
        return ForcaAIResult::error("Forca did not respond (it may be showing a dialog).");
    if (!err.empty())
        return ForcaAIResult::error(err);

    std::string jpeg;
    if (!forca_printer_camera_jpeg(dev_id, 1280, jpeg, err))
        return ForcaAIResult::error(err);

    // Keep the picture the AI judged, with the check.
    int         id = 0;
    std::string file;
    ForcaAI::instance().on_gui([&]() {
        id = s_next_check++;
        const std::time_t now = std::time(nullptr);
        std::tm           tm{};
#ifdef _WIN32
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char day[16], stamp[16];
        std::strftime(day, sizeof(day), "%Y-%m-%d", &tm);
        std::strftime(stamp, sizeof(stamp), "%H-%M-%S", &tm);
        const fs::path dir = forca_ai_default_dir() / "Bed checks" / day;
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path p = dir / (std::string(stamp) + " check " + std::to_string(id) + " " + forca_ai_clean_name(label) + ".jpg");
        fs::ofstream out(p, std::ios::binary);
        out.write(jpeg.data(), std::streamsize(jpeg.size()));
        file         = into_u8(from_path(p));
        s_checks[id] = BedCheck{ dev_id, key, file, now };
        ForcaAI::instance().log("bed check", "Camera bed check #" + std::to_string(id) + " of " + label + " (picture: " + file + ")", true);
    });
    ForcaAIResult r = ForcaAIResult::text(
        "Bed check #" + std::to_string(id) + " of " + label + ". Look at the whole build plate in the picture. Only if it "
        "is completely empty (no part, purge line, tool or loose filament), pass bed_check: " + std::to_string(id) +
        " to forca_request_print with start: true within 3 minutes. If anything could be on the plate, don't: ask the user "
        "to clear it. Forca keeps this picture with the check.");
    r.add_image(jpeg, "image/jpeg");
    return r;
}

} // namespace

ForcaAIGrant forca_ai_grant() { return grant_active() ? s_grant : ForcaAIGrant{}; }

void forca_ai_give_grant(ForcaAIGrant g)
{
    g.id    = s_next_grant++;
    s_grant = g;
    std::string names;
    for (const std::string& id : g.printers)
        names += (names.empty() ? "" : ", ") + forca_ai_printer_label(id);
    ForcaAI::instance().log("print grant", "The user gave the AI print grant #" + std::to_string(g.id) + ": up to " +
                                               std::to_string(g.prints_left) + " print(s) on " + names + " until " +
                                               hhmm(g.expires) + ", each up to " + std::to_string(g.max_print_min) +
                                               " min; camera bed checks " + (g.camera_check ? "allowed" : "not allowed") + ".",
                            true);
    watcher().ensure_running();
}

void forca_ai_revoke_grant(const std::string& why)
{
    if (s_grant.id == 0)
        return;
    const int id = s_grant.id;
    s_grant      = ForcaAIGrant{};
    s_checks.clear();
    ForcaAI::instance().log("print grant", "Print grant #" + std::to_string(id) + " ended: " + why + ".", true);
}

std::string forca_ai_mark_bed_clear(const std::string& dev_id)
{
    MachineObject* o = machine(dev_id);
    if (!o)
        return "Forca does not know this printer right now.";
    // The job identity is taken now; stale data would make a later refresh look like a new print.
    if (!o->is_connected())
        return "Forca has no live data from " + forca_ai_printer_label(dev_id) + ": select it in the Device tab first.";
    if (is_busy(o))
        return forca_ai_printer_label(dev_id) + " is printing.";
    s_beds[dev_id] = Bed{ true, job_key(o), "marked clear by the user at " + hhmm(std::time(nullptr)), std::time(nullptr) };
    ForcaAI::instance().log("bed", "The user marked the bed of " + forca_ai_printer_label(dev_id) + " clear.", true);
    watcher().ensure_running();
    return {};
}

bool forca_ai_bed_is_clear(const std::string& dev_id)
{
    auto it = s_beds.find(dev_id);
    return it != s_beds.end() && it->second.clear;
}

json forca_ai_grant_json()
{
    const ForcaAIGrant g = forca_ai_grant();
    if (!g.id)
        return { { "active", false },
                 { "note", "Without a grant forca_request_print never starts a print itself. Only the user gives grants, in "
                           "the Forca AI window, at the Advanced level." } };
    json printers = json::array();
    for (const std::string& id : g.printers) {
        auto it = s_beds.find(id);
        printers.push_back({ { "printer", id }, { "name", forca_ai_printer_label(id) },
                             { "bed", it != s_beds.end() && it->second.clear ? it->second.how : std::string("not marked clear") } });
    }
    json files = json::array();
    for (const ForcaAIGrantFile& f : g.files) {
        json jf = { { "file", f.path }, { "copies", f.copies }, { "sent", f.done } };
        if (!f.note.empty())
            jf["note_from_user"] = f.note;
        files.push_back(jf);
    }
    return { { "active", true },
             { "grant", g.id },
             { "printers", printers },
             { "files", files },
             { "files_rule", g.files.empty()
                                 ? "No files named: any plate may be printed within the grant."
                                 : "Print only these files: import them with forca_import_models, follow each note_from_user "
                                   "when you choose settings, and put no more copies on a plate than each file still owes "
                                   "(copies - sent). Forca refuses a plate with anything else on it." },
             { "prints_left", g.prints_left },
             { "until", hhmm(g.expires) },
             { "minutes_left", std::max<long long>(0, (g.expires - std::time(nullptr)) / 60) },
             { "max_print_min", g.max_print_min },
             { "camera_bed_check", g.camera_check },
             { "how", "forca_request_print with start: true, printer, and trays (the printer slot for every filament the "
                      "plate uses, e.g. {\"1\": \"Ext\"}; see forca_printer_status filament_sources). The printer must be "
                      "selected in Forca's Device tab, idle, and its bed clear: marked clear by the user" +
                          std::string(g.camera_check ? ", or your own forca_bed_check passed as bed_check" : "") +
                          ". Forca opens its print dialog, counts down 10 s (the user can cancel) and presses Send." } };
}

bool forca_ai_parse_tray(const std::string& label, std::string& ams_id, std::string& slot_id)
{
    std::string s = boost::to_upper_copy(boost::trim_copy(label));
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (s == "EXT" || s == "EXT1" || s == "EXTERNAL") {
        ams_id  = VIRTUAL_AMS_MAIN_ID_STR;
        slot_id = "0";
        return true;
    }
    if (s == "EXT2") {
        ams_id  = VIRTUAL_AMS_DEPUTY_ID_STR;
        slot_id = "0";
        return true;
    }
    if (s.size() == 4 && s.compare(0, 3, "HT-") == 0 && s[3] >= 'A' && s[3] <= 'Z') { // AMS HT: one slot per unit
        ams_id  = std::to_string(128 + (s[3] - 'A'));
        slot_id = "0";
        return true;
    }
    if (s.size() == 2 && s[0] >= 'A' && s[0] <= 'Z' && s[1] >= '1' && s[1] <= '4') {
        ams_id  = std::to_string(s[0] - 'A');
        slot_id = std::to_string(s[1] - '1');
        return true;
    }
    return false;
}

std::string forca_ai_grant_files_reason(const std::vector<ForcaAIGrantFile>& files,
                                        const std::vector<std::pair<std::string, int>>& objects, std::vector<int>& count)
{
    count.assign(files.size(), 0);
    auto key = [](const std::string& f) { return Slic3r::ForcaAI::Ledger::key_for(into_path(from_u8(f))); };
    std::vector<std::string> keys;
    for (const ForcaAIGrantFile& f : files)
        keys.push_back(key(f.path));
    for (const auto& [file, n] : objects) {
        const auto it = file.empty() ? keys.end() : std::find(keys.begin(), keys.end(), key(file));
        if (it == keys.end())
            return "The plate has an object that is not from the grant's files (" +
                   (file.empty() ? std::string("made in Forca") : file) + "); the grant only prints its own files.";
        count[it - keys.begin()] += n;
    }
    for (size_t i = 0; i < files.size(); ++i)
        if (count[i] > files[i].copies - files[i].done)
            return "The plate has " + std::to_string(count[i]) + " of " + files[i].path + ", but the grant owes only " +
                   std::to_string(std::max(0, files[i].copies - files[i].done)) + " more.";
    return {};
}

std::string forca_ai_grant_check(const ForcaAIGrantPrint& job, const std::string& printer, std::string& dev_id)
{
    const int                         print_time_s = job.print_time_s;
    const int                         bed_check    = job.bed_check;
    const std::map<int, std::string>& trays        = job.trays;
    const std::vector<int>&           used         = job.used;
    if (ForcaAI::instance().level() != ForcaAI::Level::Advanced)
        return "Printing on its own needs the Advanced control level, which the user has not chosen.";
    const ForcaAIGrant g = forca_ai_grant();
    if (!g.id)
        return "The user has not given the AI a print grant (or it expired or is used up). Leave out 'start' so the "
               "user sends the print.";
    if (!wxGetApp().preset_bundle || !wxGetApp().preset_bundle->use_bbl_network())
        return "Printing on its own works with Bambu printers only for now.";
    MachineObject* obj = nullptr;
    for (const std::string& id : g.printers)
        if (MachineObject* o = machine(id); o && (printer.empty() || printer == id || printer == o->get_dev_name())) {
            if (obj && printer.empty())
                return "The grant covers more than one printer; say which.";
            obj = o;
        }
    if (!obj)
        return printer.empty() ? "None of the grant's printers is known to Forca right now."
                               : "'" + printer + "' is not one of the grant's printers.";
    const std::string id    = obj->get_dev_id();
    const std::string label = forca_ai_printer_label(id);
    if (!forca_ai_printer_optins().count(id))
        return label + " is no longer shared with the AI.";
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (!dev || dev->get_selected_machine() != obj)
        return label + " must be the printer selected in Forca's Device tab (Forca sends to that one).";
    if (!obj->is_connected())
        return "Forca has no live data from " + label + " right now.";
    if (is_busy(obj))
        return label + " is busy (" + obj->print_status + ").";
    if (g.max_print_min > 0 && print_time_s > g.max_print_min * 60)
        return "This print takes " + std::to_string(print_time_s / 60) + " min; the grant allows at most " +
               std::to_string(g.max_print_min) + " min per print.";
    if (std::string why = bed_reason(obj, g, bed_check); !why.empty())
        return why;
    if (std::string why = tray_reason(obj, trays, used); !why.empty())
        return why;
    if (!g.files.empty()) {
        std::vector<int> count;
        if (std::string why = forca_ai_grant_files_reason(g.files, plate_objects(job.plate), count); !why.empty())
            return why;
    }
    dev_id = id;
    return {};
}

void forca_ai_grant_send(ForcaAIGrantPrint job)
{
    GrantSender sender(std::move(job));
    sender.run();
}

void register_forca_ai_autonomy_tools(ForcaAI& ai)
{
    ForcaAITool bed_check{ "forca_bed_check", "Check a printer's bed by camera",
        "Only with a print grant that allows camera bed checks: takes a fresh camera picture of a grant printer's build "
        "plate for you to judge. If the plate is completely empty, pass the returned bed_check number to "
        "forca_request_print (start: true) within 3 minutes. Forca keeps the picture.",
        { { "type", "object" }, { "properties", { { "printer", { { "type", "string" }, { "description", "Printer id or name (default: the grant's only printer)." } } } } } },
        tool_bed_check };
    bed_check.off_gui = true;
    ai.register_tool(bed_check);
}

}} // namespace Slic3r::GUI
