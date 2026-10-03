#include "ForcaPrinterCalibration.hpp"
#include "ForcaAcademy.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Tab.hpp" // input_shaper_types_for_flavor
#include "Widgets/Label.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/radiobut.h>
#include <wx/simplebook.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tokenzr.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace Slic3r { namespace GUI {

using Step = ForcaPrinterCalibration::Step;

namespace {

constexpr int PAGE_RESULT = static_cast<int>(Step::Count); // the result page follows the four test pages

// Every printer setting the track may write: backed up (serialized) before the first write to a preset.
const std::vector<std::string>& printer_keys()
{
    static const std::vector<std::string> keys = { "input_shaping_emit",   "input_shaping_type",   "input_shaping_freq_x",
                                                   "input_shaping_freq_y", "input_shaping_damp_x", "input_shaping_damp_y",
                                                   "machine_max_junction_deviation", "machine_max_jerk_x", "machine_max_jerk_y" };
    return keys;
}

struct PrinterInfo
{
    std::string                  preset;
    std::string                  nozzle; // "%.2f", as the filament track keys it
    GCodeFlavor                  flavor    = gcfMarlinLegacy;
    bool                         bbl       = false;
    bool                         klipper   = false;
    bool                         reprap    = false;
    bool                         uses_jd   = false; // Marlin 2 with junction deviation > 0 (Orca's cornering rule)
    bool                         is_system = true;  // system / default presets can't be updated in place
    std::vector<InputShaperType> shapers;           // Orca's list for this firmware, without "Disable"
};

PrinterInfo printer_info()
{
    PrinterInfo    pi;
    PresetBundle*  b = wxGetApp().preset_bundle;
    if (!b)
        return pi;
    const Preset& p = b->printers.get_edited_preset();
    pi.preset       = p.name;
    pi.is_system    = p.is_system || p.is_default;
    const DynamicPrintConfig& c = p.config;
    if (const auto* nd = c.option<ConfigOptionFloats>("nozzle_diameter"); nd && !nd->values.empty())
        pi.nozzle = wxString::Format("%.2f", nd->values.front()).ToStdString();
    if (const auto* f = c.option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor"))
        pi.flavor = f->value;
    pi.bbl     = b->is_bbl_vendor();
    pi.klipper = pi.flavor == gcfKlipper;
    pi.reprap  = pi.flavor == gcfRepRapFirmware;
    const auto* jd = c.option<ConfigOptionFloats>("machine_max_junction_deviation");
    pi.uses_jd = pi.flavor == gcfMarlinFirmware && jd && !jd->values.empty() && jd->values.front() > 0;
    for (InputShaperType t : input_shaper_types_for_flavor(pi.flavor))
        if (t != InputShaperType::Disable)
            pi.shapers.push_back(t);
    return pi;
}

bool num(const wxTextCtrl* t, double& v) { return t && t->GetValue().Trim().Trim(false).ToDouble(&v); }

wxString fmt_num(double v, int decimals) { return wxString::Format(wxString::Format("%%.%df", decimals), v); }

// A setting as text for the before/after table: the first value of a vector, else Orca's serialized form.
wxString setting_text(const DynamicPrintConfig& c, const std::string& key)
{
    const ConfigOption* o = c.option(key);
    if (!o)
        return "-";
    if (const auto* v = dynamic_cast<const ConfigOptionVectorBase*>(o)) {
        const std::vector<std::string> parts = v->vserialize();
        return parts.empty() ? wxString("-") : wxString::FromUTF8(parts.front().c_str());
    }
    return wxString::FromUTF8(o->serialize().c_str());
}

// The first value of a serialized vector ("8,8" -> "8"); a scalar stays as it is.
wxString first_of(const std::string& serialized)
{
    const size_t comma = serialized.find(',');
    return wxString::FromUTF8((comma == std::string::npos ? serialized : serialized.substr(0, comma)).c_str());
}

std::string shaper_value(InputShaperType t)
{
    const ConfigOptionDef* def = print_config_def.get("input_shaping_type");
    const size_t           i   = static_cast<size_t>(t);
    return def && i < def->enum_values.size() ? def->enum_values[i] : std::to_string(int(t));
}

wxString shaper_label(InputShaperType t)
{
    const ConfigOptionDef* def = print_config_def.get("input_shaping_type");
    const size_t           i   = static_cast<size_t>(t);
    return def && i < def->enum_labels.size() ? _(def->enum_labels[i]) : wxString::FromUTF8(shaper_value(t).c_str());
}

bool shaper_from_value(const std::string& value, InputShaperType& out)
{
    const ConfigOptionDef* def = print_config_def.get("input_shaping_type");
    if (!def)
        return false;
    for (size_t i = 0; i < def->enum_values.size(); ++i)
        if (def->enum_values[i] == value) {
            out = static_cast<InputShaperType>(i);
            return true;
        }
    return false;
}

// value = start + (height / tower height) x (end - start), clamped to the tested range. Orca steps these tests
// linearly with the layer index (GCode::interpolate_value_across_layers), so height is a close stand-in.
double from_height(double h, double tower_h, double start, double end)
{
    const double r = tower_h > 0 ? std::clamp(h / tower_h, 0.0, 1.0) : 0.0;
    return start + r * (end - start);
}

wxColour note_colour() { return wxColour("#6B6B6B"); }

} // namespace

// ---- Construction -----------------------------------------------------------------------------------------------

ForcaPrinterCalibration::ForcaPrinterCalibration(wxWindow* parent, Plater* plater, ForcaCalibrationStore& store,
                                                 std::function<void()> relayout, std::function<void()> back_to_start)
    : wxPanel(parent, wxID_ANY)
    , m_plater(plater)
    , m_store(store)
    , m_relayout(std::move(relayout))
    , m_back_to_start(std::move(back_to_start))
{
    auto* root = new wxBoxSizer(wxVERTICAL);
    auto* box  = new wxStaticBoxSizer(new wxStaticBox(this, wxID_ANY, _L("Printer calibration")), wxVERTICAL);

    auto* intro = new wxStaticText(this, wxID_ANY, forca_hard_wrap(
        _L("These calibrations tune the printer itself, once per printer and nozzle. Do them before calibrating "
           "filaments: they change how the filament tests print. Order: Input Shaping (frequency, then damping), "
           "Cornering, VFA. Steps your printer can't use are left out."), 60));
    intro->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(intro, 0, wxALL, FromDIP(6));

    m_status = new wxStaticText(this, wxID_ANY, "");
    box->Add(m_status, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    m_na_text = new wxStaticText(this, wxID_ANY, "");
    m_na_text->SetForegroundColour(wxColour("#FF6F00"));
    m_na_text->Hide();
    box->Add(m_na_text, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
    root->Add(box, 0, wxEXPAND);

    m_book = new wxSimplebook(this, wxID_ANY);
    m_book->AddPage(build_freq_page(m_book), _L("Shaper frequency"));
    m_book->AddPage(build_damp_page(m_book), _L("Shaper damping"));
    m_book->AddPage(build_cornering_page(m_book), _L("Cornering"));
    m_book->AddPage(build_vfa_page(m_book), _L("VFA"));
    m_book->AddPage(build_result_page(m_book), _L("Result")); // PAGE_RESULT
    root->Add(m_book, 0, wxTOP | wxEXPAND, FromDIP(6));

    auto* bottom = new wxBoxSizer(wxHORIZONTAL);
    auto* back   = new wxButton(this, wxID_ANY, _L("Calibrate something else"));
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_back_to_start) m_back_to_start(); });
    bottom->Add(back, 0, wxRIGHT, FromDIP(8));
    m_restore_btn = new wxButton(this, wxID_ANY, _L("Restore this printer's backup"));
    m_restore_btn->SetToolTip(_L("Put back the printer settings this printer preset had before the wizard first "
                                 "updated it."));
    m_restore_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_restore(); });
    bottom->Add(m_restore_btn, 0);
    root->Add(bottom, 0, wxTOP, FromDIP(8));

    SetSizer(root);
    refresh();
}

wxTextCtrl* ForcaPrinterCalibration::add_number(wxWindow* parent, wxSizer* row, const wxString& label, const wxString& value)
{
    row->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    auto* t = new wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition, FromDIP(wxSize(64, -1)));
    row->Add(t, 0, wxRIGHT, FromDIP(12));
    return t;
}

void ForcaPrinterCalibration::add_run_buttons(wxWindow* panel, wxSizer* box, Step step)
{
    auto* send = new wxButton(panel, wxID_ANY, _L("Slice && send to printer"));
    send->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { launch(step, true); });
    box->Add(send, 0, wxALL, FromDIP(6));
    auto* gen = new wxButton(panel, wxID_ANY, _L("Generate only (I will print it myself)"));
    gen->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { launch(step, false); });
    box->Add(gen, 0, wxALL, FromDIP(6));

    auto* row    = new wxBoxSizer(wxHORIZONTAL);
    auto* result = new wxButton(panel, wxID_ANY, _L("I have already printed it - enter result"));
    result->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { show_result(step); });
    row->Add(result, 0, wxRIGHT, FromDIP(8));
    auto* skip = new wxButton(panel, wxID_ANY, _L("Skip this step"));
    skip->SetToolTip(_L("Keep the printer's current setting and move on to the next calibration."));
    skip->Bind(wxEVT_BUTTON, [this, step](wxCommandEvent&) { on_skip(step); });
    row->Add(skip, 0);
    box->Add(row, 0, wxALL, FromDIP(6));
}

wxPanel* ForcaPrinterCalibration::build_freq_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Input Shaping - frequency")), wxVERTICAL);
    auto* coach = new wxStaticText(panel, wxID_ANY, forca_hard_wrap(
        _L("Orca's ringing tower raises the input shaper frequency with height (X and Y can use their own ranges). "
           "After it prints, find the height on the X face and on the Y face where the ringing -- the echo "
           "after corners and letters -- is weakest. The result page turns each height into a frequency."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    auto* setup = new wxBoxSizer(wxHORIZONTAL);
    setup->Add(new wxStaticText(panel, wxID_ANY, _L("Test model:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_fq_model = new wxChoice(panel, wxID_ANY);
    m_fq_model->Append(_L("Ringing Tower"));
    m_fq_model->Append(_L("Fast Tower"));
    m_fq_model->SetSelection(0);
    setup->Add(m_fq_model, 0, wxRIGHT, FromDIP(12));
    setup->Add(new wxStaticText(panel, wxID_ANY, _L("Shaper:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_fq_type = new wxChoice(panel, wxID_ANY);
    setup->Add(m_fq_type, 0);
    box->Add(setup, 0, wxALL, FromDIP(6));

    // Orca's Input_Shaping_Freq_Test_Dlg defaults: 15-110 Hz on both axes, damping 0.15.
    auto* xrow = new wxBoxSizer(wxHORIZONTAL);
    m_fq_x0 = add_number(panel, xrow, _L("X start (Hz):"), "15");
    m_fq_x1 = add_number(panel, xrow, _L("X end:"), "110");
    box->Add(xrow, 0, wxALL, FromDIP(6));
    m_fq_y_row = new wxBoxSizer(wxHORIZONTAL);
    m_fq_y0 = add_number(panel, m_fq_y_row, _L("Y start (Hz):"), "15");
    m_fq_y1 = add_number(panel, m_fq_y_row, _L("Y end:"), "110");
    box->Add(m_fq_y_row, 0, wxALL, FromDIP(6));
    auto* drow = new wxBoxSizer(wxHORIZONTAL);
    m_fq_damp  = add_number(panel, drow, _L("Damping during the test:"), "0.15");
    box->Add(drow, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, Step::ShaperFreq);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaPrinterCalibration::build_damp_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Input Shaping - damping")), wxVERTICAL);
    auto* coach = new wxStaticText(panel, wxID_ANY, forca_hard_wrap(
        _L("With your frequencies set, this tower raises the damping ratio with height. Find the height where the "
           "ringing is weakest; the result page turns it into the damping ratio. The frequencies below come from "
           "the frequency step."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    auto* setup = new wxBoxSizer(wxHORIZONTAL);
    setup->Add(new wxStaticText(panel, wxID_ANY, _L("Test model:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_dp_model = new wxChoice(panel, wxID_ANY);
    m_dp_model->Append(_L("Ringing Tower"));
    m_dp_model->Append(_L("Fast Tower"));
    m_dp_model->SetSelection(0);
    setup->Add(m_dp_model, 0, wxRIGHT, FromDIP(12));
    setup->Add(new wxStaticText(panel, wxID_ANY, _L("Shaper:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_dp_type = new wxChoice(panel, wxID_ANY);
    setup->Add(m_dp_type, 0);
    box->Add(setup, 0, wxALL, FromDIP(6));

    // Orca's Input_Shaping_Damp_Test_Dlg defaults: 30 Hz, damping 0.00 - 0.40.
    auto* frow = new wxBoxSizer(wxHORIZONTAL);
    m_dp_fx = add_number(panel, frow, _L("Frequency X (Hz):"), "30");
    m_dp_fy = add_number(panel, frow, _L("Y:"), "30");
    box->Add(frow, 0, wxALL, FromDIP(6));
    auto* drow = new wxBoxSizer(wxHORIZONTAL);
    m_dp_d0 = add_number(panel, drow, _L("Damping start:"), "0.00");
    m_dp_d1 = add_number(panel, drow, _L("End:"), "0.40");
    box->Add(drow, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, Step::ShaperDamp);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaPrinterCalibration::build_cornering_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("Cornering")), wxVERTICAL);
    auto* coach = new wxStaticText(panel, wxID_ANY, forca_hard_wrap(
        _L("This tower raises the cornering speed (jerk, junction deviation or Klipper's square corner velocity) "
           "with height. Higher is faster but rounds and shakes the corners. Find the highest point where the "
           "corners are still sharp and clean; the result page turns that height into the value."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    auto* setup = new wxBoxSizer(wxHORIZONTAL);
    setup->Add(new wxStaticText(panel, wxID_ANY, _L("Test model:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_cn_model = new wxChoice(panel, wxID_ANY);
    m_cn_model->Append(_L("Ringing Tower"));
    m_cn_model->Append(_L("Fast Tower"));
    m_cn_model->Append(_L("SCV-V2"));
    m_cn_model->SetSelection(0);
    setup->Add(m_cn_model, 0);
    box->Add(setup, 0, wxALL, FromDIP(6));

    auto* row  = new wxBoxSizer(wxHORIZONTAL);
    m_cn_start = add_number(panel, row, _L("Start:"), "1");
    m_cn_end   = add_number(panel, row, _L("End:"), "15");
    box->Add(row, 0, wxALL, FromDIP(6));
    m_cn_note = new wxStaticText(panel, wxID_ANY, "");
    m_cn_note->SetForegroundColour(note_colour());
    box->Add(m_cn_note, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, Step::Cornering);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaPrinterCalibration::build_vfa_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* box   = new wxStaticBoxSizer(new wxStaticBox(panel, wxID_ANY, _L("VFA (vertical fine artifacts)")), wxVERTICAL);
    auto* coach = new wxStaticText(panel, wxID_ANY, forca_hard_wrap(
        _L("This tower steps the outer wall speed up block by block. Fine vertical ribbing (VFA) shows up at "
           "certain speeds, from motor and belt resonance. Note the blocks where you see it (count from the "
           "bottom, the first block is 1); the result page tells you which speeds to keep your walls away from. "
           "Keep End within what your filament can extrude."), 60));
    coach->SetMinSize(FromDIP(wxSize(430, -1)));
    box->Add(coach, 0, wxALL, FromDIP(6));

    // Orca's VFA_Test_Dlg defaults: 40 - 200 mm/s, step 10.
    auto* row   = new wxBoxSizer(wxHORIZONTAL);
    m_vfa_start = add_number(panel, row, _L("Start (mm/s):"), "40");
    m_vfa_end   = add_number(panel, row, _L("End:"), "200");
    m_vfa_step  = add_number(panel, row, _L("Step:"), "10");
    box->Add(row, 0, wxALL, FromDIP(6));

    add_run_buttons(panel, box, Step::VFA);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(box, 0, wxEXPAND);
    panel->SetSizerAndFit(outer);
    return panel;
}

wxPanel* ForcaPrinterCalibration::build_result_page(wxWindow* parent)
{
    auto* panel = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_res_title = new wxStaticText(panel, wxID_ANY, _L("Enter result"));
    m_res_title->SetFont(Label::Head_14);
    sizer->Add(m_res_title, 0, wxBOTTOM, FromDIP(8));
    m_res_hint = new wxStaticText(panel, wxID_ANY, "");
    m_res_hint->SetForegroundColour(note_colour());
    m_res_hint->SetMinSize(FromDIP(wxSize(430, -1)));
    sizer->Add(m_res_hint, 0, wxBOTTOM, FromDIP(8));

    auto live = [this](wxCommandEvent&) { update_result_values(); };
    auto height_row = [&](wxWindow* p, const wxString& hl, wxTextCtrl*& h, const wxString& vl, wxTextCtrl*& v) {
        auto* r = new wxBoxSizer(wxHORIZONTAL);
        r->Add(new wxStaticText(p, wxID_ANY, hl, wxDefaultPosition, FromDIP(wxSize(170, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        h = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(64, -1)));
        h->Bind(wxEVT_TEXT, live);
        r->Add(h, 0, wxRIGHT, FromDIP(12));
        r->Add(new wxStaticText(p, wxID_ANY, vl), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        v = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(80, -1)));
        v->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_klipper_text(); });
        r->Add(v, 0);
        return r;
    };

    // Frequency: one height -> frequency per axis (RepRap: one frequency for both).
    m_res_freq = new wxPanel(panel);
    auto* fs   = new wxBoxSizer(wxVERTICAL);
    fs->Add(height_row(m_res_freq, _L("Best height, X face (mm):"), m_res_hx, _L("Frequency X (Hz):"), m_res_fx), 0, wxBOTTOM, FromDIP(4));
    m_res_y_row = height_row(m_res_freq, _L("Best height, Y face (mm):"), m_res_hy, _L("Frequency Y (Hz):"), m_res_fy);
    fs->Add(m_res_y_row, 0, wxBOTTOM, FromDIP(4));
    m_res_freq->SetSizer(fs);
    sizer->Add(m_res_freq, 0, wxBOTTOM, FromDIP(6));

    // Damping / cornering: one height -> one value.
    m_res_single = new wxPanel(panel);
    auto* ss     = new wxBoxSizer(wxVERTICAL);
    auto* sr     = new wxBoxSizer(wxHORIZONTAL);
    sr->Add(new wxStaticText(m_res_single, wxID_ANY, _L("Best height (mm):"), wxDefaultPosition, FromDIP(wxSize(170, -1))), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_res_h = new wxTextCtrl(m_res_single, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(64, -1)));
    m_res_h->Bind(wxEVT_TEXT, live);
    sr->Add(m_res_h, 0, wxRIGHT, FromDIP(12));
    m_res_single_label = new wxStaticText(m_res_single, wxID_ANY, _L("Value:"));
    sr->Add(m_res_single_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_res_v = new wxTextCtrl(m_res_single, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(80, -1)));
    m_res_v->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { update_klipper_text(); });
    sr->Add(m_res_v, 0);
    ss->Add(sr, 0);
    m_res_single->SetSizer(ss);
    sizer->Add(m_res_single, 0, wxBOTTOM, FromDIP(6));

    // VFA: block numbers -> speeds to avoid.
    m_res_vfa = new wxPanel(panel);
    auto* vs  = new wxBoxSizer(wxVERTICAL);
    auto* vr  = new wxBoxSizer(wxHORIZONTAL);
    vr->Add(new wxStaticText(m_res_vfa, wxID_ANY, _L("Blocks with ribbing (e.g. 5, 6):")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    m_res_blocks = new wxTextCtrl(m_res_vfa, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(140, -1)));
    m_res_blocks->Bind(wxEVT_TEXT, live);
    vr->Add(m_res_blocks, 0);
    vs->Add(vr, 0, wxBOTTOM, FromDIP(4));
    m_res_speeds = new wxStaticText(m_res_vfa, wxID_ANY, "");
    vs->Add(m_res_speeds, 0);
    m_res_vfa->SetSizer(vs);
    sizer->Add(m_res_vfa, 0, wxBOTTOM, FromDIP(6));

    m_emit = new wxCheckBox(panel, wxID_ANY, _L("Let Forca set input shaping at the start of each print"));
    m_emit->SetToolTip(_L("Writes the shaper settings into the G-code (Orca's \"Emit input shaping\"), overriding "
                          "what the printer's firmware has stored. Leave it off if you keep them in the firmware "
                          "(for example Klipper's printer.cfg)."));
    m_emit->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_target->Show(m_emit->GetValue()); m_book->GetPage(PAGE_RESULT)->Layout(); if (m_relayout) m_relayout(); });
    sizer->Add(m_emit, 0, wxBOTTOM, FromDIP(8));

    // Klipper keeps these in printer.cfg: the lines to paste.
    m_klipper = new wxPanel(panel);
    auto* ks  = new wxBoxSizer(wxVERTICAL);
    auto* kn  = new wxStaticText(m_klipper, wxID_ANY, forca_hard_wrap(_L("Klipper keeps these in printer.cfg. Add or "
                                                                          "update these lines there, then restart "
                                                                          "Klipper:"), 60));
    kn->SetForegroundColour(note_colour());
    ks->Add(kn, 0, wxBOTTOM, FromDIP(4));
    m_klipper_text = new wxTextCtrl(m_klipper, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(360, 96)), wxTE_MULTILINE | wxTE_READONLY);
    ks->Add(m_klipper_text, 0, wxBOTTOM, FromDIP(4));
    auto* copy = new wxButton(m_klipper, wxID_ANY, _L("Copy"));
    copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(m_klipper_text->GetValue()));
            wxTheClipboard->Close();
        }
    });
    ks->Add(copy, 0);
    m_klipper->SetSizer(ks);
    sizer->Add(m_klipper, 0, wxBOTTOM, FromDIP(8));

    // Where the result goes (the user's choice, PLAN_machine_calibration D2).
    m_target  = new wxPanel(panel);
    auto* ts  = new wxBoxSizer(wxVERTICAL);
    m_tgt_update = new wxRadioButton(m_target, wxID_ANY, _L("Update this printer preset (the old values are backed up)"),
                                     wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    ts->Add(m_tgt_update, 0, wxBOTTOM, FromDIP(4));
    auto* cr = new wxBoxSizer(wxHORIZONTAL);
    m_tgt_copy = new wxRadioButton(m_target, wxID_ANY, _L("Save as a new printer preset:"));
    cr->Add(m_tgt_copy, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_tgt_name = new wxTextCtrl(m_target, wxID_ANY, "");
    cr->Add(m_tgt_name, 1);
    ts->Add(cr, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    m_tgt_note = new wxStaticText(m_target, wxID_ANY, "");
    m_tgt_note->SetForegroundColour(note_colour());
    ts->Add(m_tgt_note, 0);
    m_target->SetSizer(ts);
    sizer->Add(m_target, 0, wxEXPAND | wxBOTTOM, FromDIP(10));

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* back    = new wxButton(panel, wxID_ANY, _L("Back"));
    back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_step(m_step); });
    auto* apply = new wxButton(panel, wxID_ANY, _L("Apply"));
    apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_apply(); });
    buttons->Add(back, 0, wxRIGHT, FromDIP(8));
    buttons->AddStretchSpacer(1);
    buttons->Add(apply, 0);
    sizer->Add(buttons, 0, wxEXPAND);

    panel->SetSizerAndFit(sizer);
    return panel;
}

// ---- State ------------------------------------------------------------------------------------------------------

std::string ForcaPrinterCalibration::step_id(Step step)
{
    switch (step) { // persisted store ids -- never rename one
    case Step::ShaperFreq: return "printer_shaper_freq";
    case Step::ShaperDamp: return "printer_shaper_damp";
    case Step::Cornering:  return "printer_cornering";
    default:               return "printer_vfa";
    }
}

wxString ForcaPrinterCalibration::step_name(Step step) const
{
    switch (step) {
    case Step::ShaperFreq: return _L("Input Shaping - frequency");
    case Step::ShaperDamp: return _L("Input Shaping - damping");
    case Step::Cornering:  return _L("Cornering");
    default:               return _L("VFA");
    }
}

ForcaCalibrationStore::Key ForcaPrinterCalibration::key(Step step, const std::string& printer) const
{
    const PrinterInfo pi = printer_info();
    ForcaCalibrationStore::Key k;
    k.printer     = printer.empty() ? pi.preset : printer;
    k.nozzle      = pi.nozzle;
    k.filament    = std::string(); // a printer calibration belongs to no filament
    k.calibration = step_id(step);
    return k;
}

bool ForcaPrinterCalibration::record(Step step, ForcaCalibrationStore::Record& out) const
{
    return m_store.get(key(step), out);
}

bool ForcaPrinterCalibration::has_result(const ForcaCalibrationStore::Record& r)
{
    return r.status == "done" || r.values.count("rv") || r.values.count("rx") || !r.note.empty();
}

bool ForcaPrinterCalibration::applies(Step step, wxString* why) const
{
    const PrinterInfo pi = printer_info();
    auto no = [why](const wxString& reason) {
        if (why)
            *why = reason;
        return false;
    };
    switch (step) {
    case Step::ShaperFreq:
    case Step::ShaperDamp:
        if (pi.bbl)
            return no(_L("Bambu Lab printers tune their own vibration compensation (input shaping), so there is "
                         "nothing to calibrate here."));
        if (pi.shapers.empty())
            return no(_L("Orca does not set input shaping for this printer's G-code flavor."));
        return true;
    case Step::Cornering:
        if (pi.bbl)
            return no(_L("Bambu Lab printers manage cornering (jerk) in their own firmware."));
        return true;
    default:
        return true;
    }
}

ForcaPrinterCalibration::Step ForcaPrinterCalibration::next_step(Step after) const
{
    for (int i = static_cast<int>(after) + 1; i < static_cast<int>(Step::Count); ++i)
        if (applies(static_cast<Step>(i)))
            return static_cast<Step>(i);
    return Step::Count;
}

void ForcaPrinterCalibration::completion(int& done, int& total) const
{
    done = total = 0;
    for (int i = 0; i < static_cast<int>(Step::Count); ++i) {
        const Step s = static_cast<Step>(i);
        if (!applies(s))
            continue;
        ++total;
        ForcaCalibrationStore::Record r;
        if (record(s, r) && (r.status == "skipped" || has_result(r)))
            ++done;
    }
}

int ForcaPrinterCalibration::pending_step() const
{
    for (int i = 0; i < static_cast<int>(Step::Count); ++i) {
        ForcaCalibrationStore::Record r;
        if (applies(static_cast<Step>(i)) && record(static_cast<Step>(i), r) && r.status == "pending")
            return i;
    }
    return -1;
}

void ForcaPrinterCalibration::seed_for_printer()
{
    const PrinterInfo pi = printer_info();

    // Shaper type lists: Orca's list for this firmware; the preset's current type is preselected.
    InputShaperType current = InputShaperType::Default;
    if (PresetBundle* b = wxGetApp().preset_bundle)
        if (const auto* o = b->printers.get_edited_preset().config.option<ConfigOptionEnum<InputShaperType>>("input_shaping_type"))
            current = o->value;
    m_shaper_values.clear();
    m_fq_type->Clear();
    m_dp_type->Clear();
    int sel = 0;
    for (size_t i = 0; i < pi.shapers.size(); ++i) {
        m_shaper_values.push_back(shaper_value(pi.shapers[i]));
        m_fq_type->Append(shaper_label(pi.shapers[i]));
        m_dp_type->Append(shaper_label(pi.shapers[i]));
        if (pi.shapers[i] == current)
            sel = static_cast<int>(i);
    }
    if (!pi.shapers.empty()) {
        m_fq_type->SetSelection(sel);
        m_dp_type->SetSelection(sel);
    }

    // RepRap uses one frequency for both axes (as Orca's dialogs do).
    m_fq_y_row->ShowItems(!pi.reprap);
    m_dp_fy->Show(!pi.reprap);

    if (m_seeded_for != pi.preset) {
        m_seeded_for = pi.preset;
        // Cornering: Orca's Cornering_Test_Dlg defaults -- junction deviation 0 - 0.25 mm, else jerk 1 - 15 mm/s.
        m_cn_start->SetValue(pi.uses_jd ? "0.000" : "1");
        m_cn_end->SetValue(pi.uses_jd ? "0.250" : "15");
    }
    wxString note = _L("Lower values give sharper corners but slower printing.");
    if (pi.uses_jd)
        note += "\n" + _L("Marlin junction deviation (mm), because this printer preset has one set.");
    else if (pi.klipper)
        note += "\n" + _L("Klipper: tested as square corner velocity (mm/s).");
    else if (pi.reprap)
        note += "\n" + _L("RepRapFirmware: jerk in mm/s (Orca converts it where needed).");
    else
        note += "\n" + _L("Classic jerk (mm/s).");
    m_cn_note->SetLabel(forca_hard_wrap(note, 60));

    // The damping test starts from the frequencies found in the frequency step.
    ForcaCalibrationStore::Record r;
    if (record(Step::ShaperFreq, r)) {
        if (auto it = r.values.find("rx"); it != r.values.end())
            m_dp_fx->SetValue(fmt_num(it->second, 1));
        if (auto it = r.values.find("ry"); it != r.values.end())
            m_dp_fy->SetValue(fmt_num(it->second, 1));
        if (!r.note.empty())
            for (size_t i = 0; i < m_shaper_values.size(); ++i)
                if (m_shaper_values[i] == r.note)
                    m_dp_type->SetSelection(static_cast<int>(i));
    }
}

void ForcaPrinterCalibration::refresh()
{
    seed_for_printer();
    if (!applies(m_step)) {
        const Step first = next_step(static_cast<Step>(-1));
        if (first != Step::Count)
            m_step = first;
    }
    ForcaCalibrationStore::Run run;
    const PrinterInfo pi = printer_info();
    m_restore_btn->Show(m_store.get_printer_run(pi.preset, run) && run.base == run.target);
    update_status();
    if (m_book->GetSelection() != PAGE_RESULT)
        show_step(m_step);
    else if (m_relayout)
        m_relayout();
}

void ForcaPrinterCalibration::begin()
{
    Step first = Step::Count;
    for (int i = 0; i < static_cast<int>(Step::Count) && first == Step::Count; ++i) {
        const Step s = static_cast<Step>(i);
        ForcaCalibrationStore::Record r;
        if (applies(s) && !(record(s, r) && (r.status == "skipped" || has_result(r))))
            first = s;
    }
    if (first == Step::Count) // everything done: start over at the first applicable step
        first = next_step(static_cast<Step>(-1));
    seed_for_printer();
    show_step(first == Step::Count ? Step::VFA : first);
}

int ForcaPrinterCalibration::current_page() const
{
    return m_book ? m_book->GetSelection() : -1;
}

void ForcaPrinterCalibration::select_step(Step step)
{
    if (step == Step::Count)
        return;
    seed_for_printer();
    show_step(step);
}

void ForcaPrinterCalibration::show_step(Step step)
{
    m_step = step;
    wxString why;
    const bool ok = applies(step, &why);
    m_na_text->SetLabel(ok ? wxString() : forca_hard_wrap(_L("This step does not apply to the selected printer: ") + why, 60));
    m_na_text->Show(!ok);
    update_status();
    // ChangeSelection, not SetSelection: no page-changed event (it would bubble up to MainFrame's tab handler).
    m_book->ChangeSelection(static_cast<int>(step));
    Layout();
    if (m_relayout)
        m_relayout();
}

void ForcaPrinterCalibration::update_status()
{
    ForcaCalibrationStore::Record r;
    if (!record(m_step, r)) {
        m_status->SetLabel(wxString::Format(_L("Step: %s."), step_name(m_step)));
        m_status->SetForegroundColour(note_colour());
        return;
    }
    const wxString date = wxString::FromUTF8(r.updated_at.c_str());
    if (r.status == "pending") {
        m_status->SetForegroundColour(wxColour("#FF6F00"));
        m_status->SetLabel(wxString::Format(_L("%s: test generated on %s - print it, then enter the result."), step_name(m_step), date));
    } else if (r.status == "skipped") {
        m_status->SetForegroundColour(note_colour());
        m_status->SetLabel(wxString::Format(_L("%s was skipped (%s); the printer's setting is kept."), step_name(m_step), date));
    } else {
        m_status->SetForegroundColour(note_colour());
        m_status->SetLabel(wxString::Format(_L("%s: last result saved on %s."), step_name(m_step), date));
    }
}

// ---- Running a test ---------------------------------------------------------------------------------------------

bool ForcaPrinterCalibration::build_params(Step step, Calib_Params& p, wxString& err)
{
    const PrinterInfo pi = printer_info();
    auto shaper = [&](wxChoice* c) -> std::string {
        const int i = c->GetSelection();
        return (i >= 0 && i < int(m_shaper_values.size())) ? m_shaper_values[size_t(i)] : std::string();
    };
    switch (step) {
    case Step::ShaperFreq: {
        double damp = 0;
        if (!num(m_fq_x0, p.freqStartX) || !num(m_fq_x1, p.freqEndX) || !num(m_fq_damp, damp) ||
            (!pi.reprap && (!num(m_fq_y0, p.freqStartY) || !num(m_fq_y1, p.freqEndY)))) {
            err = _L("Please enter numbers for the frequency range and the damping.");
            return false;
        }
        if (pi.reprap) {
            p.freqStartY = p.freqStartX;
            p.freqEndY   = p.freqEndX;
        }
        if (p.freqStartX < 0 || p.freqEndX > 500 || p.freqStartY < 0 || p.freqEndY > 500 || p.freqStartX >= p.freqEndX ||
            p.freqStartY >= p.freqEndY) {
            err = _L("Please enter valid frequencies: 0 <= start < end <= 500 Hz.");
            return false;
        }
        if (damp < 0 || damp >= 1) {
            err = _L("Please enter a damping ratio from 0 up to (not including) 1.");
            return false;
        }
        p.start       = damp;
        p.shaper_type = shaper(m_fq_type);
        p.test_model  = m_fq_model->GetSelection() == 0 ? 0 : 1;
        p.mode        = CalibMode::Calib_Input_shaping_freq;
        return true;
    }
    case Step::ShaperDamp: {
        if (!num(m_dp_fx, p.freqStartX) || (!pi.reprap && !num(m_dp_fy, p.freqStartY)) || !num(m_dp_d0, p.start) ||
            !num(m_dp_d1, p.end)) {
            err = _L("Please enter numbers for the frequencies and the damping range.");
            return false;
        }
        if (pi.reprap)
            p.freqStartY = p.freqStartX;
        if (p.freqStartX <= 0 || p.freqStartX > 500 || p.freqStartY <= 0 || p.freqStartY > 500) {
            err = _L("Please enter valid frequencies: 0 < frequency <= 500 Hz.");
            return false;
        }
        if (p.start < 0 || p.end > 1 || p.start >= p.end) {
            err = _L("Please enter a valid damping range: 0 <= start < end <= 1.");
            return false;
        }
        p.shaper_type = shaper(m_dp_type);
        p.test_model  = m_dp_model->GetSelection() == 0 ? 0 : 1;
        p.mode        = CalibMode::Calib_Input_shaping_damp;
        return true;
    }
    case Step::Cornering: {
        if (!num(m_cn_start, p.start) || !num(m_cn_end, p.end)) {
            err = _L("Please enter numbers for the start and end.");
            return false;
        }
        // Orca's Cornering_Test_Dlg limits: junction deviation <= 0.3 mm (warn above 0.25), else <= 100 (warn above 20).
        const double max_end = pi.uses_jd ? 0.3 : 100.0;
        const double warn    = pi.uses_jd ? 0.25 : 20.0;
        if (p.start < 0 || p.end > max_end || p.start >= p.end) {
            err = wxString::Format(_L("Please enter a valid range: 0 <= start < end <= %s."), fmt_num(max_end, pi.uses_jd ? 3 : 0));
            return false;
        }
        if (p.end > warn) {
            MessageDialog dlg(this, wxString::Format(_L("High values (above %s) can cause layer shifts. Continue?"),
                                                     fmt_num(warn, pi.uses_jd ? 2 : 0)),
                              step_name(step), wxICON_WARNING | wxYES_NO);
            if (dlg.ShowModal() != wxID_YES)
                return false;
        }
        p.test_model = m_cn_model->GetSelection();
        p.mode       = CalibMode::Calib_Cornering;
        return true;
    }
    default: {
        if (!num(m_vfa_start, p.start) || !num(m_vfa_end, p.end) || !num(m_vfa_step, p.step)) {
            err = _L("Please enter numbers for start, end and step.");
            return false;
        }
        // Orca's VFA_Test_Dlg rule.
        if (p.start <= 10 || p.step <= 0 || p.end < p.start + p.step) {
            err = _L("Please enter valid speeds: start above 10, step above 0, end at least start + step.");
            return false;
        }
        p.vfa_layer_height    = 0.0;  // auto: nozzle / 2
        p.nozzle_based_resize = true; // Orca's default ("Auto-scale for nozzle")
        p.mode                = CalibMode::Calib_VFA_Tower;
        return true;
    }
    }
}

void ForcaPrinterCalibration::launch(Step step, bool send_after)
{
    wxString why;
    if (!applies(step, &why)) {
        MessageDialog dlg(this, why, step_name(step), wxICON_INFORMATION | wxOK);
        dlg.ShowModal();
        return;
    }
    Calib_Params p;
    wxString     err;
    if (!build_params(step, p, err)) {
        if (!err.IsEmpty()) {
            MessageDialog dlg(this, err, step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
        }
        return;
    }

    // Same rules as the filament track: drop the previous test's throwaway preset edits, and only the session's
    // first test asks before replacing the plate (after it, the plate holds test setup only).
    if (forca_calib_test_generated())
        forca_discard_transient_preset_changes();
    const bool skip_confirm = forca_calib_test_generated();
    forca_mark_calib_test_generated();

    m_launched = p; // long-lived copy
    switch (step) {
    case Step::ShaperFreq: m_plater->calib_input_shaping_freq(m_launched, skip_confirm); break;
    case Step::ShaperDamp: m_plater->calib_input_shaping_damp(m_launched, skip_confirm); break;
    case Step::Cornering:  m_plater->Calib_Cornering(m_launched, skip_confirm); break;
    default:               m_plater->calib_VFA(m_launched, skip_confirm); break;
    }

    // Pending record: the test's range and the tower height (for the height -> value helper). A previous result
    // stays (a re-test keeps the saved value until a new one is applied).
    double tower_h = 0;
    for (const ModelObject* o : m_plater->model().objects)
        for (size_t i = 0; i < o->instances.size(); ++i)
            tower_h = std::max(tower_h, o->instance_bounding_box(i).size().z());
    ForcaCalibrationStore::Record r;
    record(step, r);
    r.status = "pending";
    r.start  = p.start;
    r.end    = p.end;
    r.values["height"] = tower_h;
    if (step == Step::ShaperFreq) {
        r.values["x0"] = p.freqStartX;
        r.values["x1"] = p.freqEndX;
        r.values["y0"] = p.freqStartY;
        r.values["y1"] = p.freqEndY;
    } else if (step == Step::VFA) {
        r.values["step"] = p.step;
    }
    if (step == Step::ShaperFreq || step == Step::ShaperDamp)
        r.values["type_index"] = double(step == Step::ShaperFreq ? m_fq_type->GetSelection() : m_dp_type->GetSelection());
    m_store.set_record(key(step), r);

    if (send_after) {
        m_plater->set_one_shot_slice_completed_callback([this, step](bool ok) {
            // Next idle, so the slice-completed event unwinds before the modal send dialog (see the filament track).
            CallAfter([this, step, ok]() {
                if (ok) {
                    forca_academy_mark_source("printer_calibration: " + step_id(step));
                    m_plater->print_current_plate();
                    show_result(step);
                } else {
                    MessageDialog dlg(this, _L("Slicing did not finish, so nothing was sent. Fix the error and try again."),
                                      step_name(step), wxICON_WARNING | wxOK);
                    dlg.ShowModal();
                }
            });
        });
        m_plater->reslice();
        m_plater->select_view_3D("Preview", true);
    }
    update_status();
    if (m_relayout)
        m_relayout();
}

// ---- Result -----------------------------------------------------------------------------------------------------

std::string ForcaPrinterCalibration::default_copy_name() const
{
    std::string name = printer_info().preset;
    const std::string suffix = " (calibrated)";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        return name;
    return name + suffix;
}

void ForcaPrinterCalibration::show_result(Step step)
{
    m_step = step;
    const PrinterInfo pi = printer_info();
    ForcaCalibrationStore::Record r;
    const bool have = record(step, r);
    const double tower_h = have && r.values.count("height") ? r.values.at("height") : 0.0;

    m_res_title->SetLabel(wxString::Format(_L("Enter result: %s"), step_name(step)));
    m_res_freq->Show(step == Step::ShaperFreq);
    m_res_y_row->ShowItems(!pi.reprap);
    m_res_single->Show(step == Step::ShaperDamp || step == Step::Cornering);
    m_res_vfa->Show(step == Step::VFA);
    for (wxTextCtrl* t : { m_res_hx, m_res_fx, m_res_hy, m_res_fy, m_res_h, m_res_v, m_res_blocks })
        t->ChangeValue("");
    m_res_speeds->SetLabel("");

    wxString hint;
    if (step == Step::ShaperFreq) {
        hint = _L("Measure from the bed to where the ringing is weakest on each face (X face: the side whose ringing "
                  "comes from X moves).");
        if (tower_h > 0)
            hint += wxString::Format(_L("\nfrequency = start + height / %.1f mm x (end - start)."), tower_h);
    } else if (step == Step::ShaperDamp || step == Step::Cornering) {
        m_res_single_label->SetLabel(step == Step::ShaperDamp ? _L("Damping ratio:")
                                     : pi.uses_jd             ? _L("Junction deviation (mm):")
                                     : pi.klipper             ? _L("Square corner velocity (mm/s):")
                                                              : _L("Jerk (mm/s):"));
        hint = step == Step::ShaperDamp ? _L("Measure from the bed to where the ringing is weakest.")
                                        : _L("Measure from the bed to the highest point where the corners are still "
                                             "sharp and clean.");
        if (tower_h > 0 && have)
            hint += wxString::Format(_L("\nvalue = %s + height / %.1f mm x (%s - %s)."), fmt_num(r.start, 3), tower_h,
                                     fmt_num(r.end, 3), fmt_num(r.start, 3));
    } else {
        double step_v = have && r.values.count("step") ? r.values.at("step") : 0;
        hint = _L("Enter the block numbers where you see fine vertical ribbing (the bottom block is 1).");
        if (have && step_v > 0)
            hint += wxString::Format(_L("\nspeed = %s + (block - 1) x %s mm/s."), fmt_num(r.start, 0), fmt_num(step_v, 0));
    }
    if (tower_h <= 0 && step != Step::VFA)
        hint += "\n" + _L("(The tower height of your test isn't known here, so type the value itself.)");
    m_res_hint->SetLabel(forca_hard_wrap(hint, 60));

    // Input shaping: emitted by Forca, or kept in the firmware. Default: what the preset does now -- on for firmware
    // that has nowhere else to keep it (Marlin / RepRap), as the preset already has it for Klipper.
    const bool shaping = (step == Step::ShaperFreq || step == Step::ShaperDamp);
    m_emit->Show(shaping);
    bool emit_now = !pi.klipper;
    if (PresetBundle* b = wxGetApp().preset_bundle)
        if (const auto* o = b->printers.get_edited_preset().config.option<ConfigOptionBool>("input_shaping_emit"); o && pi.klipper)
            emit_now = o->value;
    m_emit->SetValue(emit_now);

    // Target: update in place (user presets only) or a copy. A preset that is already a calibrated copy updates.
    const bool writes = (step == Step::Cornering) || (shaping && m_emit->GetValue());
    m_target->Show(writes);
    m_tgt_update->Enable(!pi.is_system);
    ForcaCalibrationStore::Run run;
    const bool is_copy = m_store.get_printer_run(pi.preset, run) && run.base != run.target;
    const bool update  = !pi.is_system;
    m_tgt_update->SetValue(update);
    m_tgt_copy->SetValue(!update);
    m_tgt_name->SetValue(wxString::FromUTF8(default_copy_name().c_str()));
    m_tgt_note->SetLabel(forca_hard_wrap(pi.is_system ? _L("This is a system printer preset, which can't be changed; the "
                                                           "result goes into a new preset.")
                                         : is_copy ? _L("This preset is already a calibrated copy made by the wizard.")
                                                   : _L("A copy is added to any network printer this preset is used by."),
                                         60));

    m_klipper->Show(pi.klipper && step != Step::VFA);
    update_klipper_text();

    m_book->ChangeSelection(PAGE_RESULT); // no event (see show_step)
    m_book->GetPage(PAGE_RESULT)->Layout();
    Layout();
    if (m_relayout)
        m_relayout();
}

void ForcaPrinterCalibration::update_result_values()
{
    ForcaCalibrationStore::Record r;
    if (!record(m_step, r))
        return;
    const double tower_h = r.values.count("height") ? r.values.at("height") : 0.0;
    auto val = [&](const char* k) { return r.values.count(k) ? r.values.at(k) : 0.0; };
    double h = 0;
    if (m_step == Step::ShaperFreq && tower_h > 0) {
        if (num(m_res_hx, h))
            m_res_fx->ChangeValue(fmt_num(from_height(h, tower_h, val("x0"), val("x1")), 1));
        if (num(m_res_hy, h))
            m_res_fy->ChangeValue(fmt_num(from_height(h, tower_h, val("y0"), val("y1")), 1));
    } else if ((m_step == Step::ShaperDamp || m_step == Step::Cornering) && tower_h > 0) {
        if (num(m_res_h, h))
            m_res_v->ChangeValue(fmt_num(from_height(h, tower_h, r.start, r.end), m_step == Step::Cornering && r.end > 1 ? 1 : 3));
    } else if (m_step == Step::VFA) {
        wxString          speeds;
        wxStringTokenizer tok(m_res_blocks->GetValue(), ", ");
        while (tok.HasMoreTokens()) {
            long n = 0;
            if (!tok.GetNextToken().ToLong(&n) || n < 1)
                continue;
            const double s = std::min(r.end, r.start + double(n - 1) * val("step"));
            speeds += (speeds.IsEmpty() ? "" : ", ") + fmt_num(s, 0);
        }
        m_res_speeds->SetLabel(speeds.IsEmpty() ? wxString() : wxString::Format(_L("Speeds with ribbing: %s mm/s"), speeds));
    }
    update_klipper_text();
}

void ForcaPrinterCalibration::update_klipper_text()
{
    if (!m_klipper || !m_klipper->IsShown())
        return;
    const PrinterInfo pi = printer_info();
    auto shaper = [&](wxChoice* c) {
        const int i = c->GetSelection();
        std::string v = (i >= 0 && i < int(m_shaper_values.size())) ? m_shaper_values[size_t(i)] : std::string();
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        return wxString::FromUTF8(v.c_str());
    };
    wxString text;
    if (m_step == Step::ShaperFreq) {
        const wxString type = shaper(m_fq_type);
        text = "[input_shaper]\n";
        if (type != "default")
            text += "shaper_type_x: " + type + "\n";
        text += "shaper_freq_x: " + m_res_fx->GetValue() + "\n";
        if (type != "default")
            text += "shaper_type_y: " + type + "\n";
        text += "shaper_freq_y: " + (pi.reprap ? m_res_fx->GetValue() : m_res_fy->GetValue());
    } else if (m_step == Step::ShaperDamp) {
        text = "[input_shaper]\ndamping_ratio_x: " + m_res_v->GetValue() + "\ndamping_ratio_y: " + m_res_v->GetValue();
    } else if (m_step == Step::Cornering) {
        text = "[printer]\nsquare_corner_velocity: " + m_res_v->GetValue();
    }
    m_klipper_text->ChangeValue(text);
}

std::string ForcaPrinterCalibration::write_printer_preset(std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>>& keys,
                                                          bool update_in_place, const std::string& copy_name, wxString& err)
{
    PresetBundle* b = wxGetApp().preset_bundle;
    if (!b) {
        err = _L("No preset bundle is available.");
        return {};
    }
    PresetCollection& printers = b->printers;
    // The test put throwaway values (jerk, junction deviation, ...) on the edited printer preset: never save those.
    forca_discard_transient_preset_changes();
    const std::string src_name = printers.get_edited_preset().name;
    const Preset*     src      = printers.find_preset(src_name, false);
    if (!src) {
        err = _L("The printer preset was not found.");
        return {};
    }
    std::string target = src_name;
    if (!update_in_place) {
        wxString n = wxString::FromUTF8(copy_name.c_str()).Trim().Trim(false);
        if (n.IsEmpty()) {
            err = _L("Please enter a name for the new printer preset.");
            return {};
        }
        target = printers.get_preset_name_by_alias(n.ToUTF8().data());
    } else if (src->is_system || src->is_default) {
        err = _L("A system printer preset can't be changed; save the result as a new preset.");
        return {};
    }
    const Preset* existing = printers.find_preset(target, false);
    if (existing && (existing->is_system || existing->is_default)) {
        err = _L("The name cannot be the same as a system preset name.");
        return {};
    }

    // The backup / "before": every setting the track may write, from the preset the run starts from (first time only).
    ForcaCalibrationStore::Run run;
    run.kind    = "printer";
    run.printer = src_name;
    run.nozzle  = printer_info().nozzle;
    run.base    = src_name;
    run.target  = target;
    for (const std::string& k : printer_keys())
        if (const ConfigOption* o = src->config.option(k))
            run.before_text[k] = o->serialize();
    m_store.start_run(run);

    // Build on the saved preset (a copy that already exists keeps its earlier results).
    Preset temp = existing ? *existing : *src;
    for (auto& kv : keys) {
        if (auto* nv = dynamic_cast<ConfigOptionVectorBase*>(kv.second.get())) { // keep every extruder/mode value
            const auto* ov = dynamic_cast<const ConfigOptionVectorBase*>(temp.config.option(kv.first));
            if (ov && !nv->empty() && ov->size() > nv->size())
                nv->resize(ov->size());
        }
        temp.config.set_key_value(kv.first, kv.second.release());
    }
    printers.save_current_preset(target, false, false, &temp);
    Preset* saved = printers.find_preset(target, false, true);
    if (!saved) {
        err = _L("Saving the printer preset failed.");
        return {};
    }
    saved->sync_info = existing ? "update" : "create";
    saved->save_info();
    b->update_compatible(PresetSelectCompatibleType::Never);

    // A new copy joins every network (physical) printer the original was on, so "send to printer" keeps working.
    if (target != src_name)
        for (const std::string& ph : b->physical_printers.get_printers_with_preset(src_name))
            if (PhysicalPrinter* pp = b->physical_printers.find_printer(ph))
                if (pp->add_preset(target))
                    b->physical_printers.save_printer(*pp);

    if (Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINTER))
        tab->select_preset(target, false, std::string(), true);
    return target;
}

void ForcaPrinterCalibration::on_apply()
{
    const Step        step = m_step;
    const PrinterInfo pi   = printer_info();
    ForcaCalibrationStore::Record r;
    record(step, r);
    const std::string type_value = [&]() {
        const int i = (step == Step::ShaperFreq ? m_fq_type : m_dp_type)->GetSelection();
        return (i >= 0 && i < int(m_shaper_values.size())) ? m_shaper_values[size_t(i)] : std::string();
    }();

    std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>> keys;
    wxString summary;
    double   v = 0, fx = 0, fy = 0;
    switch (step) {
    case Step::ShaperFreq: {
        if (!num(m_res_fx, fx) || (!pi.reprap && !num(m_res_fy, fy)) || fx <= 0 || fx > 500 || (!pi.reprap && (fy <= 0 || fy > 500))) {
            MessageDialog dlg(this, _L("Please enter the frequencies (above 0, up to 500 Hz)."), step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        if (pi.reprap)
            fy = fx;
        r.values["rx"] = fx;
        r.values["ry"] = fy;
        r.note         = type_value;
        summary = wxString::Format(_L("Shaper %s, X %.1f Hz, Y %.1f Hz"), wxString::FromUTF8(type_value.c_str()), fx, fy);
        if (m_emit->GetValue()) {
            InputShaperType t = InputShaperType::Default;
            shaper_from_value(type_value, t);
            keys.emplace_back("input_shaping_emit", std::make_unique<ConfigOptionBool>(true));
            keys.emplace_back("input_shaping_type", std::make_unique<ConfigOptionEnum<InputShaperType>>(t));
            keys.emplace_back("input_shaping_freq_x", std::make_unique<ConfigOptionFloat>(fx));
            keys.emplace_back("input_shaping_freq_y", std::make_unique<ConfigOptionFloat>(fy));
        }
        break;
    }
    case Step::ShaperDamp: {
        if (!num(m_res_v, v) || v < 0 || v >= 1) {
            MessageDialog dlg(this, _L("Please enter the damping ratio (0 up to, not including, 1)."), step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        r.values["rv"] = v;
        r.note         = type_value;
        summary = wxString::Format(_L("Damping ratio %.3f"), v);
        if (m_emit->GetValue()) {
            InputShaperType t = InputShaperType::Default;
            shaper_from_value(type_value, t);
            keys.emplace_back("input_shaping_emit", std::make_unique<ConfigOptionBool>(true));
            keys.emplace_back("input_shaping_type", std::make_unique<ConfigOptionEnum<InputShaperType>>(t));
            keys.emplace_back("input_shaping_damp_x", std::make_unique<ConfigOptionFloat>(v));
            keys.emplace_back("input_shaping_damp_y", std::make_unique<ConfigOptionFloat>(v));
        }
        break;
    }
    case Step::Cornering: {
        const double max_v = pi.uses_jd ? 0.3 : 100.0;
        if (!num(m_res_v, v) || v <= 0 || v > max_v) {
            MessageDialog dlg(this, wxString::Format(_L("Please enter a value above 0, up to %s."), fmt_num(max_v, pi.uses_jd ? 1 : 0)),
                              step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
        r.values["rv"] = v;
        r.values["jd"] = pi.uses_jd ? 1.0 : 0.0;
        if (pi.uses_jd) {
            keys.emplace_back("machine_max_junction_deviation", std::make_unique<ConfigOptionFloats>(std::vector<double>{ v }));
            summary = wxString::Format(_L("Junction deviation %.3f mm"), v);
        } else {
            // Klipper: Orca does not emit machine limits, but its time estimate uses them -- keep them in step.
            keys.emplace_back("machine_max_jerk_x", std::make_unique<ConfigOptionFloats>(std::vector<double>{ v }));
            keys.emplace_back("machine_max_jerk_y", std::make_unique<ConfigOptionFloats>(std::vector<double>{ v }));
            summary = wxString::Format(pi.klipper ? _L("Square corner velocity %.1f mm/s") : _L("Jerk %.1f mm/s"), v);
        }
        break;
    }
    default: {
        update_result_values();
        const wxString speeds = m_res_speeds->GetLabel();
        if (m_res_blocks->GetValue().Trim().IsEmpty()) {
            r.note  = "none";
            summary = _L("No ribbing seen - no speeds to avoid.");
        } else if (speeds.IsEmpty()) {
            MessageDialog dlg(this, _L("Please enter block numbers (1 = the bottom block), e.g. 5, 6."), step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        } else {
            r.note  = speeds.AfterFirst(':').Trim(false).ToUTF8().data();
            summary = speeds + "\n\n" + _L("Keep your outer wall speed (and other visible wall speeds) at least one "
                                           "step away from these, in the process preset.");
        }
        break;
    }
    }

    std::string saved;
    if (!keys.empty()) {
        wxString err;
        saved = write_printer_preset(keys, m_tgt_update->IsEnabled() && m_tgt_update->GetValue(),
                                     m_tgt_name->GetValue().ToUTF8().data(), err);
        if (saved.empty()) {
            MessageDialog dlg(this, err.IsEmpty() ? _L("Could not save the printer preset.") : err, step_name(step), wxICON_WARNING | wxOK);
            dlg.ShowModal();
            return;
        }
    }
    r.status         = "done";
    r.derived_preset = saved;
    m_store.set_record(key(step, pi.preset), r);
    if (!saved.empty() && saved != pi.preset) {
        m_store.set_record(key(step, saved), r); // the calibrated copy carries the result on...
        for (int i = 0; i < static_cast<int>(Step::Count); ++i) { // ...and the printer's earlier steps
            ForcaCalibrationStore::Record src_r, dst_r;
            const Step s = static_cast<Step>(i);
            if (s != step && m_store.get(key(s, pi.preset), src_r) && !m_store.get(key(s, saved), dst_r))
                m_store.set_record(key(s, saved), src_r);
        }
    }

    // Forca Academy: the printer page's calibration history (no-op when the journal is off).
    forca_academy_log_calibration(true, pi.preset, step_id(step) + ": " + into_u8(summary).substr(0, into_u8(summary).find('\n')) +
                                                       (saved.empty() ? std::string(" (kept in the firmware)") : " -> " + saved) +
                                                       " (Calibration Wizard)");
    wxString msg = wxString::Format(_L("Saved: %s."), summary);
    if (!saved.empty())
        msg += "\n\n" + wxString::Format(_L("Written to the printer preset:\n%s\n\nIt is now selected."), wxString::FromUTF8(saved.c_str()));
    else if (step != Step::VFA)
        msg += "\n\n" + _L("The printer preset was not changed (the firmware keeps this setting).");
    if (pi.klipper && step != Step::VFA)
        msg += "\n\n" + _L("Klipper: remember to put the printer.cfg lines shown on the result page into printer.cfg.");

    const Step next = next_step(step);
    msg += "\n\n" + (next == Step::Count ? _L("That completes the printer calibration. Next: calibrate your filaments "
                                              "(answer \"A filament\" when the wizard asks what you are calibrating).")
                                         : wxString::Format(_L("Next up: %s."), step_name(next)));
    MessageDialog dlg(this, msg, step_name(step), wxICON_INFORMATION | wxOK);
    dlg.ShowModal();

    seed_for_printer();
    refresh();
    if (next == Step::Count) {
        if (m_back_to_start)
            m_back_to_start();
    } else
        show_step(next);
}

void ForcaPrinterCalibration::on_skip(Step step)
{
    MessageDialog dlg(this, wxString::Format(_L("Skip %s? The printer keeps its current setting, and you can come back to "
                                                "it any time from the progress bar."), step_name(step)),
                      step_name(step), wxICON_QUESTION | wxYES_NO);
    if (dlg.ShowModal() != wxID_YES)
        return;
    ForcaCalibrationStore::Record r;
    record(step, r);
    r.status = "skipped";
    m_store.set_record(key(step), r);
    const Step next = next_step(step);
    if (next == Step::Count) {
        if (m_back_to_start)
            m_back_to_start();
    } else
        show_step(next);
}

void ForcaPrinterCalibration::on_restore()
{
    const PrinterInfo pi = printer_info();
    ForcaCalibrationStore::Run run;
    if (!m_store.get_printer_run(pi.preset, run) || run.base != run.target || run.before_text.empty())
        return;
    MessageDialog ask(this, wxString::Format(_L("Put back the printer settings \"%s\" had before the wizard first updated it "
                                                "(%s)? The wizard's printer results for it are cleared."),
                                             wxString::FromUTF8(pi.preset.c_str()), wxString::FromUTF8(run.started_at.c_str())),
                      _L("Restore backup"), wxICON_QUESTION | wxYES_NO);
    if (ask.ShowModal() != wxID_YES)
        return;

    std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>> keys;
    for (const auto& kv : run.before_text)
        if (const ConfigOptionDef* def = print_config_def.get(kv.first)) {
            std::unique_ptr<ConfigOption> opt(def->create_empty_option());
            if (opt && opt->deserialize(kv.second))
                keys.emplace_back(kv.first, std::move(opt));
        }
    wxString err;
    if (write_printer_preset(keys, true, std::string(), err).empty()) {
        MessageDialog dlg(this, err.IsEmpty() ? _L("Could not restore the printer preset.") : err, _L("Restore backup"), wxICON_WARNING | wxOK);
        dlg.ShowModal();
        return;
    }
    m_store.forget_printer_run(pi.preset);
    for (int i = 0; i < static_cast<int>(Step::Count); ++i)
        m_store.clear(key(static_cast<Step>(i), pi.preset));
    MessageDialog done(this, _L("The printer preset has its earlier settings again."), _L("Restore backup"), wxICON_INFORMATION | wxOK);
    done.ShowModal();
    refresh();
}

// ---- Progress (drawn by ForcaCalibrationProgressPanel) -----------------------------------------------------------

ForcaCalibrationWizard::ProgressModel ForcaPrinterCalibration::progress_model() const
{
    ForcaCalibrationWizard::ProgressModel m;
    m.printer_track = true;
    const PrinterInfo pi = printer_info();
    m.filament = wxString::FromUTF8(pi.preset.c_str());
    m.printer  = wxString::Format(_L("Printer calibration - nozzle %s mm"), wxString::FromUTF8(pi.nozzle.c_str()));

    static const char* names[] = { L("Shaper freq."), L("Shaper damping"), L("Cornering"), L("VFA") };
    int done = 0;
    for (int i = 0; i < static_cast<int>(Step::Count); ++i) {
        const Step s = static_cast<Step>(i);
        if (!applies(s))
            continue;
        ForcaCalibrationWizard::Step st;
        st.printer_step = i;
        st.name         = _(names[i]);
        st.current      = (m_step == s);
        ForcaCalibrationStore::Record r;
        if (record(s, r)) {
            st.pending = r.status == "pending";
            st.skipped = r.status == "skipped";
            if (st.skipped || has_result(r))
                st.fraction = 1.0;
        }
        if (st.fraction >= 1.0)
            ++done;
        m.steps.push_back(st);
    }
    m.core_steps = static_cast<int>(m.steps.size());
    m.percent    = m.core_steps > 0 ? 100.0 * done / m.core_steps : 0.0;
    m.complete   = m.core_steps > 0 && done >= m.core_steps;

    // Before/after: before = the run's backup when there is one, else the preset's saved values.
    PresetBundle* b = wxGetApp().preset_bundle;
    const Preset* saved = b ? b->printers.find_preset(pi.preset, false) : nullptr;
    ForcaCalibrationStore::Run run;
    const bool have_run = m_store.get_printer_run(pi.preset, run);
    auto before = [&](const std::string& k) -> wxString {
        if (have_run)
            if (auto it = run.before_text.find(k); it != run.before_text.end())
                return first_of(it->second);
        return saved ? setting_text(saved->config, k) : wxString("-");
    };
    auto add_row = [&](Step s, const wxString& label, const std::string& cfg_key, const std::function<wxString(const ForcaCalibrationStore::Record&)>& after) {
        if (!applies(s))
            return;
        ForcaCalibrationWizard::Row row;
        row.label  = label;
        row.before = cfg_key.empty() ? wxString("-") : before(cfg_key);
        ForcaCalibrationStore::Record r;
        if (record(s, r)) {
            row.date = wxString::FromUTF8(r.updated_at.c_str());
            if (r.status == "skipped") {
                row.state = ForcaCalibrationWizard::Row::Skipped;
                row.after = _L("skipped");
            } else if (has_result(r)) {
                row.state = r.status == "pending" ? ForcaCalibrationWizard::Row::Pending : ForcaCalibrationWizard::Row::Done;
                row.after = after(r);
            } else if (r.status == "pending") {
                row.state = ForcaCalibrationWizard::Row::Pending;
                row.after = _L("testing...");
            }
        }
        if (row.after.IsEmpty())
            row.after = "-";
        row.changed = row.state == ForcaCalibrationWizard::Row::Done && row.after != row.before;
        m.rows.push_back(row);
    };
    auto val = [](const ForcaCalibrationStore::Record& r, const char* k, int dec) {
        return r.values.count(k) ? fmt_num(r.values.at(k), dec) : wxString();
    };
    add_row(Step::ShaperFreq, _L("Shaper type"), "input_shaping_type", [](const ForcaCalibrationStore::Record& r) { return wxString::FromUTF8(r.note.c_str()); });
    add_row(Step::ShaperFreq, _L("Shaper frequency X (Hz)"), "input_shaping_freq_x", [&](const ForcaCalibrationStore::Record& r) { return val(r, "rx", 1); });
    if (!pi.reprap)
        add_row(Step::ShaperFreq, _L("Shaper frequency Y (Hz)"), "input_shaping_freq_y", [&](const ForcaCalibrationStore::Record& r) { return val(r, "ry", 1); });
    add_row(Step::ShaperDamp, _L("Damping ratio"), "input_shaping_damp_x", [&](const ForcaCalibrationStore::Record& r) { return val(r, "rv", 3); });
    add_row(Step::Cornering, pi.uses_jd ? _L("Junction deviation (mm)") : pi.klipper ? _L("Square corner velocity (mm/s)") : _L("Jerk (mm/s)"),
            pi.uses_jd ? "machine_max_junction_deviation" : "machine_max_jerk_x",
            [&](const ForcaCalibrationStore::Record& r) { return val(r, "rv", pi.uses_jd ? 3 : 1); });
    add_row(Step::VFA, _L("VFA speeds to avoid (mm/s)"), std::string(), [](const ForcaCalibrationStore::Record& r) {
        return r.note == "none" ? _L("none") : wxString::FromUTF8(r.note.c_str());
    });
    return m;
}

}} // namespace Slic3r::GUI
