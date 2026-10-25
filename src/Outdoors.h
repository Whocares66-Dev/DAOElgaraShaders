// The light of the air out of doors: the area's fog lit by its sun and its
// sky (Improved lighting on the options screen). The engine fogs every surface
// and its sky toward one color an area gives, whichever way one looks; haze
// in daylight is brighter toward the sun, in the sun's color. This keeps
// how much fog there is and the area's color on the mean, and gives the air
// the sun's light by the way each pixel looks (shaders/haze.hlsl): each frame
// it hands the device's registers the air's light and the view each surface,
// the sky and the effects reckon it along.
//
// What each area has of its own is written to the log as the party comes
// into it. A file beside the DLL, `outdoors.tune`, is a developer's: while the
// game runs it is read again whenever it is saved, `name = value` a line:
// `Haze = 0` for none; `HazeG`, how the air's glow leans toward the sun (0
// to 0.95, 0.3 if not said); `SunlitFigures` 0 or 1 in place
// of the options screen's; `Fills`, how many times the area's own its fills
// are on figures, out of doors, in place of Improved lighting's rule; `Sun` and `SunOnFigures`, how many
// times the area's own the sun is on the ground and on figures; `sky.<a field
// of dao/core/Sky.h's kSkyFields, its name with _ for spaces>`, that value
// outright. Without the file nothing of the area is changed.
#pragma once

#include <array>
#include <optional>
#include <string_view>

namespace outdoors
{

// The air's light as every fogged surface, the sky and the clouds fog
// toward it (haze.hlsl): whether it is ours, the part the sky's light makes
// (the same every way), the part the sun or the moon makes (its mean over all
// ways), and how that part's glow leans toward the light.
struct SkyHaze
{
    bool on = false;
    std::array<float, 3> sky{};
    std::array<float, 3> sun{};
    float g = 0;
};

// The air's light as it is now. Call on the game's thread.
SkyHaze Haze();

// What a surface's own shader reckons the haze it is seen through by
// (haze.hlsl's daoHazeView), five registers: the camera's axes in the world,
// right and down the picture and ahead, each with one over the picture's
// width, one over its height and the lens's width over its focal length;
// the way to the sun in the world and the lens's height over its focal
// length; the sky's fog's cap and its zenith in radians. All 0 where the
// haze is not ours. Call on the game's thread.
std::array<float, 20> HazeView();

// How strong the area's fills are on figures and props (shaders/fills.hlsl),
// 1 for vanilla's. Call on the game's thread.
float Fills();

// Whether the area's level has a sun: out of doors, its sun on the ground not
// black. The game thread's word, as of the last Poll.
bool Sunlit();

// Whether the player wants the air lit. Call on the game's thread.
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

// Logs the area's own light when it changes, keeps the air's light and the
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
