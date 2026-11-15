#include "PrintheadClearance.hpp"

#include "PrintConfig.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace Slic3r {

PrintheadProfile::PrintheadProfile(std::vector<Ring> rings) : m_rings(std::move(rings))
{
    std::sort(m_rings.begin(), m_rings.end(), [](const Ring &a, const Ring &b) { return a.radius < b.radius; });
    for (Ring &ring : m_rings) {
        ring.radius = std::max(0., ring.radius);
        ring.height = std::max(0., ring.height);
    }
}

PrintheadProfile PrintheadProfile::cone(double tip_radius, double cone_radius, double cone_height, double body_radius, double body_height)
{
    std::vector<Ring> rings{ { tip_radius, 0. }, { cone_radius, cone_height } };
    if (body_radius > 0. || body_height > 0.) {
        // The body may hang lower than the top of the cone (a fan duct beside the nozzle does).
        rings.push_back({ std::max(cone_radius, body_radius), body_height });
    }
    return PrintheadProfile(std::move(rings));
}

PrintheadProfile PrintheadProfile::from_config(const PrintConfig &config)
{
    // The margin is kept by taking the head to hang that much lower than it was measured.
    const double margin      = std::max(0., config.printhead_clearance_margin.value);
    const double cone_radius = 0.5 * config.nozzle_cone_diameter.value;
    const double cone_height = config.nozzle_cone_height.value - margin;
    if (cone_radius <= 0. || cone_height <= 0.)
        return PrintheadProfile();
    // Nonplanar paths put the centre of the nozzle tip on the surface, not the edge of its flat: the outline starts
    // at the nozzle's axis, and how deep the flat presses into a slope is the path's own business.
    const double tip_radius    = 0.;
    const double body_distance = config.printhead_body_distance.value;
    const double body_height   = config.printhead_body_height.value - margin;
    return body_distance > 0. && body_height > 0. ? cone(tip_radius, cone_radius, cone_height, body_distance, body_height) :
                                                    cone(tip_radius, cone_radius, cone_height);
}

double PrintheadProfile::height_at(double radius) const
{
    if (m_rings.empty())
        return 0.;
    if (radius <= m_rings.front().radius)
        return m_rings.front().height;
    for (size_t i = 1; i < m_rings.size(); ++i) {
        const Ring &inner = m_rings[i - 1];
        const Ring &outer = m_rings[i];
        if (radius <= outer.radius) {
            const double span = outer.radius - inner.radius;
            return span <= 0. ? outer.height : inner.height + (outer.height - inner.height) * (radius - inner.radius) / span;
        }
    }
    return m_rings.back().height;
}

double PrintheadProfile::keep_out_radius(double height_above_tip) const
{
    if (m_rings.empty() || height_above_tip > m_rings.back().height)
        return std::numeric_limits<double>::infinity();
    if (height_above_tip <= 0.)
        return 0.;
    // From the outside in: the first stretch of the outline that dips below that height.
    for (size_t i = m_rings.size() - 1; i > 0; --i) {
        const Ring &inner = m_rings[i - 1];
        const Ring &outer = m_rings[i];
        if (outer.height < height_above_tip)
            return outer.radius;
        if (inner.height < height_above_tip)
            return inner.radius + (outer.radius - inner.radius) * (height_above_tip - inner.height) / (outer.height - inner.height);
    }
    return m_rings.front().height < height_above_tip ? m_rings.front().radius : 0.;
}

bool PrintheadProfile::can_follow(double slope, double surface_height) const
{
    if (m_rings.empty() || slope < 0. || surface_height < 0.)
        return false;
    // Past the last ring the head stays at one height, and the surface reaches its full height somewhere.
    if (surface_height > m_rings.back().height)
        return false;
    const double rise = std::tan(slope);
    // Both outlines are straight between the rings and the point where the surface tops out, so those are the
    // only places the surface can come closest to the head. The surface starts at the edge of the flat tip.
    const double tip   = m_rings.front().radius;
    const auto   above = [rise, tip, surface_height](double radius) { return std::min(surface_height, rise * std::max(0., radius - tip)); };
    for (const Ring &ring : m_rings)
        if (above(ring.radius) > this->height_at(ring.radius) + 1e-9)
            return false;
    if (rise > 0.) {
        const double top = tip + surface_height / rise;
        if (surface_height > this->height_at(top) + 1e-9)
            return false;
    }
    return true;
}

double PrintheadProfile::max_slope(double surface_height) const
{
    if (! this->can_follow(0., surface_height))
        return 0.;
    // can_follow() only gets harder as the slope grows, so bisect between flat and vertical.
    double low  = 0.;
    double high = std::acos(0.); // 90 degrees
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (low + high);
        (this->can_follow(mid, surface_height) ? low : high) = mid;
    }
    return low;
}

} // namespace Slic3r
