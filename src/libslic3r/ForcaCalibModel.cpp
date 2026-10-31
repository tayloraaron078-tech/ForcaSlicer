#include "ForcaCalibModel.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Emboss.hpp"
#include "ExtrusionEntity.hpp"
#include "LocalesUtils.hpp"
#include "Model.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <utility>

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

// Hole plate geometry (mm): holes HOLE_GAP apart edge to edge, HOLE_MARGIN from the bar's edges; the notch is a slot
// in the middle of the left (small-hole) end.
static constexpr double HOLE_PLATE_H = 5.;
static constexpr double HOLE_MARGIN  = 6.;
static constexpr double HOLE_GAP     = 5.;
static constexpr double NOTCH_W      = 3.;
static constexpr double NOTCH_D      = 3.;

const std::vector<double>& forca_hole_plate_sizes()
{
    static const std::vector<double> sizes{ 3., 5., 8., 12., 20. };
    return sizes;
}

void forca_build_hole_plate(ModelObject& object)
{
    const std::vector<double>& sizes = forca_hole_plate_sizes();
    double length = 2. * HOLE_MARGIN + HOLE_GAP * double(sizes.size() - 1);
    for (double d : sizes)
        length += d;
    const double width = 2. * HOLE_MARGIN + sizes.back();

    ModelVolume* bar = object.add_volume(make_cube(length, width, HOLE_PLATE_H), ModelVolumeType::MODEL_PART, false);
    bar->name        = "Forca hole plate";

    // Cutters reach past the bar's top and bottom so the holes go cleanly through.
    double x = HOLE_MARGIN;
    for (double d : sizes) {
        TriangleMesh cutter(its_make_cylinder(d / 2., HOLE_PLATE_H + 2., PI / 180.)); // 1 degree facets
        cutter.translate(float(x + d / 2.), float(width / 2.), -1.f);
        ModelVolume* hole = object.add_volume(std::move(cutter), ModelVolumeType::NEGATIVE_VOLUME, false);
        hole->name        = "Forca hole " + float_to_string_decimal_point(d) + " mm";
        x += d + HOLE_GAP;
    }
    TriangleMesh notch = make_cube(NOTCH_D + 1., NOTCH_W, HOLE_PLATE_H + 2.);
    notch.translate(-1.f, float((width - NOTCH_W) / 2.), -1.f);
    object.add_volume(std::move(notch), ModelVolumeType::NEGATIVE_VOLUME, false)->name = "Forca notch (small holes)";
}

void forca_build_flow_weight_plate(ModelObject& object)
{
    object.add_volume(make_cube(FORCA_WEIGHT_PLATE_X, FORCA_WEIGHT_PLATE_Y, FORCA_WEIGHT_PLATE_H), ModelVolumeType::MODEL_PART, false)
        ->name = "Forca flow weight plate";
}

double forca_flow_ratio_from_weight(double expected_g, double measured_g)
{
    return expected_g > 0. && measured_g > 0. ? expected_g / measured_g : 0.;
}

double forca_density_from_meter_weight(double grams_per_meter, double diameter_mm)
{
    return grams_per_meter > 0. && diameter_mm > 0. ? grams_per_meter / (PI * diameter_mm * diameter_mm / 4.) : 0.;
}

ForcaHoleFit forca_hole_fit(const std::vector<double>& designed, const std::vector<double>& measured)
{
    ForcaHoleFit fit;
    std::vector<std::pair<double, double>> points; // (designed, measured)
    for (size_t i = 0; i < designed.size() && i < measured.size(); ++i) {
        const double d = designed[i], m = measured[i];
        if (!(m > 0.) || !(d > 0.))
            continue;
        if (std::abs(m - d) > std::max(1., 0.2 * d)) {
            fit.error = ForcaHoleFit::Error::Implausible;
            return fit;
        }
        points.emplace_back(d, m);
    }
    fit.used = points.size();
    double d_mean = 0., m_mean = 0.;
    for (const auto& [d, m] : points) {
        d_mean += d;
        m_mean += m;
    }
    if (points.size() < 2)
        return fit;
    d_mean /= double(points.size());
    m_mean /= double(points.size());
    double sdd = 0., sdm = 0.;
    for (const auto& [d, m] : points) {
        sdd += (d - d_mean) * (d - d_mean);
        sdm += (d - d_mean) * (m - m_mean);
    }
    if (sdd < 1e-9) // all the same size: no slope
        return fit;
    const double k = sdm / sdd;
    const double e = m_mean - k * d_mean;
    if (k <= 0.5) { // not a plausible print of these holes
        fit.error = ForcaHoleFit::Error::Implausible;
        return fit;
    }
    for (const auto& [d, m] : points)
        fit.worst_error = std::max(fit.worst_error, std::abs(m - (k * d + e)));

    const double percent = (1. / k - 1.) * 100.;
    const double mm      = -e / (2. * k);
    fit.percent = std::clamp(percent, -5., 5.);
    fit.mm      = std::clamp(mm, -2., 2.);
    fit.clamped = fit.percent != percent || fit.mm != mm;
    fit.error   = ForcaHoleFit::Error::None;
    return fit;
}

} // namespace Slic3r
