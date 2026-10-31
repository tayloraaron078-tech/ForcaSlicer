#include "ForcaBeforeYouPrint.hpp"

#include "ForcaAcademy.hpp"
#include "ForcaPrintJournal.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/HyperLink.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ForcaPreflight.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <libslic3r/Config.hpp>
#include <cstddef>
#include <wx/event.h>
#include <utility>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>
#include <string>
#include <vector>
#include <wx/translation.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

wxString forca_option_label(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    return def ? wxGetTranslation(from_u8(def->label)) : from_u8(key);
}

// A serialized option value as the settings pages show it: On/Off, an enum's label, a number with its unit.
wxString forca_option_value(const std::string& key, const std::string& value)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr)
        return from_u8(value);
    if ((def->type == coBool || def->type == coBools) && (value == "0" || value == "1"))
        return value == "1" ? _L("On") : _L("Off");
    if (!def->enum_values.empty()) {
        const auto it = std::find(def->enum_values.begin(), def->enum_values.end(), value);
        const size_t i = size_t(it - def->enum_values.begin());
        if (it != def->enum_values.end() && i < def->enum_labels.size())
            return wxGetTranslation(from_u8(def->enum_labels[i]));
        return from_u8(value);
    }
    wxString out = from_u8(value);
    if (!def->sidetext.empty() && !out.EndsWith("%"))
        out += " " + wxGetTranslation(from_u8(def->sidetext));
    return out;
}

namespace {

constexpr size_t MAX_CHANGES_SHOWN = 3;
constexpr size_t MAX_HISTORY_LINES = 3;

// The settings changed and not saved in the process, the edited filament and the printer preset.
std::vector<ForcaPreflightValue> unsaved_changes(PresetBundle& bundle)
{
    std::vector<ForcaPreflightValue> out;
    const std::vector<PresetCollection*> collections = { &bundle.prints, &bundle.filaments, &bundle.printers };
    for (PresetCollection* c : collections) {
        const std::vector<std::string> dirty = c->current_dirty_options();
        if (dirty.empty())
            continue;
        auto changes = forca_preflight_changes(c->get_selected_preset().config, c->get_edited_preset().config, dirty);
        out.insert(out.end(), changes.begin(), changes.end());
    }
    return out;
}

wxString history_line(const std::string& filament, const ForcaAcademyHistory& h)
{
    wxString line = format_wxstr(_L("Earlier prints with %1% on this printer: %2% successful, %3% partial, %4% failed."),
                                 from_u8(filament), h.success, h.partial, h.failed);
    if (!h.tags.empty())
        line += " " + format_wxstr(_L("Most common problem: %1%."), forca_journal_tag_label(h.tags.front().first));
    return line;
}

wxStaticText* add_text(wxWindow* parent, wxBoxSizer* sizer, const wxString& text, int wrap)
{
    auto* t = new wxStaticText(parent, wxID_ANY, text);
    t->SetFont(Label::Body_13);
    t->Wrap(wrap);
    sizer->Add(t, 0, wxBOTTOM, parent->FromDIP(3));
    return t;
}

} // namespace

bool forca_before_you_print_enabled()
{
    // Preferences stores it with set_bool ("true"), so read it with is_true, which also takes "1".
    const std::string v = wxGetApp().app_config->get("forca_before_you_print");
    return v.empty() || AppConfig::is_true(v);
}

ForcaBeforeYouPrintPanel::ForcaBeforeYouPrintPanel(wxWindow* parent, int wrap_width)
    : wxPanel(parent, wxID_ANY), m_wrap_width(wrap_width)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    m_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(m_sizer);
}

bool ForcaBeforeYouPrintPanel::refresh(int plate_idx, const std::string& printer_key)
{
    m_plate_idx   = plate_idx;
    m_printer_key = printer_key;

    Freeze();
    m_sizer->Clear(true);

    PresetBundle* bundle = wxGetApp().preset_bundle;
    Plater*       plater = wxGetApp().plater();
    if (!forca_before_you_print_enabled() || bundle == nullptr || plater == nullptr) {
        Thaw();
        Hide();
        return false;
    }

    PartPlateList& plates = plater->get_partplate_list();
    PartPlate*     plate  = plate_idx >= 0 ? plates.get_plate(plate_idx) : plates.get_curr_plate();
    // The filament presets this plate prints with (slots are 1-based).
    std::vector<std::pair<size_t, std::string>> used;
    if (plate)
        for (int id : plate->get_extruders(false))
            if (id >= 1 && size_t(id) <= bundle->filament_presets.size())
                used.emplace_back(size_t(id - 1), bundle->filament_presets[id - 1]);

    const int wrap = FromDIP(m_wrap_width);

    // 1. How earlier prints with this printer + filament went.
    if (forca_academy_enabled() && !used.empty()) {
        std::string key = m_printer_key;
        if (key.empty())
            key = bundle->physical_printers.has_selection() ? bundle->physical_printers.get_selected_printer_name()
                                                            : bundle->printers.get_edited_preset().name;
        const std::vector<ForcaAcademyPrint> prints = forca_academy_list_prints(forca_academy_dir());
        size_t                               lines  = 0;
        std::vector<ForcaAcademyHistory>     histories;
        for (const auto& [slot, filament] : used) {
            const ForcaAcademyHistory h = forca_academy_history(prints, key, filament);
            histories.push_back(h);
            if (h.recorded() == 0 || lines == MAX_HISTORY_LINES)
                continue;
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            auto* t   = new wxStaticText(this, wxID_ANY, history_line(filament, h));
            t->SetFont(Label::Body_13);
            t->Wrap(wrap - FromDIP(60));
            row->Add(t, 1, wxRIGHT, FromDIP(8));
            auto* show = new HyperLink(this, _L("Show"));
            const std::vector<boost::filesystem::path> folders = h.folders;
            const wxString filter = format_wxstr(_L("Prints with %1% on this printer"), from_u8(filament));
            show->Bind(wxEVT_LEFT_DOWN, [this, folders, filter](wxMouseEvent&) {
                ForcaPrintJournalDialog dlg(wxGetTopLevelParent(this), folders, filter);
                dlg.ShowModal();
            });
            row->Add(show, 0);
            m_sizer->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(3));
            ++lines;
        }
        const DynamicPrintConfig& process = bundle->prints.get_edited_preset().config;
        if (forca_academy_suggest_thick_bridges(histories, process.has("thick_bridges") && process.opt_bool("thick_bridges")))
            add_text(this, m_sizer,
                     _L("Earlier prints here were tagged Bridging, and Thick external bridges is off. In Forca's tests, "
                        "turning it on (Quality > Bridging) fixed sagging and torn bridges."),
                     wrap);
    }

    // 2. Settings changed and not saved.
    const std::vector<ForcaPreflightValue> changes = unsaved_changes(*bundle);
    if (!changes.empty()) {
        wxString list;
        for (size_t i = 0; i < changes.size() && i < MAX_CHANGES_SHOWN; ++i) {
            if (i > 0)
                list += ", ";
            list += format_wxstr(_L("%1% %2% (preset %3%)"), forca_option_label(changes[i].key),
                                 forca_option_value(changes[i].key, changes[i].value), forca_option_value(changes[i].key, changes[i].saved));
        }
        if (changes.size() > MAX_CHANGES_SHOWN)
            list += " " + format_wxstr(_L("and %1% more"), changes.size() - MAX_CHANGES_SHOWN);
        add_text(this, m_sizer, _L("Changed and not saved:") + " " + list, wrap);
    }

    // 3. Details: always offered (the values are always there), collapsed by default.
    const bool open   = wxGetApp().app_config->get("forca_before_you_print_details") == "1";
    auto*      toggle = new HyperLink(this, (open ? wxString::FromUTF8("\xE2\x96\xBE ") : wxString::FromUTF8("\xE2\x96\xB8 ")) + _L("Details"));
    toggle->Bind(wxEVT_LEFT_DOWN, [this, open](wxMouseEvent&) {
        wxGetApp().app_config->set("forca_before_you_print_details", open ? "0" : "1");
        // Rebuild after this click has been handled: the link is destroyed by the rebuild.
        CallAfter([this] {
            refresh(m_plate_idx, m_printer_key);
            if (on_layout_changed)
                on_layout_changed();
        });
    });
    m_sizer->Add(toggle, 0, wxBOTTOM, FromDIP(3));
    if (open) {
        DynamicPrintConfig full = bundle->full_config();
        if (plate)
            full.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(plate->get_bed_type(true)));
        wxString process;
        for (const ForcaPreflightValue& v : forca_preflight_process_details(full)) {
            if (!process.empty())
                process += wxString::FromUTF8(" \xC2\xB7 ");
            process += forca_option_label(v.key) + ": " + forca_option_value(v.key, v.value);
        }
        add_text(this, m_sizer, process, wrap);
        for (const auto& [slot, filament] : used) {
            wxString line = from_u8(filament) + ": ";
            bool     first = true;
            for (const ForcaPreflightValue& v : forca_preflight_filament_details(full, slot)) {
                line += (first ? "" : ", ") + forca_option_label(v.key) + " " + forca_option_value(v.key, v.value);
                first = false;
            }
            add_text(this, m_sizer, line, wrap);
        }
    }

    // The title goes first; it is added last so it is only there when the block is.
    auto* title = new wxStaticText(this, wxID_ANY, _L("Before you print"));
    title->SetFont(Label::Head_14);
    m_sizer->Insert(0, title, 0, wxBOTTOM, FromDIP(4));

    // The rebuilt texts missed the dialog's dark-mode pass (a reused dialog ran it before this refresh).
    wxGetApp().UpdateDarkUIWin(this);
    Layout();
    Thaw();
    Show();
    return true;
}

}} // namespace Slic3r::GUI
