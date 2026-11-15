#pragma once

#include <vector>

namespace Slic3r {

class PrintConfig;

// Forca: the printhead's outline seen from the side, turned around the nozzle axis. It answers the one question
// nonplanar printing has to ask before it lowers the nozzle next to material that is already printed: how high may
// that material stand, this far from the nozzle, before the head touches it.
//
// The outline is a list of rings: at `radius` mm from the nozzle axis the lowest part of the head is `height` mm
// above the nozzle tip. Between two rings the outline is a straight line (a cone), inside the first ring it is the
// first ring's height, and beyond the last ring it stays at the last ring's height (the carriage or the gantry).
class PrintheadProfile
{
public:
    struct Ring
    {
        double radius;
        double height;
    };

    PrintheadProfile() = default;
    // Rings in any order; they are sorted by radius. A ring may be lower than one nearer the axis: a fan duct
    // beside the nozzle hangs lower than the top of the nozzle's cone.
    explicit PrintheadProfile(std::vector<Ring> rings);

    // A plain nozzle: a flat tip of `tip_radius`, a cone widening to `cone_radius` at `cone_height` above the tip,
    // and above that a body (heater block, sock, fan duct) of `body_radius` whose lowest point is `body_height`
    // above the tip. Leave the body at 0 for a head that is all cone.
    static PrintheadProfile cone(double tip_radius, double cone_radius, double cone_height, double body_radius = 0., double body_height = 0.);

    // The outline from the printer's settings, lowered by the clearance margin and starting at the nozzle's axis.
    // Empty while the nozzle cone is not set: the outline is not known.
    static PrintheadProfile from_config(const PrintConfig &config);

    bool empty() const { return m_rings.empty(); }
    const std::vector<Ring>& rings() const { return m_rings; }

    // Height above the nozzle tip of the lowest part of the head at `radius` from the nozzle axis.
    double height_at(double radius) const;

    // Whether the nozzle can stand on a point while printed material at `radius` from it stands `height_above_tip`
    // higher than the tip.
    bool clears(double radius, double height_above_tip) const { return height_above_tip <= this->height_at(radius); }

    // Radius of the flat at the nozzle tip (the first ring).
    double tip_radius() const { return m_rings.empty() ? 0. : m_rings.front().radius; }

    // How far from the nozzle axis printed material must stay when it stands `height_above_tip` higher than the tip:
    // the largest radius at which the head is lower than that. 0 for material no higher than the tip, infinity
    // for material taller than the head beyond its last ring (or for an empty profile).
    double keep_out_radius(double height_above_tip) const;

    // Whether the nozzle can follow a surface that rises at `slope` (radians from horizontal) and is `surface_height`
    // mm tall in total: uphill of the nozzle the surface stands r * tan(slope) higher at distance r, but never more
    // than its own height. An empty profile follows nothing.
    bool can_follow(double slope, double surface_height) const;

    // The steepest slope (radians) the nozzle can follow on a surface of that total height. 0 when even a flat
    // surface of that height is out of reach.
    double max_slope(double surface_height) const;

    // No surface taller than this can be followed at any slope: the height of the head beyond its last ring.
    double max_surface_height() const { return m_rings.empty() ? 0. : m_rings.back().height; }

private:
    std::vector<Ring> m_rings;
};

} // namespace Slic3r
