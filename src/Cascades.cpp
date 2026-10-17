#include "Cascades.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace cascades
{

namespace
{

float Dot(const Vector &a, const Vector &b)
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Vector Minus(const Vector &a, const Vector &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Vector Scaled(const Vector &a, float s)
{
    return {a[0] * s, a[1] * s, a[2] * s};
}

Vector Plus(const Vector &a, const Vector &b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Vector Unit(const Vector &a)
{
    const float length = std::sqrt(Dot(a, a));
    return length > 0 ? Scaled(a, 1 / length) : a;
}

// A row of a matrix: `axis` scaled and the point `centre` taken off, so that
// the row gives (p - centre) along the axis, times `scale`, plus `offset`.
void Row(Matrix &m, int row, const Vector &axis, float scale, float centre, float offset)
{
    for (int c = 0; c < 3; ++c)
    {
        m.at((row * 4) + c) = axis.at(c) * scale;
    }
    m.at((row * 4) + 3) = offset - (centre * scale);
}

} // namespace

std::optional<Matrix> Inverse(const Matrix &m)
{
    // Gauss-Jordan with the greatest pivot of each column, in doubles.
    std::array<std::array<double, 8>, 4> rows{};
    for (std::size_t r = 0; r < 4; ++r)
    {
        for (std::size_t c = 0; c < 4; ++c)
        {
            rows.at(r).at(c) = m.at((r * 4) + c);
        }
        rows.at(r).at(4 + r) = 1;
    }
    for (std::size_t col = 0; col < 4; ++col)
    {
        std::size_t pivot = col;
        for (std::size_t r = col + 1; r < 4; ++r)
        {
            if (std::abs(rows.at(r).at(col)) > std::abs(rows.at(pivot).at(col)))
            {
                pivot = r;
            }
        }
        if (std::abs(rows.at(pivot).at(col)) < 1e-12)
        {
            return std::nullopt;
        }
        std::swap(rows.at(col), rows.at(pivot));
        const double scale = 1 / rows.at(col).at(col);
        for (double &value : rows.at(col))
        {
            value *= scale;
        }
        for (std::size_t r = 0; r < 4; ++r)
        {
            if (r == col)
            {
                continue;
            }
            const double factor = rows.at(r).at(col);
            for (std::size_t c = 0; c < 8; ++c)
            {
                rows.at(r).at(c) -= factor * rows.at(col).at(c);
            }
        }
    }
    Matrix inverse{};
    for (std::size_t r = 0; r < 4; ++r)
    {
        for (std::size_t c = 0; c < 4; ++c)
        {
            inverse.at((r * 4) + c) = static_cast<float>(rows.at(r).at(4 + c));
        }
    }
    return inverse;
}

Vector Transformed(const Matrix &m, const Vector &p)
{
    std::array<float, 4> q{};
    for (std::size_t row = 0; row < 4; ++row)
    {
        q.at(row) =
            (m.at(row * 4) * p[0]) + (m.at((row * 4) + 1) * p[1]) + (m.at((row * 4) + 2) * p[2]) + m.at((row * 4) + 3);
    }
    const float w = q[3] != 0 ? q[3] : 1;
    return {q[0] / w, q[1] / w, q[2] / w};
}

std::optional<Vector> Eye(const Matrix &toScreen)
{
    // Rows 0, 1 and 3 of the matrix, taken at (eye, 1), are 0: three
    // equations in the eye's three, by Cramer's rule.
    const std::array<std::size_t, 3> rows{0, 1, 3};
    std::array<std::array<double, 3>, 3> a{};
    std::array<double, 3> b{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            a.at(i).at(c) = toScreen.at((rows.at(i) * 4) + c);
        }
        b.at(i) = -toScreen.at((rows.at(i) * 4) + 3);
    }
    const auto det = [](const std::array<std::array<double, 3>, 3> &m) {
        return (m[0][0] * ((m[1][1] * m[2][2]) - (m[1][2] * m[2][1]))) -
               (m[0][1] * ((m[1][0] * m[2][2]) - (m[1][2] * m[2][0]))) +
               (m[0][2] * ((m[1][0] * m[2][1]) - (m[1][1] * m[2][0])));
    };
    const double whole = det(a);
    if (std::abs(whole) < 1e-12)
    {
        return std::nullopt;
    }
    Vector eye{};
    for (std::size_t c = 0; c < 3; ++c)
    {
        std::array<std::array<double, 3>, 3> m = a;
        for (std::size_t i = 0; i < 3; ++i)
        {
            m.at(i).at(c) = b.at(i);
        }
        eye.at(c) = static_cast<float>(det(m) / whole);
    }
    return eye;
}

std::optional<Fitted> FitAround(const Vector &centre, const std::array<Vector, 3> &sunAxes, float radius, int texels,
                                float stepShare, const std::optional<std::array<float, 2>> &along)
{
    if (radius <= 0 || texels <= 0)
    {
        return std::nullopt;
    }
    const float texel = 2 * radius / static_cast<float>(texels);
    const float step = texel * std::max(1.0F, std::round(stepShare * radius / texel));
    const auto snapped = [&](const Vector &axis) { return std::round(Dot(centre, axis) / step) * step; };
    const float across = snapped(sunAxes[0]);
    const float up = snapped(sunAxes[1]);
    float least = snapped(sunAxes[2]) - radius;
    float most = least + (2 * radius);
    if (along && (*along)[1] >= (*along)[0])
    {
        least = std::floor((*along)[0] / step) * step;
        most = std::max(std::ceil((*along)[1] / step) * step, least + step);
    }
    Fitted fitted;
    Row(fitted.clip, 0, sunAxes[0], 1 / radius, across, 0);
    Row(fitted.clip, 1, sunAxes[1], 1 / radius, up, 0);
    Row(fitted.clip, 2, sunAxes[2], 1 / (most - least), least, 0);
    fitted.clip[15] = 1;
    const float half = 0.5F / static_cast<float>(texels);
    Row(fitted.texture, 0, sunAxes[0], 0.5F / radius, across, 0.5F + half);
    Row(fitted.texture, 1, sunAxes[1], -0.5F / radius, up, 0.5F + half);
    Row(fitted.texture, 2, sunAxes[2], 1 / (most - least), least, 0);
    fitted.texture[15] = 1;
    return fitted;
}

std::array<Vector, 3> AxesOf(const Matrix &clip)
{
    const Vector light = Unit({clip[8], clip[9], clip[10]});
    const Vector across0 = {clip[0], clip[1], clip[2]};
    const Vector across = Unit(Minus(across0, Scaled(light, Dot(across0, light))));
    const Vector up0 = {clip[4], clip[5], clip[6]};
    const Vector up = Unit(Minus(Minus(up0, Scaled(light, Dot(up0, light))), Scaled(across, Dot(up0, across))));
    return {across, up, light};
}

std::optional<std::array<Vector, 3>> AxesToward(const Vector &going)
{
    if (Dot(going, going) <= 0)
    {
        return std::nullopt;
    }
    const Vector light = Unit(going);
    // Across is level, square to the light; for light straight down any level
    // way is, and x is taken.
    const Vector level = {light[1], -light[0], 0};
    const Vector across = Dot(level, level) > 1e-12F ? Unit(level) : Vector{1, 0, 0};
    const Vector up = {(light[1] * across[2]) - (light[2] * across[1]), (light[2] * across[0]) - (light[0] * across[2]),
                       (light[0] * across[1]) - (light[1] * across[0])};
    return std::array<Vector, 3>{across, up, light};
}

std::optional<Fitted> Fit(const Matrix &toScreen, const std::array<Vector, 3> &sunAxes, float nearest, float farthest,
                          int texels)
{
    const std::optional<Matrix> fromScreen = Inverse(toScreen);
    if (!fromScreen || texels <= 0 || farthest <= nearest)
    {
        return std::nullopt;
    }
    // The camera's place and the ways through the screen's corners and
    // middle, from the screen's near and far planes (Direct3D's z of 0 and 1).
    const auto way = [&](float x, float y) {
        return Minus(Transformed(*fromScreen, {x, y, 1}), Transformed(*fromScreen, {x, y, 0}));
    };
    const Vector ahead = Unit(way(0, 0));
    const Vector nearMiddle = Transformed(*fromScreen, {0, 0, 0});
    // The eye is behind the near plane's middle by as much as that plane is
    // from it; the slices' distances are from the near plane, a hand's width
    // from the eye, which is as near as matters.
    std::array<Vector, 8> corners{};
    int at = 0;
    for (const float distance : {nearest, farthest})
    {
        for (const float x : {-1.0F, 1.0F})
        {
            for (const float y : {-1.0F, 1.0F})
            {
                const Vector through = Unit(way(x, y));
                const Vector start = Transformed(*fromScreen, {x, y, 0});
                corners.at(at++) = Plus(start, Scaled(through, distance / std::max(Dot(through, ahead), 1e-3F)));
            }
        }
    }
    // A sphere about the slice, so the map's size does not change as the
    // camera turns, and its middle moved by whole texels across the sun's
    // frame, so its edges do not crawl as the camera moves.
    const Vector middle = Plus(nearMiddle, Scaled(ahead, (nearest + farthest) / 2));
    float radius = 0;
    for (const Vector &corner : corners)
    {
        const Vector off = Minus(corner, middle);
        radius = std::max(radius, std::sqrt(Dot(off, off)));
    }
    radius = std::ceil(radius);
    const float texel = (2 * radius) / static_cast<float>(texels);
    const float across = std::floor(Dot(middle, sunAxes[0]) / texel) * texel;
    const float up = std::floor(Dot(middle, sunAxes[1]) / texel) * texel;
    const float along = Dot(middle, sunAxes[2]);
    Fitted fitted;
    Row(fitted.clip, 0, sunAxes[0], 1 / radius, across, 0);
    Row(fitted.clip, 1, sunAxes[1], 1 / radius, up, 0);
    // The depth from the sphere's sunward side to its far side; what lies
    // sunward of it is pressed onto that side by the shadow pass.
    Row(fitted.clip, 2, sunAxes[2], 1 / (2 * radius), along - radius, 0);
    fitted.clip[15] = 1;
    // The texture's: u right and v down the map, to texel centres.
    const float half = 0.5F / static_cast<float>(texels);
    Row(fitted.texture, 0, sunAxes[0], 0.5F / radius, across, 0.5F + half);
    Row(fitted.texture, 1, sunAxes[1], -0.5F / radius, up, 0.5F + half);
    Row(fitted.texture, 2, sunAxes[2], 1 / (2 * radius), along - radius, 0);
    fitted.texture[15] = 1;
    return fitted;
}

} // namespace cascades
