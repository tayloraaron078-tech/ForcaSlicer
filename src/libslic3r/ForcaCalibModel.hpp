#ifndef slic3r_ForcaCalibModel_hpp_
#define slic3r_ForcaCalibModel_hpp_

#include <cstddef>
#include <vector>

namespace Slic3r {

class ModelObject;

// Forca: gives a solid calibration model the first layer of a thin-walled outline, like Bambu Studio's Max Volumetric
// Speed test model: adds a negative volume that removes everything more than `wall_width` (mm) inside the outline of
// the object's first layer, from below the object up to `height` (mm) above its bottom -- the first layer only. That
// layer then has an inside edge, so a brim of type "outer and inner" goes on BOTH sides of the wall, which keeps
// grippy / shrinking filaments such as PCTG from lifting. Above it the model stays solid, so vase mode still prints a
// single wall (a hollow all the way up would give it two). The wall follows the test's line width, so any nozzle works.
// Returns false (and changes nothing) when the object is empty, too thin to hollow, or `height` is not positive.
bool forca_hollow_to_outline(ModelObject& object, double wall_width, double height);

// Forca: hole size calibration (Calibration Wizard "Hole size", issue #10). A bar with one round hole per size in
// forca_hole_plate_sizes(), smallest at the notched end; the user measures each hole and forca_hole_fit() turns that
// into X-Y hole compensation (mm) and X-Y hole compensation (% of size).
const std::vector<double>& forca_hole_plate_sizes(); // designed diameters (mm), smallest first
// Adds the plate to an empty object: the bar as a model part, the holes and the notch as negative volumes (so they are
// real holes when sliced and hole compensation applies to them). The bar's corner is at the origin, holes along X.
void forca_build_hole_plate(ModelObject& object);

struct ForcaHoleFit
{
    enum class Error { None, TooFew, Implausible };
    Error  error       = Error::TooFew;
    double percent     = 0.; // X-Y hole compensation (% of size) that makes the holes come out as designed
    double mm          = 0.; // X-Y hole compensation (mm, per edge) to go with it
    double worst_error = 0.; // largest leftover error after compensation (mm); 0 with only two holes
    size_t used        = 0;  // holes measured (a measurement <= 0 means "not measured")
    bool   clamped     = false; // a value was limited to the setting's range (-5..5 %, -2..2 mm)
};
// Fits measured = k * designed + e over the measured holes (printed with both hole settings at 0) and solves for the
// compensation: percent = (1 / k - 1) * 100, mm = -e / (2 k). Needs two different measured sizes; a measurement more
// than max(1 mm, 20 %) off its designed size is treated as a typo (Implausible).
ForcaHoleFit forca_hole_fit(const std::vector<double>& designed, const std::vector<double>& measured);

} // namespace Slic3r

#endif // slic3r_ForcaCalibModel_hpp_
