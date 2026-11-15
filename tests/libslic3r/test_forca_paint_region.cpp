// Forca: the facets a Forca AI painting request covers (ForcaPaintRegion).
#include <catch2/catch_test_macros.hpp>

#include "libslic3r/ForcaPaintRegion.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <admesh/stl.h>

#include <cstddef>
#include <vector>

using namespace Slic3r;

namespace {

// A 10 mm cube from (0, 0, 0): 12 facets, 2 on each side.
const indexed_triangle_set& cube()
{
    static const indexed_triangle_set its = make_cube(10, 10, 10).its;
    return its;
}

} // namespace

TEST_CASE("The whole mesh is every facet", "[ForcaPaintRegion]")
{
    CHECK(forca_facets_in_region(cube(), Transform3d::Identity(), {}).size() == 12);
}

TEST_CASE("Facing down picks only the bottom of a cube", "[ForcaPaintRegion]")
{
    ForcaPaintRegion r;
    r.down_deg                 = 30;
    const std::vector<int> out = forca_facets_in_region(cube(), Transform3d::Identity(), r);
    REQUIRE(out.size() == 2);
    for (int i : out)
        for (int v = 0; v < 3; ++v)
            CHECK(cube().vertices[cube().indices[size_t(i)][v]].z() == 0.f);
}

TEST_CASE("A box or sphere picks the facets whose centre is inside it, in world coordinates", "[ForcaPaintRegion]")
{
    // The cube moved up 100 mm: its top facets' centres are at z = 110.
    Transform3d up = Transform3d::Identity();
    up.translate(Vec3d(0, 0, 100));

    ForcaPaintRegion top;
    top.shape  = ForcaPaintRegion::Shape::Box;
    top.center = Vec3d(5, 5, 110);
    top.size   = Vec3d(20, 20, 2);
    CHECK(forca_facets_in_region(cube(), up, top).size() == 2);
    // Without the move, nothing of the cube is up there.
    CHECK(forca_facets_in_region(cube(), Transform3d::Identity(), top).empty());

    ForcaPaintRegion ball;
    ball.shape  = ForcaPaintRegion::Shape::Sphere;
    ball.center = Vec3d(5, 5, 105);
    ball.size   = Vec3d(100, 100, 100);
    CHECK(forca_facets_in_region(cube(), up, ball).size() == 12);
    ball.size = Vec3d(1, 1, 1); // no facet centre within 0.5 mm of the cube's centre
    CHECK(forca_facets_in_region(cube(), up, ball).empty());
}

TEST_CASE("Each paint word names its own painted layer, and only filament painting and interface regions take a slot",
          "[ForcaPaintRegion]")
{
    const auto filament = forca_paint_kind("filament");
    REQUIRE(filament.has_value());
    CHECK(filament->layer == &ModelVolume::mmu_segmentation_facets);
    CHECK(filament->needs_filament);

    const auto interface_region = forca_paint_kind("support_interface");
    REQUIRE(interface_region.has_value());
    CHECK(interface_region->layer == &ModelVolume::support_interface_region_facets);
    CHECK(interface_region->needs_filament);

    const auto blocker = forca_paint_kind("seam_blocker");
    REQUIRE(blocker.has_value());
    CHECK(blocker->layer == &ModelVolume::seam_facets);
    CHECK(blocker->state == EnforcerBlockerType::BLOCKER);
    CHECK_FALSE(blocker->needs_filament);

    const auto clear = forca_paint_kind("clear_filament");
    REQUIRE(clear.has_value());
    CHECK(clear->layer == &ModelVolume::mmu_segmentation_facets);
    CHECK(clear->state == EnforcerBlockerType::NONE);
    CHECK_FALSE(clear->needs_filament);

    CHECK_FALSE(forca_paint_kind("colour").has_value());
}

TEST_CASE("Painting facets with a filament colours them, keeps earlier painting and can be cleared", "[ForcaPaintRegion]")
{
    Model        model;
    ModelVolume* v = model.add_object()->add_volume(make_cube(10, 10, 10));
    const auto   layer = &ModelVolume::mmu_segmentation_facets;

    forca_paint_facets(*v, layer, { 0, 1 }, EnforcerBlockerType::Extruder2);
    CHECK(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder2));
    CHECK_FALSE(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder3));
    // The other painted layers are untouched.
    CHECK(v->supported_facets.empty());
    CHECK(v->seam_facets.empty());

    forca_paint_facets(*v, layer, { 2 }, EnforcerBlockerType::Extruder3);
    CHECK(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder2));
    CHECK(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder3));

    forca_paint_facets(*v, layer, { 0, 1 }, EnforcerBlockerType::NONE);
    CHECK_FALSE(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder2));
    CHECK(v->mmu_segmentation_facets.has_facets(*v, EnforcerBlockerType::Extruder3));
}
