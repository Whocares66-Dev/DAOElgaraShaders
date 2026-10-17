// The sun's shadow maps fitted to the camera's view (src/Cascades.h): the
// inverse, the sun's frame from a map's matrix, and a slice's fit.
#include "Cascades.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>

using cascades::Matrix;
using cascades::Vector;

namespace
{

Matrix Times(const Matrix &a, const Matrix &b)
{
    Matrix c{};
    for (std::size_t r = 0; r < 4; ++r)
    {
        for (std::size_t k = 0; k < 4; ++k)
        {
            for (std::size_t j = 0; j < 4; ++j)
            {
                c.at((r * 4) + k) += a.at((r * 4) + j) * b.at((j * 4) + k);
            }
        }
    }
    return c;
}

// A camera at (10, 20, 2) looking along +y, z up, 60 degrees across, 4:3,
// Direct3D's depth from 0.5 m to 1000 m: clip = projection * view * p.
Matrix Camera()
{
    // The view: x right, y up, z ahead, from the world's x, z, y about the eye.
    const Matrix view{1, 0, 0, -10, 0, 0, 1, -2, 0, 1, 0, -20, 0, 0, 0, 1};
    const float yScale = 1 / std::tan(0.5F * 1.0471976F * 0.75F);
    const float xScale = yScale * 0.75F;
    const float n = 0.5F;
    const float f = 1000;
    const Matrix projection{xScale, 0, 0, 0, 0, yScale, 0, 0, 0, 0, f / (f - n), -n * f / (f - n), 0, 0, 1, 0};
    return Times(projection, view);
}

// The sun going down at 45 degrees towards +x.
std::array<Vector, 3> Sun()
{
    const float s = std::sqrt(0.5F);
    return {Vector{s, 0, s}, Vector{0, 1, 0}, Vector{s, 0, -s}};
}

} // namespace

TEST_CASE("a matrix times its inverse is the identity", "[cascades]")
{
    const Matrix m = Camera();
    const std::optional<Matrix> inverse = cascades::Inverse(m);
    REQUIRE(inverse);
    const Matrix product = Times(m, *inverse);
    for (std::size_t i = 0; i < 16; ++i)
    {
        CHECK(product.at(i) == Catch::Approx(i % 5 == 0 ? 1.0 : 0.0).margin(1e-4));
    }
}

TEST_CASE("the sun's frame is taken from a map's matrix at right angles", "[cascades]")
{
    // An engine's map: the frame scaled, the depth's row a little off square.
    const Matrix clip{0.25F, 0, 0.25F, -8, 0.01F, 0.33F, 0, -35, 0.14F, 0.02F, -0.15F, -2, 0, 0, 0, 1};
    const std::array<Vector, 3> axes = cascades::AxesOf(clip);
    for (const Vector &a : axes)
    {
        CHECK((a[0] * a[0]) + (a[1] * a[1]) + (a[2] * a[2]) == Catch::Approx(1).margin(1e-5));
    }
    for (int i = 0; i < 3; ++i)
    {
        const Vector &a = axes.at(i);
        const Vector &b = axes.at((i + 1) % 3);
        CHECK((a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) == Catch::Approx(0).margin(1e-5));
    }
}

TEST_CASE("the sun's frame from its light's way is square, level across, and across times up is the light",
          "[cascades]")
{
    for (const Vector going : {Vector{0.7F, 0.08F, -0.71F}, Vector{-0.71F, -0.34F, -0.6F}, Vector{0, 0, -2}})
    {
        const std::optional<std::array<Vector, 3>> axes = cascades::AxesToward(going);
        REQUIRE(axes);
        const auto &[across, up, light] = *axes;
        const auto dot = [](const Vector &a, const Vector &b) { return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]); };
        CHECK(dot(across, across) == Catch::Approx(1).margin(1e-5));
        CHECK(dot(up, up) == Catch::Approx(1).margin(1e-5));
        CHECK(dot(across, up) == Catch::Approx(0).margin(1e-5));
        CHECK(dot(across, light) == Catch::Approx(0).margin(1e-5));
        CHECK(across[2] == Catch::Approx(0).margin(1e-6));
        const Vector crossed = {(across[1] * up[2]) - (across[2] * up[1]), (across[2] * up[0]) - (across[0] * up[2]),
                                (across[0] * up[1]) - (across[1] * up[0])};
        for (std::size_t i = 0; i < 3; ++i)
        {
            CHECK(crossed.at(i) == Catch::Approx(light.at(i)).margin(1e-5));
        }
    }
    CHECK_FALSE(cascades::AxesToward({0, 0, 0}));
}

TEST_CASE("a slice of the view lies in its map, and the texture's matrix is the clip's halved", "[cascades]")
{
    const std::optional<cascades::Fitted> fitted = cascades::Fit(Camera(), Sun(), 0, 20, 2048);
    REQUIRE(fitted);
    // Points in the slice: ahead of the camera, and at its edges.
    for (const Vector p : {Vector{10, 30, 2}, Vector{10, 21, 2}, Vector{18, 39, 2}, Vector{2, 39, 8}})
    {
        const Vector clip = cascades::Transformed(fitted->clip, p);
        CHECK(std::abs(clip[0]) <= 1);
        CHECK(std::abs(clip[1]) <= 1);
        CHECK(clip[2] >= 0);
        CHECK(clip[2] <= 1);
        const Vector texture = cascades::Transformed(fitted->texture, p);
        const float half = 0.5F / 2048;
        CHECK(texture[0] == Catch::Approx((0.5F * clip[0]) + 0.5F + half).margin(1e-5));
        CHECK(texture[1] == Catch::Approx((-0.5F * clip[1]) + 0.5F + half).margin(1e-5));
        CHECK(texture[2] == Catch::Approx(clip[2]).margin(1e-6));
    }
    // A point well behind the camera is outside it.
    const Vector behind = cascades::Transformed(fitted->clip, {10, -40, 2});
    CHECK((std::abs(behind[0]) > 1 || std::abs(behind[1]) > 1 || behind[2] < 0 || behind[2] > 1));
}

TEST_CASE("a camera moved less than a texel leaves the map where it was", "[cascades]")
{
    const std::optional<cascades::Fitted> a = cascades::Fit(Camera(), Sun(), 0, 20, 2048);
    Matrix moved = Camera();
    moved[3] += 0.001F; // a millimetre across
    const std::optional<cascades::Fitted> b = cascades::Fit(moved, Sun(), 0, 20, 2048);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->clip[3] == Catch::Approx(b->clip[3]).margin(1e-6));
    CHECK(a->clip[7] == Catch::Approx(b->clip[7]).margin(1e-6));
}

TEST_CASE("the eye is where the camera stands", "[cascades]")
{
    const std::optional<Vector> eye = cascades::Eye(Camera());
    REQUIRE(eye);
    CHECK(eye->at(0) == Catch::Approx(10).margin(1e-3));
    CHECK(eye->at(1) == Catch::Approx(20).margin(1e-3));
    CHECK(eye->at(2) == Catch::Approx(2).margin(1e-3));
}

TEST_CASE("a map around a centre holds it, stays put within a step, and moves by whole texels", "[cascades]")
{
    const cascades::Vector centre{100.3F, 200.7F, 10.2F};
    const std::optional<cascades::Fitted> a = cascades::FitAround(centre, Sun(), 8, 2048, 0.4F);
    REQUIRE(a);
    for (const Vector p : {centre, Vector{centre[0] + 3, centre[1] - 3, centre[2]}})
    {
        const Vector clip = cascades::Transformed(a->clip, p);
        CHECK(std::abs(clip[0]) <= 1);
        CHECK(std::abs(clip[1]) <= 1);
        CHECK(clip[2] >= 0);
        CHECK(clip[2] <= 1);
    }
    // A few centimetres off: the same map.
    const std::optional<cascades::Fitted> near = cascades::FitAround({100.33F, 200.68F, 10.2F}, Sun(), 8, 2048, 0.4F);
    REQUIRE(near);
    for (std::size_t i = 0; i < 16; ++i)
    {
        CHECK(near->clip.at(i) == Catch::Approx(a->clip.at(i)).margin(1e-6));
    }
    // Far off: moved, by a whole number of texels each way.
    const std::optional<cascades::Fitted> moved = cascades::FitAround({117.0F, 190.0F, 10.2F}, Sun(), 8, 2048, 0.4F);
    REQUIRE(moved);
    for (const std::size_t row : {std::size_t{0}, std::size_t{1}})
    {
        const float texels = (moved->texture.at((row * 4) + 3) - a->texture.at((row * 4) + 3)) * 2048;
        CHECK(texels != Catch::Approx(0).margin(0.5));
        CHECK(texels == Catch::Approx(std::round(texels)).margin(2e-2));
    }
}

TEST_CASE("a map's depth spans what it is to hold, widened to whole steps", "[cascades]")
{
    const std::array<Vector, 3> sun = Sun();
    const std::optional<cascades::Fitted> fitted =
        cascades::FitAround({0, 0, 0}, sun, 8, 2048, 0.4F, std::array<float, 2>{-30.0F, 12.0F});
    REQUIRE(fitted);
    // Points at the least and the most of the sun's way given are in its depth.
    for (const float way : {-30.0F, 12.0F, 0.0F})
    {
        const Vector p{sun[2][0] * way, sun[2][1] * way, sun[2][2] * way};
        const float depth = cascades::Transformed(fitted->clip, p)[2];
        CHECK(depth >= 0);
        CHECK(depth <= 1);
    }
}
