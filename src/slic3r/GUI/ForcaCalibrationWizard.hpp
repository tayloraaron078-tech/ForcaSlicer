#ifndef slic3r_ForcaCalibrationWizard_hpp_
#define slic3r_ForcaCalibrationWizard_hpp_

#include "libslic3r/calib.hpp"  // Calib_Params, CalibMode
#include "ForcaCalibrationStore.hpp"

#include <wx/panel.h>
#include <wx/scrolwin.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

class wxStaticText;
class wxTextCtrl;
class wxChoice;
class wxPanel;
class wxSizer;
class wxBoxSizer;
class wxSimplebook;
class wxButton;
class wxCommandEvent;

namespace Slic3r {
class ConfigOption;
namespace GUI {

class Plater;
class ForcaPrinterCalibration;

// Shared with the printer track (ForcaPrinterCalibration).
wxString forca_hard_wrap(const wxString& s, size_t max_chars);  // explicit line breaks (see hard_wrap in the .cpp)
bool     forca_calib_test_generated();      // a calibration test was generated in this app session
void     forca_mark_calib_test_generated(); // (after it, calib_* skip new_project's confirm dialogs)
void     forca_discard_transient_preset_changes(); // revert a prior test's throwaway preset edits

// Forca Slicer Calibration Wizard -- native, slicer-integrated guided filament calibration.
//
// Multi-calibration shell: a shared header (active printer/nozzle, a filament picker, a calibration
// picker, a resume/history line) over a wxSimplebook of per-calibration "generate" pages plus one
// shared "result" page. Each calibration generates its native test via Plater::calib_* and writes its
// result into the run's single named, accumulating filament preset (create/update in place). Results +
// pending state persist in the datadir store so a run resumes across the offline print.
//
// Forca order (per HQ plan): Temp -> MVS -> Flow(2-pass) -> PA -> [optional flow recheck] -> Retraction ->
// Shrinkage -> [optional final check print]. Cal order == picker order == generate-page index.
//
// Lives as the left panel of the "Calibration Wizard" main tab (ForcaCalibrationTab), next to the live plate
// and the progress panel; it scrolls when the tab is shorter than its content.
class ForcaCalibrationWizard : public wxScrolled<wxPanel>
{
public:
    // Order matters: it is the calibration picker order AND the generate-page index in m_book.
    enum class Cal { Temperature, MaxVolSpeed, FlowRate, PressureAdvance, Retraction, Shrinkage, FinalCheck, Count };
    static constexpr int CORE_STEPS = 6; // Temp..Shrinkage count toward progress; the final check is optional

    ForcaCalibrationWizard(wxWindow* parent, Plater* plater);
    ~ForcaCalibrationWizard() override = default;

    // The tab was (re)opened: warn about preset changes made elsewhere mid-run, re-sync to the current presets,
    // and (once per session) show the intro.
    void on_tab_activated();

    // Light/dark mode switched: re-apply the palette to this panel and to every child still using its colours.
    void sys_color_changed();

    // Jump to a calibration (progress-bar step click); start_flow_recheck() = the optional recheck marker.
    void select_calibration(Cal cal);
    // Printer track (ForcaPrinterCalibration): progress-bar click on one of its steps.
    void select_printer_step(int step);
    void start_flow_recheck();
    // Back to the Start page to calibrate another filament (finish card / final check "Calibrate another filament").
    void start_new_calibration();
    // The user chose a filament (dialog / "Calibrate another filament"): the Start page, without asking again.
    void calibrate_a_filament();

    // Called whenever what the progress panel shows may have changed.
    void set_state_changed_callback(std::function<void()> cb) { m_on_state_changed = std::move(cb); }

    // What the progress panel draws: the six core steps (+ optional markers) and the before/after table.
    struct Step {
        Cal      cal;
        wxString name;
        double   fraction = 0;     // 0..1 filled (flow fills in halves)
        bool     skipped  = false;
        bool     pending  = false; // a test is out for printing
        bool     current  = false;
        bool     optional = false; // markers that don't count toward the percentage
        int      printer_step = -1; // printer track: ForcaPrinterCalibration::Step (cal is unused then)
    };
    struct Row {
        wxString label;
        wxString before;
        wxString after;
        wxString delta;            // e.g. "+10 C", "+2.0%"; empty when unchanged / unknown
        wxString date;             // when the result was measured
        bool     changed  = false;
        enum State { None, Pending, Done, Skipped } state = None;
    };
    struct ProgressModel {
        wxString          filament;   // profile being calibrated
        wxString          printer;
        std::vector<Step> steps;      // CORE_STEPS core steps, then the optional markers
        std::vector<Row>  rows;
        double            percent  = 0;
        bool              complete = false;
        int               core_steps    = CORE_STEPS; // steps drawn as bar segments (the rest are optional markers)
        bool              printer_track = false;      // printer calibration (filament = the printer's name)
    };
    ProgressModel progress_model() const;

private:
    static constexpr int PAGE_RESULT = static_cast<int>(Cal::Count); // shared result page follows the gen pages
    static constexpr int PAGE_START  = PAGE_RESULT + 1;               // "start a calibration" page (filament pick)
    static constexpr int PAGE_PRINTER = PAGE_START + 1;               // printer track (ForcaPrinterCalibration)

    wxPanel* build_start_page(wxWindow* parent);
    void     update_start_note();                  // explains what Start will do for the picked filament
    void     on_start_run(wxCommandEvent& evt);    // leave the Start page: first unfinished step of the picked filament
    bool     on_start_page() const;
    void     set_printer_track(bool on); // printer track shows its own page and hides the filament/calibration rows
    // A focused control (the pressed button, or the one focus returns to after a dialog) must not scroll the
    // wizard: its buttons sit at the bottom, so every new page would open scrolled down (same as Preferences).
    bool     ShouldScrollToChildOnFocus(wxWindow*) override { return false; }
    void     ask_what_to_calibrate();    // "What are you calibrating?" dialog -> printer track or filament Start page
    void     add_something_else_button(wxPanel* panel, wxSizer* row); // "Calibrate something else" -> the dialog

    wxPanel* build_temp_gen_page(wxWindow* parent);
    wxPanel* build_mvs_gen_page(wxWindow* parent);
    wxPanel* build_flow_gen_page(wxWindow* parent);
    wxPanel* build_pa_gen_page(wxWindow* parent);
    wxPanel* build_retraction_gen_page(wxWindow* parent);
    wxPanel* build_shrinkage_gen_page(wxWindow* parent);
    wxPanel* build_final_check_page(wxWindow* parent);
    wxPanel* build_result_page(wxWindow* parent);
    // The Slice & send / Generate only buttons + note (+ "already printed" button unless result_label is empty).
    void     add_run_buttons(wxPanel* panel, wxSizer* box, const wxString& result_label);

    // Shared.
    void        refresh_presets(bool resume = true); // resume = jump to a pending calibration (first open only)
    void        show_gen_page();               // show the generate page for the active calibration
    static std::string cal_id(Cal cal);        // store id, e.g. "temperature" | "mvs" | "flow" | "pa"
    std::string cal_id() const { return cal_id(m_cal); }
    wxString    cal_display() const;           // human name of the active calibration
    wxString    format_result(double value) const; // "210 C", "PA 0.042", ... for the active calibration
    void        set_cal(Cal cal);              // switch calibration + sync the picker
    std::string selected_filament_name() const;
    ForcaCalibrationStore::Key current_key() const;
    void        update_status_label();
    void        update_default_preset_name();
    std::string default_preset_name() const;
    void        discard_transient_preset_changes(); // revert a prior test's throwaway preset edits
    void        update_flow_hint();                  // live "new flow = ..." formula on the flow result page

    void on_calibration_changed(wxCommandEvent& evt);
    void on_filament_changed(wxCommandEvent& evt);
    void on_goto_result(wxCommandEvent& evt);
    void on_back_from_result(wxCommandEvent& evt);
    void on_apply_result(wxCommandEvent& evt);
    void on_generate(wxCommandEvent& evt);       // semi-auto: put the test on the plate for the user to print
    void on_slice_and_send(wxCommandEvent& evt); // fully-auto: put on plate -> slice -> send -> result
    void on_skip(wxCommandEvent& evt);           // skip the current step (keeps the current value)

    // Where the run goes after `done` (Forca order). Flow pass bookkeeping is handled by the callers.
    static Cal next_after(Cal done);
    // Relayout after content changes (panel inside the tab) + tell the progress panel.
    void relayout();
    void notify_state_changed();
    // Preset-change warning (#7): banner when the printer or the active filament changed elsewhere mid-run.
    void check_run_presets();
    void show_warning(const wxString& text, const wxString& action_label, std::function<void()> action);
    void hide_warning();
    void remember_run(const std::string& target); // the run this session is working on (for check_run_presets)
    // Flow pass to resume at, from the store (1 unless pass 1 is already done).
    int  stored_flow_pass() const;

    // Flow shared by generate / slice+send.
    bool build_params_for_current_cal(Calib_Params& out, wxString& err);
    void launch_calib(const Calib_Params& params, bool skip_confirm);
    void launch_current_test(bool send_after);
    void show_result_page();

    // Temperature.
    void seed_temperatures();
    void get_type_temp_bounds(int& lo, int& hi) const;

    // Max Volumetric Speed.
    void seed_mvs();
    void on_height_changed(wxCommandEvent& evt);        // measured height -> result helper (MVS, PA Tower, Retraction)
    void on_result_value_changed(wxCommandEvent& evt);  // live flow-ratio formula as the block number is typed

    // Flow Rate: Classic (pass 1 coarse %, pass 2 fine %) or Orca YOLO (Recommended, then Perfectionist).
    enum FlowMethod { FLOW_CLASSIC = 0, FLOW_YOLO = 1 }; // m_flow_method choice order
    bool   flow_is_yolo() const;
    double current_flow_ratio() const;                   // filament_flow_ratio of the selected filament
    double new_flow_ratio(double block_value) const;     // classic: cur*(1+N/100); YOLO: cur+N
    void   update_flow_labels();                         // pass label + coaching for method/pass/recheck
    void   on_flow_method_changed(wxCommandEvent& evt);

    // Pressure Advance.
    enum PaMethod { PA_TOWER = 0, PA_LINE = 1, PA_PATTERN = 2 }; // m_pa_method choice order
    void seed_pa();                                      // reset the range for the chosen method + extruder
    void on_pa_setup_changed(wxCommandEvent& evt);       // method or extruder changed -> reseed + recoach
    void update_pa_coach();

    // Shrinkage (Forca's own procedurally generated frame; see launch_shrinkage_test) and the final check print.
    void launch_shrinkage_test(bool skip_confirm);
    void launch_final_check(bool skip_confirm);
    double current_percent(const char* key) const;       // filament_shrink / _z of the selected filament (100 = none)

    // Create or update the run's target filament preset (works on a copy; never dirties the source).
    std::string create_or_update_filament_preset(const std::string& target_name,
                                                 const std::map<std::string, ConfigOption*>& key_values, wxString& err);

    Plater*       m_plater        { nullptr };
    Cal           m_cal           { Cal::Temperature };
    wxSimplebook* m_book          { nullptr };
    wxStaticText* m_printer_value { nullptr };
    wxStaticText* m_nozzle_value  { nullptr };
    wxChoice*     m_filament_choice{ nullptr };
    wxChoice*     m_cal_choice     { nullptr };
    wxStaticText* m_status_label  { nullptr };

    wxTextCtrl*   m_temp_start     { nullptr };
    wxTextCtrl*   m_temp_end       { nullptr };
    wxTextCtrl*   m_mvs_start       { nullptr };
    wxTextCtrl*   m_mvs_end         { nullptr };
    wxTextCtrl*   m_mvs_step        { nullptr };

    wxStaticText* m_result_label   { nullptr };
    wxTextCtrl*   m_result_value    { nullptr };
    wxBoxSizer*   m_result_row      { nullptr }; // the single-value row (hidden for Shrinkage)
    wxPanel*      m_shrink_inputs   { nullptr }; // Shrinkage: measured X / Y / Z
    wxTextCtrl*   m_shrink_x        { nullptr };
    wxTextCtrl*   m_shrink_y        { nullptr };
    wxTextCtrl*   m_shrink_z        { nullptr };
    wxTextCtrl*   m_preset_name     { nullptr };
    wxStaticText* m_result_hint     { nullptr }; // per-calibration guidance on the result page
    wxPanel*      m_height_help     { nullptr }; // "measured height -> result" helper (MVS, PA Tower, Retraction)
    wxStaticText* m_height_hint     { nullptr };
    wxStaticText* m_height_label    { nullptr }; // "Measured height (mm):" or, for Retraction, a ring count
    wxTextCtrl*   m_height_value    { nullptr };
    wxStaticText* m_flow_pass_label { nullptr }; // shows the current flow pass on the flow gen page
    wxStaticText* m_flow_coach      { nullptr };
    wxChoice*     m_flow_method     { nullptr }; // Classic / YOLO
    int           m_flow_pass       { 1 };       // flow is 2-pass (1 = coarse/Recommended, 2 = fine/Perfectionist)
    bool          m_flow_recheck    { false };   // optional single fine pass after PA; then continues to Retraction
    wxTextCtrl*   m_retract_start   { nullptr };
    wxTextCtrl*   m_retract_end     { nullptr };
    wxTextCtrl*   m_retract_step    { nullptr };
    wxChoice*     m_pa_method        { nullptr }; // PA Tower / Line / Pattern
    wxStaticText* m_pa_coach         { nullptr }; // per-method reading instructions
    wxChoice*     m_pa_extruder      { nullptr }; // DDE / Bowden (seeds the PA range)
    wxTextCtrl*   m_pa_start          { nullptr };
    wxTextCtrl*   m_pa_end            { nullptr };
    wxTextCtrl*   m_pa_step           { nullptr };
    wxPanel*      m_pa_pattern_opts   { nullptr }; // Pattern-only: optional acceleration/speed lists
    wxTextCtrl*   m_pa_accels         { nullptr };
    wxTextCtrl*   m_pa_speeds         { nullptr };

    // Params of the last launched test. Must outlive the plate: CalibPressureAdvancePattern (model().calib_pa_pattern)
    // keeps a REFERENCE to them and re-reads it on every later reslice (Orca's PA dialog keeps its own as a member).
    Calib_Params             m_launched_params;

    std::vector<std::string> m_project_filaments; // indexed by m_filament_choice selection
    ForcaCalibrationStore    m_store;             // persisted pending/result records (loads on open)

    std::function<void()> m_on_state_changed;
    wxStaticText*         m_start_note  { nullptr };
    bool                  m_run_finished{ false };   // sequence finished this visit -> next tab visit starts fresh
    wxPanel*              m_warn_panel  { nullptr }; // preset-change banner
    wxStaticText*         m_warn_text   { nullptr };
    wxButton*             m_warn_action { nullptr };
    std::function<void()> m_warn_fn;
    std::string           m_run_target;              // run being worked on this session ("" = none yet)
    std::string           m_run_printer;
    std::string           m_run_nozzle;

    // Printer track (Start page choice).
    ForcaPrinterCalibration* m_printer       { nullptr };
    bool                     m_printer_track { false };
    bool                     m_chose_filament { false };  // answered "A filament" and nothing started since
    int                      m_scrolled_page  { -1 };     // page last scrolled to the top (see relayout)
    wxSizer*                 m_filament_row  { nullptr };
    wxSizer*                 m_cal_row       { nullptr };
    wxSizer*                 m_cfg_box       { nullptr };
};

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaCalibrationWizard_hpp_
