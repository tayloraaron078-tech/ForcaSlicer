#pragma once

// Forca: the Print Risks window (roadmap #12, risk map): the risks ForcaPrintRisk finds on the current plate's sliced
// objects, as a list and as numbered markers on the Preview. Selecting a risk shows what to try, its Forca Academy
// playbook and journal prints, and moves the Preview's layer slider to where it starts.

#include "ForcaAcademy.hpp"
#include "libslic3r/ForcaPrintRisk.hpp"
#include "libslic3r/Point.hpp"

#include <boost/filesystem/path.hpp>

#include <string>
#include <vector>

class wxWindow;

namespace Slic3r { namespace GUI {

class GLCanvas3D;

// The layer slider position of the first value at or above `z` (the slider's values are layer print_z, ascending): the
// last position when `z` is above them all, -1 when there are none.
int forca_risk_slider_index(const std::vector<double>& zs, double z);

// The journal tag and playbook (playbooks/<tag>.md) of a risk: "bridging" for a bridge, "supports" for the others.
std::string forca_risk_academy_tag(PrintRiskType type);

// The recorded prints tagged `tag`, on `printer` (empty: any printer) with any of `filaments`, newest first.
std::vector<boost::filesystem::path> forca_risk_tagged_prints(const std::vector<ForcaAcademyPrint>& prints,
                                                              const std::string& printer,
                                                              const std::vector<std::string>& filaments,
                                                              const std::string& tag);

// One risk of a sliced plate, with what its explanation needs from its object.
struct ForcaPlateRisk
{
    PrintRisk          risk;
    std::string        object;
    double             threshold_deg = 0;     // the support threshold angle the overhangs were measured with
    double             max_bridge    = 0;     // max_bridge_length, mm
    bool               support_on    = false; // the object has support turned on
    std::vector<Vec3d> points;                // one per instance, on the bed at the risk's first layer, mm
};

// The risks of a plate's sliced objects (-1: the current plate), lowest first. GUI thread. Each slice is checked once
// and kept, so the Print Risks window, Forca AI and "Before you print" share the work. False (and no risks) when the
// plate is not FFF, is slicing, or is not sliced up to support yet.
bool forca_plate_risks(int plate_idx, std::vector<ForcaPlateRisk>& risks);

// Opens the Print Risks window (one at a time; not modal), or refreshes it and brings it to the front.
void forca_show_print_risks(wxWindow* parent);
// The open Print Risks window follows a light / dark theme change (from MainFrame::on_sys_color_changed).
void forca_print_risks_sys_color_changed();
// The risk markers on the Preview, while the window is open (from the preview canvas's ImGui overlay pass).
void forca_render_print_risk_markers(GLCanvas3D& canvas);

}} // namespace Slic3r::GUI
