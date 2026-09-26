#include "ForcaCalibrationWizard.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "MainFrame.hpp"
#include "GUI_ObjectList.hpp"
#include "Tab.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp" // resources_dir()

#include <boost/filesystem/path.hpp>

#include <wx/sizer.h>
#include <wx/panel.h>
#include <wx/simplebook.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/choice.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/datetime.h>
#include <wx/tokenzr.h>
#include <wx/settings.h>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Slic3r { namespace GUI {

// Absolute nozzle-temperature bounds used when the filament type is unknown/custom.
static constexpr int TEMP_ABS_MIN = 155;
static constexpr int TEMP_ABS_MAX = 500;
// Extra headroom above Orca's per-type tower max (and below its min): some filaments / high speeds
// benefit from a little more heat.
static constexpr int TEMP_HEADROOM = 30;

// Sanity bounds for a max-volumetric-speed result (mm^3/s).
static constexpr double MVS_MIN = 0.5;
static constexpr double MVS_MAX = 100.0;

// Flow rate: allowed best-block modifier (percent) and typo-guard bounds for the resulting flow ratio.
// Wide on purpose -- foaming filaments (e.g. ASA Aero ~0.45) run low; nothing realistic exceeds ~1.5.
static constexpr double FLOW_MOD_MAX = 25.0;
static constexpr double FLOW_MIN     = 0.3;
static constexpr double FLOW_MAX     = 1.5;

// Pressure advance: typo-guard result bounds (DDE ~0.02-0.08, Bowden up to ~1; 2.0 is a wide ceiling).
static constexpr double PA_MIN = 0.0;
static constexpr double PA_MAX = 2.0;

// Flow YOLO: blocks are labelled with an absolute flow-ratio offset (+-0.05 / +-0.04); wide typo guard.
static constexpr double FLOW_YOLO_MOD_MAX = 0.1;

// Retraction: typo-guard result bound (mm). Direct drive ~0.2-2, Bowden up to ~6-7.
static constexpr double RETRACT_MAX = 10.0;

// Shrinkage frame (Forca's own geometry, built procedurally in launch_shrinkage_test): a square open frame of
// outer size SHRINK_XY_MM with SHRINK_BAR_MM wide bars SHRINK_FRAME_H_MM tall, plus one corner post SHRINK_Z_MM
// tall. The user measures the outer X/Y width and the post height. Results are percents (100 = no shrink) and
// are bounded to Orca's own 50-150% range, then typo-guarded tighter (a real shrink is a few percent).
static constexpr double SHRINK_XY_MM      = 100.0;
static constexpr double SHRINK_Z_MM       = 40.0;
static constexpr double SHRINK_BAR_MM     = 10.0;
static constexpr double SHRINK_FRAME_H_MM = 5.0;
static constexpr double SHRINK_PCT_MIN    = 90.0;
static constexpr double SHRINK_PCT_MAX    = 110.0;

// True once THIS app session has generated at least one calibration test print. After that, the plate
// and presets hold only throwaway test setup, so the wizard tells calib_*/new_project to SKIP its
// confirm dialogs. Before the first test we do not skip, so the user's own unsaved work is still
// protected by the normal prompt.
static bool s_forca_calib_test_generated = false;

// Parse a comma/space-separated list of positive numbers (PA Pattern accelerations/speeds). Blank -> empty
// list (Orca then uses the print's own values). Returns false on any non-numeric or non-positive entry.
static bool parse_positive_list(const wxString& s, std::vector<double>& out)
{
    out.clear();
    wxStringTokenizer tok(s, ", ");
    while (tok.HasMoreTokens()) {
        double v = 0;
        if (!tok.GetNextToken().ToDouble(&v) || v <= 0)
            return false;
        out.push_back(v);
    }
    return true;
}

// Show the two-routes intro once per app session.
static bool s_forca_intro_shown = false;

// Hard-wrap text to at most max_chars per line at word boundaries (explicit newlines). Used instead of
// wxStaticText::Wrap(), which is unreliable when the label is set dynamically (it left text unwrapped,
// clipping it and widening the dialog).
static wxString hard_wrap(const wxString& s, size_t max_chars)
{
    wxStringTokenizer tok(s, " ");
    wxString out, line;
    while (tok.HasMoreTokens()) {
        const wxString w = tok.GetNextToken();
        if (!line.empty() && line.length() + 1 + w.length() > max_chars) {
            if (!out.empty()) out += "\n";
            out += line;
            line.Clear();
        }
        line = line.empty() ? w : (line + " " + w);
    }
    if (!line.empty()) {
        if (!out.empty()) out += "\n";
        out += line;
    }
    return out;
}

// Remove a trailing " (calibrated <...>)" suffix so a default name derives from the underlying base.
static std::string strip_calibrated_suffix(const std::string& name)
{
    const std::string marker = " (calibrated ";
    const auto pos = name.rfind(marker);
    if (pos != std::string::npos && !name.empty() && name.back() == ')')
        return name.substr(0, pos);
    return name;
}

// First value of a numeric vector option (Floats / Ints / Percents, nullable or not); nullopt when absent or nil.
static std::optional<double> first_value(const DynamicPrintConfig& cfg, const std::string& key)
{
    const ConfigOption* opt = cfg.option(key);
    if (!opt)
        return std::nullopt;
    if (const auto* o = dynamic_cast<const ConfigOptionFloatsNullable*>(opt))
        return (o->values.empty() || o->is_nil(0)) ? std::nullopt : std::optional<double>(o->values.front());
    if (const auto* o = dynamic_cast<const ConfigOptionFloats*>(opt))
        return o->values.empty() ? std::nullopt : std::optional<double>(o->values.front());
    if (const auto* o = dynamic_cast<const ConfigOptionPercentsNullable*>(opt))
        return (o->values.empty() || o->is_nil(0)) ? std::nullopt : std::optional<double>(o->values.front());
    if (const auto* o = dynamic_cast<const ConfigOptionPercents*>(opt))
        return o->values.empty() ? std::nullopt : std::optional<double>(o->values.front());
    if (const auto* o = dynamic_cast<const ConfigOptionInts*>(opt))
        return o->values.empty() ? std::nullopt : std::optional<double>(o->values.front());
    return std::nullopt;
}

// The config keys a run writes, snapshotted as the run's "before" and shown as table rows.
struct TableRowDef { ForcaCalibrationWizard::Cal cal; const char* key; const char* label; const char* fmt; };
static const std::vector<TableRowDef>& table_rows()
{
    using Cal = ForcaCalibrationWizard::Cal;
    static const std::vector<TableRowDef> rows = {
        { Cal::Temperature,     "nozzle_temperature",                L("Nozzle temperature"),   "%.0f C" },
        { Cal::MaxVolSpeed,     "filament_max_volumetric_speed",     L("Max volumetric speed"), "%.1f mm3/s" },
        { Cal::FlowRate,        "filament_flow_ratio",               L("Flow ratio"),           "%.3f" },
        { Cal::PressureAdvance, "pressure_advance",                  L("Pressure advance"),     "%.3f" },
        { Cal::Retraction,      "filament_retraction_length",        L("Retraction length"),    "%.2f mm" },
        { Cal::Shrinkage,       "filament_shrink",                   L("Shrinkage XY"),         "%.2f%%" },
        { Cal::Shrinkage,       "filament_shrinkage_compensation_z", L("Shrinkage Z"),          "%.2f%%" },
    };
    return rows;
}

// The wizard panel's own background / text colours (children inherit them).
static wxColour wizard_bg(bool dark) { return dark ? wxColour("#2B2F3A") : *wxWHITE; }
static wxColour wizard_fg(bool dark) { return dark ? wxColour("#E6EAF2") : wxColour("#26395A"); } // Forca navy on light

ForcaCalibrationWizard::ForcaCalibrationWizard(wxWindow* parent, Plater* plater)
    : wxScrolled<wxPanel>(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL)
    , m_plater(plater)
{
    const bool dark = wxGetApp().dark_mode();
    SetBackgroundColour(wizard_bg(dark));
    SetForegroundColour(wizard_fg(dark));
    SetFont(Label::Body_14);
    SetScrollRate(0, FromDIP(12));

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(12);

    auto* title = new wxStaticText(this, wxID_ANY, _L("Forca Slicer Calibration Wizard"));
    title->SetFont(Label::Head_16);
    root->Add(title, 0, wxALL, pad);

    // Preset-change banner (#7), hidden until check_run_presets() finds something.
    m_warn_panel = new wxPanel(this);
    m_warn_panel->SetBackgroundColour(wxColour("#FFF3E0"));
    auto* warn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_warn_text = new wxStaticText(m_warn_panel, wxID_ANY, "");
    m_warn_text->SetForegroundColour(wxColour("#8A4B00"));
    warn_sizer->Add(m_warn_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(6));
    m_warn_action = new wxButton(m_warn_panel, wxID_ANY, "");
    m_warn_action->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_warn_fn) m_warn_fn();
        hide_warning();
    });
    warn_sizer->Add(m_warn_action, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    auto* warn_close = new wxButton(m_warn_panel, wxID_ANY, _L("Dismiss"));
    warn_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { hide_warning(); });
    warn_sizer->Add(warn_close, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    m_warn_panel->SetSizer(warn_sizer);
    m_warn_panel->Hide();
    root->Add(m_warn_panel, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);

    // Shared header: active configuration + calibration picker.
    auto* cfg_box = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _L("Active configuration")), wxVERTICAL);
    auto add_static_row = [&](const wxString& label) -> wxStaticText* {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* l   = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, FromDIP(wxSize(180, -1)));
        auto* v   = new wxStaticText(this, wxID_ANY, "-");
        row->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        row->Add(v, 1, wxALIGN_CENTER_VERTICAL);
        cfg_box->Add(row, 0, wxALL | wxEXPAND, FromDIP(3));
        return v;
    };
    m_printer_value = add_static_row(_L("Printer:"));
    m_nozzle_value  = add_static_row(_L("Nozzle:"));

    auto add_choice_row = [&](const wxString& label) -> wxChoice* {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, FromDIP(wxSize(180, -1))), 0, wxALIGN_CENTER_VERTICAL);
        auto* c = new wxChoice(this, wxID_ANY);
        row->Add(c, 1, wxALIGN_CENTER_VERTICAL);
        cfg_box->Add(row, 0, wxALL | wxEXPAND, FromDIP(3));
        return c;
    };
    m_filament_choice = add_choice_row(_L("Filament to calibrate:"));
    m_filament_choice->Bind(wxEVT_CHOICE, &ForcaCalibrationWizard::on_filament_changed, this);
    m_cal_choice = add_choice_row(_L("Calibration:"));
    m_cal_choice->Append(_L("Temperature"));
    m_cal_choice->Append(_L("Max Volumetric Speed"));
    m_cal_choice->Append(_L("Flow Rate"));
    m_cal_choice->Append(_L("Pressure Advance"));
    m_cal_choice->Append(_L("Retraction"));
    m_cal_choice->Append(_L("Shrinkage"));
    m_cal_choice->Append(_L("Final check print (optional)"));
    m_cal_choice->SetSelection(0);
    m_cal_choice->Bind(wxEVT_CHOICE, &ForcaCalibrationWizard::on_calibration_changed, this);
    root->Add(cfg_box, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);

    m_status_label = new wxStaticText(this, wxID_ANY, "");
    root->Add(m_status_label, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // Per-calibration generate pages (in Cal order) + one shared result page.
    m_book = new wxSimplebook(this, wxID_ANY);
    m_book->AddPage(build_temp_gen_page(m_book),       _L("Temperature"));
    m_book->AddPage(build_mvs_gen_page(m_book),        _L("Max flow"));
    m_book->AddPage(build_flow_gen_page(m_book),       _L("Flow"));
    m_book->AddPage(build_pa_gen_page(m_book),         _L("PA"));
    m_book->AddPage(build_retraction_gen_page(m_book), _L("Retraction"));
    m_book->AddPage(build_shrinkage_gen_page(m_book),  _L("Shrinkage"));
    m_book->AddPage(build_final_check_page(m_book),    _L("Final check"));
    m_book->AddPage(build_result_page(m_book),         _L("Result"));      // PAGE_RESULT
    m_book->AddPage(build_start_page(m_book),          _L("Start"));       // PAGE_START
    root->Add(m_book, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, pad);

    SetSizer(root);
    refresh_presets();

    // First open: resume a test that is out for printing; otherwise begin on the Start page (pick the filament).
    ForcaCalibrationStore::Record r;
    if (m_store.get(current_key(), r) && r.status == "pending")
        show_gen_page();
    else
        start_new_calibration();
}

// ---- Page builders -----------------------------------------------------------------------------

wxPanel* ForcaCalibrationWizard::build_temp_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Temperature")), wxVERTICAL);
    const int wrap = FromDIP(460);

    auto* coach = new wxStaticText(panel, wxID_ANY,
        _L("A temperature tower prints the same shape at descending nozzle temperatures. After it prints, "
           "choose the band with the cleanest surface, least stringing and strongest layer bonding."));
    coach->Wrap(wrap);
    box->Add(coach, 0, wxALL, FromDIP(6));

    auto* range = new wxBoxSizer(wxHORIZONTAL);
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Start temp (hot):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_temp_start = new wxTextCtrl(panel, wxID_ANY, "250", wxDefaultPosition, FromDIP(wxSize(70, -1)));
    range->Add(m_temp_start, 0, wxRIGHT, FromDIP(16));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("End temp (cool):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_temp_end = new wxTextCtrl(panel, wxID_ANY, "190", wxDefaultPosition, FromDIP(wxSize(70, -1)));
    range->Add(m_temp_end, 0);
    box->Add(range, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed a tower - enter result"));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_mvs_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Max Volumetric Speed")), wxVERTICAL);
    const int wrap = FromDIP(460);

    auto* coach = new wxStaticText(panel, wxID_ANY,
        _L("This prints a single-wall tower whose speed rises with height. Watch for where the surface "
           "turns rough or the walls start under-extruding. Measure the height (mm) of the last section "
           "that still looked good -- the result page turns that height into your max flow for you."));
    coach->Wrap(wrap);
    box->Add(coach, 0, wxALL, FromDIP(6));

    auto* range = new wxBoxSizer(wxHORIZONTAL);
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Start:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_mvs_start = new wxTextCtrl(panel, wxID_ANY, "5", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_mvs_start, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("End:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_mvs_end = new wxTextCtrl(panel, wxID_ANY, "20", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_mvs_end, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Step (mm3/s):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_mvs_step = new wxTextCtrl(panel, wxID_ANY, "0.5", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_mvs_step, 0);
    box->Add(range, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed a tower - enter result"));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_flow_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Flow Rate")), wxVERTICAL);

    auto* method_row = new wxBoxSizer(wxHORIZONTAL);
    method_row->Add(new wxStaticText(panel, wxID_ANY, _L("Method:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_flow_method = new wxChoice(panel, wxID_ANY);
    m_flow_method->Append(_L("Classic (coarse + fine pass)"));         // FLOW_CLASSIC
    m_flow_method->Append(_L("YOLO (Recommended + Perfectionist)"));   // FLOW_YOLO
    m_flow_method->SetSelection(FLOW_CLASSIC);
    m_flow_method->Bind(wxEVT_CHOICE, &ForcaCalibrationWizard::on_flow_method_changed, this);
    method_row->Add(m_flow_method, 0);
    box->Add(method_row, 0, wxALL, FromDIP(6));

    m_flow_pass_label = new wxStaticText(panel, wxID_ANY, "");
    m_flow_pass_label->SetFont(Label::Head_14);
    box->Add(m_flow_pass_label, 0, wxALL, FromDIP(6));

    // Per-method coaching (set by update_flow_labels; hard-wrapped, see hard_wrap()).
    m_flow_coach = new wxStaticText(panel, wxID_ANY, "");
    m_flow_coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(m_flow_coach, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed this pass - enter result"));

    update_flow_labels();

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_retraction_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Retraction")), wxVERTICAL);

    auto* coach = new wxStaticText(panel, wxID_ANY, hard_wrap(
        _L("This prints Orca's retraction tower; the retraction length grows by one step at each ring. Find the "
           "lowest section where the stringing stops, count the rings below it, and enter that count on the "
           "result page to get the retraction length. Use the shortest length that is string-free."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    // Orca's Retraction_Test_Dlg defaults (0-2 mm, step 0.1). Bowden users typically raise End.
    auto* range = new wxBoxSizer(wxHORIZONTAL);
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Start:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_retract_start = new wxTextCtrl(panel, wxID_ANY, "0", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_retract_start, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("End:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_retract_end = new wxTextCtrl(panel, wxID_ANY, "2", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_retract_end, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Step (mm):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_retract_step = new wxTextCtrl(panel, wxID_ANY, "0.1", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_retract_step, 0);
    box->Add(range, 0, wxALL, FromDIP(6));

    auto* note = new wxStaticText(panel, wxID_ANY, hard_wrap(
        _L("Direct drive: the default 0-2 mm range fits most filaments. Bowden: raise End (for example to 6)."), 60));
    note->SetForegroundColour(wxColour("#6B6B6B"));
    box->Add(note, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed a tower - enter result"));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_shrinkage_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Shrinkage")), wxVERTICAL);

    auto* coach = new wxStaticText(panel, wxID_ANY, hard_wrap(wxString::Format(
        _L("This prints a square open frame %g mm across with a %g mm tall post in one corner. Let it cool fully "
           "(ideally a few hours), then measure with calipers: the outer width along X, the outer width along Y "
           "(measure above the first few layers to skip any elephant's foot), and the post height. The result "
           "page turns those into the filament's XY and Z shrinkage compensation."),
        SHRINK_XY_MM, SHRINK_Z_MM), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed the frame - enter measurements"));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_final_check_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Final check print (optional)")), wxVERTICAL);

    auto* coach = new wxStaticText(panel, wxID_ANY, hard_wrap(
        _L("An optional confidence check: prints the Autodesk FDM Test (Orca's bundled model) with your calibrated "
           "filament preset. It exercises overhangs, bridges, stringing, small features and dimensions in one "
           "print. Nothing is saved from it -- if something looks off, re-run that calibration from the picker "
           "above. Your calibrated preset is already saved and selected."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, wxEmptyString); // no result to enter

    auto* another_btn = new wxButton(panel, wxID_ANY, _L("Calibrate another filament"));
    another_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_new_calibration(); });
    box->Add(another_btn, 0, wxALL, FromDIP(6));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_start_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Start a calibration")), wxVERTICAL);

    auto* intro = new wxStaticText(panel, wxID_ANY, hard_wrap(
        _L("Pick the filament you are calibrating in \"Filament to calibrate\" above -- or an existing profile close "
           "to it to start from -- then press Start. The steps run in order: Temperature, Max Volumetric Speed, "
           "Flow Rate, Pressure Advance, Retraction and Shrinkage."), 60));
    intro->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(intro, 0, wxALL, FromDIP(6));

    m_start_note = new wxStaticText(panel, wxID_ANY, "");
    m_start_note->SetForegroundColour(wxColour("#FF6F00"));
    m_start_note->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(m_start_note, 0, wxALL, FromDIP(6));

    auto* start_btn = new wxButton(panel, wxID_ANY, _L("Start calibration"));
    start_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_start_run, this);
    box->Add(start_btn, 0, wxALL, FromDIP(6));

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

void ForcaCalibrationWizard::add_run_buttons(wxPanel* panel, wxSizer* box, const wxString& result_label)
{
    auto* send_btn = new wxButton(panel, wxID_ANY, _L("Slice && send to printer"));
    send_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_slice_and_send, this);
    box->Add(send_btn, 0, wxALL, FromDIP(6));

    auto* gen_btn = new wxButton(panel, wxID_ANY, _L("Generate only (I will print it myself)"));
    gen_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_generate, this);
    box->Add(gen_btn, 0, wxALL, FromDIP(6));

    auto* gen_note = new wxStaticText(panel, wxID_ANY,
        _L("Slice & send puts the test on the plate, slices it and opens your printer's send dialog; when "
           "it closes you enter your result here. Generate only puts the test on the plate (shown on the "
           "right) so you can slice/export it yourself, then come back here to enter your result."));
    gen_note->Wrap(FromDIP(460));
    gen_note->SetForegroundColour(wxColour("#6B6B6B"));
    box->Add(gen_note, 0, wxALL, FromDIP(6));

    if (result_label.IsEmpty())
        return;
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* result_btn = new wxButton(panel, wxID_ANY, result_label);
    result_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_goto_result, this);
    row->Add(result_btn, 0, wxRIGHT, FromDIP(8));
    auto* skip_btn = new wxButton(panel, wxID_ANY, _L("Skip this step"));
    skip_btn->SetToolTip(_L("Keep your current value for this setting and move on to the next calibration."));
    skip_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_skip, this);
    row->Add(skip_btn, 0);
    box->Add(row, 0, wxALL, FromDIP(6));
}

wxPanel* ForcaCalibrationWizard::build_pa_gen_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Pressure Advance")), wxVERTICAL);

    // Method + extruder: both reseed the range (Orca's PA_Calibration_Dlg defaults per combination).
    auto* setup_row = new wxBoxSizer(wxHORIZONTAL);
    setup_row->Add(new wxStaticText(panel, wxID_ANY, _L("Method:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pa_method = new wxChoice(panel, wxID_ANY);
    m_pa_method->Append(_L("PA Tower (recommended)")); // PA_TOWER
    m_pa_method->Append(_L("PA Line"));                // PA_LINE
    m_pa_method->Append(_L("PA Pattern"));             // PA_PATTERN
    m_pa_method->SetSelection(PA_TOWER);
    m_pa_method->Bind(wxEVT_CHOICE, &ForcaCalibrationWizard::on_pa_setup_changed, this);
    setup_row->Add(m_pa_method, 0, wxRIGHT, FromDIP(16));
    setup_row->Add(new wxStaticText(panel, wxID_ANY, _L("Extruder:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pa_extruder = new wxChoice(panel, wxID_ANY);
    m_pa_extruder->Append(_L("Direct drive"));
    m_pa_extruder->Append(_L("Bowden"));
    m_pa_extruder->SetSelection(0);
    m_pa_extruder->Bind(wxEVT_CHOICE, &ForcaCalibrationWizard::on_pa_setup_changed, this);
    setup_row->Add(m_pa_extruder, 0);
    box->Add(setup_row, 0, wxALL, FromDIP(6));

    // Per-method reading instructions (set by update_pa_coach; hard-wrapped, see hard_wrap()).
    m_pa_coach = new wxStaticText(panel, wxID_ANY, "");
    m_pa_coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(m_pa_coach, 0, wxALL, FromDIP(6));

    auto* range = new wxBoxSizer(wxHORIZONTAL);
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Start:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_pa_start = new wxTextCtrl(panel, wxID_ANY, "0.0", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_pa_start, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("End:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_pa_end = new wxTextCtrl(panel, wxID_ANY, "0.1", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_pa_end, 0, wxRIGHT, FromDIP(12));
    range->Add(new wxStaticText(panel, wxID_ANY, _L("Step:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_pa_step = new wxTextCtrl(panel, wxID_ANY, "0.002", wxDefaultPosition, FromDIP(wxSize(60, -1)));
    range->Add(m_pa_step, 0);
    box->Add(range, 0, wxALL, FromDIP(6));

    // Pattern-only: optional comma-separated accelerations / speeds. Blank = Orca uses the print's outer-wall
    // acceleration and an optimal speed; several values print one pattern per combination.
    m_pa_pattern_opts = new wxPanel(panel);
    auto* opts = new wxBoxSizer(wxVERTICAL);
    auto* accel_row = new wxBoxSizer(wxHORIZONTAL);
    accel_row->Add(new wxStaticText(m_pa_pattern_opts, wxID_ANY, _L("Accelerations (optional):"), wxDefaultPosition, FromDIP(wxSize(170, -1))),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pa_accels = new wxTextCtrl(m_pa_pattern_opts, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(200, -1)));
    m_pa_accels->SetToolTip(_L("Comma-separated list of printing accelerations"));
    accel_row->Add(m_pa_accels, 0);
    opts->Add(accel_row, 0, wxBOTTOM, FromDIP(4));
    auto* speed_row = new wxBoxSizer(wxHORIZONTAL);
    speed_row->Add(new wxStaticText(m_pa_pattern_opts, wxID_ANY, _L("Speeds (optional):"), wxDefaultPosition, FromDIP(wxSize(170, -1))),
                   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pa_speeds = new wxTextCtrl(m_pa_pattern_opts, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(200, -1)));
    m_pa_speeds->SetToolTip(_L("Comma-separated list of printing speeds"));
    speed_row->Add(m_pa_speeds, 0);
    opts->Add(speed_row, 0);
    m_pa_pattern_opts->SetSizer(opts);
    box->Add(m_pa_pattern_opts, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, _L("I have already printed this test - enter result"));

    seed_pa(); // default method (Tower) + DDE: range, coaching, Pattern-options visibility

    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaCalibrationWizard::build_result_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int wrap = FromDIP(460);

    auto* heading = new wxStaticText(panel, wxID_ANY, _L("Enter result"));
    heading->SetFont(Label::Head_14);
    sizer->Add(heading, 0, wxBOTTOM, FromDIP(8));

    // MVS-only helper, shown ABOVE the result field so it is read first. Converts the measured "last
    // good" height into a max-flow value. Shown/hidden by on_goto_result depending on the calibration.
    m_height_help = new wxPanel(panel);
    auto* help_sizer = new wxBoxSizer(wxVERTICAL);
    m_height_hint = new wxStaticText(m_height_help, wxID_ANY, "");
    m_height_hint->Wrap(wrap);
    m_height_hint->SetForegroundColour(wxColour("#6B6B6B"));
    help_sizer->Add(m_height_hint, 0, wxBOTTOM, FromDIP(4));
    auto* hrow = new wxBoxSizer(wxHORIZONTAL);
    m_height_label = new wxStaticText(m_height_help, wxID_ANY, _L("Measured height (mm):"), wxDefaultPosition, FromDIP(wxSize(200, -1)));
    hrow->Add(m_height_label,
              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_height_value = new wxTextCtrl(m_height_help, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(90, -1)));
    m_height_value->Bind(wxEVT_TEXT, &ForcaCalibrationWizard::on_height_changed, this);
    hrow->Add(m_height_value, 0);
    help_sizer->Add(hrow, 0);
    m_height_help->SetSizer(help_sizer);
    sizer->Add(m_height_help, 0, wxBOTTOM | wxEXPAND, FromDIP(8));

    m_result_row = new wxBoxSizer(wxHORIZONTAL);
    m_result_label = new wxStaticText(panel, wxID_ANY, _L("Result:"), wxDefaultPosition, FromDIP(wxSize(200, -1)));
    m_result_row->Add(m_result_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_result_value = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(90, -1)));
    m_result_value->Bind(wxEVT_TEXT, &ForcaCalibrationWizard::on_result_value_changed, this);
    m_result_row->Add(m_result_value, 0);
    sizer->Add(m_result_row, 0, wxBOTTOM, FromDIP(8));

    // Shrinkage-only: three caliper measurements instead of the single result value.
    m_shrink_inputs = new wxPanel(panel);
    auto* shrink_sizer = new wxBoxSizer(wxVERTICAL);
    auto add_shrink_row = [&](const wxString& label) -> wxTextCtrl* {
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        r->Add(new wxStaticText(m_shrink_inputs, wxID_ANY, label, wxDefaultPosition, FromDIP(wxSize(200, -1))),
               0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        auto* t = new wxTextCtrl(m_shrink_inputs, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(90, -1)));
        r->Add(t, 0);
        shrink_sizer->Add(r, 0, wxBOTTOM, FromDIP(4));
        return t;
    };
    m_shrink_x = add_shrink_row(wxString::Format(_L("Measured X width (%g mm):"), SHRINK_XY_MM));
    m_shrink_y = add_shrink_row(wxString::Format(_L("Measured Y width (%g mm):"), SHRINK_XY_MM));
    m_shrink_z = add_shrink_row(wxString::Format(_L("Measured post height (%g mm):"), SHRINK_Z_MM));
    m_shrink_inputs->SetSizer(shrink_sizer);
    m_shrink_inputs->Hide(); // shown by show_result_page for Shrinkage only
    sizer->Add(m_shrink_inputs, 0, wxBOTTOM, FromDIP(8));

    m_result_hint = new wxStaticText(panel, wxID_ANY, "");
    m_result_hint->SetForegroundColour(wxColour("#6B6B6B"));
    m_result_hint->SetMinSize(FromDIP(wxSize(430, -1))); // pin width so the guidance/formula can't widen the dialog
    sizer->Add(m_result_hint, 0, wxBOTTOM, FromDIP(8));

    auto* name_row = new wxBoxSizer(wxHORIZONTAL);
    name_row->Add(new wxStaticText(panel, wxID_ANY, _L("Save as preset:"), wxDefaultPosition, FromDIP(wxSize(200, -1))),
                  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_preset_name = new wxTextCtrl(panel, wxID_ANY, "");
    name_row->Add(m_preset_name, 1, wxALIGN_CENTER_VERTICAL);
    sizer->Add(name_row, 0, wxBOTTOM | wxEXPAND, FromDIP(8));

    auto* note = new wxStaticText(panel, wxID_ANY,
        _L("Name the preset whatever you like -- for example the actual filament you are calibrating, even "
           "if you started from a different profile. The result is written into that preset and it is "
           "selected; if the name already exists it is updated in place. Your starting preset is left "
           "unchanged."));
    note->Wrap(wrap);
    note->SetForegroundColour(wxColour("#6B6B6B"));
    sizer->Add(note, 0, wxBOTTOM, FromDIP(10));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* back_btn = new wxButton(panel, wxID_ANY, _L("Back"));
    back_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_back_from_result, this);
    auto* apply_btn = new wxButton(panel, wxID_ANY, _L("Apply and save to preset"));
    apply_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationWizard::on_apply_result, this);
    buttons->Add(back_btn, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer(1);
    buttons->Add(apply_btn, 0);
    sizer->Add(buttons, 0, wxEXPAND);

    panel->SetSizerAndFit(sizer);
    return panel;
}

// ---- Shared helpers ----------------------------------------------------------------------------

std::string ForcaCalibrationWizard::cal_id(Cal cal)
{
    // Persisted store ids -- never rename an existing one (saved runs are keyed by it).
    switch (cal) {
    case Cal::Temperature:     return "temperature";
    case Cal::MaxVolSpeed:     return "mvs";
    case Cal::FlowRate:        return "flow";
    case Cal::PressureAdvance: return "pa";
    case Cal::Retraction:      return "retraction";
    case Cal::Shrinkage:       return "shrinkage";
    default:                   return "final_check";
    }
}

wxString ForcaCalibrationWizard::cal_display() const
{
    switch (m_cal) {
    case Cal::Temperature:     return _L("Temperature");
    case Cal::MaxVolSpeed:     return _L("Max Volumetric Speed");
    case Cal::FlowRate:        return _L("Flow Rate");
    case Cal::PressureAdvance: return _L("Pressure Advance");
    case Cal::Retraction:      return _L("Retraction");
    case Cal::Shrinkage:       return _L("Shrinkage");
    default:                   return _L("Final check print");
    }
}

wxString ForcaCalibrationWizard::format_result(double value) const
{
    switch (m_cal) {
    case Cal::Temperature:     return wxString::Format("%d C", static_cast<int>(std::lround(value)));
    case Cal::MaxVolSpeed:     return wxString::Format("%.1f mm3/s", value);
    case Cal::FlowRate:        return wxString::Format(_L("flow ratio %.3f"), value);
    case Cal::PressureAdvance: return wxString::Format(_L("PA %.3f"), value);
    case Cal::Retraction:      return wxString::Format(_L("retraction %.2f mm"), value);
    case Cal::Shrinkage:       return wxString::Format(_L("XY shrinkage %.2f%%"), value);
    default:                   return wxEmptyString;
    }
}

void ForcaCalibrationWizard::set_cal(Cal cal)
{
    m_cal = cal;
    m_cal_choice->SetSelection(static_cast<int>(cal));
}

void ForcaCalibrationWizard::show_gen_page()
{
    // ChangeSelection, not SetSelection: no page-changed event -- inside the main notebook it would bubble up to
    // MainFrame's tab handler and be taken for a main-tab change (that recursion crashed the tab).
    m_book->ChangeSelection(static_cast<int>(m_cal)); // gen pages are added in Cal order
    if (m_cal == Cal::FlowRate)
        update_flow_labels();
    relayout();
}

void ForcaCalibrationWizard::refresh_presets(bool resume)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;

    m_printer_value->SetLabel(wxString::FromUTF8(bundle->printers.get_edited_preset().name.c_str()));

    const DynamicPrintConfig& printer_cfg = bundle->printers.get_edited_preset().config;
    wxString nozzle = "-";
    if (const auto* nd = printer_cfg.option<ConfigOptionFloats>("nozzle_diameter"); nd != nullptr && !nd->values.empty())
        nozzle = wxString::Format("%.2f mm", nd->values.front());
    m_nozzle_value->SetLabel(nozzle);

    m_project_filaments = bundle->filament_presets;
    if (m_project_filaments.empty())
        m_project_filaments.push_back(bundle->filaments.get_selected_preset_name());

    m_filament_choice->Clear();
    for (size_t i = 0; i < m_project_filaments.size(); ++i)
        m_filament_choice->Append(wxString::Format("%d: %s", static_cast<int>(i + 1), wxString::FromUTF8(m_project_filaments[i].c_str())));

    const std::string active = bundle->filaments.get_selected_preset_name();
    int sel = 0;
    for (size_t i = 0; i < m_project_filaments.size(); ++i)
        if (m_project_filaments[i] == active) { sel = static_cast<int>(i); break; }
    m_filament_choice->SetSelection(sel);

    // Resume (on first open only): if a calibration is pending for this filament, open on the first such one
    // (Forca order). Later tab re-activations keep the user's place instead.
    if (resume) {
        ForcaCalibrationStore::Record r;
        ForcaCalibrationStore::Key k = current_key();
        for (int i = 0; i < static_cast<int>(Cal::Count); ++i) {
            k.calibration = cal_id(static_cast<Cal>(i));
            if (m_store.get(k, r) && r.status == "pending") {
                set_cal(static_cast<Cal>(i));
                if (m_cal == Cal::FlowRate)
                    m_flow_pass = r.pass >= 1 ? 2 : 1;
                break;
            }
        }
    }

    // Track the run being worked on (for the preset-change warning).
    const ForcaCalibrationStore::Key cur = current_key();
    if (m_store.is_calibration_target(cur.printer, cur.nozzle, cur.filament))
        remember_run(cur.filament);

    seed_temperatures();
    seed_mvs();
    // PA is NOT reseeded here: reopening after "Generate only" must keep the printed range and method, which
    // the Tower height -> PA helper reads. It is seeded once when built and on method/extruder change.
    update_status_label();
    update_default_preset_name();
}

std::string ForcaCalibrationWizard::selected_filament_name() const
{
    if (m_filament_choice) {
        const int sel = m_filament_choice->GetSelection();
        if (sel != wxNOT_FOUND && sel < static_cast<int>(m_project_filaments.size()))
            return m_project_filaments[sel];
    }
    PresetBundle* bundle = wxGetApp().preset_bundle;
    return bundle ? bundle->filaments.get_selected_preset_name() : std::string();
}

ForcaCalibrationStore::Key ForcaCalibrationWizard::current_key() const
{
    ForcaCalibrationStore::Key k;
    k.calibration = cal_id();
    k.filament    = selected_filament_name();
    if (PresetBundle* bundle = wxGetApp().preset_bundle) {
        k.printer = bundle->printers.get_edited_preset().name;
        const auto* nd = bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
        if (nd != nullptr && !nd->values.empty())
            k.nozzle = wxString::Format("%.2f", nd->values.front()).ToStdString();
    }
    return k;
}

void ForcaCalibrationWizard::update_status_label()
{
    if (!m_status_label)
        return;

    ForcaCalibrationStore::Record r;
    const bool have = m_store.get(current_key(), r);
    if (have && r.status == "pending") {
        m_status_label->SetForegroundColour(wxColour("#FF6F00")); // attention orange
        m_status_label->SetLabel(wxString::Format(_L("%s test pending (generated %s)."),
            cal_display(), wxString::FromUTF8(r.updated_at.c_str())));
    } else if (have && r.status == "done" && m_cal == Cal::FinalCheck) {
        m_status_label->SetForegroundColour(wxColour("#6B6B6B"));
        m_status_label->SetLabel(wxString::Format(_L("Final check print generated (%s)."),
            wxString::FromUTF8(r.updated_at.c_str())));
    } else if (have && r.status == "done") {
        m_status_label->SetForegroundColour(wxColour("#6B6B6B"));
        m_status_label->SetLabel(wxString::Format(_L("Last %s result: %s (%s)."),
            cal_display(), format_result(r.value), wxString::FromUTF8(r.updated_at.c_str())));
    } else if (have && r.status == "skipped") {
        m_status_label->SetForegroundColour(wxColour("#6B6B6B"));
        m_status_label->SetLabel(wxString::Format(_L("%s was skipped (%s); your current value is kept."),
            cal_display(), wxString::FromUTF8(r.updated_at.c_str())));
    } else {
        m_status_label->SetLabel("");
    }
}

std::string ForcaCalibrationWizard::default_preset_name() const
{
    const std::string sel = selected_filament_name();
    const ForcaCalibrationStore::Key k = current_key();
    if (m_store.is_calibration_target(k.printer, k.nozzle, sel))
        return sel; // already a run target -> accumulate into it
    const std::string date = wxDateTime::Now().FormatISODate().ToStdString();
    return strip_calibrated_suffix(sel) + " (calibrated " + date + ")";
}

void ForcaCalibrationWizard::update_default_preset_name()
{
    if (m_preset_name)
        m_preset_name->SetValue(wxString::FromUTF8(default_preset_name().c_str()));
}

void ForcaCalibrationWizard::discard_transient_preset_changes()
{
    // A calibration test print sets throwaway values on the print/printer/filament presets (spiral mode,
    // temperature/flow overrides, etc.). Left dirty, they leak into the NEXT test -- e.g. MVS's spiral
    // mode pops the "spiral only works when..." warning during the flow slice. Revert them first; the real
    // results were already written to a named preset by Apply, so nothing of value is lost.
    for (Preset::Type t : { Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER }) {
        if (Tab* tab = wxGetApp().get_tab(t))
            if (tab->m_presets && tab->current_preset_is_dirty())
                tab->m_presets->discard_current_changes();
    }
    wxGetApp().load_current_presets(false);
}

// ---- Event handlers ----------------------------------------------------------------------------

void ForcaCalibrationWizard::on_calibration_changed(wxCommandEvent& /*evt*/)
{
    const int sel = m_cal_choice->GetSelection();
    m_cal = (sel >= 0 && sel < static_cast<int>(Cal::Count)) ? static_cast<Cal>(sel) : Cal::Temperature;
    if (m_cal == Cal::FlowRate) {
        m_flow_pass    = stored_flow_pass(); // pass 2 if pass 1 is already saved, else a fresh two-pass run
        m_flow_recheck = false;
    }
    update_status_label();
    update_default_preset_name();
    show_gen_page();
}

void ForcaCalibrationWizard::on_filament_changed(wxCommandEvent& /*evt*/)
{
    // The user picked a different filament here on purpose: that becomes the run (if it is one) -- no warning.
    const ForcaCalibrationStore::Key cur = current_key();
    remember_run(m_store.is_calibration_target(cur.printer, cur.nozzle, cur.filament) ? cur.filament : std::string());
    hide_warning();
    if (on_start_page())
        update_start_note();
    seed_temperatures();
    seed_mvs();
    update_status_label();
    update_default_preset_name();
    relayout();
}

void ForcaCalibrationWizard::on_goto_result(wxCommandEvent& /*evt*/)
{
    show_result_page();
}

void ForcaCalibrationWizard::show_result_page()
{
    const bool is_mvs  = (m_cal == Cal::MaxVolSpeed);
    const bool is_flow = (m_cal == Cal::FlowRate);
    const bool is_pa   = (m_cal == Cal::PressureAdvance);
    const bool is_retract = (m_cal == Cal::Retraction);
    const bool is_shrink  = (m_cal == Cal::Shrinkage);
    const int  pa_method = m_pa_method->GetSelection();
    const bool is_tower  = is_pa && pa_method == PA_TOWER;
    wxString rlabel = _L("Best temperature (C):");
    if (is_mvs)          rlabel = _L("Max flow (mm3/s):");
    else if (is_flow)    rlabel = _L("Best block number:");
    else if (is_pa)      rlabel = _L("Best PA value:");
    else if (is_retract) rlabel = _L("Retraction length (mm):");
    m_result_label->SetLabel(rlabel);
    m_result_value->SetValue("");

    // Shrinkage takes three measurements instead of the single value.
    m_result_row->ShowItems(!is_shrink);
    m_shrink_inputs->Show(is_shrink);
    if (is_shrink) {
        m_shrink_x->SetValue("");
        m_shrink_y->SetValue("");
        m_shrink_z->SetValue("");
    }

    // Show the height helper only for the towers (MVS, PA Tower, Retraction), with the formula from the settings.
    m_height_help->Show(is_mvs || is_tower || is_retract);
    m_height_value->SetValue("");
    m_height_label->SetLabel(is_retract ? _L("Rings below that section:") : _L("Measured height (mm):"));
    if (is_retract) {
        double start = 0, step = 0;
        m_retract_start->GetValue().ToDouble(&start);
        m_retract_step->GetValue().ToDouble(&step);
        m_height_hint->SetLabel(
            hard_wrap(_L("No calipers needed: each ring on the tower marks one retraction step. Find the lowest "
                         "string-free section and count the rings below it (the bottom section, 0 rings, is "
                         "Start):"), 60) +
            wxString::Format(_L("\nlength = start + rings x step = %g + rings x %g.\n"), start, step) +
            hard_wrap(_L("Enter the ring count here and the length is filled in for you, or type the length "
                         "yourself."), 60));
    }
    if (is_tower) {
        double start = 0, step = 0;
        m_pa_start->GetValue().ToDouble(&start);
        m_pa_step->GetValue().ToDouble(&step);
        m_height_hint->SetLabel(
            hard_wrap(_L("Measure the height (mm) where the corners look sharpest, then:"), 60) +
            wxString::Format(_L("\nPA = start + step x height = %g + %g x height.\n"), start, step) +
            hard_wrap(_L("Enter that height here and the PA value is filled in for you, or type the value "
                         "yourself."), 60));
    }
    if (is_mvs) {
        double start = 0, step = 0;
        m_mvs_start->GetValue().ToDouble(&start);
        m_mvs_step->GetValue().ToDouble(&step);
        m_height_hint->SetLabel(wxString::Format(
            _L("New to this? Note the height (mm) of the last good-looking section, then:\n"
               "max flow = start + height x step = %g + height x %g.\n"
               "Enter that height here and the Max flow is filled in for you, or type the flow yourself."),
            start, step));
        m_height_hint->Wrap(FromDIP(460));
    }

    // Per-calibration guidance line on the result page.
    if (is_flow)
        update_flow_hint();
    else if (is_pa && pa_method == PA_LINE)
        m_result_hint->SetLabel(hard_wrap(_L("Enter the value printed next to the line with the most even "
                                             "width."), 52));
    else if (is_pa && pa_method == PA_PATTERN)
        m_result_hint->SetLabel(hard_wrap(_L("Enter the value printed above the pattern with the sharpest, "
                                             "cleanest corners."), 52));
    else if (is_shrink)
        m_result_hint->SetLabel(hard_wrap(wxString::Format(
            _L("Enter what you measured. Leave Z empty to keep the current Z value. Current compensation: "
               "XY %.2f%%, Z %.2f%% (already applied to this print, so it is accounted for)."),
            current_percent("filament_shrink"), current_percent("filament_shrinkage_compensation_z")), 52));
    else
        m_result_hint->SetLabel("");

    update_default_preset_name();

    // Keep the whole run in ONE preset: the name is editable only for the first calibration (a fresh base
    // filament). Once results have been written into a calibration preset, lock the name to it so later
    // steps accumulate there instead of spawning many one-setting profiles.
    const ForcaCalibrationStore::Key k = current_key();
    const bool locked = m_store.is_calibration_target(k.printer, k.nozzle, selected_filament_name());
    m_preset_name->SetEditable(!locked);

    m_book->ChangeSelection(PAGE_RESULT); // no event (see show_gen_page)
    m_book->GetPage(PAGE_RESULT)->Layout(); // rows were shown/hidden per calibration
    relayout();
}

void ForcaCalibrationWizard::on_height_changed(wxCommandEvent& /*evt*/)
{
    double height = 0, start = 0, step = 0;
    if (!m_height_value->GetValue().ToDouble(&height) || height < 0 || (height == 0 && m_cal != Cal::Retraction))
        return;
    if (m_cal == Cal::PressureAdvance) {
        // PA Tower (Orca's documented formula): PA = start + step x measured height (mm).
        double end = 0;
        if (!m_pa_start->GetValue().ToDouble(&start) || !m_pa_end->GetValue().ToDouble(&end) ||
            !m_pa_step->GetValue().ToDouble(&step))
            return;
        const double pa = std::min(end, start + step * height);
        m_result_value->SetValue(wxString::Format("%.3f", pa));
        return;
    }
    if (m_cal == Cal::Retraction) {
        // Here the field is a RING COUNT: each ring marks one 1 mm band, and GCode.cpp (Calib_Retraction_tower)
        // adds one step per band, so length = start + rings * step.
        double end = 0;
        if (!m_retract_start->GetValue().ToDouble(&start) || !m_retract_end->GetValue().ToDouble(&end) ||
            !m_retract_step->GetValue().ToDouble(&step))
            return;
        const double len = std::min(end, start + std::floor(height + 1e-6) * step);
        m_result_value->SetValue(wxString::Format("%.2f", len));
        return;
    }
    if (!m_mvs_start->GetValue().ToDouble(&start) || !m_mvs_step->GetValue().ToDouble(&step))
        return;
    const double flow = start + height * step;
    m_result_value->SetValue(wxString::Format("%.1f", flow));
}

void ForcaCalibrationWizard::seed_pa()
{
    // Defaults from Orca's PA_Calibration_Dlg::reset_params. Direct drive: Tower/Line 0-0.1 step 0.002,
    // Pattern 0-0.08 step 0.005. Bowden: end 1.0, step 0.02 (Pattern 0.05).
    const bool bowden  = (m_pa_extruder->GetSelection() == 1);
    const bool pattern = (m_pa_method->GetSelection() == PA_PATTERN);
    m_pa_start->SetValue("0.0");
    m_pa_end->SetValue(bowden ? "1.0" : (pattern ? "0.08" : "0.1"));
    m_pa_step->SetValue(bowden ? (pattern ? "0.05" : "0.02") : (pattern ? "0.005" : "0.002"));
    m_pa_pattern_opts->Show(pattern);
    update_pa_coach();
}

void ForcaCalibrationWizard::update_pa_coach()
{
    wxString text;
    switch (m_pa_method->GetSelection()) {
    case PA_LINE:
        text = _L("Prints a set of lines at increasing PA values, each labelled with its value. Pick the line "
                  "with the most even width -- no bulge where the speed changes (PA too low) and no thin gap "
                  "(too high) -- and enter that line's value.");
        break;
    case PA_PATTERN:
        text = _L("Prints nested corner patterns at increasing PA values, each labelled with its value. Pick "
                  "the one with the sharpest, cleanest corners -- no bulge (PA too low) and no rounding or gaps "
                  "(too high) -- and enter its value.");
        break;
    default:
        text = _L("Prints a hollow tower whose PA rises one step per mm of height. Find the height where the "
                  "corners look sharpest -- no bulge (PA too low) and no gaps or rounding (too high) -- measure "
                  "it with calipers, and enter it on the result page to get the PA value.");
        break;
    }
    m_pa_coach->SetLabel(hard_wrap(text, 60));
}

void ForcaCalibrationWizard::on_pa_setup_changed(wxCommandEvent& /*evt*/)
{
    seed_pa();
    m_pa_coach->GetParent()->Layout(); // the PA page: re-place the shown/hidden Pattern options
    relayout();
}

void ForcaCalibrationWizard::on_back_from_result(wxCommandEvent& /*evt*/)
{
    show_gen_page();
}

// ---- Temperature -------------------------------------------------------------------------------

void ForcaCalibrationWizard::get_type_temp_bounds(int& lo, int& hi) const
{
    lo = TEMP_ABS_MIN;
    hi = TEMP_ABS_MAX;

    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;
    const Preset* preset = bundle->filaments.find_preset(selected_filament_name());
    if (!preset)
        return;
    const auto* ft = preset->config.option<ConfigOptionStrings>("filament_type");
    if (ft == nullptr || ft->values.empty())
        return;

    const wxString t = wxString::FromUTF8(ft->values.front().c_str()).Upper();

    int start = 0, end = 0;
    if (t.Contains("TPU") || t.Contains("TPE"))      { start = 240; end = 210; }
    else if (t.Contains("PCTG"))                     { start = 280; end = 240; }
    else if (t.Contains("PETG"))                     { start = 250; end = 230; }
    else if (t.Contains("PET"))                      { start = 320; end = 280; }
    else if (t.Contains("ASA") || t.Contains("ABS")) { start = 270; end = 230; }
    else if (t.Contains("PA"))                       { start = 320; end = 280; }
    else if (t.Contains("PC"))                       { start = 320; end = 280; }
    else if (t.Contains("PLA"))                      { start = 230; end = 190; }
    else return;

    lo = std::max(TEMP_ABS_MIN, end - TEMP_HEADROOM);
    hi = std::min(TEMP_ABS_MAX, start + TEMP_HEADROOM);
}

void ForcaCalibrationWizard::seed_temperatures()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle || !m_temp_start || !m_temp_end)
        return;

    const Preset* preset = bundle->filaments.find_preset(selected_filament_name());
    if (!preset)
        return;
    const auto* nt = preset->config.option<ConfigOptionInts>("nozzle_temperature");
    if (nt == nullptr || nt->values.empty())
        return;

    int lo = TEMP_ABS_MIN, hi = TEMP_ABS_MAX;
    get_type_temp_bounds(lo, hi);

    const int center = nt->values.front();
    int start = ((center + 20) / 5) * 5;
    int end   = ((center - 20) / 5) * 5;
    if (start > hi) start = hi;
    if (end < lo)   end = lo;
    if (start < end + 5) start = end + 5;
    m_temp_start->SetValue(wxString::Format("%d", start));
    m_temp_end->SetValue(wxString::Format("%d", end));
}

bool ForcaCalibrationWizard::build_params_for_current_cal(Calib_Params& out, wxString& err)
{
    out = Calib_Params();
    if (m_cal == Cal::Temperature) {
        int lo = TEMP_ABS_MIN, hi = TEMP_ABS_MAX;
        get_type_temp_bounds(lo, hi);
        long start = 0, end = 0;
        if (!m_temp_start->GetValue().ToLong(&start) || !m_temp_end->GetValue().ToLong(&end) ||
            start > hi || end < lo || start < end + 5) {
            err = wxString::Format(_L("Please enter a valid temperature range for this filament:\n"
                                      "Between %d and %d C, with Start at least 5 C above End."), lo, hi);
            return false;
        }
        out.start = static_cast<double>(start);
        out.end   = static_cast<double>(end);
        out.mode  = CalibMode::Calib_Temp_Tower;
        return true;
    }
    if (m_cal == Cal::MaxVolSpeed) {
        double start = 0, end = 0, step = 0;
        if (!m_mvs_start->GetValue().ToDouble(&start) || !m_mvs_end->GetValue().ToDouble(&end) ||
            !m_mvs_step->GetValue().ToDouble(&step) || start <= 0 || step <= 0 || end < start + step) {
            err = _L("Please enter valid values:\nStart > 0, Step > 0, End > Start + Step.");
            return false;
        }
        out.start = start;
        out.end   = end;
        out.step  = step;
        out.mode  = CalibMode::Calib_Vol_speed_Tower;
        return true;
    }

    if (m_cal == Cal::FlowRate) {
        // Nothing to validate on generate; the method/pass are tracked by m_flow_method / m_flow_pass.
        out.mode = CalibMode::Calib_Flow_Rate;
        return true;
    }

    if (m_cal == Cal::Retraction) {
        double rstart = 0, rend = 0, rstep = 0;
        if (!m_retract_start->GetValue().ToDouble(&rstart) || !m_retract_end->GetValue().ToDouble(&rend) ||
            !m_retract_step->GetValue().ToDouble(&rstep) || rstart < 0 || rstep <= 0 || rend < rstart + rstep ||
            rend > RETRACT_MAX) {
            err = wxString::Format(_L("Please enter valid retraction values:\nStart >= 0, Step > 0, "
                                      "End > Start + Step, End <= %g mm."), RETRACT_MAX);
            return false;
        }
        out.start = rstart;
        out.end   = rend;
        out.step  = rstep;
        out.mode  = CalibMode::Calib_Retraction_tower;
        return true;
    }

    if (m_cal == Cal::Shrinkage || m_cal == Cal::FinalCheck) {
        // Not Orca calib modes: plain models, no calibration g-code. Record the shrinkage nominals for the store.
        out.mode = CalibMode::Calib_None;
        if (m_cal == Cal::Shrinkage) {
            out.start = SHRINK_XY_MM;
            out.end   = SHRINK_Z_MM;
        }
        return true;
    }

    // Pressure Advance (Tower / Line / Pattern).
    double pstart = 0, pend = 0, pstep = 0;
    if (!m_pa_start->GetValue().ToDouble(&pstart) || !m_pa_end->GetValue().ToDouble(&pend) ||
        !m_pa_step->GetValue().ToDouble(&pstep) || pstart < 0 || pstep < 0.0001 || pend < pstart + pstep) {
        err = _L("Please enter valid PA values:\nStart >= 0, Step >= 0.001, End > Start + Step.");
        return false;
    }
    out.start = pstart;
    out.end   = pend;
    out.step  = pstep;
    switch (m_pa_method->GetSelection()) {
    case PA_LINE:
        out.mode          = CalibMode::Calib_PA_Line;
        out.print_numbers = true; // label each line with its PA value
        break;
    case PA_PATTERN:
        out.mode          = CalibMode::Calib_PA_Pattern;
        out.print_numbers = true;
        if (!parse_positive_list(m_pa_accels->GetValue(), out.accelerations) ||
            !parse_positive_list(m_pa_speeds->GetValue(), out.speeds)) {
            err = _L("Accelerations and speeds must be comma-separated positive numbers, or left blank.");
            return false;
        }
        // Same swapped-input guard as Orca's PA dialog.
        if (!out.accelerations.empty() && !out.speeds.empty() &&
            *std::min_element(out.accelerations.begin(), out.accelerations.end()) <=
                *std::max_element(out.speeds.begin(), out.speeds.end())) {
            err = _L("Acceleration values must be greater than speed values.\nPlease verify the inputs.");
            return false;
        }
        break;
    default:
        out.mode = CalibMode::Calib_PA_Tower; // read by measured height on the result page
        break;
    }
    return true;
}

void ForcaCalibrationWizard::launch_calib(const Calib_Params& params, bool skip_confirm)
{
    if (m_cal == Cal::Temperature)
        m_plater->calib_temp(params, skip_confirm);
    else if (m_cal == Cal::MaxVolSpeed)
        m_plater->calib_max_vol_speed(params, skip_confirm);
    else if (m_cal == Cal::FlowRate) // m_flow_pass: classic coarse/fine, or YOLO Recommended/Perfectionist
        m_plater->calib_flowrate(flow_is_yolo(), m_flow_pass, ipArchimedeanChords, skip_confirm);
    else if (m_cal == Cal::Retraction)
        m_plater->calib_retraction(params, skip_confirm);
    else if (m_cal == Cal::Shrinkage)
        launch_shrinkage_test(skip_confirm);
    else if (m_cal == Cal::FinalCheck)
        launch_final_check(skip_confirm);
    else { // Pressure Advance (params.mode picks Tower / Line / Pattern)
        // Pass the long-lived member, not the caller's local: PA Pattern keeps a reference to these params and
        // reads it again on later reslices (e.g. opening Preview); a dangling one froze the app.
        m_launched_params = params;
        m_plater->calib_pa(m_launched_params, skip_confirm);
    }
}

void ForcaCalibrationWizard::launch_current_test(bool send_after)
{
    Calib_Params params;
    wxString err;
    if (!build_params_for_current_cal(params, err)) {
        MessageDialog dlg(this, err, cal_display(), wxICON_WARNING | wxOK);
        dlg.ShowModal();
        return;
    }

    // Drop the previous test's throwaway preset edits (e.g. MVS spiral mode) before setting up this one,
    // so they don't leak into the slice. Only after the session's first test (see the flag's note).
    if (s_forca_calib_test_generated)
        discard_transient_preset_changes();

    // Make the chosen filament active so the test prints in it.
    const std::string fname = selected_filament_name();
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle && fname != bundle->filaments.get_selected_preset_name())
        if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
            tab->select_preset(fname);

    const bool has_result = (m_cal != Cal::FinalCheck); // the final check print saves nothing
    if (has_result) {
        m_store.set_pending(current_key(), params.start, params.end);
    } else {
        m_store.set_done(current_key(), 0.0, std::string()); // only marks the optional marker as printed
        m_run_finished = true;                                // the final check ends the run
    }

    // After the session's first test, the plate + presets hold only throwaway setup, so skip
    // new_project()'s confirm dialogs; the first test still asks, protecting the user's own work.
    const bool skip_confirm = s_forca_calib_test_generated;
    s_forca_calib_test_generated = true;

    if (send_after) {
        // Fully automated: put the test on the plate, slice it, then run the printer's Print action, then
        // land on the result page. The one-shot hook fires when slicing+gcode finishes.
        m_plater->set_one_shot_slice_completed_callback([this, has_result](bool ok) {
            // Defer to the next idle so the slice-completed event fully unwinds before we open the
            // (modal) send dialog. Opening it inline mis-set focus/z-order (dialog appeared only after
            // a stray click, with the "blocked window" ding).
            if (ok) {
                CallAfter([this, has_result]() {
                    m_plater->print_current_plate(); // printer-specific send dialog (modal)
                    if (has_result)
                        show_result_page();
                    else
                        notify_state_changed(); // final check: nothing to enter, stay on its page
                });
            } else {
                CallAfter([this]() {
                    MessageDialog dlg(this,
                        _L("Slicing did not finish, so nothing was sent. Fix the error and try again."),
                        cal_display(), wxICON_WARNING | wxOK);
                    dlg.ShowModal();
                });
            }
        });
        launch_calib(params, skip_confirm);
        m_plater->reslice();
        // Like the Slice button: show the Preview (in the tab's plate area) while it slices. no_slice: the
        // reslice above already started it.
        m_plater->select_view_3D("Preview", true);
        notify_state_changed();
        // The hook advances us to the result page when slicing completes.
    } else {
        // Semi-automated: put the test on the plate (it shows right beside the wizard) for the user to slice/print
        // their own way; the gen page's status line says it is pending.
        launch_calib(params, skip_confirm);
        update_status_label();
        notify_state_changed();
    }
}

// ---- Shrinkage / final check (plain models, not Orca calib modes) ------------------------------

void ForcaCalibrationWizard::launch_shrinkage_test(bool skip_confirm)
{
    if (m_plater->new_project(skip_confirm, false, _L("Shrinkage test")) == wxID_CANCEL)
        return;
    wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    // A previous test's calib mode would otherwise keep driving per-layer g-code (e.g. the retraction tower's
    // lengths); load_files() resets it for loaded models, but this mesh is added directly.
    m_plater->get_partplate_list().get_current_fff_print().set_calib_params(Calib_Params());

    // Forca's own geometry (clean-room, built here): four non-overlapping bars forming a square open frame of
    // outer size SHRINK_XY_MM, plus a post standing on one corner reaching SHRINK_Z_MM. Open so it can shrink
    // freely; large so a fraction of a percent is measurable with calipers.
    const double L = SHRINK_XY_MM, W = SHRINK_BAR_MM, H = SHRINK_FRAME_H_MM;
    auto box = [](double x, double y, double z, double dx, double dy, double dz) {
        indexed_triangle_set its = its_make_cube(dx, dy, dz); // spans [0,dx]x[0,dy]x[0,dz]
        for (Vec3f& v : its.vertices)
            v += Vec3f(float(x), float(y), float(z));
        return its;
    };
    indexed_triangle_set frame = box(0, 0, 0, L, W, H);           // front bar (full width)
    its_merge(frame, box(0, L - W, 0, L, W, H));                   // back bar (full width)
    its_merge(frame, box(0, W, 0, W, L - 2 * W, H));               // left bar (between front/back)
    its_merge(frame, box(L - W, W, 0, W, L - 2 * W, H));           // right bar
    its_merge(frame, box(0, 0, H, W, W, SHRINK_Z_MM - H));         // Z post on the front-left corner
    wxGetApp().obj_list()->load_mesh_object(TriangleMesh(std::move(frame)), _L("Forca shrinkage frame"), true);
}

void ForcaCalibrationWizard::launch_final_check(bool skip_confirm)
{
    if (m_plater->new_project(skip_confirm, false, _L("Autodesk FDM Test")) == wxID_CANCEL)
        return;
    wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);
    // Orca's bundled handy model (same file as Add > Handy models > Autodesk FDM Test).
    m_plater->load_files({ boost::filesystem::path(resources_dir()) / "handy_models" / "ksr_fdmtest_v4.drc" },
                         LoadStrategy::LoadModel);
}

double ForcaCalibrationWizard::current_percent(const char* key) const
{
    if (PresetBundle* b = wxGetApp().preset_bundle)
        if (const Preset* fp = b->filaments.find_preset(selected_filament_name()))
            if (const auto* opt = fp->config.option<ConfigOptionPercents>(key); opt && !opt->values.empty())
                return opt->values.front();
    return 100.0;
}

void ForcaCalibrationWizard::on_generate(wxCommandEvent& /*evt*/)       { launch_current_test(false); }
void ForcaCalibrationWizard::on_slice_and_send(wxCommandEvent& /*evt*/) { launch_current_test(true); }

// ---- Max Volumetric Speed ----------------------------------------------------------------------

void ForcaCalibrationWizard::seed_mvs()
{
    // Orca's defaults bracket a typical filament; the fields keep whatever the user last set otherwise.
    if (m_mvs_start && m_mvs_start->GetValue().IsEmpty()) m_mvs_start->SetValue("5");
    if (m_mvs_end   && m_mvs_end->GetValue().IsEmpty())   m_mvs_end->SetValue("20");
    if (m_mvs_step  && m_mvs_step->GetValue().IsEmpty())  m_mvs_step->SetValue("0.5");
}

// ---- Result / apply ----------------------------------------------------------------------------

void ForcaCalibrationWizard::update_flow_hint()
{
    if (!m_result_hint || m_cal != Cal::FlowRate)
        return;

    const double cur_flow = current_flow_ratio();
    const bool   yolo     = flow_is_yolo();

    wxString pass_note;
    if (m_flow_recheck)
        pass_note = _L("This is the optional recheck after Pressure Advance.");
    else if (m_flow_pass == 1)
        pass_note = _L("This is pass 1 of 2; a fine pass follows.");
    else
        pass_note = _L("This is pass 2 of 2; it is the final pass.");
    wxString base = wxString::Format(
        _L("Enter the number under the block with the smoothest, flattest top. Negative = less flow, "
           "positive = more flow. %s"), pass_note);

    double modifier = 0.0;
    wxString formula;
    if (m_result_value->GetValue().ToDouble(&modifier))
        formula = yolo ? wxString::Format(_L("New flow: %.3f + %g = %.3f"), cur_flow, modifier, new_flow_ratio(modifier))
                       : wxString::Format(_L("New flow: %.3f x (1 + %g/100) = %.3f"), cur_flow, modifier,
                                          new_flow_ratio(modifier));
    else
        formula = yolo ? wxString::Format(_L("New flow: %.3f + number."), cur_flow)
                       : wxString::Format(_L("New flow: %.3f x (1 + number/100)."), cur_flow);

    // Hard-wrap (explicit newlines) so line lengths are guaranteed; wxStaticText::Wrap() left this
    // unwrapped, clipping the text and widening the dialog. The formula stays on its own short line.
    m_result_hint->SetLabel(hard_wrap(base, 52) + "\n" + formula);
    Layout();
}

bool ForcaCalibrationWizard::flow_is_yolo() const
{
    return m_flow_method && m_flow_method->GetSelection() == FLOW_YOLO;
}

double ForcaCalibrationWizard::current_flow_ratio() const
{
    if (PresetBundle* b = wxGetApp().preset_bundle)
        if (const Preset* fp = b->filaments.find_preset(selected_filament_name()))
            if (const auto* fr = fp->config.option<ConfigOptionFloats>("filament_flow_ratio"); fr && !fr->values.empty())
                return fr->values.front();
    return 1.0;
}

double ForcaCalibrationWizard::new_flow_ratio(double block_value) const
{
    // Mirrors adjust_settings_for_flowrate_calib: classic blocks print at 1 + N/100 of the current flow; YOLO
    // blocks at (cur + N) / cur, i.e. an absolute offset.
    const double cur = current_flow_ratio();
    return flow_is_yolo() ? cur + block_value : cur * (1.0 + block_value / 100.0);
}

void ForcaCalibrationWizard::update_flow_labels()
{
    if (!m_flow_pass_label || !m_flow_coach)
        return;
    const bool yolo = flow_is_yolo();
    wxString pass;
    if (m_flow_recheck)
        pass = yolo ? _L("Recheck after PA (Perfectionist pass)") : _L("Recheck after PA (fine pass)");
    else if (yolo)
        pass = m_flow_pass == 1 ? _L("Pass 1 of 2 (Recommended)") : _L("Pass 2 of 2 (Perfectionist)");
    else
        pass = m_flow_pass == 1 ? _L("Pass 1 of 2 (coarse)") : _L("Pass 2 of 2 (fine)");
    m_flow_pass_label->SetLabel(pass);

    const wxString text = yolo
        ? _L("Orca's YOLO test prints one row of blocks, each labelled with a small flow offset such as -0.02 or "
             "+0.01. Pick the block whose top is smoothest and flattest -- not bulging (too much flow) and not "
             "gappy (too little) -- and enter its number exactly as printed. The Recommended pass is usually "
             "enough; the Perfectionist pass refines it in finer steps.")
        : _L("This prints a row of blocks, each at a slightly different flow ratio and labelled with a number. "
             "Look at the top surfaces and pick the block whose top is smoothest and flattest -- not bulging "
             "(too much flow) and not gappy (too little). You will enter that block's number. Flow runs in two "
             "passes: a coarse pass, then a fine pass around your first result.");
    m_flow_coach->SetLabel(hard_wrap(text, 60));
}

void ForcaCalibrationWizard::on_flow_method_changed(wxCommandEvent& /*evt*/)
{
    // A method change starts that method's run over (a recheck keeps its single fine pass).
    if (!m_flow_recheck)
        m_flow_pass = 1;
    update_flow_labels();
    m_flow_coach->GetParent()->Layout();
    relayout();
}

void ForcaCalibrationWizard::on_result_value_changed(wxCommandEvent& /*evt*/)
{
    if (m_cal == Cal::FlowRate)
        update_flow_hint();
}

void ForcaCalibrationWizard::on_apply_result(wxCommandEvent& /*evt*/)
{
    if (m_cal == Cal::FinalCheck)
        return; // nothing to save (its page has no result button; guard against a stale result page)

    double value = 0.0;
    double shrink_z_pct = 0.0; // Shrinkage only: the Z percent written, for the confirmation message
    std::map<std::string, ConfigOption*> key_values;

    if (m_cal == Cal::Temperature) {
        int lo = TEMP_ABS_MIN, hi = TEMP_ABS_MAX;
        get_type_temp_bounds(lo, hi);
        long temp = 0;
        if (!m_result_value->GetValue().ToLong(&temp) || temp < lo || temp > hi) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter a temperature between %d and %d C for this filament."), lo, hi),
                _L("Temperature calibration"), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        value = static_cast<double>(temp);
        key_values["nozzle_temperature"]               = new ConfigOptionInts{ static_cast<int>(temp) };
        key_values["nozzle_temperature_initial_layer"] = new ConfigOptionInts{ static_cast<int>(temp) };
    } else if (m_cal == Cal::MaxVolSpeed) {
        if (!m_result_value->GetValue().ToDouble(&value) || value < MVS_MIN || value > MVS_MAX) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter a max volumetric speed between %.1f and %.1f mm3/s."), MVS_MIN, MVS_MAX),
                _L("Max volumetric speed"), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        key_values["filament_max_volumetric_speed"] = new ConfigOptionFloats{ value };
    }

    if (m_cal == Cal::FlowRate) {
        const double mod_max  = flow_is_yolo() ? FLOW_YOLO_MOD_MAX : FLOW_MOD_MAX;
        double       modifier = 0.0;
        if (!m_result_value->GetValue().ToDouble(&modifier) || std::abs(modifier) > mod_max) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter the best block's number, between -%g and %g."), mod_max, mod_max),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        value = new_flow_ratio(modifier);
        if (value < FLOW_MIN || value > FLOW_MAX) {
            MessageDialog dlg(this,
                wxString::Format(_L("That block number would set the flow ratio to %.3f, outside the safe range "
                                    "%.2f-%.2f. Please re-check the number."), value, FLOW_MIN, FLOW_MAX),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        key_values["filament_flow_ratio"] = new ConfigOptionFloats{ value };
    }

    if (m_cal == Cal::PressureAdvance) {
        if (!m_result_value->GetValue().ToDouble(&value) || value < PA_MIN || value > PA_MAX) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter a pressure-advance value between %.2f and %.2f."), PA_MIN, PA_MAX),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        key_values["pressure_advance"]        = new ConfigOptionFloats{ value };
        key_values["enable_pressure_advance"] = new ConfigOptionBools{ true };
    }

    if (m_cal == Cal::Retraction) {
        if (!m_result_value->GetValue().ToDouble(&value) || value < 0 || value > RETRACT_MAX) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter a retraction length between 0 and %g mm."), RETRACT_MAX),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        // Per-filament override of the printer's retraction length (nullable filament option).
        key_values["filament_retraction_length"] = new ConfigOptionFloatsNullable{ value };
    }

    if (m_cal == Cal::Shrinkage) {
        // Orca's shrink % = measured / nominal x 100 (it scales the part up by 100/%). The test printed with the
        // current compensation already applied, so the true percent is current x measured / nominal.
        double mx = 0, my = 0, mz = 0;
        const bool have_z = !m_shrink_z->GetValue().Trim().IsEmpty();
        const auto bad = [](double m, double nominal) { return m < nominal * 0.8 || m > nominal * 1.2; };
        if (!m_shrink_x->GetValue().ToDouble(&mx) || !m_shrink_y->GetValue().ToDouble(&my) ||
            bad(mx, SHRINK_XY_MM) || bad(my, SHRINK_XY_MM) ||
            (have_z && (!m_shrink_z->GetValue().ToDouble(&mz) || bad(mz, SHRINK_Z_MM)))) {
            MessageDialog dlg(this,
                wxString::Format(_L("Please enter the measured X and Y widths (about %g mm) and, optionally, the post "
                                    "height (about %g mm)."), SHRINK_XY_MM, SHRINK_Z_MM),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        value = current_percent("filament_shrink") * ((mx + my) / 2.0) / SHRINK_XY_MM;
        const double z_pct = have_z ? current_percent("filament_shrinkage_compensation_z") * mz / SHRINK_Z_MM : 0.0;
        if (value < SHRINK_PCT_MIN || value > SHRINK_PCT_MAX || (have_z && (z_pct < SHRINK_PCT_MIN || z_pct > SHRINK_PCT_MAX))) {
            MessageDialog dlg(this,
                wxString::Format(_L("Those measurements give XY %.2f%% / Z %.2f%%, outside the plausible range "
                                    "%g-%g%%. Please re-check them."), value, z_pct, SHRINK_PCT_MIN, SHRINK_PCT_MAX),
                cal_display(), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        key_values["filament_shrink"] = new ConfigOptionPercents{ value };
        if (have_z) {
            key_values["filament_shrinkage_compensation_z"] = new ConfigOptionPercents{ z_pct };
            shrink_z_pct = z_pct;
        }
    }

    const std::string target_name = m_preset_name->GetValue().Trim(true).Trim(false).ToUTF8().data();
    if (target_name.empty()) {
        MessageDialog dlg(this, _L("Please enter a name for the preset."),
                          cal_display(), wxICON_WARNING | wxOK);
        dlg.ShowModal();
        return;
    }

    // First Apply of a run (the source is not yet a calibration target): snapshot the values the run starts from,
    // for the before/after table. The source preset is still untouched (the save below works on a copy).
    const ForcaCalibrationStore::Key src_key = current_key();
    const bool first_apply = !m_store.is_calibration_target(src_key.printer, src_key.nozzle, src_key.filament);
    ForcaCalibrationStore::Run run;
    if (first_apply) {
        run.printer = src_key.printer;
        run.nozzle  = src_key.nozzle;
        run.base    = src_key.filament;
        if (PresetBundle* b = wxGetApp().preset_bundle)
            if (const Preset* src = b->filaments.find_preset(src_key.filament))
                for (const TableRowDef& d : table_rows())
                    if (auto v = first_value(src->config, d.key))
                        run.before[d.key] = *v;
    }

    // A brand-new preset may reuse the name of one the user deleted (the default name is "<base> (calibrated
    // <date>)"): drop that name's old history so the new run doesn't inherit its progress or "before" values.
    if (PresetBundle* b = wxGetApp().preset_bundle)
        if (!b->filaments.find_preset(b->filaments.get_preset_name_by_alias(target_name)))
            m_store.forget_filament(src_key.printer, src_key.nozzle, target_name);

    wxString err;
    const std::string saved_name = create_or_update_filament_preset(target_name, key_values, err);
    if (saved_name.empty()) {
        MessageDialog dlg(this, err.IsEmpty() ? _L("Could not save the preset.") : err,
                          cal_display(), wxICON_WARNING | wxOK);
        dlg.ShowModal();
        return;
    }

    // Flow records how many passes are saved (1, 2; 3 = the optional recheck) so progress fills in halves and a
    // run resumes on the right pass.
    const int pass = (m_cal == Cal::FlowRate) ? (m_flow_recheck ? 3 : m_flow_pass) : 0;
    m_store.set_done(src_key, value, saved_name, pass);
    ForcaCalibrationStore::Key target_key = src_key;
    target_key.filament = saved_name;
    m_store.set_done(target_key, value, saved_name, pass);
    if (first_apply) {
        run.target = saved_name;
        m_store.start_run(run);
        // Steps skipped before the run had a preset move over to it.
        for (int i = 0; i < static_cast<int>(Cal::Count); ++i) {
            ForcaCalibrationStore::Key sk = src_key, tk = target_key;
            sk.calibration = tk.calibration = cal_id(static_cast<Cal>(i));
            ForcaCalibrationStore::Record sr, tr;
            if (m_store.get(sk, sr) && sr.status == "skipped" && !m_store.get(tk, tr))
                m_store.set_skipped(tk, saved_name);
        }
    }
    remember_run(saved_name);

    if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
        tab->select_preset(saved_name);

    wxString val_str = format_result(value);
    if (shrink_z_pct > 0)
        val_str += wxString::Format(_L(", Z %.2f%%"), shrink_z_pct);

    // Work out what comes next. Forca order: Temp -> MVS -> Flow[pass 1 -> pass 2] -> PA -> [optional flow
    // recheck] -> Retraction -> Shrinkage -> [optional final check print].
    const Cal done         = m_cal;
    const bool was_recheck = m_flow_recheck;
    Cal       next         = done;
    int       next_pass    = 1;
    wxString  next_line;
    switch (done) {
    case Cal::Temperature: next = Cal::MaxVolSpeed; next_line = _L("Next up: Max Volumetric Speed."); break;
    case Cal::MaxVolSpeed: next = Cal::FlowRate;    next_line = _L("Next up: Flow Rate."); break;
    case Cal::FlowRate:
        if (was_recheck) {
            next = Cal::Retraction;
            next_line = _L("Flow recheck saved. Next up: Retraction.");
        } else if (m_flow_pass == 1) {
            next_pass = 2;
            next_line = _L("Pass 1 saved. Next: Flow Rate pass 2.");
        } else {
            next = Cal::PressureAdvance;
            next_line = _L("Flow Rate complete. Next up: Pressure Advance.");
        }
        break;
    case Cal::PressureAdvance: next = Cal::Retraction; break; // the optional recheck is offered below
    case Cal::Retraction:      next = Cal::Shrinkage;  next_line = _L("Next up: Shrinkage."); break;
    default:
        next = Cal::FinalCheck;
        next_line = _L("That completes the calibrations. Optionally, print the final check to confirm the whole profile.");
        m_run_finished = true; // the sequence was finished in this visit: returning to the tab starts a new one
        break;
    }

    wxString msg = wxString::Format(_L("Saved %s to the filament preset:\n%s\n\nIt is now selected."),
                                    val_str, wxString::FromUTF8(saved_name.c_str()));
    bool recheck = false;
    if (done == Cal::PressureAdvance) {
        // Tuning PA can shift apparent flow, so offer one optional fine flow pass before moving on.
        msg += wxString("\n\n") + _L("Optional: tuning pressure advance can shift how flow looks. Run one fine "
                                     "Flow Rate pass now to recheck it?\n\nYes = recheck flow, then Retraction.\n"
                                     "No = go straight to Retraction.");
        MessageDialog dlg(this, msg, cal_display(), wxICON_QUESTION | wxYES_NO);
        recheck = (dlg.ShowModal() == wxID_YES);
    } else {
        msg += wxString("\n\n") + next_line;
        MessageDialog dlg(this, msg, cal_display(), wxICON_INFORMATION | wxOK);
        dlg.ShowModal();
    }

    // Sync the filament picker to the now-active (target) preset so the next step accumulates into it.
    refresh_presets(false); // no resume jump: the next step is chosen just below

    if (recheck) {
        next      = Cal::FlowRate;
        next_pass = 2; // the fine / Perfectionist pass of the chosen method
    }
    set_cal(next);
    if (next == Cal::FlowRate) {
        m_flow_pass    = next_pass;
        m_flow_recheck = recheck;
    } else {
        m_flow_recheck = false;
    }
    update_status_label();
    update_default_preset_name();
    show_gen_page();
}

std::string ForcaCalibrationWizard::create_or_update_filament_preset(
    const std::string& target_name, const std::map<std::string, ConfigOption*>& key_values, wxString& err)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle) {
        err = _L("No preset bundle is available.");
        return {};
    }

    PresetCollection* filaments = &bundle->filaments;
    const std::string old_name = selected_filament_name();

    Preset* preset = filaments->find_preset(old_name);
    if (!preset) {
        err = wxString::Format(_L("The filament preset '%s' was not found."), wxString::FromUTF8(old_name.c_str()));
        return {};
    }

    Preset temp_preset = *preset;

    const std::string resolved = filaments->get_preset_name_by_alias(target_name);
    bool    exist_preset = false;
    Preset* new_preset   = filaments->find_preset(resolved);
    if (new_preset) {
        if (new_preset->is_system) {
            err = _L("The name cannot be the same as a system preset name.");
            return {};
        }
        if (new_preset != preset) {
            err = _L("A preset with that name already exists.");
            return {};
        }
        if (new_preset != &filaments->get_edited_preset())
            new_preset = &temp_preset;
        exist_preset = true;
    } else {
        new_preset = &temp_preset;
    }

    for (const auto& item : key_values) {
        // Match the preset's own vector length: filament presets with extruder variants (e.g. the H2S's
        // "Direct Drive Standard" + "High Flow") hold one value per variant. A shorter vector made Forca throw
        // "Assigning from an empty vector" as soon as a second filament was selected. The calibrated value is
        // written to every variant (resize() repeats the first value).
        auto* nv = dynamic_cast<ConfigOptionVectorBase*>(item.second);
        const auto* ov = dynamic_cast<const ConfigOptionVectorBase*>(new_preset->config.option(item.first));
        if (nv && ov && !nv->empty() && ov->size() > nv->size())
            nv->resize(ov->size());
        new_preset->config.set_key_value(item.first, item.second);
    }

    filaments->save_current_preset(resolved, false, false, new_preset);

    new_preset = filaments->find_preset(resolved, false, true);
    if (!new_preset) {
        err = _L("Creating the new preset failed.");
        return {};
    }
    new_preset->sync_info = exist_preset ? "update" : "create";
    if (!exist_preset && wxGetApp().is_user_login())
        new_preset->user_id = wxGetApp().getAgent()->get_user_id();
    new_preset->save_info();

    bundle->update_compatible(PresetSelectCompatibleType::Never);
    if (!exist_preset)
        wxGetApp().plater()->sidebar().update_presets_from_to(Preset::TYPE_FILAMENT, old_name, new_preset->name);

    return resolved;
}

// ---- Tab integration ---------------------------------------------------------------------------

void ForcaCalibrationWizard::sys_color_changed()
{
    // Runs before GUI_App's generic dark/light pass over the main frame. That pass only converts colours in
    // StateColor's table, so the wizard's own colours (e.g. #2B2F3A) would never switch back; re-apply them here to
    // every window still wearing one of them: the light or dark palette value, or the generic pass's dark version of
    // the light ones. Windows with their own colours (notes, status, warning banner) are left to the generic pass.
    const bool     dark = wxGetApp().dark_mode();
    const wxColour bg   = wizard_bg(dark), fg = wizard_fg(dark);
    // #2D2D31 / #B2B3B5: what StateColor's dark table turns the light white / navy into (fixed, whatever the mode).
    auto palette_bg = [](const wxColour& c) {
        return c == wizard_bg(false) || c == wizard_bg(true) || c == wxColour("#2D2D31");
    };
    auto palette_fg = [](const wxColour& c) {
        return c == wizard_fg(false) || c == wizard_fg(true) || c == wxColour("#B2B3B5");
    };
    std::function<void(wxWindow*)> recolor = [&](wxWindow* w) {
        if (palette_bg(w->GetBackgroundColour()))
            w->SetBackgroundColour(bg);
        if (palette_fg(w->GetForegroundColour()))
            w->SetForegroundColour(fg);
        for (wxWindow* child : w->GetChildren())
            recolor(child);
    };
    SetBackgroundColour(bg);
    SetForegroundColour(fg);
    for (wxWindow* child : GetChildren())
        recolor(child);
    Refresh();
}

void ForcaCalibrationWizard::on_tab_activated()
{
    // Warn first (before re-syncing the picker to whatever is active now), then re-sync WITHOUT the resume jump so
    // the user lands exactly where they left the tab (including a half-filled result page).
    check_run_presets();
    const bool on_result = m_book->GetSelection() == PAGE_RESULT;
    const bool on_start  = on_start_page();
    const Cal  before    = m_cal;
    refresh_presets(false);
    // A finished run (all six steps) starts a new calibration when the user comes back to the tab.
    // Only a run FINISHED in this visit (not merely a filament that is complete from before -- the user may be
    // re-running a step on it) sends the user to the Start page.
    if (on_start || (!on_result && m_run_finished))
        start_new_calibration();
    else if (!on_result || m_cal != before)
        show_gen_page();
    else
        relayout();

    if (!s_forca_intro_shown) {
        s_forca_intro_shown = true;
        // Parented to the main window so it opens centred on Forca, not on the wizard's left-hand panel.
        MessageDialog dlg(wxGetApp().mainframe,
            _L("Pick the filament you are calibrating -- or an existing profile close to it to start from -- "
               "then pick a calibration. All the steps in a run are saved into ONE filament preset that you "
               "name on the first step. The test appears on the plate on the right, and the progress bar and "
               "before/after table above it fill in as you go.\n\n"
               "Two ways to run each calibration:\n\n"
               "1) Slice & send to printer -- Forca slices the test and opens your printer's normal send "
               "dialog; print it, then enter the result here.\n\n"
               "2) Generate only -- Forca puts the test on the plate so you can slice or export it yourself "
               "(for example to an SD card), then come back here to enter your result.\n\n"
               "You can switch to other tabs at any time; this tab picks up where you left off."),
            _L("Forca Slicer Calibration Wizard"), wxICON_INFORMATION | wxOK);
        dlg.ShowModal();
    }
}

void ForcaCalibrationWizard::select_calibration(Cal cal)
{
    if (cal == Cal::Count)
        return;
    m_run_finished = false; // working on a step again (also via Start): returning to the tab resumes here
    set_cal(cal);
    if (cal == Cal::FlowRate) {
        m_flow_pass    = stored_flow_pass();
        m_flow_recheck = false;
    }
    update_status_label();
    update_default_preset_name();
    show_gen_page();
}

bool ForcaCalibrationWizard::on_start_page() const
{
    return m_book && m_book->GetSelection() == PAGE_START;
}

void ForcaCalibrationWizard::start_new_calibration()
{
    set_cal(Cal::Temperature);
    m_flow_pass    = 1;
    m_flow_recheck = false;
    hide_warning();
    update_status_label();
    update_start_note();
    m_book->ChangeSelection(PAGE_START); // no event (see show_gen_page)
    m_book->GetPage(PAGE_START)->Layout();
    relayout();
}

void ForcaCalibrationWizard::update_start_note()
{
    if (!m_start_note)
        return;
    const ProgressModel pm = progress_model();
    wxString note;
    if (pm.complete)
        note = _L("This filament already has a completed calibration -- its results are shown on the right. Pick a "
                  "different filament above to calibrate a new one, or press Start to re-run this one.");
    else if (pm.percent > 0)
        note = wxString::Format(_L("This filament has a calibration in progress (%d%% done). Start continues where it "
                                   "left off."), int(std::round(pm.percent)));
    m_start_note->SetLabel(hard_wrap(note, 60));
    m_start_note->Show(!note.IsEmpty());
}

void ForcaCalibrationWizard::on_start_run(wxCommandEvent& /*evt*/)
{
    // Continue at the first core step without a result or skip; a finished filament re-runs from Temperature.
    Cal first = Cal::Temperature;
    const ProgressModel pm = progress_model();
    if (!pm.complete)
        for (int i = 0; i < CORE_STEPS && i < int(pm.steps.size()); ++i)
            if (pm.steps[i].fraction < 1.0) {
                first = pm.steps[i].cal;
                break;
            }
    select_calibration(first);
}

void ForcaCalibrationWizard::start_flow_recheck()
{
    m_run_finished = false;
    set_cal(Cal::FlowRate);
    m_flow_pass    = 2; // the fine / Perfectionist pass of the chosen method
    m_flow_recheck = true;
    update_status_label();
    update_default_preset_name();
    show_gen_page();
}

void ForcaCalibrationWizard::relayout()
{
    // Re-entrancy guard: laying out the parent can re-enter via size/paint handlers; never recurse.
    static bool s_in_relayout = false;
    if (s_in_relayout) {
        return;
    }
    s_in_relayout = true;
    // Never narrower than the content: the panel only scrolls vertically, so extra width would be clipped
    // (e.g. the result page's Apply button at the right edge).
    if (wxSizer* s = GetSizer()) {
        const int need = s->GetMinSize().x + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
        if (GetMinSize().x < need)
            SetMinSize(wxSize(need, 1)); // height stays tiny: the panel scrolls (see ForcaCalibrationHost::apply_sizes)
    }
    Layout();
    FitInside(); // scrolled panel: update the virtual size instead of resizing the window
    if (wxWindow* parent = GetParent())
        parent->Layout();
    notify_state_changed();
    s_in_relayout = false;
}

void ForcaCalibrationWizard::notify_state_changed()
{
    if (m_on_state_changed)
        m_on_state_changed();
}

ForcaCalibrationWizard::Cal ForcaCalibrationWizard::next_after(Cal done)
{
    switch (done) {
    case Cal::Temperature:     return Cal::MaxVolSpeed;
    case Cal::MaxVolSpeed:     return Cal::FlowRate;
    case Cal::FlowRate:        return Cal::PressureAdvance;
    case Cal::PressureAdvance: return Cal::Retraction;
    case Cal::Retraction:      return Cal::Shrinkage;
    default:                   return Cal::FinalCheck;
    }
}

int ForcaCalibrationWizard::stored_flow_pass() const
{
    ForcaCalibrationStore::Key k = current_key();
    k.calibration = cal_id(Cal::FlowRate);
    ForcaCalibrationStore::Record r;
    return (m_store.get(k, r) && r.pass == 1) ? 2 : 1; // pass 1 done -> continue with pass 2
}

void ForcaCalibrationWizard::on_skip(wxCommandEvent& /*evt*/)
{
    if (m_cal == Cal::FinalCheck)
        return;
    MessageDialog dlg(this,
        wxString::Format(_L("Skip %s? Your current value is kept, and you can come back to it any time by clicking "
                            "it in the progress bar or picking it above."), cal_display()),
        cal_display(), wxICON_QUESTION | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES)
        return;

    // Tie the skip to the run's preset only when the selected preset already IS the run target, so a skip
    // can never make the user's own starting preset look like a calibration target.
    const ForcaCalibrationStore::Key k = current_key();
    const bool is_target = m_store.is_calibration_target(k.printer, k.nozzle, k.filament);
    m_store.set_skipped(k, is_target ? k.filament : std::string());

    const Cal next = next_after(m_cal);
    set_cal(next);
    m_flow_recheck = false;
    if (next == Cal::FlowRate)
        m_flow_pass = stored_flow_pass();
    update_status_label();
    update_default_preset_name();
    show_gen_page();
}

// ---- Preset-change warning (#7) ----------------------------------------------------------------

void ForcaCalibrationWizard::remember_run(const std::string& target)
{
    m_run_target = target;
    const ForcaCalibrationStore::Key k = current_key();
    m_run_printer = k.printer;
    m_run_nozzle  = k.nozzle;
}

void ForcaCalibrationWizard::check_run_presets()
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (!b || m_run_target.empty())
        return;
    const ForcaCalibrationStore::Key k = current_key();
    if (k.printer != m_run_printer || k.nozzle != m_run_nozzle) {
        show_warning(wxString::Format(_L("The printer or nozzle changed to %s (%s mm) since this run started on %s "
                                         "(%s mm). New results will start a separate run for this printer."),
                                      wxString::FromUTF8(k.printer.c_str()), wxString::FromUTF8(k.nozzle.c_str()),
                                      wxString::FromUTF8(m_run_printer.c_str()), wxString::FromUTF8(m_run_nozzle.c_str())),
                     wxEmptyString, nullptr);
        remember_run(std::string()); // warned once; a new run starts on this printer
        return;
    }
    const std::string active = b->filaments.get_selected_preset_name();
    if (active != m_run_target && b->filaments.find_preset(m_run_target)) {
        const std::string target = m_run_target;
        show_warning(wxString::Format(_L("The active filament changed to %s in another tab, but this run is calibrating "
                                         "%s."), wxString::FromUTF8(active.c_str()), wxString::FromUTF8(target.c_str())),
                     wxString::Format(_L("Switch back to %s"), wxString::FromUTF8(target.c_str())),
                     [this, target]() {
                         if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
                             tab->select_preset(target);
                         refresh_presets(false);
                         show_gen_page();
                     });
    }
}

void ForcaCalibrationWizard::show_warning(const wxString& text, const wxString& action_label, std::function<void()> action)
{
    m_warn_text->SetLabel(hard_wrap(text, 60));
    m_warn_fn = std::move(action);
    m_warn_action->SetLabel(action_label);
    m_warn_action->Show(!action_label.IsEmpty());
    m_warn_panel->Show();
    m_warn_panel->Layout();
    relayout();
}

void ForcaCalibrationWizard::hide_warning()
{
    m_warn_fn = nullptr;
    if (m_warn_panel->IsShown()) {
        m_warn_panel->Hide();
        relayout();
    }
}

// ---- Progress model (drawn by ForcaCalibrationProgressPanel) -----------------------------------

ForcaCalibrationWizard::ProgressModel ForcaCalibrationWizard::progress_model() const
{
    ProgressModel m;
    PresetBundle* b = wxGetApp().preset_bundle;
    if (!b)
        return m;

    const ForcaCalibrationStore::Key base = current_key();
    m.filament = wxString::FromUTF8(base.filament.c_str());
    m.printer  = wxString::FromUTF8(base.printer.c_str());

    auto record = [&](Cal cal, ForcaCalibrationStore::Record& r) {
        ForcaCalibrationStore::Key k = base;
        k.calibration = cal_id(cal);
        return m_store.get(k, r);
    };
    // A step has a saved value when it was ever applied (a later re-test sets it pending but keeps the value).
    auto has_value = [](const ForcaCalibrationStore::Record& r) { return r.status == "done" || !r.derived_preset.empty(); };

    static const char* names[] = { L("Temperature"), L("Max flow"), L("Flow"), L("Pressure adv."), L("Retraction"), L("Shrinkage") };
    double total = 0;
    for (int i = 0; i < CORE_STEPS; ++i) {
        const Cal cal = static_cast<Cal>(i);
        Step st;
        st.cal     = cal;
        st.name    = _(names[i]);
        st.current = (m_cal == cal);
        ForcaCalibrationStore::Record r;
        if (record(cal, r)) {
            st.pending = (r.status == "pending");
            st.skipped = (r.status == "skipped");
            if (st.skipped)
                st.fraction = 1.0;
            else if (cal == Cal::FlowRate && r.pass == 1)
                st.fraction = 0.5; // flow fills in two halves
            else if (has_value(r))
                st.fraction = 1.0;
        }
        total += st.fraction;
        m.steps.push_back(st);
    }
    // Optional markers (not counted): the flow recheck and the final check print.
    {
        Step recheck;
        recheck.cal      = Cal::FlowRate;
        recheck.name     = _L("Flow recheck");
        recheck.optional = true;
        ForcaCalibrationStore::Record r;
        recheck.fraction = (record(Cal::FlowRate, r) && r.pass >= 3) ? 1.0 : 0.0;
        recheck.current  = (m_cal == Cal::FlowRate && m_flow_recheck);
        m.steps.push_back(recheck);

        Step fc;
        fc.cal      = Cal::FinalCheck;
        fc.name     = _L("Final check");
        fc.optional = true;
        fc.fraction = record(Cal::FinalCheck, r) ? 1.0 : 0.0;
        fc.current  = (m_cal == Cal::FinalCheck);
        m.steps.push_back(fc);
    }
    m.percent  = 100.0 * total / CORE_STEPS;
    m.complete = total >= CORE_STEPS - 1e-6;

    // Before/after table. "Before" = the run's snapshot when it has one, else the current values (nothing
    // applied yet, so current == before). "After" = the preset's current value once that step has a result.
    const Preset* fp = b->filaments.find_preset(base.filament);
    ForcaCalibrationStore::Run run;
    const bool have_run = m_store.get_run(base.printer, base.nozzle, base.filament, run);
    for (const TableRowDef& d : table_rows()) {
        Row row;
        row.label = _(d.label);
        std::optional<double> before, after;
        if (have_run) {
            if (auto it = run.before.find(d.key); it != run.before.end())
                before = it->second;
        } else if (fp) {
            before = first_value(fp->config, d.key);
        }
        auto fmt = [&](const std::optional<double>& v) {
            if (v)
                return wxString::Format(d.fmt, *v);
            return wxString(std::string(d.key) == "filament_retraction_length" ? _L("printer default") : wxString("-"));
        };
        row.before = fmt(before);

        ForcaCalibrationStore::Record r;
        if (record(d.cal, r)) {
            row.date = wxString::FromUTF8(r.updated_at.c_str());
            if (r.status == "skipped") {
                row.state = Row::Skipped;
                row.after = _L("skipped");
            } else if (has_value(r) && fp) {
                row.state = (r.status == "pending") ? Row::Pending : Row::Done;
                after     = first_value(fp->config, d.key);
                row.after = fmt(after);
            } else if (r.status == "pending") {
                row.state = Row::Pending;
                row.after = _L("testing...");
            }
        }
        if (row.after.IsEmpty())
            row.after = "-";
        if (before && after && std::abs(*after - *before) > 1e-9) {
            row.changed = true;
            if (std::string(d.key) == "filament_flow_ratio" && *before != 0)
                row.delta = wxString::Format("%+.1f%%", 100.0 * (*after - *before) / *before);
            else {
                wxString f = wxString("%+") + wxString(d.fmt).Mid(1); // "%.1f mm3/s" -> "%+.1f mm3/s"
                row.delta  = wxString::Format(f, *after - *before);
            }
        } else if (!before && after) {
            row.changed = true;
        }
        m.rows.push_back(row);
    }
    return m;
}

}} // namespace Slic3r::GUI
