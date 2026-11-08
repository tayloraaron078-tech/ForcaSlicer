#include "ForcaPrintRiskDialog.hpp"

#include "Camera.hpp"
#include "Event.hpp"
#include "ForcaAcademy.hpp"
#include "ForcaPrintJournal.hpp" // ForcaPrintJournalDialog, forca_journal_tag_label
#include "GCodeViewer.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp" // from_u8, from_path
#include "GUI_App.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "IMSlider.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/HyperLink.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ForcaPrintRisk.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/libslic3r.h"

#include <Eigen/Core>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <imgui/imgui.h>

#include <wx/busycursor.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/listbase.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/utils.h>
#include <wx/window.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;

int forca_risk_slider_index(const std::vector<double>& zs, double z)
{
    if (zs.empty())
        return -1;
    const auto it = std::lower_bound(zs.begin(), zs.end(), z - EPSILON);
    return it == zs.end() ? int(zs.size()) - 1 : int(std::distance(zs.begin(), it));
}

std::string forca_risk_academy_tag(PrintRiskType type) { return type == PrintRiskType::LongBridge ? "bridging" : "supports"; }

std::vector<fs::path> forca_risk_tagged_prints(const std::vector<ForcaAcademyPrint>& prints, const std::string& printer,
                                               const std::vector<std::string>& filaments, const std::string& tag)
{
    std::vector<fs::path> out;
    for (const ForcaAcademyPrint& p : prints) {
        if (!printer.empty() && p.printer != printer)
            continue;
        if (std::find(p.tags.begin(), p.tags.end(), tag) == p.tags.end())
            continue;
        if (std::any_of(filaments.begin(), filaments.end(),
                        [&](const std::string& f) { return std::find(p.filaments.begin(), p.filaments.end(), f) != p.filaments.end(); }))
            out.push_back(p.folder);
    }
    return out;
}

namespace {

using Row = ForcaPlateRisk;

// What a list was made from: the plate, and each object with the time its support step finished (0 = not done).
struct Stamp
{
    int                                                plate = -1;
    std::vector<std::pair<const PrintObject*, size_t>> objects;
    bool operator==(const Stamp& o) const { return plate == o.plate && objects == o.objects; }
    bool operator!=(const Stamp& o) const { return !(*this == o); }
};

// The FFF print of a plate (-1: the current one, and plate_idx becomes its index), or null.
const Print* plate_print(int& plate_idx)
{
    Plater* p = wxGetApp().plater();
    if (!p || p->printer_technology() != ptFFF)
        return nullptr;
    PartPlateList& plates = p->get_partplate_list();
    if (plate_idx < 0)
        plate_idx = plates.get_curr_plate_index();
    PartPlate* plate = plates.get_plate(plate_idx);
    if (!plate)
        return nullptr;
    PrintBase* base = nullptr;
    plate->get_print(&base, nullptr, nullptr);
    return dynamic_cast<const Print*>(base);
}

Stamp stamp_of(int plate_idx)
{
    Stamp        s;
    const Print* print = plate_print(plate_idx);
    if (!print)
        return s;
    s.plate = plate_idx;
    for (const PrintObject* o : print->objects()) {
        const PrintStateBase::StateWithTimeStamp st = o->step_state_with_timestamp(posSupportMaterial);
        s.objects.emplace_back(o, st.state == PrintStateBase::DONE ? size_t(st.timestamp) : size_t(0));
    }
    return s;
}

Stamp current_stamp() { return stamp_of(-1); }

bool sliced(const Stamp& s)
{
    return !s.objects.empty() &&
           std::all_of(s.objects.begin(), s.objects.end(), [](const auto& o) { return o.second != 0; });
}

// The last plate checked, and its risks (GUI thread).
Stamp            s_checked;
std::vector<Row> s_checked_risks;

wxString mm(double v) { return wxString::Format("%.1f", v); }

wxString type_label(PrintRiskType t)
{
    switch (t) {
    case PrintRiskType::Overhang: return _L("Overhang");
    case PrintRiskType::LongBridge: return _L("Long bridge");
    case PrintRiskType::FloatingIsland: return _L("Floating island");
    }
    return {};
}

// The marker colour of each kind of risk.
ImU32 marker_colour(PrintRiskType t)
{
    switch (t) {
    case PrintRiskType::Overhang: return IM_COL32(232, 163, 61, 255);      // orange
    case PrintRiskType::LongBridge: return IM_COL32(61, 127, 232, 255);    // blue
    case PrintRiskType::FloatingIsland: return IM_COL32(217, 71, 63, 255); // red
    }
    return IM_COL32(128, 128, 128, 255);
}

wxString height_label(const PrintRisk& r)
{
    // TRN Heights in the Print Risks list: one layer, or the first and last layer of a risk, in mm.
    return r.first_layer == r.last_layer ? format_wxstr(_L("%1% mm"), wxString::Format("%.2f", r.z_min))
                                         : format_wxstr(_L("%1% to %2% mm"), wxString::Format("%.2f", r.z_min),
                                                        wxString::Format("%.2f", r.z_max));
}

wxString size_label(const PrintRisk& r)
{
    switch (r.type) {
    // TRN Print Risks list: how far an overhang reaches past the layer below it, in mm.
    case PrintRiskType::Overhang: return format_wxstr(_L("reaches %1% mm"), mm(r.distance));
    // TRN Print Risks list: the length of a bridge, in mm.
    case PrintRiskType::LongBridge: return format_wxstr(_L("%1% mm span"), mm(r.distance));
    // TRN Print Risks list: the area of a part that starts in mid-air, in square millimetres.
    case PrintRiskType::FloatingIsland: return format_wxstr(_L("%1% mm² area"), mm(r.area));
    }
    return {};
}

// What the risk is and what to try.
wxString advice(const Row& row)
{
    const PrintRisk& r = row.risk;
    wxString         out;
    switch (r.type) {
    case PrintRiskType::Overhang:
        // TRN %1%: distance in mm; %2%: the support threshold angle in degrees.
        out = format_wxstr(_L("This overhang reaches %1% mm past the layer below, farther than the support threshold angle "
                              "(%2%°) allows without support. It may curl up or droop."),
                           mm(r.distance), wxString::Format("%d", int(row.threshold_deg)));
        if (!r.supported && !row.support_on)
            out += " " + _L("Turn on support, or turn the model so this side faces the bed.");
        break;
    case PrintRiskType::LongBridge:
        // TRN %1%: the bridge's span; %2%: the Max bridge length setting; both in mm.
        out = format_wxstr(_L("This bridge spans about %1% mm, more than Max bridge length (%2% mm). Long bridges sag."),
                           mm(r.distance), mm(row.max_bridge));
        if (!r.supported && !row.support_on)
            out += " " + _L("Turn on support, or try Thick external bridges and a lower bridge speed.");
        break;
    case PrintRiskType::FloatingIsland:
        // TRN %1%: a height in mm.
        out = format_wxstr(_L("This part starts in mid-air at %1% mm, with nothing printed under it."),
                           wxString::Format("%.2f", r.z_min));
        if (!r.supported && !row.support_on)
            out += " " + _L("Without support it is printed onto air: turn on support, or change the orientation.");
        break;
    }
    if (r.supported)
        out += " " + _L("Support is printed under it.");
    else if (row.support_on)
        out += " " + _L("Support is on, but none is printed under it: check On build plate only, support blockers and the threshold angle.");
    return out;
}

// Moves the Preview's layer slider to `z`, showing the layers up to it.
void show_layer(double z)
{
    Plater* p = wxGetApp().plater();
    if (!p)
        return;
    GLCanvas3D* canvas = p->get_preview_canvas3D();
    if (!canvas)
        return;
    GCodeViewer& viewer = canvas->get_gcode_viewer();
    IMSlider*    layers = viewer.get_layers_slider();
    IMSlider*    moves  = viewer.get_moves_slider();
    const int    index  = layers ? forca_risk_slider_index(layers->GetValues(), z) : -1;
    if (index < 0)
        return;
    layers->SetHigherValue(index);
    if (layers->is_one_layer())
        layers->SetLowerValue(index);
    layers->set_as_dirty();
    if (moves) {
        moves->SetHigherValue(moves->GetMaxValue());
        moves->set_as_dirty();
    }
    canvas->set_as_dirty();
    canvas->request_extra_frame();
    canvas->post_event(SimpleEvent(wxEVT_PAINT));
}

// Redraws the Preview (its markers changed).
void redraw_preview()
{
    Plater*     p      = wxGetApp().plater();
    GLCanvas3D* canvas = p ? p->get_preview_canvas3D() : nullptr;
    if (!canvas)
        return;
    canvas->set_as_dirty();
    canvas->request_extra_frame();
    canvas->post_event(SimpleEvent(wxEVT_PAINT));
}

class ForcaPrintRiskDialog;
ForcaPrintRiskDialog* s_dialog = nullptr; // the open window (GUI thread only)

class ForcaPrintRiskDialog : public DPIDialog
{
public:
    explicit ForcaPrintRiskDialog(wxWindow* parent)
        : DPIDialog(parent, wxID_ANY, _L("Print Risks"), wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    {
        SetBackgroundColour(*wxWHITE);
        SetFont(Label::Body_14);
        const int pad  = FromDIP(10);
        auto*     root = new wxBoxSizer(wxVERTICAL);

        m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_status, 0, wxEXPAND | wxALL, pad);

        m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(680, 260)), wxLC_REPORT | wxLC_SINGLE_SEL);
        add_columns();
        root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

        auto* show_row = new wxBoxSizer(wxHORIZONTAL);
        m_show_markers = new ::CheckBox(this);
        m_show_markers->SetValue(true);
        show_row->Add(m_show_markers, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        show_row->Add(new wxStaticText(this, wxID_ANY, _L("Show numbered markers on the Preview")), 0, wxALIGN_CENTER_VERTICAL);
        root->Add(show_row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

        m_advice = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(m_advice, 0, wxEXPAND | wxALL, pad);

        // Forca Academy: how often the journal tagged this kind of problem, and its playbook.
        auto* academy = new wxBoxSizer(wxHORIZONTAL);
        m_journal     = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_show_prints = new HyperLink(this, _L("Show"));
        m_playbook    = new HyperLink(this, wxEmptyString);
        academy->Add(m_journal, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        academy->Add(m_show_prints, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(16));
        academy->Add(m_playbook, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(academy, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);

        m_rules = new wxStaticText(this, wxID_ANY,
                                   _L("Forca checks each layer against the one below it: an overhang reaches farther than "
                                      "the support threshold angle allows (30° when it is 0), a bridge is longer than Max "
                                      "bridge length, a floating island has nothing under it. The list follows each new "
                                      "slice of the plate."));
        m_rules->Wrap(FromDIP(660));
        root->Add(m_rules, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_refresh     = new Button(this, _L("Refresh"));
        m_refresh->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_close = new Button(this, _L("Close"));
        m_close->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        buttons->Add(m_refresh, 0);
        buttons->AddStretchSpacer(1);
        buttons->Add(m_close, 0);
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);

        m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh(); });
        m_close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Close(); });
        m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { select(e.GetIndex()); });
        m_show_markers->Bind(wxEVT_TOGGLEBUTTON, [](wxCommandEvent& e) {
            redraw_preview();
            e.Skip();
        });
        m_show_prints->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { show_prints(); });
        m_playbook->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (!m_playbook_file.empty())
                wxLaunchDefaultApplication(from_path(m_playbook_file));
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) {
            m_timer.Stop();
            s_dialog = nullptr;
            redraw_preview(); // the markers go
            Destroy();
        });
        // A new slice of the plate (or another plate) refreshes the list once it is sliced.
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (IsShown() && current_stamp() != m_stamp)
                refresh();
        });

        clear_selection();
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
        apply_colours();
        m_timer.Start(1000);
    }

    // A light / dark theme change while the window is open.
    void sys_color_changed()
    {
        wxGetApp().UpdateDlgDarkUI(this);
        wxGetApp().UpdateAllStaticTextDarkUI(this);
        apply_colours();
        Refresh();
    }

    // Reads the current plate's sliced objects again.
    void refresh()
    {
        m_list->DeleteAllItems();
        m_rows.clear();
        m_stamp = current_stamp();
        clear_selection();
        redraw_preview();

        Plater* p = wxGetApp().plater();
        if (!p || p->printer_technology() != ptFFF)
            return set_status(_L("Print risks are found for FFF prints only."));
        if (p->is_background_process_slicing())
            return set_status(_L("The plate is still slicing. The list follows when it is done."));
        if (!sliced(m_stamp))
            return set_status(_L("Slice the plate to see its risks."));

        forca_plate_risks(-1, m_rows);
        for (size_t n = 0; n < m_rows.size(); ++n) {
            const Row& row = m_rows[n];
            const long i   = m_list->InsertItem(m_list->GetItemCount(), wxString::Format("%d", int(n + 1)));
            m_list->SetItem(i, 1, type_label(row.risk.type));
            m_list->SetItem(i, 2, from_u8(row.object));
            m_list->SetItem(i, 3, height_label(row.risk));
            m_list->SetItem(i, 4, size_label(row.risk));
            m_list->SetItem(i, 5, row.risk.supported ? _L("Yes") : _L("No"));
        }
        load_journal();
        redraw_preview();
        set_status(m_rows.empty() ? _L("No risks found on this plate.")
                                  : format_wxstr(_L_PLURAL("%1% risk found on this plate.", "%1% risks found on this plate.",
                                                           m_rows.size()),
                                                 m_rows.size()));
    }

    // Selects a row (a click on its marker).
    void select_row(size_t index)
    {
        if (index >= m_rows.size())
            return;
        m_list->SetItemState(long(index), wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        m_list->EnsureVisible(long(index));
    }

    // The markers, numbered as the list: on the layers the Preview shows, at each risk's first layer.
    void render_markers(GLCanvas3D& canvas)
    {
        if (!m_show_markers->GetValue() || m_rows.empty() || current_stamp() != m_stamp)
            return;
        IMSlider* layers = canvas.get_gcode_viewer().get_layers_slider();
        if (!layers || layers->GetValues().empty())
            return;
        const double top = layers->GetHigherValueD();

        const Camera&             camera   = wxGetApp().plater()->get_camera();
        const Eigen::Matrix4d     m        = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        const std::array<int, 4>& viewport = camera.get_viewport();
        ImDrawList*               draw     = ImGui::GetBackgroundDrawList();
        const float               radius   = 0.75f * ImGui::GetFontSize();
        const ImVec2              mouse    = ImGui::GetIO().MousePos;
        const bool                free     = !ImGui::GetIO().WantCaptureMouse; // not over an ImGui window
        int                       hovered  = -1;
        for (size_t n = 0; n < m_rows.size(); ++n) {
            const Row& row = m_rows[n];
            if (row.risk.z_min > top + EPSILON)
                continue;
            for (const Vec3d& pt : row.points) {
                const Eigen::Vector4d c = m * Eigen::Vector4d(pt.x(), pt.y(), pt.z(), 1.);
                if (c.w() <= 0.)
                    continue;
                const ImVec2 s(float((0.5 + 0.5 * c.x() / c.w()) * viewport[2]), float((0.5 - 0.5 * c.y() / c.w()) * viewport[3]));
                if (s.x < 0.f || s.y < 0.f || s.x > float(viewport[2]) || s.y > float(viewport[3]))
                    continue;
                const bool selected = long(n) == m_selected;
                draw->AddCircleFilled(s, selected ? 1.25f * radius : radius, marker_colour(row.risk.type));
                draw->AddCircle(s, selected ? 1.25f * radius : radius, IM_COL32(255, 255, 255, 255), 0, selected ? 3.f : 1.5f);
                const std::string number = std::to_string(n + 1);
                const ImVec2      size   = ImGui::CalcTextSize(number.c_str());
                draw->AddText(ImVec2(s.x - 0.5f * size.x, s.y - 0.5f * size.y), IM_COL32(255, 255, 255, 255), number.c_str());
                const float dx = mouse.x - s.x, dy = mouse.y - s.y;
                if (free && dx * dx + dy * dy <= radius * radius)
                    hovered = int(n);
            }
        }
        if (hovered < 0)
            return;
        const Row& row = m_rows[size_t(hovered)];
        ImGui::SetTooltip("%d. %s, %s: %s", hovered + 1, into_u8(type_label(row.risk.type)).c_str(), row.object.c_str(),
                          into_u8(size_label(row.risk)).c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            wxGetApp().CallAfter([hovered]() {
                if (s_dialog)
                    s_dialog->select_row(size_t(hovered));
            });
    }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        m_refresh->Rescale();
        m_close->Rescale();
        m_show_markers->Rescale();
        m_list->ClearAll();
        add_columns();
        refresh();
        GetSizer()->SetSizeHints(this);
        Refresh();
    }

private:
    // The hand-picked colours, after the dark-mode pass that resets every label's.
    void apply_colours() { m_rules->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B"))); }

    void add_columns()
    {
        m_list->AppendColumn("#", wxLIST_FORMAT_LEFT, FromDIP(40));
        m_list->AppendColumn(_L("Risk"), wxLIST_FORMAT_LEFT, FromDIP(120));
        m_list->AppendColumn(_L("Object"), wxLIST_FORMAT_LEFT, FromDIP(170));
        m_list->AppendColumn(_L("Height"), wxLIST_FORMAT_LEFT, FromDIP(140));
        m_list->AppendColumn(_L("Size"), wxLIST_FORMAT_LEFT, FromDIP(120));
        // TRN Print Risks list column: is support printed under the risk ("Yes" / "No").
        m_list->AppendColumn(_L("Support"), wxLIST_FORMAT_LEFT, FromDIP(80));
    }

    void set_status(const wxString& text)
    {
        m_status->SetLabel(text);
        Layout();
    }

    void set_advice(const wxString& text)
    {
        m_advice->SetLabel(text);
        m_advice->Wrap(-1); // Wrap() skips a width it already used, even for a new label
        m_advice->Wrap(FromDIP(660));
    }

    void clear_selection()
    {
        m_selected = -1;
        set_advice(_L("Select a risk, here or its marker on the Preview, to see what to try and to show its layer."));
        m_journal->SetLabel(wxEmptyString);
        m_show_prints->Hide();
        m_playbook->Hide();
        Layout();
    }

    // The journal prints and the printer / filaments of this plate, for the Academy line.
    void load_journal()
    {
        m_prints.clear();
        m_printer.clear();
        m_filaments.clear();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        Plater*       p      = wxGetApp().plater();
        if (!forca_academy_enabled() || !bundle || !p)
            return;
        m_prints  = forca_academy_list_prints(forca_academy_dir());
        m_printer = bundle->physical_printers.has_selection() ? bundle->physical_printers.get_selected_printer_name()
                                                              : bundle->printers.get_edited_preset().name;
        if (PartPlate* plate = p->get_partplate_list().get_curr_plate())
            for (int id : plate->get_extruders(false))
                if (id >= 1 && size_t(id) <= bundle->filament_presets.size())
                    m_filaments.push_back(bundle->filament_presets[size_t(id - 1)]);
    }

    void select(long index)
    {
        if (index < 0 || size_t(index) >= m_rows.size())
            return;
        m_selected     = index;
        const Row& row = m_rows[size_t(index)];
        set_advice(advice(row));
        show_academy(row);
        Layout();
        redraw_preview();
        Plater* p = wxGetApp().plater();
        if (p && !p->is_preview_shown())
            wxGetApp().mainframe->select_tab(wxString(TAB_ID_PREVIEW));
        // After the tab switch has laid out the Preview and its slider.
        const double z = row.risk.z_min;
        wxGetApp().CallAfter([z]() { show_layer(z); });
    }

    void show_academy(const Row& row)
    {
        m_journal->SetLabel(wxEmptyString);
        m_show_prints->Hide();
        m_playbook->Hide();
        m_tagged.clear();
        m_playbook_file.clear();
        if (!forca_academy_enabled())
            return;
        const std::string tag   = forca_risk_academy_tag(row.risk.type);
        const wxString    label = forca_journal_tag_label(tag);
        m_tagged                = forca_risk_tagged_prints(m_prints, m_printer, m_filaments, tag);
        if (!m_tagged.empty()) {
            // TRN %1%: a number of recorded prints; %2%: a journal tag such as "Bridging".
            m_journal->SetLabel(format_wxstr(_L_PLURAL("Your journal: %1% print with this printer and filament was tagged %2%.",
                                                       "Your journal: %1% prints with this printer and filament were tagged %2%.",
                                                       m_tagged.size()),
                                             m_tagged.size(), label));
            m_tag_label = label;
            m_show_prints->Show();
        }
        const fs::path page = forca_academy_dir() / "playbooks" / (tag + ".md");
        boost::system::error_code ec;
        if (fs::is_regular_file(page, ec)) {
            m_playbook_file = page;
            // TRN %1%: a journal tag such as "Bridging"; opens the Forca Academy page about it.
            m_playbook->SetLabel(format_wxstr(_L("Open the %1% playbook"), label));
            m_playbook->Show();
        }
    }

    void show_prints()
    {
        if (m_tagged.empty())
            return;
        // TRN %1%: a journal tag such as "Bridging".
        ForcaPrintJournalDialog dlg(this, m_tagged, format_wxstr(_L("Prints tagged %1% with this printer and filament"), m_tag_label));
        dlg.ShowModal();
    }

    wxStaticText*                  m_status       = nullptr;
    wxListCtrl*                    m_list         = nullptr;
    ::CheckBox*                    m_show_markers = nullptr;
    wxStaticText*                  m_advice       = nullptr;
    wxStaticText*                  m_journal      = nullptr;
    HyperLink*                     m_show_prints  = nullptr;
    HyperLink*                     m_playbook     = nullptr;
    wxStaticText*                  m_rules        = nullptr;
    Button*                        m_refresh      = nullptr;
    Button*                        m_close        = nullptr;
    wxTimer                        m_timer;
    std::vector<Row>               m_rows;
    Stamp                          m_stamp;
    long                           m_selected = -1;
    std::vector<ForcaAcademyPrint> m_prints;
    std::string                    m_printer;
    std::vector<std::string>       m_filaments;
    std::vector<fs::path>          m_tagged;
    wxString                       m_tag_label;
    fs::path                       m_playbook_file;
};

} // namespace

bool forca_plate_risks(int plate_idx, std::vector<ForcaPlateRisk>& risks)
{
    risks.clear();
    Plater* p = wxGetApp().plater();
    if (!p || p->is_background_process_slicing())
        return false;
    const Stamp stamp = stamp_of(plate_idx);
    if (!sliced(stamp))
        return false;
    if (stamp != s_checked) {
        wxBusyCursor busy;
        s_checked_risks.clear();
        for (const auto& [o, done] : stamp.objects) {
            const PrintObjectConfig& c = o->config();
            for (PrintRisk& r : forca_print_risks(*o)) {
                Row row{ std::move(r), o->model_object()->name,
                         c.support_threshold_angle.value > 0 ? double(c.support_threshold_angle.value) : 30.,
                         c.max_bridge_length.value, c.enable_support.value, {} };
                // The slices are in the object's own coordinates; each instance's shift places them on the bed.
                const Vec2d centre = row.risk.bbox.center();
                for (const PrintInstance& inst : o->instances())
                    row.points.emplace_back(centre.x() + unscaled(inst.shift.x()), centre.y() + unscaled(inst.shift.y()), row.risk.z_min);
                s_checked_risks.push_back(std::move(row));
            }
        }
        std::stable_sort(s_checked_risks.begin(), s_checked_risks.end(),
                         [](const Row& a, const Row& b) { return a.risk.z_min < b.risk.z_min; });
        s_checked = stamp;
    }
    risks = s_checked_risks;
    return true;
}

void forca_show_print_risks(wxWindow* parent)
{
    if (!s_dialog)
        s_dialog = new ForcaPrintRiskDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe));
    s_dialog->refresh();
    s_dialog->Show();
    s_dialog->Raise();
}

void forca_print_risks_sys_color_changed()
{
    if (s_dialog)
        s_dialog->sys_color_changed();
}

void forca_render_print_risk_markers(GLCanvas3D& canvas)
{
    if (s_dialog && s_dialog->IsShown())
        s_dialog->render_markers(canvas);
}

}} // namespace Slic3r::GUI
