#ifndef slic3r_GLGizmoSupportInterfaceRegions_hpp_
#define slic3r_GLGizmoSupportInterfaceRegions_hpp_

// [regional-supports fork] Phase B slice B2a — support-interface region paint gizmo (skeleton).
// Paints per-facet region indices (MMU-style) into ModelVolume::support_interface_region_facets,
// the storage channel added in B1. B2a exposes a SINGLE fixed region (index 0 -> facet state 1) with
// no region list; B2b adds the add/select/delete region list with a filament swatch per row.
// Forked from GLGizmoFdmSupports but stripped of the support-preview threading it does not need.

#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <map>

namespace Slic3r::GUI {

class GLGizmoSupportInterfaceRegions : public GLGizmoPainterBase
{
public:
    GLGizmoSupportInterfaceRegions(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoSupportInterfaceRegions() override = default;

    void render_painter_gizmo() override;

    // Per-facet state is the 1-based filament index (MMU-style), so a painted "region" is fully described
    // by its interface filament — no separate region->filament mapping is stored. Two disjoint areas painted
    // with the same filament are one material region; the regional gap is shared (0) by decision.
    static const constexpr size_t EXTRUDERS_LIMIT = static_cast<size_t>(EnforcerBlockerType::ExtruderMax);

    // BBS-style tool hotkeys (C/S/F/G)
    bool on_key_down_select_tool_type(int keyCode);

protected:
    void on_render_input_window(float x, float y, float bottom_limit) override;
    std::string on_get_name() const override;
    void on_set_state() override;
    void render_tooltip_button(float x, float y);
    wxString handle_snapshot_action_name(bool shift_down, Button button_down) const override;

    std::string get_gizmo_entering_text() const override { return _u8L("Entering support-interface region painting"); }
    std::string get_gizmo_leaving_text() const override { return _u8L("Leaving support-interface region painting"); }
    std::string get_action_snapshot_name() const override { return _u8L("Support-interface region painting editing"); }

    // Active filament to paint with (0-based); facet state = idx + 1.
    size_t m_selected_filament_idx = 0;
    // Filament display colors, mirrored from the project (index i -> filament i+1). State 0 = unpainted (neutral).
    std::vector<ColorRGBA> m_filament_colors;

    // Left button paints the selected filament's state (idx + 1); right button erases.
    EnforcerBlockerType get_left_button_state_type()  const override { return EnforcerBlockerType(m_selected_filament_idx + 1); }
    EnforcerBlockerType get_right_button_state_type() const override { return EnforcerBlockerType(-1); }

    // BBS
    wchar_t m_current_tool = 0;

    // [regional-supports fork] Comfort features ported from the FDM-supports gizmo. The behavior flags
    // (m_vertical_only / m_horizontal_only / m_paint_on_overhangs_only / m_highlight_by_angle_threshold_deg) and
    // their painting logic already live in GLGizmoPainterBase; only the UI and the overhang-angle default/highlight
    // are added here. m_support_threshold_angle caches the object's auto-support angle so the "on overhangs only"
    // threshold defaults sensibly (-1 = needs (re)initializing this session).
    int m_support_threshold_angle = -1;

private:
    bool on_init() override;

    void update_model_object() override;
    void update_from_model_object(bool first_update) override;
    void tool_changed(wchar_t old_tool, wchar_t new_tool);
    // Object/global auto-support threshold angle (deg), or 0 when auto-support is off. Used to seed the
    // "Highlight overhangs" / "On overhangs only" angle. Ported from GLGizmoFdmSupports.
    int  get_selection_support_threshold_angle();

    void on_opening() override;
    void on_shutdown() override;
    PainterGizmoType get_painter_type() const override;

    // (Re)load filament colors from the project when the palette changed.
    void update_filament_colors();

    // This map holds all translated description texts, so they can be referenced during layout calculations.
    std::map<std::string, wxString> m_desc;

    std::vector<std::pair<wxString, wxString>> m_shortcuts_brush;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_bucket_fill;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_gap_fill;
};

} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoSupportInterfaceRegions_hpp_
