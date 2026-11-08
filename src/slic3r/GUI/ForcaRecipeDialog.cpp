#include "ForcaRecipeDialog.hpp"

#include "ForcaAI.hpp"             // forca_set_filament_slot
#include "ForcaAcademy.hpp"        // forca_academy_dir / forca_academy_read_record / forca_academy_relative
#include "ForcaBeforeYouPrint.hpp" // forca_option_label / forca_option_value
#include "ForcaExperiment.hpp"
#include "ForcaRecipe.hpp"
#include "GUI.hpp" // from_u8 / into_u8 / from_path / show_error
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "format.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>
#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/utils.h>

#include <cstddef>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;
using nlohmann::json;

namespace {

std::string today()
{
    const std::time_t t = std::time(nullptr);
    std::tm           tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

const json& section(const json& j, const char* key)
{
    static const json none = json::object();
    return j.is_object() && j.contains(key) && j[key].is_object() ? j[key] : none;
}

// Where a recipe comes from: a recorded print (or none) and the presets it is based on.
struct RecipeSource
{
    json                             record;   // the print's record.json (empty: no print)
    json                             settings; // its settings.json (empty: recorded before Forca kept it)
    fs::path                         print_folder;
    std::string                      process;   // parent presets
    std::vector<std::pair<int, std::string>> filaments; // 0-based slot, parent preset
    std::string                      winner_key, winner_value; // an experiment's winning value ("" = none)
    std::string                      source;    // the recipe page's line on how it was made
    std::string                      name;      // suggested recipe name
};

// One preset of the recipe as the dialog shows it.
struct RecipePart
{
    std::string                 type; // "process" | "filament"
    int                         slot = -1;
    const Preset*               parent = nullptr;
    std::vector<ForcaRecipeRow> rows;
    wxCheckListBox*             list = nullptr;
    std::vector<size_t>         listed; // list item -> row
};

wxString shorten(const wxString& s) { return s.length() > 60 ? s.Left(57) + "..." : s; }

class ForcaRecipeDialog : public wxDialog
{
public:
    ForcaRecipeDialog(wxWindow* parent, RecipeSource src)
        : wxDialog(parent, wxID_ANY, _L("Make a recipe"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_src(std::move(src))
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        const int wrap = FromDIP(620);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        auto* intro = new wxStaticText(this, wxID_ANY,
            _L("Save the settings of this print as new presets, based on the presets it was printed with, and switch to them. "
               "Untick a setting to leave it out. Printer settings are not part of a recipe."));
        intro->Wrap(wrap);
        root->Add(intro, 0, wxALL, pad);

        auto* name_row = new wxBoxSizer(wxHORIZONTAL);
        name_row->Add(new wxStaticText(this, wxID_ANY, _L("Name")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_name = new wxTextCtrl(this, wxID_ANY, from_u8(m_src.name), wxDefaultPosition, wxSize(FromDIP(360), -1));
        name_row->Add(m_name, 1);
        root->Add(name_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        // D3: with settings.json both baselines are known; without it only what was changed at the time.
        m_full = !section(m_src.settings, "process").empty();
        if (m_full) {
            const wxString choices[] = { _L("The presets as they are now"), _L("Only what was changed when the print was sent") };
            m_baseline = new wxRadioBox(this, wxID_ANY, _L("Compare with"), wxDefaultPosition, wxDefaultSize, 2, choices, 1, wxRA_SPECIFY_COLS);
            m_baseline->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent&) { fill(); });
            root->Add(m_baseline, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
        } else if (!m_src.record.empty()) {
            auto* note = new wxStaticText(this, wxID_ANY,
                _L("This print was recorded before Forca kept all of its settings, so the recipe has only the settings that were "
                   "changed from the presets when it was sent."));
            note->Wrap(wrap);
            root->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
        }

        PresetBundle* b = wxGetApp().preset_bundle;
        auto add_part = [&](const std::string& type, int slot, const std::string& parent_name) {
            PresetCollection& presets = type == "process" ? b->prints : b->filaments;
            const Preset*     p       = presets.find_preset(parent_name);
            if (p == nullptr || p->name != parent_name) {
                m_missing.push_back(parent_name);
                return;
            }
            RecipePart part{ type, slot, p };
            const wxString label = type == "process" ?
                                       format_wxstr(_L("Process, based on %1%"), from_u8(parent_name)) :
                                       format_wxstr(_L("Filament %1%, based on %2%"), slot + 1, from_u8(parent_name));
            auto* box = new wxStaticBoxSizer(wxVERTICAL, this, label);
            part.list = new wxCheckListBox(box->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxSize(FromDIP(600), FromDIP(110)));
            box->Add(part.list, 1, wxEXPAND | wxALL, FromDIP(4));
            root->Add(box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
            m_parts.push_back(std::move(part));
        };
        add_part("process", -1, m_src.process);
        for (const auto& [slot, name] : m_src.filaments)
            add_part("filament", slot, name);

        if (!m_missing.empty()) {
            wxString names;
            for (const std::string& n : m_missing)
                names += (names.empty() ? "" : ", ") + from_u8(n);
            auto* missing = new wxStaticText(this, wxID_ANY, format_wxstr(_L("These presets no longer exist, so they are left out: %1%"), names));
            missing->Wrap(wrap);
            root->Add(missing, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
        }
        m_unused = new wxStaticText(this, wxID_ANY, "");
        root->Add(m_unused, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_save        = new wxButton(this, wxID_OK, _L("Save and use"));
        auto* cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->AddStretchSpacer(1);
        buttons->Add(m_save, 0, wxRIGHT, FromDIP(6));
        buttons->Add(cancel, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, pad);
        m_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (save())
                EndModal(wxID_OK);
        });

        fill();
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    // The rows of every part for the chosen baseline; the lists show the usable ones (ticked), the rest is named below.
    void fill()
    {
        const bool changed_only = m_baseline != nullptr && m_baseline->GetSelection() == 1;
        const json& unsaved     = section(m_src.record, "unsaved_changes");
        wxString    unused;
        bool        any         = false;
        for (RecipePart& part : m_parts) {
            const char* type    = part.type == "process" ? "process" : "filament";
            const json& values  = m_full ? section(m_src.settings, type) : section(unsaved, type);
            const json& changed = section(unsaved, type);
            part.rows = forca_recipe_rows(part.parent->config, values, part.slot, changed_only ? &changed : nullptr);
            add_winner(part);
            part.list->Clear();
            part.listed.clear();
            for (size_t i = 0; i < part.rows.size(); ++i) {
                const ForcaRecipeRow& r = part.rows[i];
                if (!r.problem.empty()) {
                    unused += (unused.empty() ? "" : "\n") +
                              format_wxstr(r.problem == "cut" ? _L("%1% is left out: only the start of its value was recorded.") :
                                                                _L("%1% is left out: the recorded value doesn't fit this setting now."),
                                           forca_option_label(r.key));
                    continue;
                }
                part.list->Append(forca_option_label(r.key) + ": " + shorten(forca_option_value(r.key, r.parent)) + "  ->  " +
                                  shorten(forca_option_value(r.key, r.value)));
                part.list->Check(part.list->GetCount() - 1);
                part.listed.push_back(i);
                any = true;
            }
        }
        if (!any)
            unused += (unused.empty() ? "" : "\n") + _L("No setting differs from the presets, so there is nothing to save.");
        m_unused->SetLabel(unused);
        m_save->Enable(any);
        Layout();
    }

    // An experiment's winning value goes into the part whose preset holds that setting.
    void add_winner(RecipePart& part)
    {
        if (m_src.winner_key.empty())
            return;
        const ForcaExperimentTarget target = forca_experiment_preset_target(m_src.winner_key);
        if ((target.kind == ForcaExperimentTarget::Filament) != (part.type == "filament"))
            return;
        DynamicPrintConfig config = part.parent->config;
        if (!forca_experiment_set_value(config, target.key, m_src.winner_value))
            return;
        for (auto it = part.rows.begin(); it != part.rows.end(); ++it)
            if (it->key == target.key) {
                part.rows.erase(it);
                break;
            }
        const ConfigOption* now = part.parent->config.option(target.key);
        const ConfigOption* win = config.option(target.key);
        if (now == nullptr || win == nullptr || *now == *win)
            return;
        part.rows.push_back({ target.key, now->serialize(), win->serialize(), {} });
    }

    bool save()
    {
        const std::string name = into_u8(m_name->GetValue().Strip(wxString::both));
        if (name.empty()) {
            show_error(this, _L("Enter a name for the recipe."));
            return false;
        }
        PresetBundle* b = wxGetApp().preset_bundle;
        if (b->prints.current_is_dirty() || b->filaments.current_is_dirty()) {
            show_error(this, _L("Save or discard your changes to the process and filament presets first."));
            return false;
        }

        // The parts with ticked settings, each with its new preset's name; a name in use is refused, never replaced.
        struct ToSave { RecipePart* part; std::string name; std::vector<ForcaRecipeRow> rows; };
        std::vector<ToSave> todo;
        size_t filament_parts = 0;
        for (RecipePart& part : m_parts)
            filament_parts += part.type == "filament" ? 1 : 0;
        for (RecipePart& part : m_parts) {
            std::vector<ForcaRecipeRow> rows;
            for (size_t i = 0; i < part.listed.size(); ++i)
                if (part.list->IsChecked(unsigned(i)))
                    rows.push_back(part.rows[part.listed[i]]);
            if (rows.empty())
                continue;
            const std::string preset_name = part.type == "filament" && filament_parts > 1 ?
                                                // TRN A filament preset of a recipe when the print used several filaments: %1% = the recipe's name, %2% = the filament's number.
                                                into_u8(format_wxstr(_L("%1% (filament %2%)"), from_u8(name), part.slot + 1)) :
                                                name;
            PresetCollection& presets = part.type == "process" ? b->prints : b->filaments;
            if (const Preset* p = presets.find_preset(preset_name); p != nullptr && p->name == preset_name) {
                show_error(this, part.type == "process" ?
                                     format_wxstr(_L("A process preset named \"%1%\" already exists. Choose another name."), from_u8(preset_name)) :
                                     format_wxstr(_L("A filament preset named \"%1%\" already exists. Choose another name."), from_u8(preset_name)));
                return false;
            }
            todo.push_back({ &part, preset_name, std::move(rows) });
        }

        std::vector<ForcaRecipePreset> saved;
        for (ToSave& t : todo) {
            PresetCollection& presets = t.part->type == "process" ? b->prints : b->filaments;
            Preset            temp    = *t.part->parent;
            std::string       bad;
            if (!forca_recipe_apply(temp.config, t.rows, bad)) {
                show_error(this, format_wxstr(_L("Forca could not save the preset \"%1%\"."), from_u8(t.name)));
                return false;
            }
            const std::string prev_selected = presets.get_selected_preset_name();
            presets.save_current_preset(t.name, false, false, &temp); // saves to disk and selects it in the collection
            Preset* p = presets.find_preset(t.name, false, true);
            if (p == nullptr || p->name != t.name) {
                show_error(this, format_wxstr(_L("Forca could not save the preset \"%1%\"."), from_u8(t.name)));
                return false;
            }
            p->sync_info = "create";
            if (wxGetApp().is_user_login())
                p->user_id = wxGetApp().getAgent()->get_user_id();
            p->save_info();
            if (t.part->type == "filament") // the slot gets it below; the Filament tab keeps showing what it showed
                presets.select_preset_by_name(prev_selected, true);
            saved.push_back({ t.part->type, t.name, t.part->parent->name, t.part->slot, t.rows });
        }
        b->update_compatible(PresetSelectCompatibleType::Never);

        // D5: put the recipe in use; a part without ticked settings uses its parent.
        std::string err;
        for (const RecipePart& part : m_parts) {
            std::string use = part.parent->name;
            for (const ForcaRecipePreset& s : saved)
                if (s.type == part.type && s.slot == part.slot)
                    use = s.name;
            if (part.type == "process") {
                Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
                if (tab != nullptr && (use != part.parent->name || b->prints.get_selected_preset_name() != use)) // a new one: show it in the tab
                    tab->select_preset(use);
            } else if (size_t(part.slot) < b->filament_presets.size() && b->filament_presets[size_t(part.slot)] != use &&
                       !forca_set_filament_slot(size_t(part.slot), use, err)) {
                show_error(this, from_u8(err));
            }
        }

        // D6: the recipe's page, for the user and for any AI reading the Academy.
        const fs::path dir       = forca_academy_dir();
        const std::string rel    = m_src.print_folder.empty() ? std::string() : forca_academy_relative(dir, m_src.print_folder);
        const std::string title  = m_src.record.is_object() ? m_src.record.value("title", std::string()) : std::string();
        const std::string printer = m_src.record.is_object() && m_src.record.contains("printer") ?
                                        m_src.record["printer"].value("preset", std::string()) : std::string();
        std::string source = m_src.source;
        if (!printer.empty())
            source += " Printed on the printer preset " + printer + ".";
        if (m_baseline != nullptr)
            source += m_baseline->GetSelection() == 1 ? " Only the settings changed when the print was sent." :
                                                        " Every setting that differs from the presets as they were when the recipe was saved.";
        const fs::path page = forca_recipe_write_page(dir, name, today(), rel, title, source, saved, err);
        forca_academy_refresh_index();
        if (page.empty()) {
            show_error(this, from_u8(err));
            return true; // the presets are saved and in use
        }
        wxMessageDialog msg(this, format_wxstr(_L("Saved the recipe \"%1%\" and switched to its presets. Its page is in the Academy's recipes folder."), from_u8(name)),
                            _L("Make a recipe"), wxYES_NO | wxICON_INFORMATION);
        msg.SetYesNoLabels(_L("Open page"), _L("Close"));
        if (msg.ShowModal() == wxID_YES)
            wxLaunchDefaultApplication(from_path(page));
        return true;
    }

    RecipeSource            m_src;
    bool                    m_full     = false;
    wxRadioBox*             m_baseline = nullptr;
    wxTextCtrl*             m_name     = nullptr;
    wxStaticText*           m_unused   = nullptr;
    wxButton*               m_save     = nullptr;
    std::vector<RecipePart> m_parts;
    std::vector<std::string> m_missing;
};

// The presets and values of a recorded print.
bool load_print(const fs::path& folder, RecipeSource& src)
{
    if (!forca_academy_read_record(folder, src.record))
        return false;
    if (!forca_academy_read_settings(folder, src.settings))
        src.settings = json::object();
    src.print_folder = folder;
    src.process      = src.record.value("process", std::string());
    for (const int slot : forca_recipe_used_slots(src.record))
        for (const json& f : src.record["filaments"])
            if (f.value("slot", 0) == slot + 1)
                src.filaments.emplace_back(slot, f.value("preset", std::string()));
    src.name = src.record.value("title", std::string());
    return true;
}

bool show(wxWindow* parent, RecipeSource src)
{
    ForcaRecipeDialog dlg(parent, std::move(src));
    return dlg.ShowModal() == wxID_OK;
}

} // namespace

bool forca_make_recipe_from_print(wxWindow* parent, const fs::path& print_folder)
{
    RecipeSource src;
    if (!load_print(print_folder, src)) {
        show_error(parent, _L("This print's record.json is missing or damaged."));
        return false;
    }
    src.source = "Made from a successful print.";
    return show(parent, std::move(src));
}

bool forca_make_recipe_from_experiment(wxWindow* parent, const fs::path& folder, const ForcaExperimentPlan& plan, int winner)
{
    RecipeSource   src;
    const fs::path dir = forca_academy_dir();
    if (plan.prints.empty() || !load_print(dir / plan.prints.back(), src)) { // no print recorded: the plan's presets
        src          = RecipeSource();
        src.process  = plan.process;
        src.filaments = { { 0, plan.filament } };
        src.name     = plan.title;
    }
    const std::vector<std::string> names = forca_experiment_copy_names(plan);
    if (winner > 0 && size_t(winner) <= plan.variants.size()) {
        src.winner_key   = plan.key;
        src.winner_value = plan.variants[size_t(winner - 1)];
    }
    const std::string copy = winner >= 0 && size_t(winner) < names.size() ? names[size_t(winner)] : std::string();
    src.source = "Made from the winning copy (" + copy + ") of the experiment [" + plan.title + "](../" +
                 forca_academy_relative(dir, folder) + "/experiment.md).";
    return show(parent, std::move(src));
}

}} // namespace Slic3r::GUI
