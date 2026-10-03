#ifndef slic3r_ForcaCalibModel_hpp_
#define slic3r_ForcaCalibModel_hpp_

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

} // namespace Slic3r

#endif // slic3r_ForcaCalibModel_hpp_
