#include "Reflected.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace reflected
{

namespace
{

using cascades::Vector;

// How steep a step between neighbours may be and they still be one surface,
// as metres along the sun per metre across it: up to some 76 degrees to the
// map. Past it the step is an edge, a roof's over the ground, and the patch's
// facing is taken from its other neighbour.
constexpr float kSteep = 4;

Vector Minus(const Vector &a, const Vector &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float Dot(const Vector &a, const Vector &b)
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Vector Cross(const Vector &a, const Vector &b)
{
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}

std::optional<Vector> Normalized(const Vector &v)
{
    const float length = std::sqrt(Dot(v, v));
    if (length <= 0)
    {
        return std::nullopt;
    }
    return Vector{v[0] / length, v[1] / length, v[2] / length};
}

// The way along a row or a column through a patch, from its neighbours on
// the same surface: both if both are, else the one that is.
std::optional<Vector> Along(const std::optional<Vector> &before, const Vector &here, const std::optional<Vector> &after)
{
    if (before && after)
    {
        return Minus(*after, *before);
    }
    if (after)
    {
        return Minus(*after, here);
    }
    if (before)
    {
        return Minus(here, *before);
    }
    return std::nullopt;
}

std::mutex g_lock;
std::array<std::shared_ptr<const Patches>, 2> g_kept;
std::uint64_t g_generation = 0;

// Where each texel of the grid is in the world, or nothing where nothing was
// drawn. Direct3D 9's pixel k is at 2k / texels - 1 across the map's clip
// space, its centre on the pixel's corner (LevelShadows.cpp's copy reads it so).
std::vector<std::optional<Vector>> PlacesOf(std::span<const float> depths, int size, int step, int texels,
                                            const cascades::Matrix &fromClip)
{
    const auto across = [&](int i) {
        return (2.0F * (static_cast<float>(step * i) + (static_cast<float>(step) / 2)) / static_cast<float>(texels)) -
               1;
    };
    std::vector<std::optional<Vector>> at(depths.size());
    for (int j = 0; j < size; ++j)
    {
        for (int i = 0; i < size; ++i)
        {
            const std::size_t cell = (static_cast<std::size_t>(j) * size) + i;
            if (const float depth = depths[cell]; depth > 0 && depth < 1)
            {
                at[cell] = cascades::Transformed(fromClip, {across(i), -across(j), depth});
            }
        }
    }
    return at;
}

// Which way the surface at a cell faces, towards the sun's side, from its
// neighbours on the same surface; nothing where it has none both ways.
std::optional<Vector> FacingAt(const std::vector<std::optional<Vector>> &at, int size, int i, int j,
                               const Vector &toSun, float steepest)
{
    const Vector &here = *at[(static_cast<std::size_t>(j) * size) + i];
    const auto neighbour = [&](int ni, int nj) -> std::optional<Vector> {
        if (ni < 0 || nj < 0 || ni >= size || nj >= size)
        {
            return std::nullopt;
        }
        const std::optional<Vector> &there = at[(static_cast<std::size_t>(nj) * size) + ni];
        if (!there || std::abs(Dot(Minus(*there, here), toSun)) > steepest)
        {
            return std::nullopt;
        }
        return there;
    };
    const std::optional<Vector> row = Along(neighbour(i - 1, j), here, neighbour(i + 1, j));
    const std::optional<Vector> column = Along(neighbour(i, j - 1), here, neighbour(i, j + 1));
    if (!row || !column)
    {
        return std::nullopt;
    }
    const std::optional<Vector> facing = Normalized(Cross(*row, *column));
    if (!facing || Dot(*facing, toSun) >= 0)
    {
        return facing;
    }
    return Vector{-(*facing)[0], -(*facing)[1], -(*facing)[2]};
}

} // namespace

std::optional<Patches> Make(std::span<const float> depths, std::span<const std::array<float, 3>> colours, int size,
                            int step, int texels, const cascades::Matrix &clip)
{
    const std::optional<cascades::Matrix> fromClip = cascades::Inverse(clip);
    const auto cells = static_cast<std::size_t>(size) * static_cast<std::size_t>(size);
    const std::optional<Vector> toSun = Normalized({-clip[8], -clip[9], -clip[10]});
    if (!fromClip || !toSun || size < 2 || step < 1 || texels < 1 || depths.size() != cells || colours.size() != cells)
    {
        return std::nullopt;
    }
    Patches patches{.size = size, .step = step, .texels = texels, .spacing = 0, .clip = clip, .grid = {}};
    patches.grid.resize(cells);
    const std::vector<std::optional<Vector>> at = PlacesOf(depths, size, step, texels, *fromClip);
    const float stepAcross = 2.0F * static_cast<float>(step) / static_cast<float>(texels);
    const Vector middle = cascades::Transformed(*fromClip, {0, 0, 0.5F});
    const Vector next = cascades::Transformed(*fromClip, {stepAcross, 0, 0.5F});
    patches.spacing = std::sqrt(Dot(Minus(next, middle), Minus(next, middle)));
    for (int j = 0; j < size; ++j)
    {
        for (int i = 0; i < size; ++i)
        {
            const std::size_t cell = (static_cast<std::size_t>(j) * size) + i;
            if (!at[cell])
            {
                continue;
            }
            if (const std::optional<Vector> facing = FacingAt(at, size, i, j, *toSun, kSteep * patches.spacing))
            {
                patches.grid[cell] = {.at = *at[cell], .facing = *facing, .colour = colours[cell]};
            }
        }
    }
    return patches;
}

bool Gather(const Patches &patches, const cascades::Vector &place, float reach, const std::array<float, 3> &sent,
            dao::core::probes::Coefficients &light)
{
    const Vector clip = cascades::Transformed(patches.clip, place);
    if (std::abs(clip[0]) > 1 || std::abs(clip[1]) > 1 || patches.spacing <= 0)
    {
        return false;
    }
    const auto texels = static_cast<float>(patches.texels);
    const auto step = static_cast<float>(patches.step);
    const auto cellOf = [&](float k) { return static_cast<int>(std::lround((k - (step / 2)) / step)); };
    const int ci = cellOf((clip[0] + 1) * 0.5F * texels);
    const int cj = cellOf((1 - clip[1]) * 0.5F * texels);
    const int cells = static_cast<int>(std::ceil(reach / patches.spacing));
    // Each patch fills, seen from the place, the solid angle of a square of
    // its area seen face on, 4 asin(a / (a + 4 r^2)), turned by how it faces
    // the place: a / r^2 far off, and no more than the half of all there is
    // next to it. Over flat ground a metre down it comes to some 2 percent
    // under the ground's exact solid angle (tests/test_reflected.cpp).
    const float area = patches.spacing * patches.spacing;
    for (int j = std::max(0, cj - cells); j <= std::min(patches.size - 1, cj + cells); ++j)
    {
        for (int i = std::max(0, ci - cells); i <= std::min(patches.size - 1, ci + cells); ++i)
        {
            const Patch &patch = patches.grid[(static_cast<std::size_t>(j) * patches.size) + i];
            const Vector toPatch = Minus(patch.at, place);
            const float squared = Dot(toPatch, toPatch);
            if (Dot(patch.facing, patch.facing) == 0 || squared > reach * reach || squared <= 0)
            {
                continue;
            }
            const float distance = std::sqrt(squared);
            const float faced = -Dot(patch.facing, toPatch) / distance;
            if (faced <= 0)
            {
                continue;
            }
            const float filled = 4 * std::asin(area / (area + (4 * squared))) * faced;
            const std::array<float, 9> harmonics =
                dao::core::probes::Harmonics({toPatch[0] / distance, toPatch[1] / distance, toPatch[2] / distance});
            for (std::size_t term = 0; term < light.size(); ++term)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    light.at(term).at(channel) +=
                        sent.at(channel) * patch.colour.at(channel) * filled * harmonics.at(term);
                }
            }
        }
    }
    return true;
}

void Keep(std::size_t map, std::shared_ptr<const Patches> patches)
{
    const std::scoped_lock lock(g_lock);
    if (map < g_kept.size())
    {
        g_kept.at(map) = std::move(patches);
        ++g_generation;
    }
}

std::shared_ptr<const Patches> Kept(std::size_t map)
{
    const std::scoped_lock lock(g_lock);
    return map < g_kept.size() ? g_kept.at(map) : nullptr;
}

std::uint64_t Generation()
{
    const std::scoped_lock lock(g_lock);
    return g_generation;
}

void Forget()
{
    const std::scoped_lock lock(g_lock);
    g_kept = {};
    ++g_generation;
}

} // namespace reflected
