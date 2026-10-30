// An interior's night by its own lamps and candles: beside each of a level's
// lightmaps, where the plugin has one for its room (night\<the makers'
// name>, the relighter's, lit by the level's fittings alone), the level's
// shaders are handed it and go to it as the daylight goes
// (shaders/relit_night.hlsl; the user, 2026-10-29: "i wonder if we can take
// the old lighting work and leverage it in the new day / night cycle. so
// things transition from the regular daytime to the properly lit
// nighttime").
#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DPixelShader9;

namespace nightlightmaps
{

// Notes each lightmap the engine makes that has a night of the plugin's.
// Call once, as the plugin loads.
void Install();

// Says the engine is opening a level (the path of its .rim): its night
// lightmaps and probes are taken only for the game's own copy of it, in the
// game's levels' folder; a mod's or an add-in's copy keeps its own night.
// Any thread.
void LevelOpened(const std::filesystem::path &levelRim);

// Whether a level's night lightmaps and probes are taken (LevelOpened). Any thread.
bool LevelOn(std::wstring_view level);

// Notes whether a pixel shader made at an address reads a lightmap and has a
// sampler for its night (`code` its bytecode).
void CreatedPixel(const std::vector<std::byte> &code, IDirect3DPixelShader9 *shader);

// Hands a level's shader the night of the lightmap it is about to read and
// how far to go to it (`picture`: the frame's own picture is drawn into,
// not a portrait's). Call before each draw.
void Drawing(IDirect3DDevice9 *device, bool picture);

// Ends a frame: whether a night lightmap was bound in it, said by Baked until
// the next frame ends. Call on the drawing thread, at a frame's end.
void Presented();

// Whether the last frame's level was drawn with night lightmaps: its lamps'
// light is in them and in their probes by night, and their lights are not to
// be counted again (the user, 2026-10-30: "are we maybe double-counting the
// lights w/ both the dynamic & baked?"). Any thread.
bool Baked();

} // namespace nightlightmaps
