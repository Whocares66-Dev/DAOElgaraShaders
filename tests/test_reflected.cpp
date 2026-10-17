// The sun's light as the level sends it back (src/Reflected.h): patches made
// from a map's depths, and what they send a place.
#include "Reflected.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace
{

constexpr int kSize = 128;
constexpr int kStep = 16;
constexpr int kTexels = 2048;

// A map of the sun straight overhead, 64 m across, its depth from 10 m up
// (0) to 10 m down (1): the ground at height 0 is at depth 0.5.
cascades::Matrix Overhead()
{
    return {1.0F / 32, 0, 0, 0, 0, 1.0F / 32, 0, 0, 0, 0, -1.0F / 20, 0.5F, 0, 0, 0, 1};
}

// The depths of the ground z = slope * x under it.
std::vector<float> Ground(float slope)
{
    std::vector<float> depths(static_cast<std::size_t>(kSize) * kSize);
    for (int j = 0; j < kSize; ++j)
    {
        for (int i = 0; i < kSize; ++i)
        {
            const float x = ((2.0F * (static_cast<float>(kStep * i) + (kStep / 2.0F)) / kTexels) - 1) * 32;
            depths[(static_cast<std::size_t>(j) * kSize) + i] = 0.5F - (slope * x / 20);
        }
    }
    return depths;
}

// Colours of `share` of each light, one a texel.
std::vector<std::array<float, 3>> Grey(float share)
{
    return std::vector<std::array<float, 3>>(static_cast<std::size_t>(kSize) * kSize, {share, share, share});
}

} // namespace

TEST_CASE("the patches of flat ground face up, half a metre apart", "[reflected]")
{
    const std::optional<reflected::Patches> patches =
        reflected::Make(Ground(0), Grey(1), kSize, kStep, kTexels, Overhead());
    REQUIRE(patches);
    CHECK(patches->spacing == Catch::Approx(0.5F).margin(1e-4));
    const reflected::Patch &patch = patches->grid[(64 * kSize) + 64];
    CHECK(patch.at[2] == Catch::Approx(0).margin(1e-3));
    CHECK(patch.facing[2] == Catch::Approx(1).margin(1e-4));
}

TEST_CASE("a slope's patches face across it, and nothing drawn is no patch", "[reflected]")
{
    std::vector<float> depths = Ground(0.5F);
    depths[(10 * kSize) + 10] = 1; // nothing drawn there
    const std::optional<reflected::Patches> patches =
        reflected::Make(depths, Grey(1), kSize, kStep, kTexels, Overhead());
    REQUIRE(patches);
    const reflected::Patch &patch = patches->grid[(64 * kSize) + 64];
    CHECK(patch.facing[0] == Catch::Approx(-0.5F / std::sqrt(1.25F)).margin(1e-3));
    CHECK(patch.facing[2] == Catch::Approx(1 / std::sqrt(1.25F)).margin(1e-3));
    const reflected::Patch &none = patches->grid[(10 * kSize) + 10];
    CHECK(none.facing[0] == 0);
    CHECK(none.facing[1] == 0);
    CHECK(none.facing[2] == 0);
}

TEST_CASE("sunlit ground sends a place over it its light from below, as much as the disc it sees", "[reflected]")
{
    const std::optional<reflected::Patches> patches =
        reflected::Make(Ground(0), Grey(1), kSize, kStep, kTexels, Overhead());
    REQUIRE(patches);
    dao::core::probes::Coefficients light{};
    REQUIRE(reflected::Gather(*patches, {0.1F, 0.2F, 1}, 10, {1, 1, 1}, light));
    // Ground of radiance 1 seen from 1 m up to 10 m out: a solid angle of
    // 2 pi (1 - 1 / sqrt(101)), and of it, by the cosine below, pi (100 / 101).
    // Each patch is taken as seen on its axis, which comes to some 2 and 3
    // percent under these with patches of half a metre.
    const float pi = std::numbers::pi_v<float>;
    const float solid = 2 * pi * (1 - (1 / std::sqrt(101.0F)));
    const float below = pi * 100 / 101;
    CHECK(light[0][0] == Catch::Approx(0.282095F * solid).epsilon(0.04));
    CHECK(light[2][0] == Catch::Approx(-0.488603F * below).epsilon(0.04));
    CHECK(light[1][0] == Catch::Approx(0).margin(0.02));
    CHECK(light[3][0] == Catch::Approx(0).margin(0.02));
}

TEST_CASE("ground sends nothing to a place under it, nor past the map", "[reflected]")
{
    const std::optional<reflected::Patches> patches =
        reflected::Make(Ground(0), Grey(1), kSize, kStep, kTexels, Overhead());
    REQUIRE(patches);
    dao::core::probes::Coefficients light{};
    REQUIRE(reflected::Gather(*patches, {0, 0, -1}, 10, {1, 1, 1}, light));
    CHECK(light[0][0] == 0);
    CHECK_FALSE(reflected::Gather(*patches, {40, 0, 1}, 10, {1, 1, 1}, light));
}

TEST_CASE("a patch gives back its own colour's share of the sun", "[reflected]")
{
    std::vector<std::array<float, 3>> colours = Grey(1);
    for (std::array<float, 3> &colour : colours)
    {
        colour = {0.5F, 0.25F, 0};
    }
    const std::optional<reflected::Patches> white =
        reflected::Make(Ground(0), Grey(1), kSize, kStep, kTexels, Overhead());
    const std::optional<reflected::Patches> brown =
        reflected::Make(Ground(0), colours, kSize, kStep, kTexels, Overhead());
    REQUIRE(white);
    REQUIRE(brown);
    dao::core::probes::Coefficients fromWhite{};
    dao::core::probes::Coefficients fromBrown{};
    REQUIRE(reflected::Gather(*white, {0, 0, 1}, 10, {1, 1, 1}, fromWhite));
    REQUIRE(reflected::Gather(*brown, {0, 0, 1}, 10, {1, 1, 1}, fromBrown));
    CHECK(fromBrown[0][0] == Catch::Approx(0.5F * fromWhite[0][0]));
    CHECK(fromBrown[0][1] == Catch::Approx(0.25F * fromWhite[0][1]));
    CHECK(fromBrown[0][2] == 0);
}
