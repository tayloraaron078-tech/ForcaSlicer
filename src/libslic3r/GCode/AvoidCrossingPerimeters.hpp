#ifndef slic3r_AvoidCrossingPerimeters_hpp_
#define slic3r_AvoidCrossingPerimeters_hpp_

#include "../ExPolygon.hpp"
#include "../EdgeGrid.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include <vector>
#include "libslic3r/MultiMaterialSegmentation.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace Slic3r {

// Forward declarations.
class GCode;
class Layer;
class Point;

class AvoidCrossingPerimeters
{
public:
    // Routing around the objects vs. inside a single object.
    void        use_external_mp(bool use = true) { m_use_external_mp = use; };
    bool        used_external_mp() { return m_use_external_mp; }
    void        use_external_mp_once()  { m_use_external_mp_once = true; }
    bool        used_external_mp_once() { return m_use_external_mp_once; }
    void        disable_once()          { m_disabled_once = true; }
    bool        disabled_once() const   { return m_disabled_once; }
    void        reset_once_modifiers()  { m_use_external_mp_once = false; m_disabled_once = false; }

    // Per-layer geometry that depends only on the layer: lslices shrunk by about half an external perimeter
    // width, with an edge grid, used to tell whether a travel stays inside an object. It is independent of
    // the instance being printed, so it is shared by all instances and may be computed ahead on another thread.
    struct LayerData {
        ExPolygons               lslices_offset;
        std::vector<BoundingBox> lslices_offset_bboxes;
        EdgeGrid::Grid           grid_lslice;
    };
    static std::shared_ptr<const LayerData> compute_layer_data(const Layer &layer);
    using PrecomputedLayerData = std::vector<std::pair<const Layer*, std::shared_ptr<const LayerData>>>;
    // Hands over data computed ahead for the layers about to be printed.
    void        set_precomputed_layer_data(PrecomputedLayerData &&data) { m_precomputed = std::move(data); }

    // Takes the layer data from the precomputed set, reuses the data of the previous call for the same layer, or computes it.
    void        init_layer(const Layer &layer);

    Polyline    travel_to(const GCode& gcodegen, const Point& point)
    {
        bool could_be_wipe_disabled;
        return this->travel_to(gcodegen, point, &could_be_wipe_disabled);
    }

    Polyline    travel_to(const GCode& gcodegen, const Point& point, bool* could_be_wipe_disabled);

    struct Boundary {
        // Collection of boundaries used for detection of crossing perimeters for travels
        Polygons                        boundaries;
        // Bounding box of boundaries
        BoundingBoxf                    bbox;
        // Precomputed distances of all points in boundaries
        std::vector<std::vector<float>> boundaries_params;
        // Used for detection of intersection between line and any polygon from boundaries
        EdgeGrid::Grid                  grid;

        void clear()
        {
            boundaries.clear();
            boundaries_params.clear();
        }
    };

private:
    bool           m_use_external_mp { false };
    // just for the next travel move
    bool           m_use_external_mp_once { false };
    // this flag disables reduce_crossing_wall just for the next travel move
    // we enable it by default for the first travel move in print
    bool           m_disabled_once { true };

    const Layer                      *m_layer_data_layer { nullptr };
    std::shared_ptr<const LayerData>  m_layer_data;
    PrecomputedLayerData              m_precomputed;
    // Store all needed data for travels inside object
    Boundary m_internal;
    // Store all needed data for travels outside object
    Boundary m_external;
};

} // namespace Slic3r

#endif // slic3r_AvoidCrossingPerimeters_hpp_
