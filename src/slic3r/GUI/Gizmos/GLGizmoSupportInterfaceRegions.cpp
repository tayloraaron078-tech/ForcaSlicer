#include "GLGizmoSupportInterfaceRegions.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp" // Slic3r::save_object_mesh(ModelObject&)

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "GLGizmoUtils.hpp"

#include <glad/gl.h>

#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {

GLGizmoSupportInterfaceRegions::GLGizmoSupportInterfaceRegions(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
    m_tool_type   = ToolType::BRUSH;
    m_cursor_type = TriangleSelector::CursorType::CIRCLE;
}

std::string GLGizmoSupportInterfaceRegions::on_get_name() const
{
    return _u8L("Support Interface Regions");
}

bool GLGizmoSupportInterfaceRegions::on_init()
{
    m_desc["clipping_of_view"] = _L("Section view");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["remove_all"]       = _L("Erase all");
    m_desc["tool_type"]        = _L("Tool type");
    m_desc["smart_fill_angle"] = _L("Smart fill angle");
    m_desc["gap_area"]         = _L("Gap area");
    m_desc["perform"]          = _L("Apply");
    m_desc["on_overhangs_only"]= _L("On overhangs only");
    m_desc["highlight_by_angle"]= _L("Highlight overhangs");

    const wxString ctrl  = GUI::shortkey_ctrl_prefix();
    const wxString alt   = GUI::shortkey_alt_prefix();
    const wxString shift = GUI::shortkey_shift_prefix();

    std::pair<wxString, wxString> paint_shortcut    = {_L("Left mouse button"),         _L("Paint region")};
    std::pair<wxString, wxString> erase_shortcut    = {_L("Right mouse button"),        _L("Erase")};
    std::pair<wxString, wxString> remove_shortcut   = {shift + _L("Left mouse button"), _L("Erase")};
    std::pair<wxString, wxString> clipping_shortcut = {alt + _L("Mouse wheel"),         m_desc["clipping_of_view"]};

    m_shortcuts_brush = {
        paint_shortcut,
        erase_shortcut,
        remove_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["cursor_size"]},
        clipping_shortcut
    };
    m_shortcuts_bucket_fill = {
        paint_shortcut,
        erase_shortcut,
        remove_shortcut,
        {ctrl + _L("Mouse wheel"), m_desc["smart_fill_angle"]},
        clipping_shortcut
    };
    m_shortcuts_gap_fill = {
        {ctrl + _L("Mouse wheel"), m_desc["gap_area"]}
    };

    return true;
}

void GLGizmoSupportInterfaceRegions::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// BBS
bool GLGizmoSupportInterfaceRegions::on_key_down_select_tool_type(int keyCode)
{
    switch (keyCode) {
    case 'F': m_current_tool = ImGui::FillButtonIcon;   break;
    case 'S': m_current_tool = ImGui::SphereButtonIcon; break;
    case 'C': m_current_tool = ImGui::CircleButtonIcon; break;
    case 'G': m_current_tool = ImGui::GapFillIcon;      break;
    default:  return false;
    }
    return true;
}

void GLGizmoSupportInterfaceRegions::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == On) {
        // Re-seed the overhang-highlight angle from the object on the next input-window render.
        m_support_threshold_angle = -1;
    } else if (get_state() == Off) {
        // [regional-supports fork] Clear the overhang slope highlight so it doesn't persist after the gizmo closes.
        m_parent.use_slope(false);
        ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
        if (mo)
            Slic3r::save_object_mesh(*mo);
    }
}

// [regional-supports fork] Ported from GLGizmoFdmSupports. Returns the object's (or global) auto-support
// threshold angle so "On overhangs only" defaults to the same faces auto-support would target; 0 when
// auto-support is off, -1 when there is no selection.
int GLGizmoSupportInterfaceRegions::get_selection_support_threshold_angle()
{
    auto sel_info = m_c->selection_info();
    if (sel_info == nullptr || sel_info->model_object() == nullptr)
        return -1;

    const DynamicPrintConfig& obj_cfg = sel_info->model_object()->config.get();
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    bool enable_support = obj_cfg.option("enable_support") ? obj_cfg.opt_bool("enable_support") : glb_cfg.opt_bool("enable_support");
    SupportType support_type = obj_cfg.option("support_type") ? obj_cfg.opt_enum<SupportType>("support_type") : glb_cfg.opt_enum<SupportType>("support_type");
    int support_threshold_angle = obj_cfg.option("support_threshold_angle") ? obj_cfg.opt_int("support_threshold_angle") : glb_cfg.opt_int("support_threshold_angle");

    bool auto_support = enable_support && is_auto(support_type);
    return auto_support ? support_threshold_angle : 0;
}

void GLGizmoSupportInterfaceRegions::on_render_input_window(float x, float y, float bottom_limit)
{
    if (! m_c->selection_info() || ! m_c->selection_info()->model_object())
        return;

    float scale = m_parent.get_scale();
#ifdef WIN32
    int dpi = get_dpi_for_window(wxGetApp().GetTopWindow());
    scale *= (float) dpi / (float) DPI_DEFAULT;
#endif // WIN32

    wchar_t old_tool = m_current_tool;

    // [regional-supports fork] Seed the "on overhangs only" angle from the object's auto-support threshold the
    // first time the window renders this session (m_support_threshold_angle reset to -1 on gizmo open).
    int support_threshold_angle = get_selection_support_threshold_angle();
    if (m_support_threshold_angle == -1) {
        m_highlight_by_angle_threshold_deg = float(support_threshold_angle);
        m_parent.set_slope_normal_angle(90.f - m_highlight_by_angle_threshold_deg);
    }
    m_support_threshold_angle = support_threshold_angle;

    const float approx_height = m_imgui->scaled(15.f);
    y = std::min(y, bottom_limit - approx_height);

    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float clipping_slider_left = m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f);
    const float cursor_slider_left   = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float smart_fill_left      = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float gap_area_left        = m_imgui->calc_text_size(m_desc.at("gap_area")).x + m_imgui->scaled(1.5f);
    const float highlight_left       = m_imgui->calc_text_size(m_desc.at("highlight_by_angle")).x + m_imgui->scaled(1.5f);
    const float empty_button_width   = m_imgui->calc_button_size("").x;

    const float sliders_left_width = std::max(highlight_left, std::max(cursor_slider_left, std::max(clipping_slider_left, std::max(smart_fill_left, gap_area_left))));
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;
    const float space_size         = m_imgui->get_style_scaling() * 8;
    const float sliders_width      = m_imgui->scaled(7.0f);
    const float drag_left_width    = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;
    const float max_tooltip_width  = ImGui::GetFontSize() * 20.0f;

    // Filament palette: pick which interface filament to paint with. The painted facet stores this
    // filament's 1-based index as its state, so a "region" is defined by its filament (see class note).
    update_filament_colors();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Interface filament"));
    const int n_filaments = std::min((int) m_filament_colors.size(), (int) EXTRUDERS_LIMIT);
    if (m_selected_filament_idx >= (size_t) std::max(n_filaments, 1))
        m_selected_filament_idx = 0;
    for (int i = 0; i < n_filaments; ++i) {
        if (i != 0) ImGui::SameLine();
        const ColorRGBA& c = m_filament_colors[i];
        const ImVec4 col(c.r(), c.g(), c.b(), 1.0f);
        const bool selected = (m_selected_filament_idx == (size_t) i);
        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.f);
        }
        const std::string btn_id = "##si_filament_" + std::to_string(i);
        if (ImGui::ColorButton(btn_id.c_str(), col,
                               ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
                               ImVec2(24.f * scale, 24.f * scale)))
            m_selected_filament_idx = (size_t) i;
        if (selected) {
            ImGui::PopStyleVar(1);
            ImGui::PopStyleColor(1);
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Filament") + " " + std::to_string(i + 1), max_tooltip_width);
    }

    ImGui::Separator();

    // Tool type row (Circle / Sphere / Fill / Gap fill).
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("tool_type"));

    std::array<wchar_t, 4> tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 4> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wxString, 4> tool_tips = { _L("Circle"), _L("Sphere"), _L("Fill"), _L("Gap Fill") };

    for (int i = 0; i < (int) tool_ids.size(); i++) {
        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.3f));

        bool is_active = m_current_tool == tool_ids[i];
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f * scale, 4.f * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        ImGui::PushStyleColor(ImGuiCol_Button, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, is_active ? ImVec4(0.f, .59f, .53f, .25f) : ImVec4(.6f, .6f, .6f, .2f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, is_active ? ImVec4(0.f, .59f, .53f, .30f) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_BorderActive, is_active ? ImGuiWrapper::COL_ORCA : ImVec4(0, 0, 0, 0));
        bool btn_clicked = m_imgui->glyph_button(icons[i], ImVec2(16.f * scale, 16.f * scale));
        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1f));

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = (m_current_tool == ImGui::SphereButtonIcon) ? TriangleSelector::CursorType::SPHERE : TriangleSelector::CursorType::CIRCLE;
        m_tool_type   = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

        // [regional-supports fork] Constrain brush strokes to a straight vertical/horizontal line (mutually
        // exclusive). The actual stroke-locking is done in GLGizmoPainterBase during Dragging.
        if (m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only) && m_vertical_only)
            m_horizontal_only = false;
        if (m_imgui->bbl_checkbox(_L("Horizontal"), m_horizontal_only) && m_horizontal_only)
            m_vertical_only = false;
    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("smart_fill_angle"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, "%.f", 1.0f, true))
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
    } else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type   = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("gap_area"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");

        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Gap fill", UndoRedo::SnapshotType::GizmoAction);
            for (int i = 0; i < (int) m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                ts->update_selector_triangles();
                ts->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }
    }

    // [regional-supports fork] "On overhangs only" restricts painting to faces steeper than the highlight angle;
    // the "Highlight overhangs" slider sets that angle and shows the overhang faces in the 3D view. Ported from
    // the FDM-supports gizmo. The overhang filtering is applied in GLGizmoPainterBase (it passes
    // m_highlight_by_angle_threshold_deg to the selection when m_paint_on_overhangs_only is set). Not for Gap-fill.
    if (m_current_tool != ImGui::GapFillIcon) {
        m_imgui->bbl_checkbox(m_desc.at("on_overhangs_only"), m_paint_on_overhangs_only);
        if (ImGui::IsItemHovered())
            m_imgui->tooltip(_L("Paint only on overhang faces, per the Highlight overhangs angle below."), max_tooltip_width);
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("highlight_by_angle"));
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    if (m_imgui->bbl_slider_float_style("##angle_threshold_deg", &m_highlight_by_angle_threshold_deg, 0.f, 90.f, "%.f", 1.0f, true,
                                        _L("Highlight faces according to overhang angle."))) {
        m_parent.set_slope_normal_angle(90.f - m_highlight_by_angle_threshold_deg);
        if (! m_parent.is_using_slope()) {
            m_parent.use_slope(true);
            m_parent.set_as_dirty();
        }
    }

    ImGui::Separator();

    // Section view (clipping) slider.
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    } else {
        if (m_imgui->button(m_desc.at("clipping_of_view"))) {
            wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
        }
    }
    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    bool slider_clp = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width + sliders_left_width);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    bool drag_clp = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");
    if (slider_clp || drag_clp)
        m_c->object_clipper()->set_position_by_ratio(clp_dist, true);

    ImGui::Separator();

    render_tooltip_button(x, y);

    // Erase-all (enabled only if any model-part volume carries region paint).
    bool any_painted = false;
    for (const ModelVolume* mv : m_c->selection_info()->model_object()->volumes)
        if (mv->is_support_interface_region_painted()) { any_painted = true; break; }

    ImGui::SameLine();
    m_imgui->disabled_begin(! any_painted);
    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Erase all region paint", UndoRedo::SnapshotType::GizmoAction);
        ModelObject* mo  = m_c->selection_info()->model_object();
        int          idx = -1;
        for (ModelVolume* mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();

    ImGui::SameLine();
    GLGizmoUtils::begin_right_aligned_buttons({_L("Done")});
    if (m_imgui->button(_L("Done")))
        m_parent.reset_all_gizmos();

    GizmoImguiEnd();
    ImGui::PopStyleVar(1); // ImGuiStyleVar_FramePadding
    ImGuiWrapper::pop_toolbar_style();
}

void GLGizmoSupportInterfaceRegions::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

void GLGizmoSupportInterfaceRegions::render_tooltip_button(float x, float y)
{
    auto get_shortcuts = [this]() -> std::vector<std::pair<wxString, wxString>> {
        switch (m_current_tool) {
        case ImGui::CircleButtonIcon:
        case ImGui::SphereButtonIcon: return m_shortcuts_brush;
        case ImGui::FillButtonIcon:   return m_shortcuts_bucket_fill;
        case ImGui::GapFillIcon:      return m_shortcuts_gap_fill;
        default:                      return {};
        }
    };
    GLGizmoUtils::render_tooltip_button(m_imgui, m_parent, get_shortcuts(), x, y);
}

void GLGizmoSupportInterfaceRegions::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->support_interface_region_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoSupportInterfaceRegions::update_filament_colors()
{
    if (Plater* plater = wxGetApp().plater())
        m_filament_colors = plater->get_extruders_colors();
}

void GLGizmoSupportInterfaceRegions::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    update_filament_colors();

    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_triangle_selectors.clear();
    if (mo == nullptr || m_filament_colors.empty())
        return;

    // State 0 = unpainted (neutral); state k (1-based) = filament k's color.
    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.reserve(m_filament_colors.size() + 1);
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.insert(ebt_colors.end(), m_filament_colors.begin(), m_filament_colors.end());

    const EnforcerBlockerType max_ebt = (EnforcerBlockerType) std::min(m_filament_colors.size(), (size_t) EnforcerBlockerType::ExtruderMax);

    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        m_triangle_selectors.back()->deserialize(mv->support_interface_region_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
    }
}

void GLGizmoSupportInterfaceRegions::on_opening()
{
}

void GLGizmoSupportInterfaceRegions::on_shutdown()
{
}

PainterGizmoType GLGizmoSupportInterfaceRegions::get_painter_type() const
{
    return PainterGizmoType::SUPPORT_INTERFACE_REGION;
}

wxString GLGizmoSupportInterfaceRegions::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    if (shift_down)
        return _L("Unselect all");
    if (button_down == Button::Left)
        return _L("Paint support-interface region");
    return _L("Erase support-interface region");
}

} // namespace Slic3r::GUI
