// The lights of effects (a flaming blade, a spell, a fire's glow) on every
// surface they reach. The engine gives each mesh and each figure a few of
// its lights, chosen by how much of a light's reach is left at the object's
// place: a wall whose middle is out of reach of a flaming sword a metre from
// it took none of its light, and the ground out of doors, whose shaders
// reckon no point light at all, never one (both seen in the Arl's dungeon,
// 2026-10-17, in vanilla as with the plugin). Here the engine's lists of
// such lights are read each frame and every lit shader reckons all of them
// (build_shaders.py's with_effect_lights, effect_lights.hlsl), and the
// engine is kept from choosing them for anything itself, so none is counted
// twice.
//
// Which lights: a point light of an effect comes to the light manager as two
// nodes, one in the figures' unflagged list (+0x84) and one in the level's
// (+0xC), and those two lists hold little else (the dungeon's +0xC held one
// lamp before Flaming Weapons was cast and five lights after; logged
// 2026-10-17). The level's are read; the engine's choice from both is
// stopped by its own means, each list's most for an object (+4 of the list,
// set at 0x00462210) made 0.
#pragma once

#include <d3d9.h>

#include <array>
#include <cstddef>
#include <vector>

namespace effectlights
{

// Places the detours. False where one could not be placed: the engine's own
// choice of the effects' lights stays.
bool Install();

// Whether the effects' lights are the plugin's: the game thread's word,
// with Improved lighting. Without, the engine chooses them again as each
// manager next takes lights in, and the shaders are handed none.
void Want(bool on);

// Reads the lights of the area's effects as the engine would hand them to a
// shader. For dao::OnFrame.
void Read();

// Hands the shaders the lights last read (effect_lights.hlsl's registers),
// as the scene starts.
void Hand(IDirect3DDevice9 *device);

// The lights handed for this scene, in their order, each its place and reach:
// what their shadows are drawn from (LevelShadows.cpp). The render thread's.
const std::vector<std::array<float, 4>> &Handed();

} // namespace effectlights
