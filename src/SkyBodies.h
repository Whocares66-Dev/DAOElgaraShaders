// The sun's picture by day and the moon's by night, in every area the day and
// night cycle runs in (#62).
//
// The sky has one disc, drawn where the engine's one sun comes from, in the
// picture its sky dome's material gives the sky shader's CelestialBody: a day
// dome (sb_day, sb_day_base.mao) names DefaultSun.dds, a night dome (sb_night,
// sb_night_base.mao) names none and has sky.mat's default, DefaultMoon (read
// in the game's archives, 2026-10-28). The clock keeps the area's own dome
// and turns its sun into the moon by night (DayNight.h), so Redcliffe's moon
// was the sun's picture drawn white, and the docks' sun, a night level's under
// a borrowed day, the moon's (a Nexus report, issue 62). Here the sky's draw
// is given the picture of the body the engine's sun is at the hour, each read
// through the game's files, so a mod's moon or sun is the one drawn.
#pragma once

#include <d3d9.h>

#include <cstddef>
#include <vector>

namespace skybodies
{

// Reads the two pictures, once, the first time it is called after the game
// has started. For dao::OnFrame: the game's thread.
void Poll();

// Notes whether a pixel shader made on the device reads the sky's disc
// (smpCelestialBody), and from which sampler. Call from CreatePixelShader.
void CreatedPixel(const std::vector<std::byte> &code, IDirect3DPixelShader9 *shader);

// Binds the hour's body's picture where the sky's shader reads its disc, if
// the shader now set is a sky's and the clock runs; Drawn gives the engine's
// picture back. Call about a draw, on the drawing thread.
void Drawing(IDirect3DDevice9 *device);
void Drawn(IDirect3DDevice9 *device);

} // namespace skybodies
