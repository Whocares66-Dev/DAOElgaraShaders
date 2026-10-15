// What a material's textures say of it that the game's own material does not:
// which texels are metal (the game has no metal channel), and how rough its
// normal map is over a patch of texels. The rules data/materials.ini is made
// by (elgarashaders_materials) and the plugin works out a material the table
// does not know by (issue 19). Nothing here touches the game or the process:
// the textures come in as pictures, and what comes out is a map and a share,
// and a measure. TextureRule.cpp has the rules' numbers and why each is what
// it is. A rule for another kind of material (issue 11: skin, cloth's sheen,
// hair, glass seen through) gets a file of its own beside this, over the
// pictures and resizing this one has.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace materials
{

// A picture as floats from 0 to 1: rows from its top, `channels` a texel.
struct Picture
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0;
    std::vector<float> values;

    [[nodiscard]] float At(std::uint32_t x, std::uint32_t y, std::uint32_t channel) const
    {
        return values[(((static_cast<std::size_t>(y) * width) + x) * channels) + channel];
    }
};

// A picture of red, green, blue, alpha bytes as floats over 255.
Picture FromRgba8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba);

// A picture averaged down or up to `width` by `height`, as Pillow's BOX filter
// resizes one, which is what the table tool does.
Picture Resized(const Picture &picture, std::uint32_t width, std::uint32_t height);

// Which texels of a material are metal and how much of it: the map, one
// channel at map_size's side (a quarter of the specular mask's, 256 at the
// most, 4 at the least), and the share of the texels in use the rule found
// metal before a plate's shaded metal was joined to it.
struct MetalMap
{
    Picture metal;
    double share = 0.0;
};

// The rule for one texel (metal_texels): its own colour, its mirror's (the
// specular mask's colour) and its gloss, all 0 to 1. With `surely`, asked
// twice as hard: a stud that needs no neighbour to be believed.
bool IsMetalTexel(const std::array<float, 3> &own, const std::array<float, 3> &mirror, float gloss,
                  bool surely = false);

// The rule for a material's diffuse and specular mask (both RGBA). With
// `plate` (a suit of plate: its project is ARM_MAS, ARM_HVY or ARM_DWP),
// metal with shade painted over it is metal where it joins metal.
MetalMap MakeMetalMap(const Picture &diffuse, const Picture &specular, bool plate);

// Whether a map is more edge than area (ragged): specks, where the paint did
// not say clearly. A creature's hide whose map is so is left a hide.
bool Ragged(const Picture &metal);

// The map's side for a specular mask of that side.
std::array<std::uint32_t, 2> MapSize(std::uint32_t width, std::uint32_t height);

// How rough a normal map is at each size a pixel may cover: its side, and the
// spread of its normals over patches of 2, 4, ... 64 texels across, in the
// mean over the map (the shader's DaoBumps). The game keeps a normal's x in
// the alpha and its y in the green.
struct Bumps
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<double, 6> spreads{};
};

Bumps MeasureBumps(const Picture &normalMap);

} // namespace materials
