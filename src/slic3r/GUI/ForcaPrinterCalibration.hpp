#ifndef slic3r_ForcaPrinterCalibration_hpp_
#define slic3r_ForcaPrinterCalibration_hpp_

#include "ForcaCalibrationWizard.hpp" // ProgressModel, the shared ForcaCalibrationStore
#include "libslic3r/calib.hpp"        // Calib_Params

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class wxSimplebook;
class wxStaticText;
class wxTextCtrl;
class wxChoice;
class wxCheckBox;
class wxRadioButton;
class wxBoxSizer;
class wxSizer;
class wxButton;

namespace Slic3r {
class ConfigOption;
namespace GUI {

class Plater;

// Forca: the Calibration Wizard's printer track -- the calibrations that belong to a PRINTER (once per printer +
// nozzle) rather than to a filament: Input Shaping (frequency, then damping) -> Cornering -> VFA. It is one page of
// the wizard's book; the wizard's Start page chooses between it and the filament track.
//
// Steps a printer can't use are shown with the reason (Bambu printers tune their own vibration compensation and
// cornering; Orca knows no input shaper for some G-code flavors). Each test is Orca's own (Plater::calib_*); the
// result page turns a measured tower height into the value. Results go, per the user's choice, into the printer
// preset itself -- the old values are backed up in the store and can be restored -- or into a new
// "<printer> (calibrated)" copy, which is added to every network (physical) printer the original was on. For Klipper
// the shaper and square-corner-velocity results are also given as printer.cfg lines, since Klipper keeps them there
// (and Orca does not emit machine limits for Klipper).
class ForcaPrinterCalibration : public wxPanel
{
public:
    enum class Step { ShaperFreq, ShaperDamp, Cornering, VFA, Count }; // Forca order (PLAN_machine_calibration D1)

    ForcaPrinterCalibration(wxWindow* parent, Plater* plater, ForcaCalibrationStore& store,
                            std::function<void()> relayout, std::function<void()> back_to_start);

    void refresh();              // the printer may have changed: applicability, seeds, status line, restore button
    void begin();                // Start pressed: the first applicable step without a result
    void select_step(Step step); // progress-bar click
    ForcaCalibrationWizard::ProgressModel progress_model() const;
    void completion(int& done, int& total) const; // applicable steps with a result (or skipped) / applicable steps
    int  pending_step() const; // a step whose test is out for printing (-1 = none), to resume on first open
    int  current_page() const; // the step / result page shown (wizard: scroll a new page to the top)

    static std::string step_id(Step step); // persisted store ids -- never rename one

private:
    // Page builders (the book holds the four test pages, then the result page).
    wxPanel* build_freq_page(wxWindow* parent);
    wxPanel* build_damp_page(wxWindow* parent);
    wxPanel* build_cornering_page(wxWindow* parent);
    wxPanel* build_vfa_page(wxWindow* parent);
    wxPanel* build_result_page(wxWindow* parent);
    void     add_run_buttons(wxWindow* panel, wxSizer* box, Step step);
    wxTextCtrl* add_number(wxWindow* parent, wxSizer* row, const wxString& label, const wxString& value);

    bool     applies(Step step, wxString* why = nullptr) const;
    Step     next_step(Step after) const;           // next applicable step, or Count
    wxString step_name(Step step) const;
    ForcaCalibrationStore::Key key(Step step, const std::string& printer = std::string()) const;
    bool     record(Step step, ForcaCalibrationStore::Record& out) const;
    static bool has_result(const ForcaCalibrationStore::Record& r);

    void show_step(Step step);                       // the step's test page (or why it doesn't apply)
    void update_status();
    void seed_for_printer();                         // shaper types, JD vs jerk defaults, RepRap single-axis
    bool build_params(Step step, Calib_Params& out, wxString& err);
    void launch(Step step, bool send_after);
    void show_result(Step step);
    void update_result_values();                     // measured height -> value (live), VFA blocks -> speeds
    void update_klipper_text();
    void on_apply();
    void on_skip(Step step);
    void on_restore();
    // Write keys into the printer preset (update in place) or a copy; returns the preset written, "" on error.
    std::string write_printer_preset(std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>>& keys,
                                     bool update_in_place, const std::string& copy_name, wxString& err);
    std::string default_copy_name() const;

    Plater*                m_plater { nullptr };
    ForcaCalibrationStore& m_store;
    std::function<void()>  m_relayout;
    std::function<void()>  m_back_to_start;
    Step                   m_step { Step::ShaperFreq };
    Calib_Params           m_launched; // long-lived: Orca's calib code may keep a reference to its params
    std::string            m_seeded_for; // printer preset the test pages' defaults were set up for

    wxStaticText* m_status { nullptr };
    wxStaticText* m_na_text { nullptr };   // "this step doesn't apply to your printer: ..."
    wxButton*     m_restore_btn { nullptr };
    wxSimplebook* m_book { nullptr };

    // Test pages.
    wxChoice*   m_fq_model { nullptr };
    wxChoice*   m_fq_type { nullptr };
    wxTextCtrl* m_fq_x0 { nullptr };
    wxTextCtrl* m_fq_x1 { nullptr };
    wxTextCtrl* m_fq_y0 { nullptr };
    wxTextCtrl* m_fq_y1 { nullptr };
    wxBoxSizer* m_fq_y_row { nullptr };
    wxTextCtrl* m_fq_damp { nullptr };
    wxChoice*   m_dp_model { nullptr };
    wxChoice*   m_dp_type { nullptr };
    wxTextCtrl* m_dp_fx { nullptr };
    wxTextCtrl* m_dp_fy { nullptr };
    wxTextCtrl* m_dp_d0 { nullptr };
    wxTextCtrl* m_dp_d1 { nullptr };
    wxChoice*   m_cn_model { nullptr };
    wxTextCtrl* m_cn_start { nullptr };
    wxTextCtrl* m_cn_end { nullptr };
    wxStaticText* m_cn_note { nullptr };
    wxTextCtrl* m_vfa_start { nullptr };
    wxTextCtrl* m_vfa_end { nullptr };
    wxTextCtrl* m_vfa_step { nullptr };
    std::vector<std::string> m_shaper_values; // Orca's enum values (what the test and the preset store)

    // Result page.
    wxStaticText* m_res_title { nullptr };
    wxStaticText* m_res_hint { nullptr };
    wxPanel*      m_res_freq { nullptr };
    wxTextCtrl*   m_res_hx { nullptr };  // measured height, X face
    wxTextCtrl*   m_res_fx { nullptr };
    wxBoxSizer*   m_res_y_row { nullptr };
    wxBoxSizer*   m_res_hy_row { nullptr };
    wxTextCtrl*   m_res_hy { nullptr };
    wxTextCtrl*   m_res_fy { nullptr };
    wxPanel*      m_res_single { nullptr }; // damping / cornering: one height -> one value
    wxStaticText* m_res_single_label { nullptr };
    wxTextCtrl*   m_res_h { nullptr };
    wxTextCtrl*   m_res_v { nullptr };
    wxPanel*      m_res_vfa { nullptr };
    wxTextCtrl*   m_res_blocks { nullptr };
    wxStaticText* m_res_speeds { nullptr };
    wxCheckBox*   m_emit { nullptr };
    wxPanel*      m_klipper { nullptr };
    wxTextCtrl*   m_klipper_text { nullptr };
    wxPanel*      m_target { nullptr };
    wxRadioButton* m_tgt_update { nullptr };
    wxRadioButton* m_tgt_copy { nullptr };
    wxTextCtrl*   m_tgt_name { nullptr };
    wxStaticText* m_tgt_note { nullptr };
};

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaPrinterCalibration_hpp_
