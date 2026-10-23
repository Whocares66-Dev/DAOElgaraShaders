// The level's own surfaces in the engine's sun shadow maps: terrain,
// buildings and furnishings, and trees' trunks and branches, so
// that the sun is shadowed by the world as it is drawn, not only where the
// level's lightmaps were baked to say. The engine draws only figures and what
// they carry into its two shadow maps (pssmmesh.vsh and pssmskinmesh.vsh,
// one partition's matrix each; traced 2026-10-16); every shader that takes
// the sun reads those maps already, a figure's and the ground's alike.
//
// The level's draws into the scene are noted as the device has them
// (buffers, declaration, the vertex shader and its constants), each kept by
// what it draws and where until another level is opened, and as the engine
// leaves a shadow map it has drawn figures into, before it blurs it, every
// one kept is drawn into it too: by the shader
// build's shadow variant of each draw's vertex shader (build_shaders.py's
// with_shadow_place), which places the vertex by the partition's matrix, and
// a pixel shader that writes the depth as the engine's does
// (shadow_cast.hlsl). What is behind the camera casts once it has been seen.
#pragma once

#include <d3d9.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace levelshadows
{

// The device's own draw, as it was before the plugin's detour: the level is
// drawn by it, so that its draws are not noted again.
using DrawIndexedFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);

// Reads the shadow variants in `override` and the game's shaders they are
// made from, in the game's shaders.erf under `game`.
void Start(const std::filesystem::path &override, const std::filesystem::path &game, DrawIndexedFn draw);

// The device's own SetSamplerState, past Graphics.cpp's detour of it: the
// sampler a receiver is lent the atlas on is set and given back exactly, and
// the detour's record of what the game asked of it is left as the game left
// it. Until it is given, the device's.
using SetSamplerStateFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
void UseSamplerState(SetSamplerStateFn set);

// Whether the level is drawn into the shadow maps. The game thread's word.
void Want(bool on);

// Whether the effects' lights' shadows are drawn (effect_lights.hlsl's
// DaoEffectShadow), each light's from the figures about it: the game
// thread's word, with Improved lighting.
void WantEffectShadows(bool on);
// A vertex shader the game made, from `code`.
void Created(const DWORD *code, IDirect3DVertexShader9 *shader);

// Says the game made a pixel shader: a receiver of the sun's shadow is given
// the atlas on the sampler it reads the engine's near map on.
void CreatedPixel(const DWORD *code, IDirect3DPixelShader9 *shader);

// An indexed draw about to be made, into the frame's own picture or not.
void Drawing(IDirect3DDevice9 *device, bool picture, D3DPRIMITIVETYPE type, INT base, UINT least, UINT vertices,
             UINT first, UINT count);

// A draw not indexed about to be made: a receiver is lent the maps as an
// indexed one is. One a frame in Redcliffe castle's courtyard (2026-10-23),
// whose taps of the old aim's maps read what the draw before had left on the
// sampler, and its tree went light for a second each time the moon's maps
// were aimed anew (the user: "trees pulsate b/t light / dark").
void DrawingPlain(IDirect3DDevice9 *device, bool picture);

// A draw not of the scene about to be made (the colour grade's chain): a pass
// that reads the maps for the air (haze_pass.hlsl) is lent them and their
// registers, as they were for the frame's picture.
void Air(IDirect3DDevice9 *device);
// The draw Drawing or Air was told of made: what it lent the draw is given
// back.
void Drawn(IDirect3DDevice9 *device);

// The device's first target about to change: where the level is drawn into a
// shadow map the engine is done with.
void Leaving(IDirect3DDevice9 *device);

// A developer's: the kinds of draw left out of the maps, 1 terrain, 2
// buildings and furnishings, 4 trees' branches, 8 their fronds and leaves, 16
// the figures.
void Skip(unsigned kinds);

// The way the sun's light travels, a unit vector, which the maps fitted to
// the view are drawn along: the lighting's own, the game thread's word.
void Sun(const std::array<float, 3> &going);

// What the camera looks at, its focus, which the maps are fitted around; none
// where there is no camera to follow. The game thread's word.
void Focus(const std::optional<std::array<float, 3>> &focus);

// A level is opened: the draws seen of the last are let go. Any thread.
void Forget();

// What the shaders that receive the sun's shadows are to read (shadow.hlsl's
// daoShadowData): the maps fitted to the view, where the device's first
// target is the frame's own picture (`picture`) and the level is drawn into
// them; else the engine's own. Call as the scene starts and as the first
// target changes.
void Receiving(IDirect3DDevice9 *device, bool picture);

// The frame is done.
void Presented();

// The device is to be reset or released: everything held of it let go.
void Release();

} // namespace levelshadows
