#include "ForcaAIDialog.hpp"

#include "ForcaAI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/clipbrd.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

namespace Slic3r { namespace GUI {

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
           "using the key below. Forca never lets the AI save over your files, and never sends anything to a printer "
           "without your approval here in Forca."));
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

    auto* feed_label = new wxStaticText(this, wxID_ANY, _L("What the AI has done (newest first):"));
    root->Add(feed_label, 0, wxLEFT | wxRIGHT, pad);
    m_activity = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 260)), wxLC_REPORT | wxLC_SINGLE_SEL);
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
