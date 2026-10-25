#include "ForcaExperimentDialog.hpp"

#include "ForcaAcademy.hpp"        // forca_academy_dir / forca_academy_refresh_index
#include "ForcaBeforeYouPrint.hpp" // forca_option_label / forca_option_value
#include "ForcaExperiment.hpp"
#include "ForcaPrintJournal.hpp" // forca_journal_tag_label
#include "GUI.hpp" // from_u8 / into_u8 / from_path / show_error / show_info
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Tab.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Format/bbs_3mf.hpp" // LoadStrategy
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp" // resources_dir
#include "libslic3r/libslic3r.h" // PI, Axis

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <wx/arrstr.h>
#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/gdicmn.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/toplevel.h>
#include <wx/window.h>

#include <algorithm>
#include <cstddef>
#include <ctime>
#include <exception>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;

namespace {

constexpr size_t MAX_VARIANTS = 3; // baseline + up to 3 (D6)

struct TestModel
{
    const char* id;   // plan.json "model"
    const char* file; // in resources/forca_experiments
};
const TestModel BUILT_IN[] = { { "bridge", "bridge_test.stl" }, { "overhang", "overhang_test.stl" }, { "stringing", "stringing_test.stl" } };

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

bool is_switch(const ConfigOptionDef* def) { return def != nullptr && (def->type == coBool || def->type == coBools); }
bool is_list(const ConfigOptionDef* def) { return def != nullptr && !is_switch(def) && !def->enum_values.empty() && (def->type == coEnum || def->type == coEnums); }
bool is_number(const ConfigOptionDef* def)
{
    if (def == nullptr || is_list(def))
        return false;
    switch (def->type) {
    case coInt: case coInts: case coFloat: case coFloats: case coPercent: case coPercents: case coFloatOrPercent: case coFloatsOrPercents: return true;
    default: return false;
    }
}

// Every setting that can differ per copy on one plate and that the dialog can edit (switches, lists, numbers).
std::vector<std::string> all_plate_settings()
{
    std::vector<std::string> keys;
    for (const std::string& k : PrintObjectConfig().keys())
        keys.push_back(k);
    for (const std::string& k : PrintRegionConfig().keys())
        keys.push_back(k);
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [](const std::string& k) {
                                  const ConfigOptionDef* def = print_config_def.get(forca_experiment_source_key(k));
                                  const ConfigOptionDef* own = print_config_def.get(k);
                                  return own == nullptr || own->label.empty() || !(is_switch(def) || is_list(def) || is_number(def));
                              }),
               keys.end());
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    std::sort(keys.begin(), keys.end(), [](const std::string& a, const std::string& b) { return forca_option_label(a).CmpNoCase(forca_option_label(b)) < 0; });
    return keys;
}

// The space between copies printed by object: the printer's extruder clearance, plus a per-object skirt on both
// sides (Orca's sequential check grows each object by half the clearance and its skirt).
double copy_gap(const DynamicPrintConfig& config)
{
    double gap = config.opt_float("extruder_clearance_radius") + 2.;
    if (config.opt_serialize("skirt_type") == "perobject" && config.opt_int("skirt_loops") > 0)
        gap += 2. * (config.opt_float("skirt_distance") + config.opt_int("skirt_loops") * 1.5 * config.opt_float("nozzle_diameter", 0));
    return gap;
}

// Puts the copies on their plates (making the plates that are missing), each plate's copies in reading order in a
// grid centred on it, `gap` apart. Copies print in object order, A first, which is the order Orca's check assumes.
void place_copies(Plater* p, const std::vector<size_t>& objects, const ForcaExperimentLayout& layout,
                  const std::vector<size_t>& plates, double gap)
{
    if (objects.empty() || plates.size() != objects.size())
        return;
    PartPlateList& list = p->get_partplate_list();
    while (list.get_plate_count() < int(plates.back() + 1) && list.create_plate(true) >= 0)
        ;
    if (layout.turned)
        for (size_t i : objects) {
            ModelInstance* inst = p->model().objects[i]->instances.front();
            inst->set_rotation(Z, inst->get_rotation(Z) + 0.5 * PI);
            p->model().objects[i]->invalidate_bounding_box();
        }
    const Vec3d  s3 = p->model().objects[objects.front()]->instance_bounding_box(0).size();
    const Vec2d  size(s3.x(), s3.y());
    const size_t cols = std::max<size_t>(layout.cols, 1);
    for (size_t first = 0; first < objects.size();) {
        const size_t plate = plates[first];
        size_t       last  = first;
        while (last < objects.size() && plates[last] == plate)
            ++last;
        const BoundingBoxf3 bed   = list.get_plate(int(std::min<size_t>(plate, size_t(list.get_plate_count() - 1))))->get_build_volume();
        const size_t        n     = last - first;
        const size_t        c     = std::min(cols, n);
        const size_t        rows  = (n + c - 1) / c;
        const Vec2d         total(c * size.x() + (c - 1) * gap, rows * size.y() + (rows - 1) * gap);
        const Vec2d         start(bed.center().x() - 0.5 * total.x() + 0.5 * size.x(), bed.center().y() + 0.5 * total.y() - 0.5 * size.y());
        for (size_t k = 0; k < n; ++k) {
            ModelObject*        o      = p->model().objects[objects[first + k]];
            ModelInstance*      inst   = o->instances.front();
            const BoundingBoxf3 box    = o->instance_bounding_box(0);
            const Vec2d         target = start + Vec2d(double(k % c) * (size.x() + gap), -double(k / c) * (size.y() + gap));
            inst->set_offset(inst->get_offset() + Vec3d(target.x() - box.center().x(), target.y() - box.center().y(), 0.));
            o->invalidate_bounding_box();
        }
        first = last;
    }
    list.reload_all_objects();
}

class ForcaExperimentDialog : public wxDialog
{
public:
    ForcaExperimentDialog(wxWindow* parent, const std::string& tag, const wxString& tag_label)
        : wxDialog(parent, wxID_ANY, _L("Plan an experiment"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_tag(tag)
        , m_tag_label(tag_label)
        , m_config(wxGetApp().preset_bundle->full_config())
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        const int wrap = FromDIP(600);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        auto* intro = new wxStaticText(this, wxID_ANY,
            _L("Change one setting and keep everything else the same. Forca builds a test plate in a new project with one copy "
               "per value: A keeps the current value (the baseline), the others try the new values. Print it and compare each "
               "copy with A."));
        intro->Wrap(wrap);
        root->Add(intro, 0, wxALL, pad);

        PresetBundle* b       = wxGetApp().preset_bundle;
        m_printer             = b->printers.get_selected_preset_name();
        m_filament            = b->filament_presets.empty() ? std::string() : b->filament_presets.front();
        m_process             = b->prints.get_selected_preset_name();
        auto* presets         = new wxStaticText(this, wxID_ANY,
            format_wxstr(_L("The plate is for the presets selected now: %1% / %2% / %3%."), m_printer, m_filament, m_process));
        presets->Wrap(wrap);
        root->Add(presets, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(10));
        grid->AddGrowableCol(1, 1);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Setting to change")), 0, wxALIGN_CENTER_VERTICAL);
        m_setting = new wxChoice(this, wxID_ANY);
        m_keys    = forca_experiment_candidates(tag);
        for (const std::string& k : m_keys)
            m_setting->Append(forca_option_label(k));
        m_setting->Append(_L("Another setting") + dots);
        grid->Add(m_setting, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Now (copy A)")), 0, wxALIGN_CENTER_VERTICAL);
        m_now = new wxStaticText(this, wxID_ANY, "");
        grid->Add(m_now, 1, wxALIGN_CENTER_VERTICAL);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Values to try")), 0, wxALIGN_TOP | wxTOP, FromDIP(4));
        auto* values = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < MAX_VARIANTS; ++i) {
            auto* label = new wxStaticText(this, wxID_ANY, wxString::Format("%c", char('B' + i)));
            m_text[i]   = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(90), -1));
            m_pick[i]   = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(150), -1));
            values->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
            values->Add(m_text[i], 0, wxRIGHT, FromDIP(4));
            values->Add(m_pick[i], 0, wxRIGHT, FromDIP(12));
            m_text[i]->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_hypothesis(); });
            m_pick[i]->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_hypothesis(); });
        }
        grid->Add(values, 1);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Hypothesis")), 0, wxALIGN_TOP | wxTOP, FromDIP(4));
        m_hypothesis = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(-1, FromDIP(60)), wxTE_MULTILINE);
        m_hypothesis->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { if (!m_filling) m_hypothesis_edited = true; });
        grid->Add(m_hypothesis, 1, wxEXPAND);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Test model")), 0, wxALIGN_CENTER_VERTICAL);
        m_model = new wxChoice(this, wxID_ANY);
        m_model->Append(_L("Bridge test (built in)"));
        m_model->Append(_L("Overhang test (built in)"));
        m_model->Append(_L("Stringing test: two towers (built in)"));
        // Every object in the project can be the test model; the selected one (if any) is offered first.
        Plater*   p        = wxGetApp().plater();
        const int selected = p ? p->get_selected_object_idx() : -1;
        if (p != nullptr) {
            if (selected >= 0 && size_t(selected) < p->model().objects.size())
                m_objects.push_back(selected);
            for (size_t i = 0; i < p->model().objects.size(); ++i)
                if (int(i) != selected)
                    m_objects.push_back(int(i));
        }
        for (int i : m_objects)
            m_model->Append(format_wxstr(_L("Object in this project: %1%"), p->model().objects[size_t(i)]->name));
        m_model->SetSelection(tag == "bridging" ? 0 : tag == "stringing" ? 2 : selected >= 0 ? 3 : 1);
        grid->Add(m_model, 1, wxEXPAND);
        root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

        auto* note = new wxStaticText(this, wxID_ANY,
            _L("Forca prints the test plate by object: each copy prints on its own, start to finish, so no copy's travel, "
               "strings or temperature reaches another. The copies are spaced so the print head clears them, on more than "
               "one plate if they don't all fit on one."));
        note->Wrap(wrap);
        root->Add(note, 0, wxALL, pad);
        auto* wizard = new wxStaticText(this, wxID_ANY,
            _L("To find a value from scratch, use a Calibration Wizard test instead: it sweeps the value on one print."));
        wizard->Wrap(wrap);
        root->Add(wizard, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* create  = new wxButton(this, wxID_OK, _L("Create the test plate"));
        auto* cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->AddStretchSpacer(1);
        buttons->Add(create, 0, wxRIGHT, FromDIP(6));
        buttons->Add(cancel, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, pad);

        m_setting->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { setting_picked(); });
        create->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { create_plate(); });

        if (!m_keys.empty()) {
            m_setting->SetSelection(0);
            show_setting(m_keys.front());
        } else
            show_setting({});
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    void setting_picked()
    {
        const int sel = m_setting->GetSelection();
        if (sel >= 0 && size_t(sel) < m_keys.size()) {
            show_setting(m_keys[size_t(sel)]);
            return;
        }
        // "Another setting...": anything that can differ per copy on one plate (D3).
        const std::vector<std::string> all = all_plate_settings();
        wxArrayString                  names;
        for (const std::string& k : all)
            names.Add(forca_option_label(k));
        wxSingleChoiceDialog dlg(this, _L("Only settings that can differ between objects on one plate are listed."), _L("Another setting"), names);
        if (dlg.ShowModal() != wxID_OK || dlg.GetSelection() < 0) {
            const auto it = std::find(m_keys.begin(), m_keys.end(), m_key);
            m_setting->SetSelection(it == m_keys.end() ? wxNOT_FOUND : int(it - m_keys.begin()));
            return;
        }
        const std::string key = all[size_t(dlg.GetSelection())];
        if (std::find(m_keys.begin(), m_keys.end(), key) == m_keys.end()) {
            m_keys.push_back(key);
            m_setting->Insert(forca_option_label(key), unsigned(m_keys.size() - 1));
        }
        m_setting->SetSelection(int(std::find(m_keys.begin(), m_keys.end(), key) - m_keys.begin()));
        show_setting(key);
    }

    // The value as the settings pages show it (an override shows like the setting it stands in for).
    wxString shown(const std::string& value) const { return forca_option_value(forca_experiment_source_key(m_key), value); }

    void show_setting(const std::string& key)
    {
        m_key                      = key;
        m_def                      = key.empty() ? nullptr : print_config_def.get(forca_experiment_source_key(key));
        m_baseline                 = key.empty() ? std::string() : forca_experiment_current_value(m_config, key);
        const size_t count         = is_switch(m_def) ? 1 : MAX_VARIANTS;
        const std::vector<std::string> suggested = key.empty() ? std::vector<std::string>() : forca_experiment_suggest_values(key, m_baseline, count);
        m_now->SetLabel(key.empty() ? wxString() : shown(m_baseline));

        m_choices.clear();
        if (is_switch(m_def))
            m_choices = { "1", "0" };
        else if (is_list(m_def))
            m_choices = m_def->enum_values;
        m_filling = true;
        for (size_t i = 0; i < MAX_VARIANTS; ++i) {
            const bool pick = !m_choices.empty();
            m_pick[i]->Clear();
            if (pick) {
                m_pick[i]->Append(_L("(none)"));
                for (const std::string& c : m_choices)
                    m_pick[i]->Append(shown(c));
                int sel = 0;
                if (i < suggested.size())
                    sel = int(std::find(m_choices.begin(), m_choices.end(), suggested[i]) - m_choices.begin()) + 1;
                m_pick[i]->SetSelection(sel);
            }
            m_text[i]->ChangeValue(!pick && i < suggested.size() ? from_u8(suggested[i]) : wxString());
            m_pick[i]->Show(pick && i < count);
            m_text[i]->Show(!pick && !key.empty());
        }
        m_filling = false;
        update_hypothesis();
        Layout();
        Fit();
    }

    // The values the user entered, in order, without blanks, repeats or the baseline.
    std::vector<std::string> variants() const
    {
        std::vector<std::string> out;
        for (size_t i = 0; i < MAX_VARIANTS; ++i) {
            std::string v;
            if (!m_choices.empty()) {
                const int sel = m_pick[i]->IsShown() ? m_pick[i]->GetSelection() : 0;
                if (sel > 0 && size_t(sel) <= m_choices.size())
                    v = m_choices[size_t(sel - 1)];
            } else if (m_text[i]->IsShown()) {
                v = into_u8(m_text[i]->GetValue().Strip(wxString::both));
            }
            if (!v.empty() && v != m_baseline && std::find(out.begin(), out.end(), v) == out.end())
                out.push_back(v);
        }
        return out;
    }

    void update_hypothesis()
    {
        if (m_filling || m_hypothesis_edited)
            return;
        const std::vector<std::string> vs = variants();
        wxString                       text;
        if (!m_key.empty() && !vs.empty())
            text = m_tag.empty() ? format_wxstr(_L("Changing %1% from %2% to %3% gives a better print."), forca_option_label(m_key), shown(m_baseline), shown(vs.front())) :
                                   // TRN %4% is a print problem, e.g. "Stringing".
                                   format_wxstr(_L("Changing %1% from %2% to %3% reduces this problem: %4%."), forca_option_label(m_key), shown(m_baseline), shown(vs.front()), m_tag_label);
        m_filling = true;
        m_hypothesis->ChangeValue(text);
        m_filling = false;
    }

    // A value the setting accepts: it parses, and a number is within the setting's limits (never below zero).
    bool valid(const std::string& value) const
    {
        DynamicPrintConfig trial;
        try {
            trial.set_deserialize_strict(m_key, value);
        } catch (const std::exception&) {
            return false;
        }
        if (is_number(m_def)) {
            try {
                const double v = std::stod(value.back() == '%' ? value.substr(0, value.size() - 1) : value);
                return v >= std::max(0., double(m_def->min)) && v <= double(m_def->max);
            } catch (const std::exception&) {
                return false;
            }
        }
        return true;
    }

    void create_plate()
    {
        Plater* p = wxGetApp().plater();
        if (p == nullptr || m_key.empty())
            return;
        const wxString                 label = forca_option_label(m_key);
        const std::vector<std::string> vs    = variants();
        if (vs.empty()) {
            show_error(this, _L("Enter at least one value to try that differs from the current one."));
            return;
        }
        for (const std::string& v : vs)
            if (!valid(v)) {
                show_error(this, format_wxstr(_L("%1% is not a valid value for %2%."), v, label));
                return;
            }

        // The model: a built-in test, or an object's shape (saved before the new project clears the plate).
        const int   model_sel = m_model->GetSelection();
        const int   built_in  = int(std::size(BUILT_IN));
        fs::path    model_file;
        fs::path    temp_file;
        std::string model_id;
        if (model_sel >= 0 && model_sel < built_in) {
            model_file = fs::path(resources_dir()) / "forca_experiments" / BUILT_IN[model_sel].file;
            model_id   = BUILT_IN[model_sel].id;
        } else if (model_sel >= built_in && size_t(model_sel - built_in) < m_objects.size() &&
                   size_t(m_objects[size_t(model_sel - built_in)]) < p->model().objects.size()) {
            const ModelObject* o = p->model().objects[size_t(m_objects[size_t(model_sel - built_in)])];
            TriangleMesh       mesh = o->mesh();
            if (!o->instances.empty())
                mesh.transform(o->instances.front()->get_matrix_no_offset());
            temp_file = into_path(wxStandardPaths::Get().GetTempDir()) / fs::unique_path("forca-experiment-%%%%-%%%%.stl");
            if (!store_stl(temp_file.string().c_str(), &mesh, true)) {
                show_error(this, _L("Could not save a copy of the object."));
                return;
            }
            model_file = temp_file;
            model_id   = o->name;
        }
        boost::system::error_code ec;
        if (model_file.empty() || !fs::is_regular_file(model_file, ec)) {
            show_error(this, _L("The test model is missing from Forca's resources folder."));
            return;
        }
        struct TempFile
        {
            fs::path f;
            ~TempFile()
            {
                boost::system::error_code ec;
                if (!f.empty())
                    fs::remove(f, ec);
            }
        } remove_temp{ temp_file };

        // Where the copies go, printed by object: the extruder clearance between them, and as many plates as needed.
        const size_t copies = vs.size() + 1;
        const double gap    = copy_gap(m_config);
        ForcaExperimentLayout layout;
        {
            Model m;
            try {
                m = Model::read_from_file(model_file.string());
            } catch (const std::exception&) {}
            BoundingBoxf bed;
            if (const auto* area = m_config.option<ConfigOptionPoints>("printable_area"))
                for (const Vec2d& pt : area->values)
                    bed.merge(pt);
            if (m.objects.empty() || !bed.defined) {
                show_error(this, _L("Forca could not load the test model."));
                return;
            }
            const Vec3d size = m.objects.front()->raw_mesh_bounding_box().size();
            layout = forca_experiment_layout(size.x(), size.y(), size.z(), bed.size().x(), bed.size().y(),
                                             m_config.opt_float("printable_height"), gap,
                                             m_config.opt_float("extruder_clearance_height_to_rod"),
                                             m_config.opt_float("extruder_clearance_height_to_lid"));
            if (layout.per_plate == 0) {
                show_error(this, _L("The test model does not fit on the bed."));
                return;
            }
        }
        const std::vector<size_t> plates = forca_experiment_plates(copies, layout.per_plate);

        ForcaExperimentPlan plan;
        plan.date       = today();
        plan.printer    = m_printer;
        plan.filament   = m_filament;
        plan.process    = m_process;
        plan.tag        = m_tag;
        plan.key        = m_key;
        plan.label      = into_u8(label);
        plan.variants   = vs;
        plan.hypothesis = into_u8(m_hypothesis->GetValue());
        plan.model      = model_id;
        plan.by_object  = true;
        plan.plates     = plates;
        std::string title = plan.label + ": " + into_u8(shown(m_baseline));
        for (const std::string& v : vs)
            title += " / " + into_u8(shown(v));
        plan.title = title;

        // A new project (the usual "save changes?" question first). Copy A keeps the presets' value as it is after that.
        if (p->new_project(false, false, from_u8(plan.title)) == wxID_CANCEL)
            return;
        plan.baseline = forca_experiment_current_value(wxGetApp().preset_bundle->full_config(), m_key);
        plan.shown    = { into_u8(shown(plan.baseline)) };
        for (const std::string& v : vs)
            plan.shown.push_back(into_u8(shown(v)));
        const std::vector<std::string> names = forca_experiment_copy_names(plan);

        const std::vector<fs::path> files(copies, model_file);
        const std::vector<size_t>   loaded = p->load_files(files, LoadStrategy::LoadModel, false);
        if (loaded.size() != copies) {
            show_error(this, _L("Forca could not load the test model."));
            return;
        }
        for (size_t i = 0; i < copies; ++i) {
            ModelObject* o = p->model().objects[loaded[i]];
            o->name        = names[i];
            if (i > 0) { // A, the baseline, has no setting of its own
                DynamicPrintConfig c = o->config.get();
                c.set_deserialize_strict(m_key, vs[i - 1]);
                o->config.assign_config(c);
            }
        }
        // Forca places the copies itself: the arrange job could leave them stacked when the plate prints by object.
        // Each plate's own print sequence is set, not the process preset's, so the presets stay as they are.
        place_copies(p, loaded, layout, plates, gap);
        PartPlateList& plate_list = p->get_partplate_list();
        for (int i = 0; i < plate_list.get_plate_count(); ++i)
            plate_list.get_plate(i)->set_print_seq(PrintSequence::ByObject);
        if (ObjectList* list = wxGetApp().obj_list())
            list->reload_all_plates(); // shows the names and the per-object settings
        p->changed_objects(loaded);
        plate_list.select_plate(0);
        p->config_change_notification(*plate_list.get_plate(0)->config(), "print_sequence");
        p->update();

        std::string    err;
        const fs::path folder = forca_experiment_create(forca_academy_dir(), plan, err);
        if (folder.empty())
            show_error(this, from_u8(err));
        else {
            forca_academy_refresh_index();
            const size_t count = plates.back() + 1;
            show_info(this,
                      count == 1 ?
                          format_wxstr(_L("The test plate is ready. It prints by object (set in the plate's settings; the process "
                                          "preset is unchanged). Print it, then compare each copy with A, the baseline. The plan "
                                          "is saved in Forca Academy: %1%"), from_path(folder)) :
                          // TRN %1% is a number of plates (2 or more), worded as a label so it needs no plural forms.
                          format_wxstr(_L("Not all copies fit on one plate printed by object, so the test is spread over "
                                          "more than one (plates: %1%). Each plate prints by object (set in the plate's "
                                          "settings; the process preset is unchanged). Print every plate, then compare each "
                                          "copy with A, the baseline. The plan is saved in Forca Academy: %2%"),
                                       count, from_path(folder)),
                      _L("Plan an experiment"));
        }
        EndModal(wxID_OK);
    }

    std::string                 m_tag;
    wxString                    m_tag_label;
    DynamicPrintConfig          m_config; // the presets' full config when the dialog opened
    std::string                 m_printer, m_filament, m_process;
    std::vector<std::string>    m_keys;    // the settings in m_setting, in order
    std::string                 m_key;     // the chosen one
    const ConfigOptionDef*      m_def = nullptr; // its definition (for an override: the setting it stands in for)
    std::string                 m_baseline;
    std::vector<std::string>    m_choices; // a switch's or a list's values (m_pick), empty for a number (m_text)
    std::vector<int>            m_objects; // the project's objects in m_model, after the built-in tests
    bool                        m_filling           = false;
    bool                        m_hypothesis_edited = false;
    wxChoice*                   m_setting    = nullptr;
    wxStaticText*               m_now        = nullptr;
    wxTextCtrl*                 m_text[MAX_VARIANTS] = {};
    wxChoice*                   m_pick[MAX_VARIANTS] = {};
    wxTextCtrl*                 m_hypothesis = nullptr;
    wxChoice*                   m_model      = nullptr;
};

} // namespace

bool forca_plan_experiment(wxWindow* parent, const std::string& tag, const wxString& tag_label)
{
    ForcaExperimentDialog dlg(parent, tag, tag_label);
    return dlg.ShowModal() == wxID_OK;
}

namespace {

constexpr int ID_NEW_PLATE = wxID_HIGHEST + 1;

class ForcaJudgeDialog : public wxDialog
{
public:
    ForcaJudgeDialog(wxWindow* parent, const fs::path& folder, const ForcaExperimentPlan& plan)
        : wxDialog(parent, wxID_ANY, _L("Compare the copies"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_folder(folder)
        , m_plan(plan)
        , m_names(forca_experiment_copy_names(plan))
    {
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        const int wrap = FromDIP(640);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        auto* title = new wxStaticText(this, wxID_ANY, from_u8(plan.title));
        title->SetFont(Label::Head_14);
        root->Add(title, 0, wxALL, pad);
        auto* intro = new wxStaticText(this, wxID_ANY,
            format_wxstr(_L("How did each copy come out compared with A, the baseline? Hypothesis: %1%"),
                         plan.hypothesis.empty() ? wxString("-") : from_u8(plan.hypothesis)));
        intro->Wrap(wrap);
        root->Add(intro, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
        auto* prints = new wxStaticText(this, wxID_ANY,
            plan.prints.empty() ? _L("No print of this plate has been recorded yet. You can still compare the copies.") :
                                  // TRN Worded as a label so it needs no plural forms.
                                  format_wxstr(_L("Recorded prints of this plate: %1%"), plan.prints.size()));
        prints->Wrap(wrap);
        root->Add(prints, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* grid = new wxFlexGridSizer(3, FromDIP(8), FromDIP(10));
        grid->AddGrowableCol(2, 1);
        const wxString ratings[] = { _L("Better than A"), _L("About the same"), _L("Worse than A") };
        for (size_t i = 0; i < m_names.size(); ++i) {
            grid->Add(new wxStaticText(this, wxID_ANY, from_u8(m_names[i])), 0, wxALIGN_CENTER_VERTICAL);
            if (i == 0) {
                grid->Add(new wxStaticText(this, wxID_ANY, _L("(baseline)")), 0, wxALIGN_CENTER_VERTICAL);
            } else {
                auto* r = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, 3, ratings);
                const std::string old = i - 1 < plan.ratings.size() ? plan.ratings[i - 1] : std::string();
                r->SetSelection(old == "better" ? 0 : old == "worse" ? 2 : 1);
                r->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { ratings_changed(); });
                m_ratings.push_back(r);
                grid->Add(r, 0, wxALIGN_CENTER_VERTICAL);
            }
            auto* note = new wxTextCtrl(this, wxID_ANY, i < plan.notes.size() ? from_u8(plan.notes[i]) : wxString(), wxDefaultPosition,
                                        wxSize(FromDIP(300), -1));
            note->SetHint(_L("Note (optional)"));
            note->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { m_saved = false; });
            m_notes.push_back(note);
            grid->Add(note, 1, wxEXPAND);
        }
        root->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);

        auto* winner_row = new wxBoxSizer(wxHORIZONTAL);
        winner_row->Add(new wxStaticText(this, wxID_ANY, _L("Works best")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        m_winner = new wxChoice(this, wxID_ANY);
        for (const std::string& n : m_names)
            m_winner->Append(from_u8(n));
        m_winner_picked = plan.winner >= 0;
        m_winner->SetSelection(m_winner_picked ? std::min(plan.winner, int(m_names.size()) - 1) : 0);
        m_winner->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            m_winner_picked = true;
            m_saved         = false;
            update_buttons();
        });
        winner_row->Add(m_winner, 0, wxRIGHT, pad);
        auto* photos = new wxButton(this, wxID_ANY, _L("Add photos") + dots);
        photos->SetToolTip(_L("Copy photos of the printed plate into the experiment's folder."));
        winner_row->Add(photos, 0);
        root->Add(winner_row, 0, wxALL, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* save    = new wxButton(this, wxID_ANY, _L("Save the comparison"));
        m_lesson      = new wxButton(this, wxID_ANY, _L("Write a lesson") + dots);
        m_apply       = new wxButton(this, wxID_ANY, _L("Apply to preset"));
        m_apply->SetToolTip(_L("Set the winning value in the preset you are editing. It isn't saved: you decide whether to save the preset."));
        auto* next  = new wxButton(this, wxID_ANY, _L("Plan the next experiment") + dots);
        auto* close = new wxButton(this, wxID_CANCEL, _L("Close"));
        for (wxButton* b : { save, m_lesson, m_apply, next })
            buttons->Add(b, 0, wxRIGHT, FromDIP(6));
        buttons->AddStretchSpacer(1);
        buttons->Add(close, 0);
        root->Add(buttons, 0, wxEXPAND | wxALL, pad);

        save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { save_comparison(); });
        photos->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_photos(); });
        m_lesson->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { write_lesson(); });
        m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_to_preset(); });
        next->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (!save_comparison())
                return;
            if (forca_plan_experiment(this, m_plan.tag, m_plan.tag.empty() ? wxString() : forca_journal_tag_label(m_plan.tag)))
                EndModal(ID_NEW_PLATE);
        });

        m_saved = m_plan.state == "judged";
        if (!m_winner_picked)
            ratings_changed();
        update_buttons();
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

private:
    std::vector<std::string> rating_values() const
    {
        static const char* const keys[] = { "better", "same", "worse" };
        std::vector<std::string> out;
        for (const wxChoice* c : m_ratings)
            out.push_back(keys[std::clamp(c->GetSelection(), 0, 2)]);
        return out;
    }

    void ratings_changed()
    {
        m_saved = false;
        if (!m_winner_picked) // Forca's proposal until the user picks a winner themselves
            m_winner->SetSelection(forca_experiment_proposed_winner(rating_values()));
        update_buttons();
    }

    void update_buttons() { m_apply->Enable(m_winner->GetSelection() > 0); }

    bool save_comparison()
    {
        if (m_saved)
            return true;
        std::vector<std::string> notes;
        for (const wxTextCtrl* n : m_notes)
            notes.push_back(into_u8(n->GetValue().Strip(wxString::both)));
        std::string err;
        if (!forca_experiment_judge(m_folder, rating_values(), notes, m_winner->GetSelection(), err) ||
            !forca_experiment_load(m_folder, m_plan)) {
            show_error(this, from_u8(err));
            return false;
        }
        m_saved = true;
        forca_academy_refresh_index();
        return true;
    }

    void add_photos()
    {
        wxFileDialog dlg(this, _L("Add photos"), "", "",
                         _L("Photos and videos") + " (*.jpg;*.jpeg;*.png;*.webp;*.heic;*.bmp;*.gif;*.tif;*.tiff;*.mp4;*.mov;*.avi)|"
                         "*.jpg;*.jpeg;*.png;*.webp;*.heic;*.bmp;*.gif;*.tif;*.tiff;*.mp4;*.mov;*.avi;*.JPG;*.JPEG;*.PNG",
                         wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        wxArrayString paths;
        dlg.GetPaths(paths);
        std::vector<fs::path> files;
        for (const wxString& p : paths)
            files.push_back(into_path(p));
        std::string err;
        forca_academy_add_photos(m_folder, files, err);
        if (!err.empty())
            show_error(this, from_u8(err));
    }

    void write_lesson()
    {
        if (!save_comparison())
            return;
        const fs::path dir = forca_academy_dir();
        wxTextEntryDialog dlg(this, _L("Forca drafted this lesson from the comparison. Edit it, then add it to lessons.md."),
                              _L("Write a lesson"), from_u8(forca_experiment_lesson_draft(m_plan, forca_academy_relative(dir, m_folder), today())),
                              wxOK | wxCANCEL | wxTE_MULTILINE);
        dlg.SetSize(FromDIP(640), FromDIP(320));
        if (dlg.ShowModal() != wxID_OK)
            return;
        std::string err;
        if (!forca_academy_append_lesson(dir, into_u8(dlg.GetValue()), err)) {
            show_error(this, from_u8(err));
            return;
        }
        forca_academy_refresh_index();
        show_info(this, _L("The lesson was added to lessons.md."), _L("Write a lesson"));
    }

    void apply_to_preset()
    {
        const int w = m_winner->GetSelection();
        if (w <= 0 || size_t(w) > m_plan.variants.size() || !save_comparison())
            return;
        const ForcaExperimentTarget target = forca_experiment_preset_target(m_plan.key);
        const Preset::Type          type   = target.kind == ForcaExperimentTarget::Filament ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINT;
        PresetBundle*               b      = wxGetApp().preset_bundle;
        PresetCollection&           presets = target.kind == ForcaExperimentTarget::Filament ? b->filaments : b->prints;
        Tab*                        tab     = wxGetApp().get_tab(type);
        DynamicPrintConfig          config  = presets.get_edited_preset().config;
        const std::string           value   = m_plan.variants[size_t(w - 1)];
        if (tab == nullptr || !forca_experiment_set_value(config, target.key, value)) {
            show_error(this, format_wxstr(_L("%1% is not a valid value for %2%."), value, forca_option_label(target.key)));
            return;
        }
        tab->load_config(config); // edits the preset; the user saves it (or not)
        show_info(this,
                  format_wxstr(_L("%1% is now %2% in the preset \"%3%\". It isn't saved yet: save the preset to keep it."),
                               forca_option_label(target.key), forca_option_value(target.key, value), presets.get_edited_preset().name),
                  _L("Apply to preset"));
    }

    fs::path                   m_folder;
    ForcaExperimentPlan        m_plan;
    std::vector<std::string>   m_names;
    std::vector<wxChoice*>     m_ratings; // per variant
    std::vector<wxTextCtrl*>   m_notes;   // per copy, baseline first
    wxChoice*                  m_winner        = nullptr;
    wxButton*                  m_lesson        = nullptr;
    wxButton*                  m_apply         = nullptr;
    bool                       m_winner_picked = false;
    bool                       m_saved         = false;
};

} // namespace

ForcaJudgeOutcome forca_judge_experiment(wxWindow* parent, const fs::path& folder)
{
    ForcaExperimentPlan plan;
    if (!forca_experiment_load(folder, plan)) {
        show_error(parent, _L("This experiment's plan.json is missing or damaged."));
        return ForcaJudgeOutcome::Closed;
    }
    ForcaJudgeDialog dlg(parent, folder, plan);
    return dlg.ShowModal() == ID_NEW_PLATE ? ForcaJudgeOutcome::NewPlate : ForcaJudgeOutcome::Closed;
}

}} // namespace Slic3r::GUI
