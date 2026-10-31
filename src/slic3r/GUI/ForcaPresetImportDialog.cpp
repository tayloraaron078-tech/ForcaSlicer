#include "ForcaPresetImportDialog.hpp"

#include "GUI.hpp" // from_u8 / into_u8
#include "GUI_App.hpp"
#include "GUI_Utils.hpp" // DPIDialog
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/ForcaPresetImport.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/system/error_code.hpp>

#include <wx/colour.h>
#include <wx/defs.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace {

// One analysed file and the controls of its row.
struct Row
{
    PresetImportItem         item;
    ::CheckBox              *use      = nullptr;
    TextInput               *name     = nullptr;
    ::ComboBox              *parent   = nullptr;
    ::ComboBox              *printers = nullptr;
    Button                  *details  = nullptr;
    std::vector<std::string> parent_names;  // what each parent item stands for ("" = keep the file's)
    std::vector<std::string> printer_names; // what each printer item stands for ("" = keep the file's)
};

// The short line under a file's name: where it comes from and what needs a look.
wxString status_text(const PresetImportItem &item)
{
    if (item.bambu_system)
        return item.forca_has_it ? _L("Already built into Forca") : _L("A Bambu Studio built-in preset Forca doesn't have yet");
    if (!item.readable)
        return _L("Can't be read as a preset.");
    std::vector<wxString> parts;
    if (item.source == PresetImportItem::Source::BambuStudio)
        parts.push_back(_L("From Bambu Studio"));
    if (item.parent == PresetImportItem::Parent::Missing)
        // TRN %1% is the name of the preset the imported one is based on.
        parts.push_back(format_wxstr(_L("Based on \"%1%\", which Forca doesn't have"), item.inherits));
    if (!item.unsupported.empty())
        parts.push_back(_L("Has settings Forca doesn't have"));
    if (!item.printers.empty() && item.missing_printers.size() == item.printers.size())
        parts.push_back(_L("None of its printers is installed"));
    if (item.clashes_with_user_preset || item.clashes_with_system_preset)
        parts.push_back(_L("Renamed: a preset already has its name"));
    wxString text;
    for (const wxString &part : parts)
        text += (text.empty() ? wxString() : wxString(L" \u00B7 ")) + part;
    return text;
}

// The settings a file changes compared with its parent, the ones Forca names differently and the ones it lacks.
wxString details_text(const PresetImportItem &item)
{
    wxString text = format_wxstr(_L("File: %1%"), item.file) + "\n";
    if (item.bambu_system)
        return text + (item.forca_has_it ?
                           // TRN %1% is a preset name.
                           format_wxstr(_L("One of Bambu Studio's built-in presets. Forca has the same preset built in, "
                                           "\"%1%\": select it in the list, there is nothing to import."), item.name) :
                           format_wxstr(_L("One of Bambu Studio's built-in presets, \"%1%\". Forca doesn't have it yet: "
                                           "built-in presets come with Forca's printer profiles and can't be imported "
                                           "from Bambu Studio."), item.name));
    if (!item.readable)
        return text + _L("Can't be read as a preset.");
    if (!item.best_parent.empty())
        // TRN %1% is a preset name.
        text += "\n" + format_wxstr(_L("Changes compared with \"%1%\":"), item.best_parent) + "\n";
    else
        text += "\n" + _L("Its settings:") + "\n";
    for (const PresetImportItem::Override &change : item.overrides)
        text += item.best_parent.empty() ? format_wxstr("  %1%: %2%\n", change.key, change.value) :
                                           // TRN %1% is a setting's name, %2% the imported value, %3% the value of the preset it's based on.
                                           "  " + format_wxstr(_L("%1%: %2% (was %3%)"), change.key, change.value, change.parent_value) + "\n";
    if (item.overrides.empty())
        text += "  " + _L("No changes.") + "\n";
    if (!item.renamed.empty()) {
        text += "\n" + _L("Named differently in Forca (taken over):") + "\n";
        for (const auto &[from, to] : item.renamed)
            text += format_wxstr("  %1% → %2%\n", from, to);
    }
    if (!item.unsupported.empty()) {
        text += "\n" + _L("Not in Forca (kept in the preset's notes):") + "\n";
        for (const auto &[key, value] : item.unsupported)
            text += format_wxstr("  %1% = %2%\n", key, value);
    }
    if (!item.missing_printers.empty()) {
        text += "\n" + _L("Printers it names that aren't installed:") + "\n";
        for (const std::string &printer : item.missing_printers)
            text += "  " + from_u8(printer) + "\n";
    }
    return text;
}

const PresetCollection &collection_of(const PresetBundle &bundle, Preset::Type type)
{
    if (type == Preset::TYPE_PRINT)
        return bundle.prints;
    if (type == Preset::TYPE_PRINTER)
        return bundle.printers;
    return bundle.filaments;
}

class PresetImportDialog : public DPIDialog
{
public:
    PresetImportDialog(wxWindow *parent, std::vector<PresetImportItem> items, bool repair)
        : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                    repair ? _L("Repair presets") : _L("Import presets"),
                    wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        auto *sizer = new wxBoxSizer(wxVERTICAL);

        auto *intro = new Label(this,
                                repair ? _L("These presets can't be loaded because the preset each one is based on is missing. "
                                            "Choose the preset to base it on. Nothing changes until you press Repair.") :
                                         _L("Check what Forca takes over from each file. Nothing is saved until you press Import."),
                                LB_AUTO_WRAP);
        sizer->Add(intro, 0, wxEXPAND | wxALL, FromDIP(10));

        // One row per file: the controls on one line, what needs a look on the line under it.
        m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_list->SetBackgroundColour(*wxWHITE);
        m_list->SetScrollRate(0, FromDIP(10));
        m_rows_sizer = new wxBoxSizer(wxVERTICAL);
        auto        *header      = new wxBoxSizer(wxHORIZONTAL);
        wxSizerItem *check_space = header->AddSpacer(0);
        for (const auto &[title, width] : {std::make_pair(_L("Name"), NAME_WIDTH), std::make_pair(_L("Based on"), PARENT_WIDTH),
                                           std::make_pair(_L("Printer"), PRINTER_WIDTH)}) {
            auto *head = new Label(m_list, Label::Head_14, title);
            head->SetMinSize(wxSize(FromDIP(width), -1));
            header->Add(head, 0, wxRIGHT, FromDIP(GAP));
        }
        m_rows_sizer->Add(header, 0, wxBOTTOM, FromDIP(4));
        for (PresetImportItem &item : items)
            add_row(std::move(item));
        if (!m_rows.empty())
            check_space->AssignSpacer(wxSize(m_rows.front()->use->GetBestSize().x + FromDIP(GAP), 0));
        m_list->SetSizer(m_rows_sizer);
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        // What Forca takes over from the row last worked on.
        m_details_title = new Label(this, Label::Head_14, wxEmptyString);
        sizer->Add(m_details_title, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        m_details = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(170)),
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
        sizer->Add(m_details, 0, wxEXPAND | wxALL, FromDIP(10));
        set_list_min_size();

        auto *buttons = new DialogButtons(this, {repair ? L("Repair") : L("Import"), "Cancel"}, repair ? _L("Repair") : _L("Import"));
        buttons->GetFIRST()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (confirm_names())
                EndModal(wxID_OK);
        });
        buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
        sizer->Add(buttons, 0, wxEXPAND);

        if (!m_rows.empty())
            show_details(*m_rows.front());
        SetSizerAndFit(sizer);
        m_list->FitInside();
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    // The rows the user kept ticked, with their choices.
    std::vector<std::pair<const PresetImportItem *, PresetImportChoice>> choices() const
    {
        std::vector<std::pair<const PresetImportItem *, PresetImportChoice>> out;
        for (const auto &row : m_rows) {
            if (!row->item.readable || !row->use->GetValue())
                continue;
            PresetImportChoice choice;
            choice.name = into_u8(row->name->GetTextCtrl()->GetValue().Strip(wxString::both));
            if (int i = row->parent->GetSelection(); i >= 0 && size_t(i) < row->parent_names.size())
                choice.parent = row->parent_names[i];
            if (int i = row->printers->GetSelection(); i >= 0 && size_t(i) < row->printer_names.size() && !row->printer_names[i].empty())
                choice.printers = {row->printer_names[i]};
            choice.replace_existing = m_replace.count(choice.name) != 0;
            // TRN The heading above the imported file's settings that Forca has no setting for, in the preset's notes.
            choice.notes_heading = into_u8(_L("Settings from the imported file that Forca doesn't have:"));
            out.emplace_back(&row->item, std::move(choice));
        }
        return out;
    }

protected:
    void on_dpi_changed(const wxRect &) override
    {
        for (const auto &row : m_rows) {
            row->use->Rescale();
            if (row->name != nullptr) {
                row->name->Rescale();
                row->parent->Rescale();
                row->printers->Rescale();
                row->details->Rescale();
            }
        }
        m_list->SetScrollRate(0, FromDIP(10));
        m_details->SetMinSize(wxSize(-1, FromDIP(170)));
        set_list_min_size();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    static constexpr int NAME_WIDTH = 320, PARENT_WIDTH = 320, PRINTER_WIDTH = 260, GAP = 8;

    // Wide enough for a row's controls and its status line, with room for the scroll bar.
    void set_list_min_size()
    {
        m_list->SetMinSize(wxSize(m_rows_sizer->CalcMin().x + FromDIP(30), FromDIP(300)));
        m_list->FitInside();
    }

    void add_row(PresetImportItem item)
    {
        auto          row    = std::make_unique<Row>();
        Row          &r      = *row;
        r.item               = std::move(item);
        PresetBundle &bundle = *wxGetApp().preset_bundle;
        auto         *line   = new wxBoxSizer(wxHORIZONTAL);

        r.use = new ::CheckBox(m_list);
        r.use->SetValue(r.item.readable);
        r.use->Enable(r.item.readable);
        line->Add(r.use, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(GAP));

        if (!r.item.readable) {
            auto *file = new Label(m_list, wxEmptyString);
            file->SetLabelText(from_u8(boost::filesystem::path(r.item.file).filename().string()));
            line->Add(file, 0, wxALIGN_CENTER_VERTICAL);
        } else {
            // Name: the file's, or a free one when a preset already has it.
            std::string name = r.item.name;
            if (r.item.clashes_with_user_preset || r.item.clashes_with_system_preset)
                // TRN %1% is a preset name; the name an imported preset gets when a preset already has its name.
                name = forca_free_preset_name(collection_of(bundle, r.item.type), name, into_u8(_L("%1% (imported)")));
            r.name = new TextInput(m_list, from_u8(name), "", "", wxDefaultPosition, wxSize(FromDIP(NAME_WIDTH), -1));
            line->Add(r.name, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(GAP));

            // Based on: the parent the import uses or suggests first, then the other filaments of its type.
            r.parent = new ::ComboBox(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(PARENT_WIDTH), -1), 0, nullptr,
                                      wxCB_READONLY);
            if (r.item.parent == PresetImportItem::Parent::None) {
                r.parent->Append(_L("Nothing: a full preset of its own"));
                r.parent_names.push_back(std::string());
                r.parent->Enable(false);
            } else {
                if (!r.item.best_parent.empty()) {
                    r.parent->Append(from_u8(r.item.best_parent));
                    r.parent_names.push_back(r.item.best_parent);
                }
                if (r.item.type == Preset::TYPE_FILAMENT)
                    for (const std::string &candidate : forca_filament_parent_candidates(bundle, r.item.filament_type, r.item.printers))
                        if (candidate != r.item.best_parent) {
                            r.parent->Append(from_u8(candidate));
                            r.parent_names.push_back(candidate);
                        }
            }
            if (!r.parent_names.empty() && !(r.item.parent == PresetImportItem::Parent::Missing && r.item.best_parent.empty()))
                r.parent->SetSelection(0);
            line->Add(r.parent, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(GAP));

            // Printer: the file's own, or one of the installed printers (the selected one first when none of the
            // file's is installed).
            r.printers = new ::ComboBox(m_list, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(PRINTER_WIDTH), -1), 0, nullptr,
                                        wxCB_READONLY);
            r.printers->Append(r.item.printers.empty() ? _L("As the preset it's based on") : _L("As in the file"));
            r.printer_names.push_back(std::string());
            for (const Preset &printer : bundle.printers)
                if (printer.is_visible && !printer.is_default) {
                    r.printers->Append(from_u8(printer.name));
                    r.printer_names.push_back(printer.name);
                }
            int printer = 0;
            if (!r.item.printers.empty() && r.item.missing_printers.size() == r.item.printers.size()) {
                const auto it = std::find(r.printer_names.begin(), r.printer_names.end(), bundle.printers.get_selected_preset_name());
                if (it != r.printer_names.end())
                    printer = int(it - r.printer_names.begin());
            }
            r.printers->SetSelection(printer);
            line->Add(r.printers, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(GAP));

            r.details = new Button(m_list, _L("Details"));
            r.details->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
            r.details->Bind(wxEVT_BUTTON, [this, &r](wxCommandEvent &) { show_details(r); });
            line->Add(r.details, 0, wxALIGN_CENTER_VERTICAL);

            // Working on a row shows its details too.
            r.name->GetTextCtrl()->Bind(wxEVT_SET_FOCUS, [this, &r](wxFocusEvent &e) {
                show_details(r);
                e.Skip();
            });
            for (::ComboBox *combo : {r.parent, r.printers})
                combo->Bind(wxEVT_COMBOBOX, [this, &r](wxCommandEvent &e) {
                    show_details(r);
                    e.Skip();
                });
        }
        // A file's controls and its status line, kept together and apart from the next file.
        auto *group = new wxBoxSizer(wxVERTICAL);
        group->Add(line, 0);
        if (const wxString status = status_text(r.item); !status.empty()) {
            auto *status_line = new wxBoxSizer(wxHORIZONTAL);
            status_line->AddSpacer(r.use->GetBestSize().x + FromDIP(GAP)); // under the name, past the check box
            auto *label = new Label(m_list, Label::Body_13, wxControl::EscapeMnemonics(status));
            label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#5C5C5C")));
            status_line->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            group->Add(status_line, 0, wxTOP, FromDIP(2));
        }
        m_rows_sizer->Add(group, 0, wxTOP | wxBOTTOM, FromDIP(5));
        m_rows.push_back(std::move(row));
    }

    void show_details(const Row &row)
    {
        const wxString name = row.item.name.empty() ? from_u8(boost::filesystem::path(row.item.file).filename().string()) :
                                                      from_u8(row.item.name);
        // TRN Heading above the details of one imported file; %1% is the preset's name.
        m_details_title->SetLabelText(format_wxstr(_L("What Forca takes over from \"%1%\":"), name));
        m_details->ChangeValue(details_text(row.item));
        m_details->ShowPosition(0);
        Layout();
    }

    // Before importing: a name a system preset has can't be used; one of the user's own is replaced only on a yes.
    bool confirm_names()
    {
        PresetBundle &bundle = *wxGetApp().preset_bundle;
        m_replace.clear();
        for (const auto &[item, choice] : choices()) {
            const Preset *existing = collection_of(bundle, item->type).find_preset(choice.name, false);
            if (choice.name.empty()) {
                MessageDialog(this, _L("Every preset to import needs a name."), _L("Import presets"), wxOK | wxICON_WARNING).ShowModal();
                return false;
            }
            if (existing == nullptr)
                continue;
            if (existing->is_system || existing->is_default) {
                MessageDialog(this, format_wxstr(_L("A system preset is already called \"%1%\". Choose another name."), choice.name),
                              _L("Import presets"), wxOK | wxICON_WARNING)
                    .ShowModal();
                return false;
            }
            if (MessageDialog(this, format_wxstr(_L("You already have a preset called \"%1%\". Replace it?"), choice.name),
                              _L("Import presets"), wxYES_NO | wxICON_QUESTION)
                    .ShowModal() != wxID_YES)
                return false;
            m_replace.insert(choice.name);
        }
        return true;
    }

    wxScrolledWindow                 *m_list          = nullptr;
    wxBoxSizer                       *m_rows_sizer    = nullptr;
    Label                            *m_details_title = nullptr;
    wxTextCtrl                       *m_details       = nullptr;
    std::vector<std::unique_ptr<Row>> m_rows;
    std::set<std::string>             m_replace; // names of the user's presets the user agreed to replace
};

} // namespace

int forca_review_preset_import(wxWindow *parent, const std::vector<std::string> &files, bool repair)
{
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    std::vector<PresetImportItem> items;
    for (const std::string &file : files)
        items.push_back(forca_analyse_preset_file(bundle, file));
    if (items.empty())
        return 0;

    PresetImportDialog dlg(parent, std::move(items), repair);
    if (dlg.ShowModal() != wxID_OK)
        return -1;
    int imported = 0;
    for (const auto &[item, choice] : dlg.choices())
        imported += forca_import_preset(bundle, *item, choice) ? 1 : 0;
    return imported;
}

void forca_offer_preset_repair()
{
    // Still missing: not loaded since, and the file is still there.
    PresetBundle &bundle = *wxGetApp().preset_bundle;
    std::vector<std::string> files;
    for (const PresetCollection::PresetMissingParent &entry : bundle.filaments.presets_missing_parent()) {
        boost::system::error_code ec;
        if (bundle.filaments.find_preset(entry.name, false) == nullptr && boost::filesystem::exists(entry.file, ec))
            files.push_back(entry.file);
    }
    if (files.empty() || wxGetApp().plater() == nullptr)
        return;
    wxGetApp().plater()->get_notification_manager()->push_notification(
        NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
        into_u8(_L("Some of your filament presets can't be loaded because the preset they are based on is missing.")),
        into_u8(_L("Repair them")), [files](wxEvtHandler *) {
            if (forca_review_preset_import(wxGetApp().mainframe, files, true) > 0) {
                wxGetApp().load_current_presets();
                wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
                wxGetApp().mainframe->update_side_preset_ui();
            }
            return true;
        });
}

}} // namespace Slic3r::GUI
