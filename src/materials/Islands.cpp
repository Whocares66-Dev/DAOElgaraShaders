// Islands.h has what this is for.
#include "materials/Islands.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <numeric>
#include <utility>

namespace materials
{
namespace
{

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

struct Sets
{
    std::vector<std::size_t> parent;
    explicit Sets(std::size_t n) : parent(n)
    {
        std::iota(parent.begin(), parent.end(), std::size_t{0});
    }
    std::size_t Find(std::size_t i)
    {
        while (parent[i] != i)
        {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }
    void Join(std::size_t a, std::size_t b)
    {
        a = Find(a);
        b = Find(b);
        if (a != b)
        {
            parent[std::max(a, b)] = std::min(a, b);
        }
    }
};

// A texture coordinate as a key, to a 1/4096 of the texture: corners that
// meet across two chunks are written the same to that.
std::pair<std::int64_t, std::int64_t> Key(const std::array<float, 2> &uv)
{
    return {std::llround(uv[0] * 4096.0F), std::llround(uv[1] * 4096.0F)};
}

// Every triangle of every chunk, numbered in order, joined to those it shares
// a vertex or a corner's coordinates with.
Sets Joined(std::span<const UvMesh> meshes)
{
    std::size_t triangles = 0;
    for (const auto &mesh : meshes)
    {
        triangles += mesh.triangles.size() / 3;
    }
    Sets sets(triangles);
    std::map<std::pair<std::int64_t, std::int64_t>, std::size_t> corner;
    std::size_t first = 0;
    for (const auto &mesh : meshes)
    {
        std::vector<std::size_t> byVertex(mesh.uv.size(), kNone);
        for (std::size_t c = 0; c < mesh.triangles.size() - (mesh.triangles.size() % 3); ++c)
        {
            const std::size_t t = first + (c / 3);
            const std::uint32_t v = mesh.triangles[c];
            if (v >= mesh.uv.size())
            {
                continue;
            }
            if (byVertex[v] != kNone)
            {
                sets.Join(t, byVertex[v]);
            }
            byVertex[v] = t;
            const auto [at, fresh] = corner.emplace(Key(mesh.uv[v]), t);
            if (!fresh)
            {
                sets.Join(t, at->second);
            }
        }
        first += mesh.triangles.size() / 3;
    }
    return sets;
}

std::uint32_t Wrapped(std::int64_t i, std::uint32_t n)
{
    const auto m = static_cast<std::int64_t>(n);
    return static_cast<std::uint32_t>(((i % m) + m) % m);
}

// Marks with `island` the texel centres inside a triangle given in texels
// (either winding), the texture repeated past its edges.
void Draw(const std::array<std::array<float, 2>, 3> &p, int island, Islands &out)
{
    const auto edge = [](const std::array<float, 2> &a, const std::array<float, 2> &b, float x, float y) {
        return ((b[0] - a[0]) * (y - a[1])) - ((b[1] - a[1]) * (x - a[0]));
    };
    const float area = edge(p[0], p[1], p[2][0], p[2][1]);
    if (area == 0.0F)
    {
        return;
    }
    const auto minX = static_cast<std::int64_t>(std::floor(std::min({p[0][0], p[1][0], p[2][0]})));
    const auto maxX = static_cast<std::int64_t>(std::ceil(std::max({p[0][0], p[1][0], p[2][0]})));
    const auto minY = static_cast<std::int64_t>(std::floor(std::min({p[0][1], p[1][1], p[2][1]})));
    const auto maxY = static_cast<std::int64_t>(std::ceil(std::max({p[0][1], p[1][1], p[2][1]})));
    for (std::int64_t y = minY; y < maxY; ++y)
    {
        for (std::int64_t x = minX; x < maxX; ++x)
        {
            const float cx = static_cast<float>(x) + 0.5F;
            const float cy = static_cast<float>(y) + 0.5F;
            const bool inside = edge(p[1], p[2], cx, cy) / area >= 0 && edge(p[2], p[0], cx, cy) / area >= 0 &&
                                edge(p[0], p[1], cx, cy) / area >= 0;
            if (inside)
            {
                out.label[(static_cast<std::size_t>(Wrapped(y, out.height)) * out.width) + Wrapped(x, out.width)] =
                    island;
            }
        }
    }
}

} // namespace

Islands IslandsOf(std::span<const UvMesh> meshes, std::uint32_t width, std::uint32_t height)
{
    Sets sets = Joined(meshes);
    Islands out{width, height, 0, std::vector<int>(static_cast<std::size_t>(width) * height, -1)};
    std::map<std::size_t, int> number;
    std::size_t first = 0;
    for (const auto &mesh : meshes)
    {
        for (std::size_t t = 0; t < mesh.triangles.size() / 3; ++t)
        {
            std::array<std::array<float, 2>, 3> p{};
            bool whole = true;
            for (std::size_t c = 0; c < 3 && whole; ++c)
            {
                const std::uint32_t v = mesh.triangles[(t * 3) + c];
                whole = v < mesh.uv.size();
                if (whole)
                {
                    p[c] = {mesh.uv[v][0] * static_cast<float>(width), mesh.uv[v][1] * static_cast<float>(height)};
                }
            }
            if (!whole)
            {
                continue;
            }
            const auto [named, fresh] = number.emplace(sets.Find(first + t), out.count);
            if (fresh)
            {
                ++out.count;
            }
            Draw(p, named->second, out);
        }
        first += mesh.triangles.size() / 3;
    }
    return out;
}

} // namespace materials
