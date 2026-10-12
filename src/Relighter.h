// A level lit again from its own files: lightmaps of our own, to stand in
// the game's place (a file of an atlas's name in the plugin's override folder
// is what the game lights the level by; seen 2026-10-11).
//
// The game's lightmaps were baked from lights of its makers' editor that did
// not ship, and what they hold in most places is an even light with no
// source: a hall beside lava as evenly red as an office is evenly white.
// What did ship is where the light should come from. A level's layouts have
// its lamps, torches and fires as point lights; its models have materials
// that say which of them is a window or a flow of lava. So each texel of a
// lightmap is lit here by those: the lamps with a ray cast for their shadow,
// what glows as a surface that sends light from all of itself, and what
// comes back off the surfaces about (the user, 2026-10-11: "What if we got
// rid of baked in ambient light? That would force us to relight properly";
// "Can we add our own lights? ... there should be light from the windows").
//
// Nothing of the engine is called: it reads files and writes files.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace relighter
{

// Something that glows: every model's part whose material's name has `named`
// in it, and every part of a model whose own name has, sends this light from
// each square metre of itself.
struct Glow
{
    std::wstring named;
    std::array<float, 3> colour{};
    // How much of that light the thing's own surface is lit by. Lava and a
    // window's pane are drawn bright by their own materials and want none; a
    // crystal is stone like any other, lit by its lightmap, and with none it
    // was a grey rock in a pool of its own blue light.
    float own = 0;
};

// A flame: every model a room places whose name has `named` in it is a small
// light, where it stands: `colour` is its light, by the square of the
// distance (Relighter.cpp's Straight), and `reach` how far it carries at all. A level's candles are
// such models, a flame's effect each, and have no light of the level's own:
// the mage tower has 244 of them and 83 lamps, and its library by
// candlelight alone was black (the user, 2026-10-11: "these candles are
// giving off no light").
struct Flame
{
    std::wstring named;
    std::array<float, 3> colour{};
    float reach = 0;
    // A fire in a brazier is placed at the bottom of its bowl, and burns a
    // man's height above it: a light put where the model is was shut in the
    // bowl, and Orzammar's great doors stood in the dark eight metres from
    // two fires (toured 2026-10-11). `lift` is how far above its place the
    // light is, and `clear` how far about the light nothing casts a shadow
    // of it, the bowl's own size; both in metres.
    float lift = 0;
    float clear = 0;
    // Not 0 for a light that lands: a shaft of light let down through a roof
    // is placed by some levels where it comes in, twenty metres up, and its
    // light is the pool where it lands. The light is then put `lift` over
    // the first surface straight under the model's place.
    float lands = 0;
};

// A light from far off that comes in through openings, the moon or the sun
// through windows: every part whose material's name has `named` in it is an
// opening, and a surface is lit where the way back along the light, `travels`
// reversed, goes out through one before it meets anything of the level. A
// window that only glows lights the ceiling beside it as evenly as a lamp's
// shade (the user, 2026-10-11: "why's ceiling so evenly lit? don't think the
// window light will be that even"); a beam lands as patches on the floor
// and the far wall.
struct Beam
{
    std::wstring named;
    std::array<float, 3> colour{};  // the light on a surface that faces it
    std::array<float, 3> travels{}; // the way the light travels, of any length
    // And the sky's own light through the same openings, from every one of
    // them whichever way it faces: each opening glows this much into the
    // room and downward, as the sky lights what is under a window and not
    // the ceiling over it. The moon alone comes in on one side of a tower
    // and lands far across the floor; a window on the other side let nothing
    // in at all (the user, 2026-10-11: "still no moonlight coming in from
    // windows").
    std::array<float, 3> sky{};
};

struct Options
{
    std::uint32_t side = 256;   // a lightmap is made no larger a side
    float lamps = 1.0F;         // what a lamp's own colour is multiplied by
    float reach = 1.0F;         // and its reach
    float furthest = 60.0F;     // a lamp that reaches further than this, in metres, is left out
    float surfaces = 0.5F;      // of the light on a surface, what it sends on
    int bounces = 2;            // how many times light is followed off a surface
    std::size_t rays = 48;      // cast from a texel for what comes back
    float kept = 0.0F;          // of the game's own baked light, what is laid under ours
    std::vector<Glow> glows;    // what glows in this level
    std::vector<Flame> flames;  // and what burns in it
    // How far the level's light is brought to vanilla's bake: at 1 its mean
    // colour over every texel lit is that of vanilla's lightmaps over the same
    // texels, and its bright end (the texel brighter than nineteen in twenty)
    // is as bright as vanilla's. Where the light falls is
    // the fittings' to say; how bright the level is and what colour, taken
    // all together, is vanilla's. Without it each level's strengths and
    // colours were set by eye and drifted: Orzammar an orange vanilla has
    // not, Aeducan Thaig three and a half times as bright as vanilla's bake
    // (the user, 2026-10-11: "things are way too bright. i feel like we're
    // moving in the wrong direction. to keep it simple, preserve vanilla
    // hue, saturation, etc."). 0 leaves the light as the fittings give it.
    float match = 1.0F;
    // How far the level's brightness is brought to vanilla's bake as well, by
    // the bright end. 0 leaves it as the fittings' strengths give it, which
    // is the rule: vanilla's bake is no measure of how bright a level is
    // where vanilla lights the level by its ambient colour and its lamps
    // more than by its bake. Orzammar's Commons brought to its bake's bright
    // end was a third as bright as the bake the user had called "pretty
    // good" and all but black (seen 2026-10-12). The tower's bake is a fair
    // measure of the tower, and its recipe says 1.
    float bright = 0.0F;
    // What casts the rays: Embree (1) where the tool is built with it, as the
    // 64-bit one is (bake/CMakeLists.txt), or DAOLib's tree of triangles (0),
    // a room at a time, which is all the 32-bit tool has. Embree has the
    // whole world in one scene: a room far off is in the way of light there,
    // where the tree tries only the rooms about a texel's own.
    int caster = 1;
    std::vector<Beam> beams;    // and what comes in from far off
};

// Lights a level again and writes its lightmaps into a folder, each a DDS of
// its atlas's own name.
// @param levelRim the level's .rim, as ProbeBaker's
// @param game the game's folder
// @return how many lightmaps were written; 0 if the level could not be read
std::size_t Relight(const std::filesystem::path &levelRim, const std::filesystem::path &game,
                    const std::filesystem::path &out, const Options &options);

} // namespace relighter
