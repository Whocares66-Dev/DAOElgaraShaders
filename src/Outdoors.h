// The light out of doors (Improved lighting on the options screen): whether
// the area is out of doors and sunlit, the one sun on figures there and the
// area's fills, and the hour's sun and sky where the day and the night run
// (DayNight.h), set on the area each frame. The air is fogged as the engine
// fogs it, toward the area's fog color, the hour's under the clock.
//
// What each area has of its own is written to the log as the party comes
// into it. A file beside the DLL, `outdoors.tune`, is a developer's: while the
// game runs it is read again whenever it is saved, `name = value` a line:
// `SunlitFigures` 0 or 1 in place of the options screen's; `Fills`, how many
// times the area's own its fills are on figures, out of doors, in place of
// Improved lighting's rule; `Sun` and `SunOnFigures`, how many times the
// area's own the sun is on the ground and on figures; `DawnHaze` and
// `DuskHaze`, the day's haze's depth at dawn and dusk (DayNight.cpp's
// DayHaze); `Fill`, `Window` and `WindowReach`, an interior's share of its
// makers' light, its windows' light and their reach (DayNight.h's
// TryIndoors); `sky.<a field of dao/core/Sky.h's kSkyFields, its name with _
// for spaces>`, that value outright. Without the file nothing of the area is
// changed.
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace outdoors
{

// How strong the area's fills are on figures and props (shaders/fills.hlsl),
// 1 for vanilla's. Call on the game's thread.
float Fills();

// Whether the area's level has a sun: out of doors, its sun on the ground not
// black. The game thread's word, as of the last Poll.
bool Sunlit();

// Whether the player's area is on the day and night's list (data/areas), the
// option on or off. The game thread's word, as of the last Poll.
bool CyclingArea();

// Whether the player wants Improved lighting. Call on the game's thread.
void Want(bool on);

// Whether the player wants figures out of doors lit more by the sun or the
// moon and less by the area's fills (Improved lighting on the options screen).
// Call on the game's thread.
void WantSunlitFigures(bool on);

// Whether the day and night run (DayNight.h): the options screen's Day / night
// cycle. Call on the game's thread.
void WantClock(bool on);

// A number of the developer's tuning file, or `otherwise` where it has none.
float Tune(std::string_view key, float otherwise);

// Logs the area's own light when it changes, keeps the hour's light and the
// tuning file's. For dao::OnFrame: the atmosphere is read and set on the
// game's thread. Looked at every half second, and on the frame after
// SceneChanged.
void Poll();

// Says the scene has just changed: the game's mode (the party picker opened
// or closed) or an area loaded. Call on the game's thread.
void SceneChanged();

// How many times the scene has changed (SceneChanged) or gone from out of
// doors to not or back (Poll): what was worked out from it is to be handed on
// again when this moves.
int Changes();

} // namespace outdoors
