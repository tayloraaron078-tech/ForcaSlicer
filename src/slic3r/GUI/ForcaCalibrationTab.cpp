#include "ForcaCalibrationTab.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp" // resources_dir()

#include <wx/button.h>
#include <wx/settings.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/datetime.h>
#include <wx/filedlg.h>
#include <wx/sizer.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace Slic3r { namespace GUI {

// ---- Palette (Forca flair; light + dark) -------------------------------------------------------

namespace {
struct Palette {
    wxColour bg, text, subtle, grid, header_bg, header_text, alt_row, changed_bg, track, fill, fill_text, teal, orange, grey;
};
Palette palette()
{
    if (wxGetApp().dark_mode())
        return { wxColour("#2B2F3A"), wxColour("#E6EAF2"), wxColour("#9AA3B5"), wxColour("#3C4250"),
                 wxColour("#33415E"), wxColour("#E6EAF2"), wxColour("#30343F"), wxColour("#1F4D4A"),
                 wxColour("#3A4152"), wxColour("#3D7BF0"), *wxWHITE, wxColour("#26B3A5"), wxColour("#FF8A33"),
                 wxColour("#7D8699") };
    return { *wxWHITE, wxColour("#26395A"), wxColour("#6B7488"), wxColour("#DCE3EF"),
             wxColour("#E7EEFA"), wxColour("#26395A"), wxColour("#F6F8FC"), wxColour("#E0F4F1"),
             wxColour("#E9EEF6"), wxColour("#2F6FDE"), *wxWHITE, wxColour("#009688"), wxColour("#FF6F00"),
             wxColour("#A0A8B8") };
}

const char* CFG_LEFT_W = "forca_calib_left_width"; // DIP
const char* CFG_TOP_H  = "forca_calib_top_height"; // DIP
} // namespace

// ---- ForcaSash ---------------------------------------------------------------------------------

ForcaSash::ForcaSash(wxWindow* parent, bool vertical, std::function<void(int)> on_drag, std::function<void()> on_done)
    : wxPanel(parent, wxID_ANY)
    , m_vertical(vertical)
    , m_on_drag(std::move(on_drag))
    , m_on_done(std::move(on_done))
{
    const int thickness = FromDIP(6);
    SetMinSize(vertical ? wxSize(thickness, -1) : wxSize(-1, thickness));
    SetBackgroundColour(palette().grid);
    SetCursor(wxCursor(vertical ? wxCURSOR_SIZEWE : wxCURSOR_SIZENS));

    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        const wxPoint p = ClientToScreen(e.GetPosition());
        m_last = m_vertical ? p.x : p.y;
        CaptureMouse();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        if (!HasCapture())
            return;
        const wxPoint p   = ClientToScreen(e.GetPosition());
        const int     pos = m_vertical ? p.x : p.y;
        if (pos != m_last && m_on_drag)
            m_on_drag(pos - m_last);
        m_last = pos;
    });
    auto finish = [this]() {
        if (HasCapture())
            ReleaseMouse();
        if (m_on_done)
            m_on_done();
    };
    Bind(wxEVT_LEFT_UP, [finish](wxMouseEvent&) { finish(); });
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { if (m_on_done) m_on_done(); });
}

void ForcaSash::sys_color_changed()
{
    SetBackgroundColour(palette().grid);
    Refresh();
}

// ---- ForcaBannerPanel --------------------------------------------------------------------------

ForcaBannerPanel::ForcaBannerPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    const std::string path = resources_dir() + "/images/forca_calibration_banner.png";
    if (!m_image.LoadFile(wxString::FromUTF8(path.c_str()), wxBITMAP_TYPE_PNG))
        m_image = wxImage(); // missing file -> just the background
    Bind(wxEVT_PAINT, &ForcaBannerPanel::on_paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { Refresh(); e.Skip(); });
}

void ForcaBannerPanel::on_paint(wxPaintEvent& /*evt*/)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(palette().bg));
    dc.Clear();
    const wxSize sz  = GetClientSize();
    const int    pad = FromDIP(10);
    if (!m_image.IsOk() || sz.x <= 2 * pad || sz.y <= 2 * pad)
        return;
    // Fit inside the padded area, keeping the aspect ratio; only ever scale down (the source is the full size).
    const double scale = std::min({ double(sz.x - 2 * pad) / m_image.GetWidth(), double(sz.y - 2 * pad) / m_image.GetHeight(), 1.0 });
    const wxSize target(std::max(1, int(m_image.GetWidth() * scale)), std::max(1, int(m_image.GetHeight() * scale)));
    if (target != m_scaled_for) {
        m_scaled     = wxBitmap(m_image.Scale(target.x, target.y, wxIMAGE_QUALITY_HIGH));
        m_scaled_for = target;
    }
    dc.DrawBitmap(m_scaled, (sz.x - target.x) / 2, (sz.y - target.y) / 2, true);
}

// ---- ForcaCalibrationProgressPanel -------------------------------------------------------------

ForcaCalibrationProgressPanel::ForcaCalibrationProgressPanel(wxWindow* parent, ForcaCalibrationWizard* wizard)
    : wxPanel(parent, wxID_ANY)
    , m_wizard(wizard)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_save_btn = new wxButton(this, wxID_ANY, _L("Save summary image"));
    m_save_btn->SetToolTip(_L("Save this before/after summary as a PNG image."));
    m_save_btn->Bind(wxEVT_BUTTON, &ForcaCalibrationProgressPanel::on_save_image, this);
    m_save_btn->Hide();
    m_new_btn = new wxButton(this, wxID_ANY, _L("Calibrate another filament"));
    m_new_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_wizard) m_wizard->start_new_calibration(); });
    m_new_btn->Hide();

    Bind(wxEVT_PAINT, &ForcaCalibrationProgressPanel::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &ForcaCalibrationProgressPanel::on_left_down, this);
    Bind(wxEVT_MOTION, &ForcaCalibrationProgressPanel::on_motion, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { position_buttons(); Refresh(); e.Skip(); });
    refresh_model();
}

void ForcaCalibrationProgressPanel::refresh_model()
{
    if (m_wizard)
        m_model = m_wizard->progress_model();
    m_save_btn->Show(m_model.complete);
    m_new_btn->Show(m_model.complete);
    position_buttons();
    Refresh();
}

void ForcaCalibrationProgressPanel::on_paint(wxPaintEvent& /*evt*/)
{
    wxAutoBufferedPaintDC dc(this);
    draw(dc, GetClientSize(), true);
}

void ForcaCalibrationProgressPanel::position_buttons()
{
    // Finish-card buttons in the top-right corner: [Calibrate another filament] [Save summary image]. Done on
    // size/model changes, never from the paint handler (moving children while painting can re-trigger paints).
    if (!m_save_btn->IsShown())
        return;
    const wxSize bs = m_save_btn->GetBestSize();
    const wxSize ns = m_new_btn->GetBestSize();
    const int    x  = GetClientSize().x - bs.x - FromDIP(12);
    m_save_btn->SetPosition(wxPoint(x, FromDIP(8)));
    m_new_btn->SetPosition(wxPoint(x - ns.x - FromDIP(8), FromDIP(8)));
}

void ForcaCalibrationProgressPanel::draw(wxDC& dc, const wxSize& size, bool record_hits)
{
    const Palette pal = palette();
    const int     pad = FromDIP(12);
    if (record_hits) {
        m_step_hits.clear();
        m_tips.clear();
    }

    dc.SetBackground(wxBrush(pal.bg));
    dc.Clear();

    // Header: what is being calibrated (the finish card title when complete).
    int y = FromDIP(10);
    dc.SetFont(Label::Head_16);
    dc.SetTextForeground(pal.text);
    const wxString title = m_model.complete ? _L("Calibration complete") : _L("Calibrating");
    dc.DrawText(title, pad, y);
    int tx = pad + dc.GetTextExtent(title).x + FromDIP(10);
    dc.SetFont(Label::Head_14);
    dc.SetTextForeground(pal.fill);
    const wxString fil = m_model.filament.IsEmpty() ? wxString("-") : m_model.filament;
    dc.DrawText(fil, tx, y + FromDIP(2));
    y += FromDIP(26);
    dc.SetFont(Label::Body_12);
    dc.SetTextForeground(pal.subtle);
    dc.DrawText(m_model.printer, pad, y);
    y += FromDIP(22);

    // Progress bar: six equal segments, filled by completed steps, percent centered.
    const int bar_h = FromDIP(26);
    const wxRect bar(pad, y, std::max(10, size.x - 2 * pad), bar_h);
    const int    radius = bar_h / 2;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.track));
    dc.DrawRoundedRectangle(bar, radius);
    const int    n_core   = ForcaCalibrationWizard::CORE_STEPS;
    const double seg_w    = double(bar.width) / n_core;
    const int    fill_w   = int(std::round(bar.width * std::clamp(m_model.percent, 0.0, 100.0) / 100.0));
    if (fill_w > 0) {
        dc.SetBrush(wxBrush(pal.fill));
        dc.DrawRoundedRectangle(wxRect(bar.x, bar.y, std::max(fill_w, bar_h), bar.height), radius);
    }
    // Segment dividers.
    dc.SetPen(wxPen(pal.bg, std::max(1, FromDIP(2))));
    for (int i = 1; i < n_core; ++i) {
        const int x = bar.x + int(std::round(seg_w * i));
        dc.DrawLine(x, bar.y + FromDIP(4), x, bar.GetBottom() - FromDIP(3));
    }
    // Percent, centered; white over the fill, text colour over the track.
    dc.SetFont(Label::Head_14);
    const wxString pct = wxString::Format("%d%%", int(std::round(m_model.percent)));
    const wxSize   pe  = dc.GetTextExtent(pct);
    const int      px  = bar.x + (bar.width - pe.x) / 2;
    dc.SetTextForeground(px + pe.x / 2 < bar.x + fill_w ? pal.fill_text : pal.text);
    dc.DrawText(pct, px, bar.y + (bar.height - pe.y) / 2);
    y += bar_h + FromDIP(6);

    // Step labels under their segments (clickable): status dot + name; the current step is bold.
    dc.SetFont(Label::Body_12);
    const int label_h = dc.GetCharHeight() + FromDIP(4);
    for (int i = 0; i < n_core && i < int(m_model.steps.size()); ++i) {
        const auto& st   = m_model.steps[i];
        const int   sx   = bar.x + int(std::round(seg_w * i));
        const int   sw   = int(std::round(seg_w));
        dc.SetFont(st.current ? Label::Head_12 : Label::Body_12);
        const wxSize te  = dc.GetTextExtent(st.name);
        const int    dot = FromDIP(8);
        const int    lx  = sx + std::max(0, (sw - te.x - dot - FromDIP(4)) / 2);
        wxColour dot_col = pal.track;
        if (st.skipped)                 dot_col = pal.grey;
        else if (st.fraction >= 1.0)    dot_col = pal.teal;
        else if (st.fraction > 0.0)     dot_col = pal.fill;
        else if (st.pending)            dot_col = pal.orange;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(dot_col));
        dc.DrawEllipse(lx, y + (label_h - dot) / 2, dot, dot);
        dc.SetTextForeground(st.skipped ? pal.subtle : pal.text);
        dc.DrawText(st.name, lx + dot + FromDIP(4), y + (label_h - te.y) / 2);
        if (record_hits) {
            m_step_hits.push_back({ wxRect(sx, bar.y, sw, bar_h + FromDIP(6) + label_h), st.cal });
            wxString tip = st.skipped ? _L("Skipped") : st.fraction >= 1.0 ? _L("Done") : st.fraction > 0 ? _L("Pass 1 of 2 done")
                                                      : st.pending ? _L("Test printing") : _L("Not started");
            m_tips.push_back({ m_step_hits.back().rect, st.name + ": " + tip + " - " + _L("click to open this step") });
        }
    }
    y += label_h + FromDIP(4);

    // Optional markers (not counted).
    dc.SetFont(Label::Body_12);
    dc.SetTextForeground(pal.subtle);
    int ox = pad;
    const wxString opt_lbl = _L("Optional:");
    dc.DrawText(opt_lbl, ox, y);
    ox += dc.GetTextExtent(opt_lbl).x + FromDIP(10);
    for (size_t i = n_core; i < m_model.steps.size(); ++i) {
        const auto& st = m_model.steps[i];
        const int   d  = FromDIP(9);
        const int   cy = y + (dc.GetCharHeight() - d) / 2;
        dc.SetPen(wxPen(st.fraction >= 1.0 ? pal.teal : pal.grey, std::max(1, FromDIP(1))));
        dc.SetBrush(st.fraction >= 1.0 ? wxBrush(pal.teal) : *wxTRANSPARENT_BRUSH);
        wxPoint diamond[4] = { { ox + d / 2, cy }, { ox + d, cy + d / 2 }, { ox + d / 2, cy + d }, { ox, cy + d / 2 } };
        dc.DrawPolygon(4, diamond);
        dc.SetFont(st.current ? Label::Head_12 : Label::Body_12);
        dc.SetTextForeground(pal.text);
        dc.DrawText(st.name, ox + d + FromDIP(4), y);
        const int w = d + FromDIP(4) + dc.GetTextExtent(st.name).x;
        if (record_hits)
            m_step_hits.push_back({ wxRect(ox, y, w, dc.GetCharHeight()), st.cal, st.cal == ForcaCalibrationWizard::Cal::FlowRate });
        ox += w + FromDIP(16);
    }
    y += dc.GetCharHeight() + FromDIP(12);

    // Before/after table.
    const int row_h   = FromDIP(24);
    const int table_w = std::max(10, size.x - 2 * pad);
    const int c0 = pad, c1 = pad + table_w * 38 / 100, c2 = pad + table_w * 58 / 100, c3 = pad + table_w * 80 / 100;
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.header_bg));
    dc.DrawRoundedRectangle(wxRect(pad, y, table_w, row_h), FromDIP(4));
    dc.SetFont(Label::Head_12);
    dc.SetTextForeground(pal.header_text);
    const int ty = (row_h - dc.GetCharHeight()) / 2;
    dc.DrawText(_L("Setting"), c0 + FromDIP(22), y + ty);
    dc.DrawText(_L("Before"), c1, y + ty);
    dc.DrawText(_L("After"), c2, y + ty);
    dc.DrawText(_L("Change"), c3, y + ty);
    y += row_h;

    for (size_t i = 0; i < m_model.rows.size(); ++i) {
        const auto& row = m_model.rows[i];
        if (i % 2 == 1) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.alt_row));
            dc.DrawRectangle(pad, y, table_w, row_h);
        }
        if (row.changed) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(pal.changed_bg));
            dc.DrawRoundedRectangle(wxRect(c2 - FromDIP(6), y + FromDIP(2), (pad + table_w) - c2 + FromDIP(4), row_h - FromDIP(4)), FromDIP(4));
        }
        // Status icon: teal = saved, orange = testing, grey = skipped, hollow = not yet.
        const int d  = FromDIP(10);
        const int iy = y + (row_h - d) / 2;
        switch (row.state) {
        case ForcaCalibrationWizard::Row::Done:    dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(pal.teal)); break;
        case ForcaCalibrationWizard::Row::Pending: dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(pal.orange)); break;
        case ForcaCalibrationWizard::Row::Skipped: dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(pal.grey)); break;
        default: dc.SetPen(wxPen(pal.grey, std::max(1, FromDIP(1)))); dc.SetBrush(*wxTRANSPARENT_BRUSH); break;
        }
        dc.DrawEllipse(c0 + FromDIP(4), iy, d, d);

        const int ry = y + (row_h - dc.GetCharHeight()) / 2;
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(pal.text);
        dc.DrawText(row.label, c0 + FromDIP(22), ry);
        dc.SetTextForeground(pal.subtle);
        dc.DrawText(row.before, c1, ry);
        dc.SetFont(row.changed ? Label::Head_12 : Label::Body_12);
        dc.SetTextForeground(row.state == ForcaCalibrationWizard::Row::Skipped ? pal.subtle : pal.text);
        dc.DrawText(row.after, c2, ry);
        if (!row.delta.IsEmpty()) {
            dc.SetFont(Label::Body_12);
            dc.SetTextForeground(pal.teal);
            dc.DrawText(row.delta, c3, ry);
        }
        if (record_hits && !row.date.IsEmpty()) {
            const wxString what = row.state == ForcaCalibrationWizard::Row::Skipped ? _L("Skipped on %s")
                                : row.state == ForcaCalibrationWizard::Row::Pending ? _L("Test generated on %s")
                                                                                    : _L("Measured on %s");
            m_tips.push_back({ wxRect(pad, y, table_w, row_h), wxString::Format(what, row.date) });
        }
        y += row_h;
    }
    dc.SetPen(wxPen(pal.grid, std::max(1, FromDIP(1))));
    dc.DrawLine(pad, y, pad + table_w, y);
}

void ForcaCalibrationProgressPanel::on_left_down(wxMouseEvent& evt)
{
    for (const Hit& h : m_step_hits)
        if (h.rect.Contains(evt.GetPosition())) {
            if (m_wizard) {
                if (h.recheck)
                    m_wizard->start_flow_recheck();
                else
                    m_wizard->select_calibration(h.cal);
            }
            return;
        }
    evt.Skip();
}

void ForcaCalibrationProgressPanel::on_motion(wxMouseEvent& evt)
{
    wxString tip;
    bool     over_step = false;
    for (const Tip& t : m_tips)
        if (t.rect.Contains(evt.GetPosition())) {
            tip = t.text;
            break;
        }
    for (const Hit& h : m_step_hits)
        if (h.rect.Contains(evt.GetPosition()))
            over_step = true;
    SetCursor(over_step ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
    if (tip != m_current_tip) {
        m_current_tip = tip;
        if (tip.IsEmpty())
            UnsetToolTip();
        else
            SetToolTip(tip);
    }
    evt.Skip();
}

void ForcaCalibrationProgressPanel::on_save_image(wxCommandEvent& /*evt*/)
{
    // The finish card: the same drawing at a fixed, camera-friendly size, plus a small footer.
    const wxSize size(FromDIP(760), std::max(GetClientSize().y, FromDIP(360)) + FromDIP(28));
    wxBitmap     bmp(size.x, size.y);
    {
        wxMemoryDC mdc(bmp);
        draw(mdc, size, false);
        mdc.SetFont(Label::Body_10);
        mdc.SetTextForeground(palette().subtle);
        mdc.DrawText(wxString::Format(_L("Forca Slicer Calibration Wizard - %s"), wxDateTime::Now().FormatISODate()),
                     FromDIP(12), size.y - FromDIP(20));
        mdc.SelectObject(wxNullBitmap);
    }

    wxString name = m_model.filament;
    name.Replace("/", "_");
    name.Replace("\\", "_");
    name.Replace(":", "_");
    wxFileDialog dlg(this, _L("Save calibration summary"), wxEmptyString, name + " - calibration.png",
                     "PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK)
        return;
    if (!bmp.ConvertToImage().SaveFile(dlg.GetPath(), wxBITMAP_TYPE_PNG)) {
        MessageDialog err(this, _L("Could not save the image."), _L("Save calibration summary"), wxICON_WARNING | wxOK);
        err.ShowModal();
    }
}

// ---- ForcaCalibrationHost ----------------------------------------------------------------------

ForcaCalibrationHost::ForcaCalibrationHost(Plater* plater) : m_plater(plater) {}

void ForcaCalibrationHost::build()
{
    m_host = m_plater->canvas_host_panel();
    wxSizer* canvas_sizer = m_host->GetSizer();

    m_wizard   = new ForcaCalibrationWizard(m_host, m_plater);
    m_progress = new ForcaCalibrationProgressPanel(m_host, m_wizard);
    m_wizard->set_state_changed_callback([this]() { if (m_progress) m_progress->refresh_model(); });

    // Remembered split sizes (DIP), with defaults that suit a 1080p window.
    auto cfg_int = [](const char* key, int def) {
        const std::string v = wxGetApp().app_config->get(key);
        try { return v.empty() ? def : std::stoi(v); } catch (...) { return def; }
    };
    m_left_w = m_host->FromDIP(cfg_int(CFG_LEFT_W, 540));
    m_top_h  = m_host->FromDIP(cfg_int(CFG_TOP_H, 380)); // fits header + bar + labels + 7-row table

    auto save = [this]() {
        wxGetApp().app_config->set(CFG_LEFT_W, std::to_string(m_host->ToDIP(m_left_w)));
        wxGetApp().app_config->set(CFG_TOP_H, std::to_string(m_host->ToDIP(m_top_h)));
    };
    m_vsash = new ForcaSash(m_host, true, [this](int d) { m_left_w += d; apply_sizes(); }, save);
    m_hsash = new ForcaSash(m_host, false, [this](int d) { m_top_h += d; apply_sizes(); }, save);

    m_banner = new ForcaBannerPanel(m_host);

    // Wrap (don't rebuild) the canvas sizer: the GL canvases keep their parent.
    m_host->SetSizer(nullptr, false);
    auto* left = new wxBoxSizer(wxVERTICAL); // wizard (3/4) over the Forca banner (1/4)
    left->Add(m_wizard, 3, wxEXPAND);
    left->Add(m_banner, 1, wxEXPAND);
    auto* right = new wxBoxSizer(wxVERTICAL);
    right->Add(m_progress, 0, wxEXPAND);
    right->Add(m_hsash, 0, wxEXPAND);
    right->Add(canvas_sizer, 1, wxEXPAND);
    auto* outer = new wxBoxSizer(wxHORIZONTAL);
    outer->Add(left, 0, wxEXPAND);
    outer->Add(m_vsash, 0, wxEXPAND);
    outer->Add(right, 1, wxEXPAND);
    m_host->SetSizer(outer);

    for (wxWindow* w : tab_windows())
        w->Hide();
}

std::vector<wxWindow*> ForcaCalibrationHost::tab_windows() const
{
    return { m_wizard, m_banner, m_progress, m_vsash, m_hsash };
}

void ForcaCalibrationHost::apply_sizes()
{
    // Keep usable room for the wizard, the table and the plate.
    const wxSize host = m_host->GetClientSize();
    // Never narrower than the wizard's content (it scrolls vertically only, so extra width would be clipped).
    int min_left = m_host->FromDIP(380);
    if (wxSizer* s = m_wizard->GetSizer())
        min_left = std::max(min_left, s->GetMinSize().x + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, m_wizard));
    m_left_w = std::clamp(m_left_w, min_left, std::max(min_left, host.x - m_host->FromDIP(320)));
    m_top_h  = std::clamp(m_top_h, m_host->FromDIP(200), std::max(m_host->FromDIP(200), host.y - m_host->FromDIP(220)));
    // Tiny min height: the wizard scrolls, so the left column splits 3:1 with the banner instead of the wizard's
    // full content height claiming it all.
    m_wizard->SetMinSize(wxSize(m_left_w, 1));
    m_progress->SetMinSize(wxSize(-1, m_top_h));
    m_host->Layout();
    m_wizard->FitInside();
    m_progress->Refresh();
}

void ForcaCalibrationHost::sys_color_changed()
{
    if (!m_wizard)
        return; // not built yet: it picks up the current mode when it is
    m_wizard->sys_color_changed();
    m_vsash->sys_color_changed();
    m_hsash->sys_color_changed();
    m_progress->Refresh(); // the progress panel and banner read the palette as they paint
    m_banner->Refresh();
}

void ForcaCalibrationHost::set_active(bool active)
{
    if (m_building)
        return; // never re-enter while the tab's panels are being created
    if (active && !m_wizard) {
        m_building = true;
        build();
        m_building = false;
    }
    if (!m_wizard || active == m_active)
        return;
    m_active = active;

    m_host->Freeze();
    for (wxWindow* w : tab_windows())
        w->Show(active);
    if (active)
        apply_sizes();
    else
        m_host->Layout();
    m_host->Thaw();

    if (active) {
        // Deferred: activation may show the one-time intro (modal), which must not run inside the notebook's
        // page-changed event.
        m_wizard->CallAfter([this]() {
            m_wizard->on_tab_activated();
            m_progress->refresh_model();
        });
    }
}

}} // namespace Slic3r::GUI
