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
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DPixelShader9;

namespace nightlightmaps
{

// Notes each lightmap the engine makes that has a night of the plugin's.
// Call once, as the plugin loads.
void Install();

// Notes whether a pixel shader made at an address reads a lightmap and has a
// sampler for its night (`code` its bytecode).
void CreatedPixel(const std::vector<std::byte> &code, IDirect3DPixelShader9 *shader);

// Hands a level's shader the night of the lightmap it is about to read and
// how far to go to it (`picture`: the frame's own picture is drawn into,
// not a portrait's). Call before each draw.
void Drawing(IDirect3DDevice9 *device, bool picture);

} // namespace nightlightmaps
