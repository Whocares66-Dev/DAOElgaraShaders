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

#include <filesystem>

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

// Whether the level's lamps named as fires are lit by the plugin in place of
// the engine (daynight::FireLamp): the game thread's word, with Want's.
void WantLamps(bool on);

// Whether lights that can light nothing on the screen are left out (a
// developer's: outdoors.tune's LightCull = 0 keeps them). The game thread's.
void WantCulled(bool on);

// How bright fires and lamps are lit, a share of their own light: the game
// thread's word (daynight::FireStrength, or outdoors.tune's FireStrength).
void FireStrength(float share);

// Says a level was opened (`levelRim` its archive of rooms): its lamps named
// as fires are read from its layouts, off the calling thread. Any thread.
void LevelOpened(const std::filesystem::path &levelRim);

// Reads the lights of the area's effects as the engine would hand them to a
// shader. For dao::OnFrame.
void Read();

// Hands the shaders the lights last read (effect_lights.hlsl's registers),
// as the scene starts and as the first target changes; none where the target
// is not the frame's own picture. A portrait and the record's and the
// inventory's figures are drawn into targets of their own, in a scene of
// their own: given the area's lights, Redcliffe castle's room fires (DAOLib's
// RunningEffects since ca02aa6) lit them pale, Morrigan's black hair
// lavender (issue 51). `sceneStarts`: a scene begins. The scene takes the
// lights last read as it first draws into the picture, and a target changed
// back to the picture in it hands those same lights again, the ones its
// shadow maps were drawn for.
void Hand(IDirect3DDevice9 *device, bool picture, bool sceneStarts);

// The lights handed for this scene, in their order, each its place and reach:
// what their shadows are drawn from (LevelShadows.cpp). The render thread's.
const std::vector<std::array<float, 4>> &Handed();

} // namespace effectlights
