// Forca's own version (version.inc FORCA_VERSION): the update check compares it with Forca's GitHub release tags.
#include <catch2/catch_all.hpp>

#include "libslic3r/Semver.hpp"
#include "libslic3r/libslic3r.h" // FORCA_VERSION (via the generated libslic3r_version.h)

#include <string>

using namespace Slic3r;

namespace {
Semver ver(const std::string& s)
{
    const auto v = Semver::parse(s);
    REQUIRE(v.has_value());
    return *v;
}
} // namespace

TEST_CASE("Forca's version is a valid semantic version", "[ForcaVersion]")
{
    CHECK(Semver::parse(FORCA_VERSION).has_value());
}

TEST_CASE("Forca alpha releases sort in release order", "[ForcaVersion]")
{
    CHECK(ver("0.1.0-alpha.1") < ver("0.1.0-alpha.2"));
    CHECK(ver("0.1.0-alpha.2") < ver("0.1.0-alpha.10"));
    CHECK(ver("0.1.0-alpha.10") < ver("0.1.0-beta.1"));
    CHECK(ver("0.1.0-beta.1") < ver("0.1.0"));
    CHECK(ver("0.1.0") < ver("0.1.1"));
    CHECK_FALSE(ver("0.1.0-alpha.1") < ver("0.1.0-alpha.1"));
}
