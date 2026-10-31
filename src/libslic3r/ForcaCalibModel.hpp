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

// Forca: weight-based flow calibration (Calibration Wizard, Flow Rate method "Weigh a plate"). A solid plate whose
// weight the slice predicts; the user weighs the print on a 0.01 g scale (about 6-7 g of PLA, so 0.2 % resolution).
constexpr double FORCA_WEIGHT_PLATE_X = 40.; // mm
constexpr double FORCA_WEIGHT_PLATE_Y = 40.;
constexpr double FORCA_WEIGHT_PLATE_H = 3.;
// Adds the plate to an empty object, its corner at the origin.
void forca_build_flow_weight_plate(ModelObject& object);
// The flow ratio that makes the printer deliver what the slice asks for. The slice's grams already include the flow
// ratio the plate was printed with (GCode::_extrude), so measured / expected is the printer's own extrusion error and
// the new ratio is its inverse, expected / measured, whatever the old ratio was. 0 when a weight isn't positive.
double forca_flow_ratio_from_weight(double expected_g, double measured_g);
// Filament density (g/cm3) from the weight of 1 m of filament (g) and its diameter (mm): a 1 m length is pi r^2 cm3
// (r in mm). 0 when a value isn't positive.
double forca_density_from_meter_weight(double grams_per_meter, double diameter_mm);

} // namespace Slic3r

#endif // slic3r_ForcaCalibModel_hpp_
