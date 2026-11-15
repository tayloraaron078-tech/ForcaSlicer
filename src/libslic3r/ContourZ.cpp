#include "Exception.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Line.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "SLA/IndexedMesh.hpp"
#include "libslic3r.h"
#include <Eigen/Core>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <vector>
#include <initializer_list>
#include <limits>
#include <algorithm>
#include <math.h>
#include <string>
#include <utility>
#include "Config.hpp"
#include "Polyline.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr);

static double follow_slope_down(double angle_rad, double dist)
{
	return -dist * std::sin(angle_rad);
}

static double slope_from_normal(const Eigen::Vector3d& normal)
{
    // Ensure the normal is normalized
    Eigen::Vector3d n = normal.normalized();

    // Compute angle between normal and z-axis
    double angle_rad = std::acos(std::abs(n.z()));  // angle between normal and vertical
	return angle_rad;
}

// Forca: the layer whose top surface a point of a nonplanar ironing pass lies on. Neighbouring points mostly lie
// on the same layer, so `last` is tried first.
static const Layer* nonplanar_ironing_base(const Layer &layer, const std::vector<Layer::NonplanarIroningSource> &sources, const Point &point, const Layer *last)
{
    for (const Layer::NonplanarIroningSource &source : sources)
        if (source.layer == last && source.area.contains(point))
            return last;
    for (const Layer::NonplanarIroningSource &source : sources)
        if (source.area.contains(point))
            return source.layer;
    // Between two areas (they are a hair apart) the point stays on the layer of the point before it.
    for (const ExPolygon &slice : layer.lslices)
        if (slice.contains(point))
            return &layer;
    return last;
}

// Forca: how far above this layer's print Z the model's surface is at a point under the nozzle tip of a nonplanar
// ironing pass (negative: below). Minus infinity where the point tells nothing: off the model, or under material
// that is not a top surface of a lower layer.
static double nonplanar_ironing_surface(const Layer &layer, const sla::IndexedMesh &mesh, double x, double y, const Layer *base)
{
    const sla::IndexedMesh::hit_result hit = mesh.query_ray_hit({x, y, base->slice_z + mesh.ground_level()}, {0.0, 0.0, 1.0});
    if (! hit.is_hit())
        return -std::numeric_limits<double>::infinity();
    const double d = hit.distance() - (base->print_z - base->slice_z);
    if (d < -base->height || d > base->height + 0.03)
        // Material goes on above: on this layer it stands as high as the layer.
        return base == &layer ? 0. : -std::numeric_limits<double>::infinity();
    return d + base->print_z - layer.print_z;
}

static bool contour_extrusion_path(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionPath &path)
{
    if (path.role() != erTopSolidInfill && path.role() != erIroning && path.role() != erExternalPerimeter && path.role() != erPerimeter) {
		return false;
	}
	// Forca: the paths of a nonplanar top surface get their Z from contour_nonplanar_shell().
	if (path.nonplanar_shell)
		return false;
	
	Layer *layer = region->layer();
	coordf_t min_z = region->region().config().zaa_min_z;

	const Points3 &points = path.polyline.points;
	double resolution_mm = 0.1;

	const bool   nonplanar = path.role() == erIroning && ! layer->nonplanar_ironing_sources.empty();
	// The layer whose top surface the current point lies on: this layer, but for a nonplanar ironing pass.
	const Layer *base      = layer;
	const double tip_radius = 0.5 * layer->object()->print()->config().nozzle_tip_diameter.value;

	double minimize_perimeter_height_angle = region->region().config().zaa_minimize_perimeter_height;

	Pointf3s contoured_points;
	bool was_contoured = false;

    if (points.size() < 2) {
        // Safety check. The loop below does not handle paths with less than two points correctly.
        return false;
    }

    for (Points3::const_iterator it = points.begin(); it != points.end()-1; ++it) {
		Vec2d p1d(unscale_(it->x()), unscale_(it->y()));
		Vec2d p2d(unscale_((it+1)->x()), unscale_((it+1)->y()));
		Linef line(p1d, p2d);

		double length_mm = line.length();
		int num_segments = int(std::ceil(length_mm / resolution_mm));
		Vec2d delta = line.vector();

        if (num_segments == 0) {
            continue;
        }

        for (int i = 0; i < num_segments + 1; i++) {
            Vec2d p = p1d + delta * i / num_segments;

            coordf_t x = p.x();
			coordf_t y = p.y();

			// Forca: a nonplanar ironing pass also runs over the top surfaces of lower layers. Such a point is
			// placed like an ironing point of the layer it lies on, and then moved down to that layer.
			if (nonplanar)
				base = nonplanar_ironing_base(*layer, layer->nonplanar_ironing_sources, Point::new_scale(x, y), base);
			const coordf_t mesh_slice_z = base->slice_z + mesh.ground_level();
			const coordf_t height = base->height;

			sla::IndexedMesh::hit_result hit_up = mesh.query_ray_hit({x, y, mesh_slice_z}, {0.0, 0.0, 1.0});

			double d = hit_up.is_hit() ? hit_up.distance() - (base->print_z - base->slice_z) : 0.;

			double max_up = min_z;
			double min_down = -(height - min_z);
			double half_width = path.width / 2.0;
			if (path.role() == erIroning) {
				max_up = height;
				min_down = -(height + 0.1);
			}

            if (is_perimeter(path.role()) && hit_up.is_hit()) {
				const Vec3d &normal = hit_up.normal();
                double slope_rad     = slope_from_normal(normal);
                double slope_degrees = slope_rad * 180.0 / M_PI;

                if (d > min_down && minimize_perimeter_height_angle > 0 && minimize_perimeter_height_angle < slope_degrees) {
                    double adjustment = follow_slope_down(slope_rad, half_width);
                    if (adjustment > 0) {
                        throw RuntimeError("ContourZ: got positive adjustment");
                    }
                    d += adjustment;
                    if (d < min_down) {
                        d = min_down;
                    }
                }
            }

            if (d < -height || d > max_up + 0.03) {
                // this point is too far from the mesh edge, probably because this is not a top surface. Do not contour it.
                d = 0;
            }

            if (d < min_down) {
                d = min_down;
            } else if (d > max_up) {
                d = max_up;
            }

            if (is_perimeter(path.role()) && d > 0) {
                // do not increase height of perimeters as this may create an appearance of a seam
                d = 0;
            }

            d += base->print_z - layer->print_z;

            // Forca: the nozzle's tip is flat. On a slope its centre is on the surface, where ironing works, and its
            // uphill edge presses into the slope by what the slope rises over the tip's radius. In a hollow (the foot
            // of a slope) the surface under the tip's edge stands higher than that, and there the tip is lifted so
            // that it presses no deeper than on the slope itself.
            if (nonplanar && tip_radius > 0.) {
                double pressed_in = 0.;
                if (hit_up.is_hit()) {
                    const Vec3d &normal = hit_up.normal();
                    if (std::abs(normal.z()) > EPSILON)
                        pressed_in = tip_radius * std::hypot(normal.x(), normal.y()) / std::abs(normal.z());
                }
                for (int direction = 0; direction < 8; ++direction) {
                    const double angle = 0.25 * M_PI * direction;
                    const double edge_x = x + tip_radius * std::cos(angle), edge_y = y + tip_radius * std::sin(angle);
                    d = std::max(d, nonplanar_ironing_surface(*layer, mesh, edge_x, edge_y,
                                    nonplanar_ironing_base(*layer, layer->nonplanar_ironing_sources, Point::new_scale(edge_x, edge_y), base)) - pressed_in);
                }
            }

            if (std::abs(d) > EPSILON) {
				was_contoured = true;
			}

            Vec3d new_point = {p.x(), p.y(), d};

            if (contoured_points.size() >= 2 && i != 0) {
                // Normally, if the new point is collinear with the last two points, we do not add
                // it to the list of contoured points. Instead we update the last point to be the
                // new point. This is to avoid creating a large number of very short segments.
                //
                // However, if the new point corresponds to a point in the original path (i == 0),
                // even if it is collinear, we add it anyway. This is to avoid creating a degenerate
                // polygon with only two points, which may cause issues in downstream code.
                double dist = Linef3::distance_to_infinite_squared(new_point, contoured_points[contoured_points.size() - 2],
                                                                   contoured_points[contoured_points.size() - 1]);
                if (dist < EPSILON * EPSILON) {
                    contoured_points[contoured_points.size() - 1] = new_point;
                    continue;
                }
            }

            contoured_points.push_back(new_point);
        }
    }

    if (!was_contoured) {
		return false;
	}

	Polyline3 polyline;
	for (const Vec3d &point : contoured_points) {
		polyline.append(Point3(scale_(point.x()), scale_(point.y()), scale_(point.z())));
	}

	path.polyline = std::move(polyline);
	path.z_contoured = true;
	return true;
}

static void contour_extrusion_multipath(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionMultiPath &multipath) 
{
	for (ExtrusionPath &path : multipath.paths) {
		contour_extrusion_path(region, mesh, path);
	}
}

static void contour_extrusion_loop(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionLoop &loop) 
{
	for (ExtrusionPath &path : loop.paths) {
		contour_extrusion_path(region, mesh, path);
	}
}

static void contour_extrusion_entitiy_collection(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntityCollection &collection)
{
	for (ExtrusionEntity *entity : collection.entities) {
		contour_extrusion_entity(region, mesh, entity);
	}
}

static void contour_extrusion_entity(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntity *extr)
{
	const ExtrusionPathSloped *sloped = dynamic_cast<const ExtrusionPathSloped*>(extr);
	if (sloped != nullptr) {
		throw RuntimeError("ExtrusionPathSloped not implemented");
		return;
	}

	ExtrusionMultiPath *multipath = dynamic_cast<ExtrusionMultiPath*>(extr);
	if (multipath != nullptr) {
		contour_extrusion_multipath(region, mesh, *multipath);
		return;
	}

	ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(extr);
	if (path != nullptr) {
		contour_extrusion_path(region, mesh, *path);
		return;
	}

	ExtrusionLoop *loop = dynamic_cast<ExtrusionLoop*>(extr);
	if (loop != nullptr) {
		contour_extrusion_loop(region, mesh, *loop);
		return;
	}

	const ExtrusionLoopSloped *loop_sloped = dynamic_cast<const ExtrusionLoopSloped*>(extr);
	if (loop_sloped != nullptr) {
		throw RuntimeError("ExtrusionLoopSloped not implemented");
		return;
	}

	ExtrusionEntityCollection *collection = dynamic_cast<ExtrusionEntityCollection*>(extr);
	if (collection != nullptr) {
		contour_extrusion_entitiy_collection(region, mesh, *collection);
		return;
	}

	throw RuntimeError("ContourZ: ExtrusionEntity type not implemented: " + std::string(typeid(*extr).name()));
	return;
}

static void handle_extrusion_collection(LayerRegion *region, const sla::IndexedMesh &mesh, ExtrusionEntityCollection &collection, std::initializer_list<ExtrusionRole> roles) {
    for (ExtrusionEntity* extr : collection.entities) {
        if (!contains(roles, extr->role())) {
			continue;
		}

		contour_extrusion_entity(region, mesh, extr);
    }
}

// Forca: the paths of a nonplanar top surface, put on the model's surface. A path runs over the steps of several
// layers. It is cut where it passes from one step to the next, because the amount to extrude follows the height above
// the layer under the step, and the pieces are marked so that the nozzle stays down from one to the next.
static void contour_nonplanar_shell(const Layer &layer, const sla::IndexedMesh &mesh, ExtrusionEntityCollection &collection)
{
    const double    RESOLUTION = 0.1; // mm
    ExtrusionEntitiesPtr out;
    for (ExtrusionEntity *entity : collection.entities) {
        ExtrusionPath *path = dynamic_cast<ExtrusionPath*>(entity);
        if (path == nullptr || ! path->nonplanar_shell || path->z_contoured || path->polyline.points.size() < 2) {
            out.emplace_back(entity);
            continue;
        }
        std::vector<std::pair<const Layer*, Pointf3s>> runs;
        const Layer *base   = &layer;
        double       last_d = 0.;
        bool         first  = true;
        const Points3 &points = path->polyline.points;
        for (size_t i = 1; i < points.size(); ++i) {
            const Vec2d a(unscale_(points[i - 1].x()), unscale_(points[i - 1].y())), b(unscale_(points[i].x()), unscale_(points[i].y()));
            const int   segments = std::max(1, int(std::ceil((b - a).norm() / RESOLUTION)));
            for (int k = i == 1 ? 0 : 1; k <= segments; ++k) {
                const Vec2d p = a + (b - a) * (double(k) / segments);
                base = nonplanar_ironing_base(layer, layer.nonplanar_shell_sources, Point::new_scale(p.x(), p.y()), base);
                const double d = nonplanar_ironing_surface(layer, mesh, p.x(), p.y(), base);
                // This layer's own step is part of the skin as well; elsewhere on this layer the path is between steps.
                const bool on_step = base != &layer ||
                    std::any_of(layer.nonplanar_shell_sources.begin(), layer.nonplanar_shell_sources.end(),
                                [&layer, &p](const Layer::NonplanarIroningSource &source) {
                                    return source.layer == &layer && source.area.contains(Point::new_scale(p.x(), p.y()));
                                });
                if (std::isfinite(d) && (on_step || first))
                    last_d = d;
                // Before the first point on a step the path has no height yet: it takes that step's.
                if (first && on_step && std::isfinite(d)) {
                    for (auto &run : runs)
                        for (Vec3d &point : run.second)
                            point.z() = d;
                    first = false;
                }
                const Vec3d point(p.x(), p.y(), last_d);
                if (runs.empty() || runs.back().first != base) {
                    Pointf3s start;
                    if (! runs.empty())
                        start.emplace_back(runs.back().second.back());
                    runs.emplace_back(base, std::move(start));
                }
                Pointf3s &run = runs.back().second;
                if (run.size() >= 2 && k != 0 && k != segments &&
                    Linef3::distance_to_infinite_squared(point, run[run.size() - 2], run.back()) < EPSILON * EPSILON)
                    run.back() = point;
                else
                    run.emplace_back(point);
            }
        }
        size_t made = 0;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (runs[i].second.size() < 2)
                continue;
            ExtrusionPath *piece = new ExtrusionPath(*path);
            Polyline3      polyline;
            for (const Vec3d &point : runs[i].second)
                polyline.append(Point3(scale_(point.x()), scale_(point.y()), scale_(point.z())));
            piece->polyline            = std::move(polyline);
            piece->z_contoured         = true;
            piece->z_contour_shift     = float(layer.print_z - runs[i].first->print_z);
            piece->nonplanar_continues = true;
            out.emplace_back(piece);
            ++made;
        }
        if (made == 0) {
            out.emplace_back(entity);
            continue;
        }
        static_cast<ExtrusionPath*>(out.back())->nonplanar_continues = false;
        delete entity;
    }
    collection.entities = std::move(out);
}

void Layer::make_contour_z(const sla::IndexedMesh &mesh)
{
	// Forca: the nonplanar top surface that this layer prints.
	if (! this->nonplanar_shell_sources.empty())
		for (LayerRegion *region : this->regions())
			for (ExtrusionEntity *entity : region->fills.entities)
				if (auto *collection = dynamic_cast<ExtrusionEntityCollection*>(entity))
					contour_nonplanar_shell(*this, mesh, *collection);

	for (LayerRegion *region : this->regions()) {
        if (!region->region().config().zaa_enabled) {
            // Forca: a nonplanar ironing pass gets its Z without Z contouring too.
            if (! this->nonplanar_ironing_sources.empty())
                handle_extrusion_collection(region, mesh, region->fills, {erIroning});
            continue;
        }

        handle_extrusion_collection(region, mesh, region->fills, {erTopSolidInfill, erIroning, erPerimeter, erExternalPerimeter, erMixed});
        handle_extrusion_collection(region, mesh, region->perimeters, {erPerimeter, erExternalPerimeter, erMixed});
    }
}
} // namespace Slic3r
