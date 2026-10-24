// The layers of a level's terrain palette that give off light of their own,
// lava's, as data/glowing.ini says them: each layer's strength handed to the
// terrain shaders (shaders/glow.hlsl's mml_vDaoGlow) in the material objects
// that draw with that palette, as the game reads them.
#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace levelfiles
{
class Level;
}

namespace glowing
{

// How strongly each of a palette's eight layers glows, a share of its own colour.
using Layers = std::array<float, 8>;

// Reads `folder`'s glowing.ini. Before the game reads anything.
void Read(const std::filesystem::path &folder);

// A palette's layers' glow, by its texture's name (any case), or none where
// the table does not name it. Any thread, once Read has run.
std::optional<Layers> Of(std::string_view palette);

// A material object's text with its palette's glow added as mml_vDaoGlow, or
// none where its palette (its `palette` texture) does not glow.
std::optional<std::vector<std::byte>> Patched(std::span<const std::byte> bytes);

// A light a glowing layer gives the ground about it: where, its colour at
// its place and how far it reaches, metres.
struct Light
{
    std::array<float, 3> at{};
    std::array<float, 3> colour{};
    float reach = 0;
};

// The lights the glowing layers of a level's own ground give off, one for
// each few metres of glowing ground, found where its masks weigh a glowing
// layer: as bright as that ground's own colour times its glow over its
// area. Off the game's thread: it reads the level's files.
std::vector<Light> LightsOf(levelfiles::Level &level);

} // namespace glowing
