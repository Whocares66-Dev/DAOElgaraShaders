// A texture's islands: the parts of it the model's meshes map to it, each a
// connected run of triangles in texture space (issue 24). A piece's parts are
// its islands (a blade, a grip, a plate, a strap), which is what decides
// which texels are one material: the texel rule's say is taken over each
// island whole, not texel by texel, where painted wear and shade broke a
// blade or a shield's face into specks (the review sheets, 2026-10-16).
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace materials
{

// One mesh chunk as texture space has it: each vertex's texture coordinate
// and the triangles' vertices.
struct UvMesh
{
    std::vector<std::array<float, 2>> uv;
    std::vector<std::uint32_t> triangles; // three a triangle
};

// Which island each texel of a width by height texture is in: -1 where no
// triangle covers it. Islands are numbered in the order their first triangle
// comes, chunk by chunk, so the same meshes always number them alike.
struct Islands
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int count = 0;
    std::vector<int> label;
};

// Triangles sharing a vertex, or a corner at the same texture coordinate in
// another chunk, are one island; each is drawn into the texture at its texel
// centres (wrapped: a coordinate past 1 is the texture repeated).
Islands IslandsOf(std::span<const UvMesh> meshes, std::uint32_t width, std::uint32_t height);

} // namespace materials
