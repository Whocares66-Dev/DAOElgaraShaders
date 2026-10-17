// The sun's light as the level sends it back: reflective shadow maps
// (Dachsbacher and Stamminger, 2005). Every surface the sun reaches is in its
// shadow map, so the level's map texels, each a patch of the level the sun
// lights, are all the light it sends back; a figure is given what the
// patches about it send its way. Nothing is baked: the patches are the
// map's, drawn for the sun as it is (LevelShadows.cpp), and move with it.
// The light is not stopped by what stands between a patch and a figure.
#pragma once

#include "Cascades.h"

#include "dao/core/LightProbes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace reflected
{

// A patch of the level the sun lights: where it is, which way it faces,
// towards the sun's side, and the share of each colour of light it gives
// back. A facing of 0 is no patch.
struct Patch
{
    cascades::Vector at{};
    cascades::Vector facing{};
    std::array<float, 3> colour{};
};

// A map's patches on a grid of `size` a side: the texels at step * i + step / 2
// of each row and column of a map `texels` a side.
struct Patches
{
    int size = 0;
    int step = 0;
    int texels = 0;
    float spacing = 0; // metres between neighbours, across the sun
    cascades::Matrix clip{};
    std::vector<Patch> grid; // by rows
};

// Returns a map's patches from its depths and colours at the grid's texels,
// by rows: the map's own depth, from 0 nearest the sun to 1, where 1 is
// nothing drawn and 0 a surface clamped to the map's near side; and the
// colour of the surface there. Nothing if `clip` cannot be turned back.
std::optional<Patches> Make(std::span<const float> depths, std::span<const std::array<float, 3>> colours, int size,
                            int step, int texels, const cascades::Matrix &clip);

// Adds to `light` what the patches within `reach` metres of `place` send it,
// as radiance by direction: each patch the sun's colour on it (`sent`) times
// its own colour, over the solid angle it fills.
// @return false where `place` is not over the grid
bool Gather(const Patches &patches, const cascades::Vector &place, float reach, const std::array<float, 3> &sent,
            dao::core::probes::Coefficients &light);

// Keeps the newest patches of a map, by its index (0 the near one). Any thread.
void Keep(std::size_t map, std::shared_ptr<const Patches> patches);

// Returns the newest patches of a map, or null.
std::shared_ptr<const Patches> Kept(std::size_t map);

// Returns a number that changes whenever any map's patches do.
std::uint64_t Generation();

// Drops every map's patches, as a level is left.
void Forget();

} // namespace reflected
