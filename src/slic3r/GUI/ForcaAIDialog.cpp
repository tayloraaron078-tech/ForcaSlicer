#include "ForcaAIDialog.hpp"

#include "ForcaAI.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/clipbrd.h>
#include <wx/listctrl.h>
#include <wx/choicdlg.h>
#include <wx/filedlg.h>
#include <wx/numdlg.h>
#include <wx/textdlg.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>

#include <boost/filesystem/path.hpp>

namespace Slic3r { namespace GUI {

namespace {

// Asks the user for a print grant (HQ PLAN_forca_ai.md §10a). False if they cancel. `clear_now`: the printers whose
// bed they say is clear right now.
bool ask_for_grant(wxWindow* parent, ForcaAIGrant& grant, std::vector<std::string>& clear_now)
{
    std::vector<std::string> ids;
    for (const std::string& id : forca_ai_known_printers())
        if (forca_ai_printer_optins().count(id))
            ids.push_back(id);
    if (ids.empty()) {
        MessageDialog(parent, _L("Share a printer with the AI first (the printer list in this window)."), _L("Forca AI"),
                      wxICON_INFORMATION | wxOK).ShowModal();
        return false;
    }

    wxDialog dlg(parent, wxID_ANY, _L("Let the AI print on its own"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    dlg.SetFont(Label::Body_14);
    const int pad  = dlg.FromDIP(12);
    auto*     root = new wxBoxSizer(wxVERTICAL);
    auto*     intro = new wxStaticText(&dlg, wxID_ANY,
        _L("Within this grant the AI may start prints itself, with no click from you: Forca opens its print dialog, counts "
           "down 10 seconds (you can cancel) and presses Send. It only sends to an idle printer whose bed is clear, and only "
           "when the print dialog has nothing to ask you. A wrong print can still damage your printer or waste filament, "
           "and a printer running unattended is a fire risk: give only what you are comfortable with. The grant ends when "
           "it is used up, when time runs out, when you revoke it, choose Guarded, or restart Forca."));
    intro->Wrap(dlg.FromDIP(520));
    root->Add(intro, 0, wxALL, pad);

    root->Add(new wxStaticText(&dlg, wxID_ANY, _L("Printers:")), 0, wxLEFT | wxRIGHT, pad);
    auto* printers = new wxCheckListBox(&dlg, wxID_ANY, wxDefaultPosition, dlg.FromDIP(wxSize(520, 70)));
    for (const std::string& id : ids)
        printers->Append(wxString::FromUTF8(forca_ai_printer_label(id).c_str()));
    if (ids.size() == 1)
        printers->Check(0);
    root->Add(printers, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, dlg.FromDIP(6));

    auto* grid  = new wxFlexGridSizer(2, dlg.FromDIP(8), dlg.FromDIP(12));
    auto  spin  = [&](const wxString& label, int lo, int hi, int value) {
        grid->Add(new wxStaticText(&dlg, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL);
        auto* s = new wxSpinCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, dlg.FromDIP(wxSize(90, -1)), wxSP_ARROW_KEYS, lo, hi, value);
        grid->Add(s, 0);
        return s;
    };
    // What to print (optional): the AI imports these and prints the copies; Forca sends nothing else under the grant.
    std::vector<ForcaAIGrantFile> files;
    root->Add(new wxStaticText(&dlg, wxID_ANY, _L("What to print (optional - without files, any plate):")), 0, wxLEFT | wxRIGHT, pad);
    auto* file_list = new wxListCtrl(&dlg, wxID_ANY, wxDefaultPosition, dlg.FromDIP(wxSize(520, 110)), wxLC_REPORT | wxLC_SINGLE_SEL);
    file_list->AppendColumn(_L("File"), wxLIST_FORMAT_LEFT, dlg.FromDIP(200));
    file_list->AppendColumn(_L("Copies"), wxLIST_FORMAT_LEFT, dlg.FromDIP(60));
    file_list->AppendColumn(_L("Note for the AI"), wxLIST_FORMAT_LEFT, dlg.FromDIP(240));
    root->Add(file_list, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, dlg.FromDIP(6));
    // Rebuilds the list and keeps one row selected, so Copies / Note act on the file just added or edited
    // without the user having to click it first.
    auto show_files = [&](int select) {
        file_list->DeleteAllItems();
        for (size_t i = 0; i < files.size(); ++i) {
            const long row = file_list->InsertItem(long(i), into_path(from_u8(files[i].path)).filename().wstring());
            file_list->SetItem(row, 1, wxString::Format("%d", files[i].copies));
            file_list->SetItem(row, 2, wxString::FromUTF8(files[i].note.c_str()));
        }
        if (!files.empty()) {
            select = std::clamp(select, 0, int(files.size()) - 1);
            file_list->SetItemState(select, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            file_list->EnsureVisible(select);
        }
    };
    auto selected = [&]() { return int(file_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)); };
    auto file_buttons = new wxBoxSizer(wxHORIZONTAL);
    auto add_button   = [&](const wxString& label, std::function<void()> fn) {
        auto* b = new wxButton(&dlg, wxID_ANY, label);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent&) { fn(); });
        file_buttons->Add(b, 0, wxRIGHT, dlg.FromDIP(8));
    };
    add_button(_L("Add files..."), [&]() {
        wxFileDialog pick(&dlg, _L("Files the AI should print"), wxEmptyString, wxEmptyString,
                          "Models (*.stl;*.3mf;*.step;*.stp;*.obj;*.amf;*.drc)|*.stl;*.3mf;*.step;*.stp;*.obj;*.amf;*.drc",
                          wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
        if (pick.ShowModal() != wxID_OK)
            return;
        wxArrayString paths;
        pick.GetPaths(paths);
        const int first_new = int(files.size());
        for (const wxString& p : paths)
            files.push_back({ into_u8(p), 1, 0, {} });
        show_files(first_new);
    });
    add_button(_L("Copies..."), [&]() {
        const int row = selected();
        if (row < 0)
            return;
        const long n = wxGetNumberFromUser(_L("How many copies of this file?"), wxEmptyString, _L("Copies"),
                                           files[row].copies, 1, 500, &dlg);
        if (n > 0)
            files[row].copies = int(n);
        show_files(row);
    });
    add_button(_L("Note..."), [&]() {
        const int row = selected();
        if (row < 0)
            return;
        // wxTextEntryDialog, not wxGetTextFromUser: Cancel must keep the note (wxGetTextFromUser returns "" on Cancel).
        wxTextEntryDialog ask(&dlg, _L("Instructions for the AI for this file (orientation, material, settings...):"),
                              _L("Note for the AI"), wxString::FromUTF8(files[row].note.c_str()));
        if (ask.ShowModal() == wxID_OK)
            files[row].note = into_u8(ask.GetValue());
        show_files(row);
    });
    add_button(_L("Remove"), [&]() {
        const int row = selected();
        if (row < 0)
            return;
        files.erase(files.begin() + row);
        show_files(row);
    });
    root->Add(file_buttons, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(6));

    wxSpinCtrl* prints = spin(_L("Prints the AI may start:"), 1, 20, 1);
    wxSpinCtrl* hours  = spin(_L("For the next (hours):"), 1, 24, 4);
    wxSpinCtrl* longest = spin(_L("Longest print (hours):"), 1, 48, 4);
    root->Add(grid, 0, wxALL, pad);

    auto* clear = new wxCheckBox(&dlg, wxID_ANY, _L("The bed of these printers is clear now"));
    root->Add(clear, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
    auto* camera = new wxCheckBox(&dlg, wxID_ANY, _L("Also let the AI decide by camera that a bed is clear"));
    root->Add(camera, 0, wxLEFT | wxRIGHT, pad);
    auto* camera_note = new wxStaticText(&dlg, wxID_ANY,
        _L("Off: a bed counts as clear only after you click 'Bed is clear' in this window, once per print. On: the AI may "
           "also judge a fresh camera picture itself (Forca keeps the picture), so prints can follow each other while you "
           "are away. If it misses something on the bed, the printer will print into it."));
    camera_note->Wrap(dlg.FromDIP(520));
    camera_note->SetForegroundColour(wxColour("#6B7488"));
    root->Add(camera_note, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer(1);
    buttons->Add(new wxButton(&dlg, wxID_CANCEL, _L("Cancel")), 0, wxRIGHT, dlg.FromDIP(8));
    auto* ok = new wxButton(&dlg, wxID_OK, _L("Give grant"));
    buttons->Add(ok, 0);
    root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);
    ok->Bind(wxEVT_BUTTON, [&](wxCommandEvent& e) {
        wxArrayInt checked;
        if (printers->GetCheckedItems(checked) == 0) {
            MessageDialog(&dlg, _L("Tick at least one printer."), _L("Forca AI"), wxICON_INFORMATION | wxOK).ShowModal();
            return;
        }
        e.Skip();
    });
    dlg.SetSizerAndFit(root);
    dlg.CenterOnParent();
    if (dlg.ShowModal() != wxID_OK)
        return false;

    grant = ForcaAIGrant{};
    for (unsigned i = 0; i < printers->GetCount(); ++i)
        if (printers->IsChecked(i))
            grant.printers.insert(ids[i]);
    grant.prints_left   = prints->GetValue();
    grant.expires       = std::time(nullptr) + std::time_t(hours->GetValue()) * 3600;
    grant.max_print_min = longest->GetValue() * 60;
    grant.camera_check  = camera->GetValue();
    grant.files         = files;
    clear_now.clear();
    if (clear->GetValue())
        clear_now.assign(grant.printers.begin(), grant.printers.end());
    return true;
}

} // namespace

ForcaAIDialog::ForcaAIDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Forca AI"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetFont(Label::Body_14);
    const int pad  = FromDIP(12);
    auto*     root = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(this, wxID_ANY, _L("Forca AI"));
    title->SetFont(Label::Head_16);
    root->Add(title, 0, wxALL, pad);

    auto* intro = new wxStaticText(this, wxID_ANY,
        _L("Let an AI assistant (for example Claude Code) see and work in Forca. It connects only from this computer, "
           "using the key below. At the Guarded level Forca never lets the AI save over your files, and never sends "
           "anything to a printer without your approval here in Forca."));
    intro->Wrap(FromDIP(560));
    root->Add(intro, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    m_enable = new wxCheckBox(this, wxID_ANY, _L("Let an AI connect to Forca"));
    m_enable->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        ForcaAI::instance().set_enabled(m_enable->GetValue());
        refresh();
    });
    root->Add(m_enable, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    m_state = new wxStaticText(this, wxID_ANY, "");
    m_state->SetFont(Label::Head_14);
    root->Add(m_state, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

    // Control level (HQ PLAN_forca_ai.md §10a): only this window changes it -- no AI tool can.
    auto* level_label = new wxStaticText(this, wxID_ANY, _L("How much the AI may do:"));
    root->Add(level_label, 0, wxLEFT | wxRIGHT, pad);
    m_guarded  = new wxRadioButton(this, wxID_ANY, _L("Guarded (recommended) - new files only; you approve every print request"),
                                   wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_advanced = new wxRadioButton(this, wxID_ANY, _L("Advanced - may overwrite your files and presets; no approval card"));
    root->Add(m_guarded, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    root->Add(m_advanced, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    auto* level_note = new wxStaticText(this, wxID_ANY,
        _L("Unless you give it a print grant below, you press Send yourself. At every level the AI never deletes files "
           "or discards your unsaved edits, and it cannot heat, move or change your printers."));
    level_note->Wrap(FromDIP(560));
    level_note->SetForegroundColour(wxColour("#6B7488"));
    root->Add(level_note, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
    m_guarded->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        ForcaAI::instance().set_level(ForcaAI::Level::Guarded);
        refresh();
    });
    m_advanced->Bind(wxEVT_RADIOBUTTON, [this](wxCommandEvent&) {
        MessageDialog dlg(this,
            _L("Advanced lets the AI save over your own files and change your own presets. Forca copies each file into "
               "Documents\\Forca AI\\Backups before the AI overwrites it, but a mistake can still cost you work.\n\n"
               "Print requests will open the print dialog straight away, without an approval card. Nothing prints until "
               "you press Send, so check the plate, printer and filament before you do: a wrong print can damage your "
               "printer, and never leave a printer running unattended.\n\nSwitch to Advanced?"),
            _L("Forca AI"), wxICON_WARNING | wxYES_NO | wxNO_DEFAULT);
        if (dlg.ShowModal() == wxID_YES)
            ForcaAI::instance().set_level(ForcaAI::Level::Advanced);
        refresh(); // back to Guarded on No
    });

    auto* cmd_label = new wxStaticText(this, wxID_ANY, _L("To connect Claude Code, run this once in a terminal:"));
    root->Add(cmd_label, 0, wxLEFT | wxRIGHT, pad);
    m_command = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(560, -1)), wxTE_READONLY);
    root->Add(m_command, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, FromDIP(6));

    auto* buttons  = new wxBoxSizer(wxHORIZONTAL);
    auto* copy_btn = new wxButton(this, wxID_ANY, _L("Copy command"));
    copy_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_command->GetValue()));
            wxTheClipboard->Close();
        }
    });
    buttons->Add(copy_btn, 0, wxRIGHT, FromDIP(8));
    auto* key_btn = new wxButton(this, wxID_ANY, _L("Make a new key"));
    key_btn->SetToolTip(_L("Stops every AI that uses the old key; connect them again with the new command."));
    key_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        MessageDialog dlg(this, _L("Make a new key? Every AI connected with the old key stops working until you "
                                   "connect it again with the new command."),
                          _L("Forca AI"), wxICON_QUESTION | wxYES_NO);
        if (dlg.ShowModal() == wxID_YES) {
            ForcaAI::instance().regenerate_token();
            refresh();
        }
    });
    buttons->Add(key_btn, 0);
    root->Add(buttons, 0, wxALL, pad);

    // Per-printer opt-in: nothing is shared until the user ticks it (HQ PLAN_forca_ai.md §3a).
    auto* printers_label = new wxStaticText(this, wxID_ANY, _L("Printers the AI may use (it cannot see the others):"));
    root->Add(printers_label, 0, wxLEFT | wxRIGHT, pad);
    m_printers = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 70)));
    m_printers->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent& e) {
        const int row = e.GetInt();
        if (row >= 0 && row < int(m_printer_ids.size()))
            forca_ai_set_printer_optin(m_printer_ids[row], m_printers->IsChecked(row));
        refresh(); // "may pause" follows "may use"
    });
    root->Add(m_printers, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM | wxEXPAND, FromDIP(6));

    // A separate, stronger permission: pausing a print it sees failing (resuming only its own pause).
    auto* pausers_label = new wxStaticText(this, wxID_ANY,
        _L("Printers the AI may also PAUSE when its camera shows a problem (it can resume only its own pause):"));
    pausers_label->Wrap(FromDIP(560));
    root->Add(pausers_label, 0, wxLEFT | wxRIGHT, pad);
    m_pausers = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 70)));
    m_pausers->Bind(wxEVT_CHECKLISTBOX, [this](wxCommandEvent& e) {
        const int row = e.GetInt();
        if (row >= 0 && row < int(m_printer_ids.size()))
            forca_ai_set_pause_optin(m_printer_ids[row], m_pausers->IsChecked(row));
        refresh();
    });
    root->Add(m_pausers, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM | wxEXPAND, FromDIP(6));

    // Pre-approved autonomy (HQ PLAN_forca_ai.md §10a): a print grant, Advanced level only.
    auto* grant_label = new wxStaticText(this, wxID_ANY, _L("Printing on its own:"));
    root->Add(grant_label, 0, wxLEFT | wxRIGHT, pad);
    m_grant_text = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_grant_text, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    auto* grant_buttons = new wxBoxSizer(wxHORIZONTAL);
    m_grant_btn  = new wxButton(this, wxID_ANY, _L("Give a print grant..."));
    m_bed_btn    = new wxButton(this, wxID_ANY, _L("Bed is clear..."));
    m_revoke_btn = new wxButton(this, wxID_ANY, _L("Revoke"));
    grant_buttons->Add(m_grant_btn, 0, wxRIGHT, FromDIP(8));
    grant_buttons->Add(m_bed_btn, 0, wxRIGHT, FromDIP(8));
    grant_buttons->Add(m_revoke_btn, 0);
    root->Add(grant_buttons, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
    m_grant_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        ForcaAIGrant             grant;
        std::vector<std::string> clear_now;
        if (ask_for_grant(this, grant, clear_now)) {
            forca_ai_give_grant(grant);
            for (const std::string& id : clear_now)
                if (const std::string why = forca_ai_mark_bed_clear(id); !why.empty())
                    MessageDialog(this, wxString::FromUTF8(why.c_str()), _L("Forca AI"), wxICON_WARNING | wxOK).ShowModal();
        }
        refresh();
    });
    m_revoke_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        forca_ai_revoke_grant("the user revoked it");
        refresh();
    });
    m_bed_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        const ForcaAIGrant       grant = forca_ai_grant();
        std::vector<std::string> ids(grant.printers.begin(), grant.printers.end());
        if (ids.empty())
            return;
        size_t pick = 0;
        if (ids.size() > 1) {
            wxArrayString names;
            for (const std::string& id : ids)
                names.Add(wxString::FromUTF8(forca_ai_printer_label(id).c_str()));
            wxSingleChoiceDialog choose(this, _L("Which printer's bed is clear?"), _L("Forca AI"), names);
            if (choose.ShowModal() != wxID_OK)
                return;
            pick = size_t(choose.GetSelection());
        }
        if (const std::string why = forca_ai_mark_bed_clear(ids[pick]); !why.empty())
            MessageDialog(this, wxString::FromUTF8(why.c_str()), _L("Forca AI"), wxICON_WARNING | wxOK).ShowModal();
        refresh();
    });

    auto* feed_label = new wxStaticText(this, wxID_ANY, _L("What the AI has done (newest first):"));
    root->Add(feed_label, 0, wxLEFT | wxRIGHT, pad);
    m_activity = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 200)), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_activity->AppendColumn(_L("Time"), wxLIST_FORMAT_LEFT, FromDIP(70));
    m_activity->AppendColumn(_L("Action"), wxLIST_FORMAT_LEFT, FromDIP(150));
    m_activity->AppendColumn(_L("Details"), wxLIST_FORMAT_LEFT, FromDIP(330));
    root->Add(m_activity, 1, wxALL | wxEXPAND, pad);

    auto* close_row = new wxBoxSizer(wxHORIZONTAL);
    close_row->AddStretchSpacer(1);
    close_row->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);
    root->Add(close_row, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);

    // Persistent window: closing only hides it (MainFrame owns the instance).
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        if (e.CanVeto()) {
            Hide();
            e.Veto();
        } else {
            e.Skip();
        }
    });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); }, wxID_CANCEL);

    SetSizerAndFit(root);
    ForcaAI::instance().add_listener([this]() { if (IsShown()) refresh(); });
    refresh();
    CenterOnParent();
}

void ForcaAIDialog::refresh()
{
    ForcaAI& ai = ForcaAI::instance();
    m_enable->SetValue(ai.enabled());
    m_guarded->SetValue(ai.level() == ForcaAI::Level::Guarded);
    m_advanced->SetValue(ai.level() == ForcaAI::Level::Advanced);

    const bool         advanced = ai.level() == ForcaAI::Level::Advanced;
    const ForcaAIGrant grant    = forca_ai_grant();
    wxString           grant_text;
    if (!advanced) {
        grant_text = _L("Needs the Advanced level.");
    } else if (!grant.id) {
        grant_text = _L("No print grant: the AI cannot start a print by itself.");
    } else {
        char until[8];
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &grant.expires);
#else
        localtime_r(&grant.expires, &tm);
#endif
        std::strftime(until, sizeof(until), "%H:%M", &tm);
        grant_text = wxString::Format(_L("Print grant #%d: %d print(s) left, until %s, each up to %d h. Camera bed check: %s."),
                                      grant.id, grant.prints_left, until, grant.max_print_min / 60,
                                      grant.camera_check ? _L("on") : _L("off"));
        for (const std::string& id : grant.printers)
            grant_text += "\n" + wxString::FromUTF8(forca_ai_printer_label(id).c_str()) + ": " +
                          (forca_ai_bed_is_clear(id) ? _L("bed marked clear") : _L("bed not marked clear"));
        for (const ForcaAIGrantFile& f : grant.files)
            grant_text += wxString("\n") + wxString(into_path(from_u8(f.path)).filename().wstring()) + ": " +
                          wxString::Format(_L("%d of %d sent"), f.done, f.copies);
    }
    m_grant_text->SetLabel(grant_text);
    m_grant_text->Wrap(FromDIP(560));
    m_grant_btn->Enable(advanced);
    m_bed_btn->Enable(advanced && grant.id != 0);
    m_revoke_btn->Enable(grant.id != 0);

    switch (ai.state()) {
    case ForcaAI::State::Connected: m_state->SetForegroundColour(wxColour("#009688")); break; // Forca teal
    case ForcaAI::State::Listening: m_state->SetForegroundColour(wxColour("#2F6FDE")); break; // Forca blue
    case ForcaAI::State::Error:     m_state->SetForegroundColour(wxColour("#D32F2F")); break;
    default:                        m_state->SetForegroundColour(wxColour("#6B7488")); break;
    }
    m_state->SetLabel(wxString::FromUTF8(ai.state_text().c_str()));
    m_command->SetValue(ai.enabled() ? wxString::FromUTF8(ai.claude_code_command().c_str())
                                     : _L("(turn Forca AI on to see the command)"));

    // Printers: rebuild only when the set Forca knows changes (keeps the user's scroll/focus), then sync ticks.
    const std::vector<std::string> ids = forca_ai_known_printers();
    if (!m_printers_built || ids != m_printer_ids) {
        m_printers_built = true;
        m_printer_ids    = ids;
        m_printers->Clear();
        m_pausers->Clear();
        for (const std::string& id : ids) {
            m_printers->Append(wxString::FromUTF8(forca_ai_printer_label(id).c_str()));
            m_pausers->Append(wxString::FromUTF8(forca_ai_printer_label(id).c_str()));
        }
        if (ids.empty()) {
            m_printers->Append(_L("(no printers yet -- connect one in the Device tab)"));
            m_pausers->Append(_L("(no printers yet)"));
        }
    }
    const std::set<std::string> shared = forca_ai_printer_optins();
    const std::set<std::string> pause  = forca_ai_pause_optins();
    for (size_t i = 0; i < m_printer_ids.size(); ++i) {
        m_printers->Check(int(i), shared.count(m_printer_ids[i]) > 0);
        m_pausers->Check(int(i), pause.count(m_printer_ids[i]) > 0); // unticks itself while the printer is not shared
    }

    const auto feed = ai.activity();
    m_activity->Freeze();
    m_activity->DeleteAllItems();
    long row = 0;
    for (auto it = feed.rbegin(); it != feed.rend(); ++it, ++row) {
        const long idx = m_activity->InsertItem(row, wxString::FromUTF8(it->time.c_str()));
        m_activity->SetItem(idx, 1, wxString::FromUTF8(it->action.c_str()));
        m_activity->SetItem(idx, 2, wxString::FromUTF8(it->summary.c_str()));
        if (!it->ok)
            m_activity->SetItemTextColour(idx, wxColour("#D32F2F"));
    }
    m_activity->Thaw();
    Layout();
}

}} // namespace Slic3r::GUI
