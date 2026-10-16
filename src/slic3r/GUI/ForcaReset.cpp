#include "ForcaReset.hpp"

#include "ForcaAcademy.hpp" // forca_academy_dir
#include "GUI.hpp"          // from_u8 / into_u8 / into_path / from_path
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp" // data_dir

#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/modalhook.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/timer.h>

#include <ctime>
#include <iterator>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {
std::vector<std::string> s_move_errors; // from forca_reset_apply_moves, shown once the config has loaded
} // namespace

std::string forca_reset_stamp()
{
    std::time_t now = std::time(nullptr);
    std::tm     tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H%M", &tm);
    return buf;
}

ForcaResetPlan forca_reset_plan(const ForcaResetParts& parts, const ForcaResetPaths& p)
{
    ForcaResetPlan plan;
    auto           move = [&plan](const fs::path& from, const fs::path& to) { plan.moves.emplace_back(from, to); };
    const fs::path forca_data = p.backup_dir / "Forca data"; // single files from the data folder
    // Folders outside the data folder first.
    if (parts.academy) {
        if (p.academy_dir.filename() == "Forca Academy")
            move(p.academy_dir, p.backup_dir / "Forca Academy");
        else // a journal kept directly in a folder of the user's (forca_academy_folder_in): only Forca's own entries
            for (const std::string& entry : forca_academy_own_entries())
                move(p.academy_dir / entry, p.backup_dir / "Forca Academy" / entry);
    }
    if (parts.ai_files)
        move(p.ai_files_dir, p.backup_dir / "Forca AI");
    if (parts.app) {
        // A fresh install: the whole data folder and the local one (they hold everything below too).
        move(p.local_dir, p.backup_dir / "Local app data");
        move(p.data_dir, p.backup_dir / "App data");
        return plan;
    }
    if (parts.academy)
        move(p.data_dir / "forca" / "academy_tracking.json", forca_data / "academy_tracking.json");
    if (parts.ai) {
        move(p.data_dir / "forca" / "ai", forca_data / "ai"); // key of older versions, activity log, ledger, pauses
        move(p.local_dir / "forca-ai-key.txt", forca_data / "forca-ai-key.txt");
        plan.clear_settings = { "forca_ai_enabled", "forca_ai_level", "forca_ai_printers", "forca_ai_pause_printers" };
    }
    if (parts.calibration)
        move(p.data_dir / "forca" / "calibration_results.json", forca_data / "calibration_results.json");
    return plan;
}

fs::path forca_reset_marker(const fs::path& data_dir)
{
    fs::path marker = data_dir;
    marker += ".forca-reset";
    return marker;
}

bool forca_reset_write_plan(const fs::path& data_dir, const ForcaResetPlan& plan, std::string& err)
{
    json moves = json::array();
    for (const auto& [from, to] : plan.moves)
        moves.push_back({ { "from", from.generic_string() }, { "to", to.generic_string() } });
    const json                j = { { "moves", moves }, { "clear_settings", plan.clear_settings } };
    boost::nowide::ofstream out(forca_reset_marker(data_dir).string(), std::ios::binary | std::ios::trunc);
    out << j.dump(2) << "\n";
    if (!out) {
        err = "could not write " + forca_reset_marker(data_dir).string();
        return false;
    }
    return true;
}

bool forca_reset_read_plan(const fs::path& data_dir, ForcaResetPlan& plan)
{
    boost::nowide::ifstream in(forca_reset_marker(data_dir).string(), std::ios::binary);
    if (!in)
        return false;
    try {
        const json j = json::parse(in);
        plan         = ForcaResetPlan();
        for (const json& m : j.value("moves", json::array()))
            plan.moves.emplace_back(fs::path(m.value("from", std::string())), fs::path(m.value("to", std::string())));
        for (const json& k : j.value("clear_settings", json::array()))
            plan.clear_settings.push_back(k.get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

namespace {

void copy_recursive(const fs::path& from, const fs::path& to)
{
    if (fs::is_directory(from)) {
        fs::create_directories(to);
        for (fs::directory_iterator it(from), end; it != end; ++it)
            copy_recursive(it->path(), to / it->path().filename());
    } else
        fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

} // namespace

bool forca_reset_move(const fs::path& from, const fs::path& to, std::string& err)
{
    boost::system::error_code ec;
    if (from.empty() || !fs::exists(from, ec))
        return true; // nothing to move
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec)
        return true;
    // Another drive (or a folder that can't be renamed): copy, then remove the original.
    try {
        copy_recursive(from, to);
        fs::remove_all(from);
        return true;
    } catch (const std::exception& e) {
        err = "could not move " + from.string() + " to " + to.string() + ": " + e.what();
        return false;
    }
}

std::vector<std::string> forca_reset_apply_moves(const fs::path& data_dir)
{
    std::vector<std::string> errors;
    ForcaResetPlan           plan;
    if (!forca_reset_read_plan(data_dir, plan))
        return errors;
    for (const auto& [from, to] : plan.moves) {
        std::string err;
        if (!forca_reset_move(from, to, err))
            errors.push_back(err);
    }
    boost::system::error_code ec;
    fs::create_directories(data_dir, ec); // Forca starts in an empty data folder after a fresh-install reset
    if (plan.clear_settings.empty())
        fs::remove(forca_reset_marker(data_dir), ec);
    else { // the settings are cleared once the config has loaded (forca_reset_apply_settings)
        plan.moves.clear();
        std::string err;
        forca_reset_write_plan(data_dir, plan, err);
    }
    s_move_errors = errors;
    return errors;
}

void forca_reset_apply_settings()
{
    ForcaResetPlan plan;
    const fs::path data(data_dir());
    if (forca_reset_read_plan(data, plan) && plan.moves.empty()) {
        if (AppConfig* cfg = wxGetApp().app_config) {
            for (const std::string& key : plan.clear_settings)
                cfg->set(key, key == "forca_ai_enabled" ? "0" : "");
            cfg->save();
        }
        boost::system::error_code ec;
        fs::remove(forca_reset_marker(data), ec);
        BOOST_LOG_TRIVIAL(info) << "forca_reset: settings reset";
    }
    if (!s_move_errors.empty()) {
        wxString text = _L("Forca was reset, but some files could not be moved to the backup folder:");
        for (const std::string& e : s_move_errors)
            text += "\n" + from_u8(e);
        s_move_errors.clear();
        MessageDialog(nullptr, text, _L("Reset Forca data"), wxOK | wxICON_WARNING).ShowModal();
    }
}

namespace {

class ForcaResetDialog : public wxDialog
{
public:
    explicit ForcaResetDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, _L("Reset Forca data"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        const int wrap = FromDIP(560);
        auto*     root = new wxBoxSizer(wxVERTICAL);
        auto*     intro = new wxStaticText(this, wxID_ANY,
            _L("Start again from scratch. Nothing is deleted: everything you tick moves to a backup folder in your "
               "Documents (Forca reset backups), where you can copy things back or delete it later. Forca closes, and "
               "the reset happens when you open it again."));
        intro->Wrap(wrap);
        root->Add(intro, 0, wxALL, pad);

        auto add = [&](const wxString& title, const wxString& detail) {
            auto* cb = new wxCheckBox(this, wxID_ANY, title);
            cb->SetValue(true);
            cb->SetFont(Label::Head_14);
            auto* text = new wxStaticText(this, wxID_ANY, detail);
            text->Wrap(wrap - FromDIP(24));
            root->Add(cb, 0, wxLEFT | wxRIGHT | wxTOP, pad);
            root->Add(text, 0, wxLEFT | wxRIGHT, FromDIP(34));
            cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { update(); });
            return cb;
        };
        m_app = add(_L("Forca itself (like a fresh install)"),
                    _L("All settings, your own presets, printers, network plug-ins and logs. Forca opens as on the first "
                       "day, so this also covers the AI and Calibration Wizard data below."));
        m_academy = add(_L("Forca Academy print journal"),
                        _L("Every print record, note, photo, lesson, playbook and experiment in the Academy folder."));
        m_ai = add(_L("Forca AI"),
                   _L("A new key (AI clients must be connected again), no shared printers or pause permission, the "
                      "Guarded level, Forca AI switched off, and its activity log."));
        m_ai_files = add(_L("Projects saved by the AI"), _L("The Forca AI folder in your Documents: AI-saved projects, backups and "
                                                            "bed-check pictures."));
        m_calibration = add(_L("Calibration Wizard history"),
                            _L("The wizard's progress and results history. Values you saved to your presets stay."));

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_ok          = new wxButton(this, wxID_OK, _L("Reset and close Forca"));
        auto* cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->AddStretchSpacer(1);
        buttons->Add(m_ok, 0, wxRIGHT, FromDIP(6));
        buttons->Add(cancel, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, pad);
        m_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { confirm(); });

        SetSizerAndFit(root);
        CenterOnParent();
        update();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    void update()
    {
        // A fresh install includes the data-folder parts.
        for (wxCheckBox* cb : { m_ai, m_calibration })
            cb->Enable(!m_app->GetValue());
        m_ok->Enable(m_app->GetValue() || m_academy->GetValue() || m_ai->GetValue() || m_ai_files->GetValue() ||
                     m_calibration->GetValue());
    }

    void confirm()
    {
        if (MessageDialog(this,
                          _L("Forca closes now and resets the ticked parts when you open it again. Everything goes to the "
                             "backup folder first. Continue?"),
                          _L("Reset Forca data"), wxYES_NO | wxICON_WARNING)
                .ShowModal() != wxID_YES)
            return;
        ForcaResetParts parts;
        parts.app         = m_app->GetValue();
        parts.academy     = m_academy->GetValue();
        parts.ai          = m_ai->GetValue() || parts.app;
        parts.ai_files    = m_ai_files->GetValue();
        parts.calibration = m_calibration->GetValue() || parts.app;

        ForcaResetPaths paths;
        const fs::path  documents = into_path(wxStandardPaths::Get().GetDocumentsDir());
        paths.data_dir            = fs::path(data_dir());
        paths.local_dir           = into_path(wxStandardPaths::Get().GetUserLocalDataDir());
        paths.academy_dir         = forca_academy_dir();
        paths.ai_files_dir        = documents / "Forca AI";
        paths.backup_dir          = documents / "Forca reset backups" / forca_reset_stamp();
        std::string err;
        if (!forca_reset_write_plan(paths.data_dir, forca_reset_plan(parts, paths), err)) {
            show_error(this, from_u8(err));
            return;
        }
        BOOST_LOG_TRIVIAL(info) << "forca_reset: reset planned, backup " << paths.backup_dir.string();
        EndModal(wxID_OK);
    }

    wxCheckBox* m_app         = nullptr;
    wxCheckBox* m_academy     = nullptr;
    wxCheckBox* m_ai          = nullptr;
    wxCheckBox* m_ai_files    = nullptr;
    wxCheckBox* m_calibration = nullptr;
    wxButton*   m_ok          = nullptr;
};

// Closes the main window once no modal dialog is open any more: wxWidgets refuses to close it while one is
// (wxTopLevelWindowBase::OnCloseWindow), but only after MainFrame's close handler has shut Forca down, which leaves the
// process running with no window. If a dialog stays open, Forca stays open and resets at the next start.
void close_forca_when_no_dialog()
{
    auto* timer = new wxTimer(); // not deleted: at most one per reset, and Forca is closing
    timer->Bind(wxEVT_TIMER, [timer, tries = 100](wxTimerEvent&) mutable {
        if (wxModalDialogHook::GetOpenCount() > 0 && --tries > 0)
            return;
        timer->Stop();
        if (wxModalDialogHook::GetOpenCount() == 0)
            wxGetApp().mainframe->Close();
        else
            BOOST_LOG_TRIVIAL(warning) << "forca_reset: a dialog is still open, Forca resets at the next start";
    });
    timer->Start(100);
}

} // namespace

void forca_reset_show_dialog(wxWindow* parent)
{
    {
        ForcaResetDialog dlg(parent);
        if (dlg.ShowModal() != wxID_OK)
            return;
    }
    // Leave the modal window the reset was opened from (Preferences, the Print Journal). Only now, with the reset
    // dialog's own loop finished, is that window's loop the running one, the only one EndModal can end. Then close
    // like the user would (an unsaved project still asks); if they keep Forca open, the reset waits for the next start.
    for (wxWindow* w = parent; w && w != wxGetApp().mainframe; w = w->GetParent())
        if (auto* dlg = dynamic_cast<wxDialog*>(w); dlg && dlg->IsModal()) {
            dlg->EndModal(wxID_CANCEL);
            break;
        }
    close_forca_when_no_dialog();
}

}} // namespace Slic3r::GUI
