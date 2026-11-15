#include "ForcaPaintRegion.hpp"

#include "Model.hpp"
#include "Point.hpp"
#include "TriangleSelector.hpp"
#include "libslic3r.h" // PI

#include <admesh/stl.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

std::vector<int> forca_facets_in_region(const indexed_triangle_set& its, const Transform3d& to_world, const ForcaPaintRegion& r)
{
    std::vector<int>      out;
    const Matrix3d        normal_matrix = to_world.linear().inverse().transpose();
    const double          cos_limit     = r.down_deg >= 0 ? std::cos(r.down_deg * PI / 180.) : 0.;
    const Vec3d           half          = 0.5 * r.size;
    const double          radius        = 0.5 * r.size.minCoeff();
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const stl_triangle_vertex_indices& f = its.indices[i];
        const Vec3d a = to_world * its.vertices[f[0]].cast<double>();
        const Vec3d b = to_world * its.vertices[f[1]].cast<double>();
        const Vec3d c = to_world * its.vertices[f[2]].cast<double>();
        const Vec3d centroid = (a + b + c) / 3.;
        if (r.shape == ForcaPaintRegion::Shape::Box && ((centroid - r.center).cwiseAbs() - half).maxCoeff() > 0.)
            continue;
        if (r.shape == ForcaPaintRegion::Shape::Sphere && (centroid - r.center).norm() > radius)
            continue;
        if (r.down_deg >= 0) {
            // The facet's own normal (from its winding), in world coordinates.
            const Vec3f n_local = (its.vertices[f[1]] - its.vertices[f[0]]).cross(its.vertices[f[2]] - its.vertices[f[0]]);
            const Vec3d n       = (normal_matrix * n_local.cast<double>());
            if (n.norm() <= 0. || -n.normalized().z() < cos_limit)
                continue;
        }
        out.push_back(int(i));
    }
    return out;
}

std::optional<ForcaPaintKind> forca_paint_kind(const std::string& paint)
{
    using EBT = EnforcerBlockerType;
    if (paint == "filament")
        return ForcaPaintKind{ &ModelVolume::mmu_segmentation_facets, EBT::NONE, true };
    if (paint == "clear_filament")
        return ForcaPaintKind{ &ModelVolume::mmu_segmentation_facets };
    if (paint == "support_enforcer")
        return ForcaPaintKind{ &ModelVolume::supported_facets, EBT::ENFORCER };
    if (paint == "support_blocker")
        return ForcaPaintKind{ &ModelVolume::supported_facets, EBT::BLOCKER };
    if (paint == "clear_support")
        return ForcaPaintKind{ &ModelVolume::supported_facets };
    if (paint == "support_interface")
        return ForcaPaintKind{ &ModelVolume::support_interface_region_facets, EBT::NONE, true };
    if (paint == "clear_support_interface")
        return ForcaPaintKind{ &ModelVolume::support_interface_region_facets };
    if (paint == "seam_enforcer")
        return ForcaPaintKind{ &ModelVolume::seam_facets, EBT::ENFORCER };
    if (paint == "seam_blocker")
        return ForcaPaintKind{ &ModelVolume::seam_facets, EBT::BLOCKER };
    if (paint == "clear_seam")
        return ForcaPaintKind{ &ModelVolume::seam_facets };
    return std::nullopt;
}

void forca_paint_facets(ModelVolume& volume, FacetsAnnotation ModelVolume::*layer, const std::vector<int>& facets,
                        EnforcerBlockerType state)
{
    if (facets.empty())
        return;
    TriangleSelector selector(volume.mesh());
    selector.deserialize((volume.*layer).get_data(), false);
    for (int i : facets)
        selector.set_facet(i, state);
    (volume.*layer).set(selector);
}

} // namespace Slic3r
