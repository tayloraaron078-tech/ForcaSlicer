#include "ForcaCalibModel.hpp"

#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "Model.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"

#include <algorithm>
#include <memory>

namespace Slic3r {

bool forca_hollow_to_outline(ModelObject& object, double wall_width, double height)
{
    if (wall_width <= 0. || height <= 0.)
        return false;

    // The object's model parts, in object coordinates.
    indexed_triangle_set parts;
    for (const ModelVolume* v : object.volumes)
        if (v->is_model_part()) {
            TriangleMesh m = v->mesh();
            m.transform(v->get_matrix());
            its_merge(parts, m.its);
        }
    if (parts.indices.empty())
        return false;

    // The outline of the first layer, shrunk by the wall width: what the negative volume removes.
    const BoundingBoxf3 bb = bounding_box(parts);
    const float         z  = float(bb.min.z() + std::min(0.5 * height, 0.5 * (bb.max.z() - bb.min.z())));
    const std::vector<ExPolygons> sections = slice_mesh_ex(parts, { z });
    if (sections.empty() || sections.front().empty())
        return false;
    const ExPolygons inner = offset_ex(sections.front(), -float(scale_(wall_width)));
    if (inner.empty())
        return false;

    // A prism of that area from below the object up to `height` above its bottom (only its overlap with the object
    // matters). Its top stays below the middle of the second layer, so only the first layer is hollowed.
    const double z0    = bb.min.z() - 1.;
    const double depth = 1. + height;
    Emboss::ProjectTransform project(std::make_unique<Emboss::ProjectZ>(depth / SCALING_FACTOR),
                                     Transform3d(Eigen::Translation<double, 3>(0., 0., z0) * Eigen::Scaling(SCALING_FACTOR)));
    indexed_triangle_set hole = Emboss::polygons2model(inner, project);
    if (hole.indices.empty())
        return false;
    if (its_volume(hole) < 0.f) // outward-facing normals, whatever the projection's winding
        its_flip_triangles(hole);

    ModelVolume* v = object.add_volume(TriangleMesh(std::move(hole)), ModelVolumeType::NEGATIVE_VOLUME, false);
    v->name        = "Forca: hollow (thin-walled test)";
    return true;
}

} // namespace Slic3r
