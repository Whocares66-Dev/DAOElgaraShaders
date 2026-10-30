#pragma once

#include <d3d9.h>

#include <string_view>

// The GPU's time between marks in a frame, by D3D9's timestamp queries: a
// developer's measure (issue 61, 2026-10-28), never a player's. Only with a
// file `gpu-timing` beside the DLL as the game starts: 0.2.1 crashed in
// Direct3D as the game shut down with timestamp queries of the plugin's still
// made (621cd87), and no build without the file makes one.
//
// A mark is put where the frame's drawing changes target (the scene, a
// shadow map, the picture) and where the plugin's own parts begin; the time
// from one mark to the next is the part's that the first names. Each frame's
// queries are read some frames later, never waited on, and every kLogFrames
// frames each part's mean time a frame is logged, the most first.
namespace gputimer
{

// Reads whether the file is there. Call once, as the plugin starts.
void Start();

// Whether the timer runs.
bool On();

// A mark: the GPU's time from here to the next mark is `name`'s.
void Mark(IDirect3DDevice9 *device, std::string_view name);

// A part of the plugin's own, drawn amid the engine's: the marks made in it
// (its targets) are named `name: <target>`, and at its End the time is again
// the part's that was running before it.
void Begin(IDirect3DDevice9 *device, std::string_view name);
void End(IDirect3DDevice9 *device);

// A draw made in the part now running, for the log's counts.
void Drawn();

// The frame is ended (Present): its last part is closed and an older frame's
// times are read.
void Presented(IDirect3DDevice9 *device);

// Every query let go: before the device is reset.
void Release();

} // namespace gputimer
