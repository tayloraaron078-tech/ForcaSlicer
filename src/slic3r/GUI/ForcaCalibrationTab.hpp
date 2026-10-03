#ifndef slic3r_ForcaCalibrationTab_hpp_
#define slic3r_ForcaCalibrationTab_hpp_

#include "ForcaCalibrationWizard.hpp"

#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/panel.h>

#include <functional>
#include <vector>

class wxButton;
class wxDC;

namespace Slic3r { namespace GUI {

class Plater;

// A thin drag handle between two areas of the Calibration Wizard tab. Reports drag deltas (pixels) along its axis.
class ForcaSash : public wxPanel
{
public:
    ForcaSash(wxWindow* parent, bool vertical, std::function<void(int)> on_drag, std::function<void()> on_done);
    void sys_color_changed(); // light/dark mode switched

private:
    bool                     m_vertical; // true = a vertical bar dragged left/right
    std::function<void(int)> m_on_drag;
    std::function<void()>    m_on_done;
    int                      m_last { 0 };
};

// The Forca banner under the wizard (bottom quarter of the left column): the image scaled to fit, aspect kept.
class ForcaBannerPanel : public wxPanel
{
public:
    explicit ForcaBannerPanel(wxWindow* parent);

private:
    void     on_paint(wxPaintEvent& evt);
    wxImage  m_image;
    wxBitmap m_scaled; // cached for m_scaled_for
    wxSize   m_scaled_for;
};

// Top-right area of the tab: the run's progress bar (six core steps, percent centered, step labels clickable,
// optional-step markers) and the before/after table for the filament being calibrated. When the run is complete
// it becomes the finish card, which can be saved as a PNG.
class ForcaCalibrationProgressPanel : public wxPanel
{
public:
    ForcaCalibrationProgressPanel(wxWindow* parent, ForcaCalibrationWizard* wizard);

    void refresh_model(); // re-read the wizard's progress model and repaint

private:
    void on_paint(wxPaintEvent& evt);
    void on_left_down(wxMouseEvent& evt);
    void on_motion(wxMouseEvent& evt);
    void on_save_image(wxCommandEvent& evt);
    void position_buttons(); // finish-card buttons, top-right
    // Draws everything into `dc` over `size`; records hit areas when `record_hits` (the on-screen paint).
    void draw(wxDC& dc, const wxSize& size, bool record_hits);

    struct Hit { wxRect rect; ForcaCalibrationWizard::Cal cal; bool recheck = false; int printer_step = -1; };
    struct Tip { wxRect rect; wxString text; };

    ForcaCalibrationWizard*               m_wizard { nullptr };
    ForcaCalibrationWizard::ProgressModel m_model;
    std::vector<Hit>                      m_step_hits;
    std::vector<Tip>                      m_tips;
    wxString                              m_current_tip;
    wxButton*                             m_save_btn { nullptr };
    wxButton*                             m_new_btn  { nullptr }; // finish card: "Calibrate another filament"
};

// Owns the Calibration Wizard tab's layout. The tab is a third notebook page on the shared Plater window (like
// Prepare/Preview); in calibration mode the Plater hides its sidebar and this host shows, inside the Plater's
// canvas host panel, [wizard | sash | [progress | sash | the real 3D/Preview canvas]]. The GL canvases are never
// reparented -- their sizer is wrapped. Panels are created on first activation (presets are loaded by then).
class ForcaCalibrationHost
{
public:
    explicit ForcaCalibrationHost(Plater* plater);

    void set_active(bool active);
    void sys_color_changed(); // light/dark mode switched (MainFrame::on_sys_color_changed)
    ForcaCalibrationWizard* wizard() const { return m_wizard; }

private:
    void build();
    void apply_sizes();
    std::vector<wxWindow*> tab_windows() const; // everything shown only in the Calibration Wizard tab

    Plater*                        m_plater   { nullptr };
    wxWindow*                      m_host     { nullptr }; // the Plater's canvas host panel
    ForcaCalibrationWizard*        m_wizard   { nullptr };
    ForcaCalibrationProgressPanel* m_progress { nullptr };
    ForcaBannerPanel*              m_banner   { nullptr };
    ForcaSash*                     m_vsash    { nullptr };
    ForcaSash*                     m_hsash    { nullptr };
    int                            m_left_w   { 0 };        // px
    int                            m_top_h    { 0 };        // px
    bool                           m_active   { false };
    bool                           m_building { false };
};

}} // namespace Slic3r::GUI

#endif // slic3r_ForcaCalibrationTab_hpp_
