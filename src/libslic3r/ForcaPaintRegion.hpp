#pragma once

// Forca: which facets of a mesh a painting request covers (Forca AI's forca_paint_regions). A facet is in the region when
// its centroid, in world coordinates, is inside the shape and, if asked, it faces down within an angle of straight down.
// Also what each kind of painting means for a part, and painting whole facets of it.

#include "Model.hpp"
#include "Point.hpp"
#include "TriangleSelector.hpp"

#include <optional>
#include <string>
#include <vector>

struct indexed_triangle_set;

namespace Slic3r {

// One of a part's painted layers and the state a request gives its facets.
struct ForcaPaintKind
{
    FacetsAnnotation ModelVolume::*layer         = nullptr;
    EnforcerBlockerType           state          = EnforcerBlockerType::NONE; // NONE clears
    bool                          needs_filament = false;                     // the state is a filament slot, still to be set
};

// The meaning of a request's `paint` word: filament (colour painting), support_enforcer, support_blocker,
// support_interface, seam_enforcer, seam_blocker, clear_filament, clear_support, clear_support_interface, clear_seam.
// Nothing for any other word.
std::optional<ForcaPaintKind> forca_paint_kind(const std::string& paint);

// Paints whole facets of `volume` (indices into its mesh) on `layer`, keeping what else is painted there.
void forca_paint_facets(ModelVolume& volume, FacetsAnnotation ModelVolume::*layer, const std::vector<int>& facets,
                        EnforcerBlockerType state);

struct ForcaPaintRegion
{
    enum class Shape { All, Box, Sphere };
    Shape  shape    = Shape::All;
    Vec3d  center   = Vec3d::Zero(); // world mm
    Vec3d  size     = Vec3d::Zero(); // box: its size; sphere: diameter = the smallest of the three; mm
    double down_deg = -1;            // >= 0: only facets whose normal is within this angle of straight down
};

// The indices of the facets of `its` (placed by `to_world`) inside `region`, ascending.
std::vector<int> forca_facets_in_region(const indexed_triangle_set& its, const Transform3d& to_world, const ForcaPaintRegion& region);

} // namespace Slic3r
