// The light of the air out of doors: the area's fog lit by its sun
// (Improved lighting on the options screen). The engine fogs every surface and its
// sky towards one colour an area gives, whichever way one looks; haze in
// daylight is brighter and warmer towards the sun. This keeps the area's
// colour and how much fog there is, and adds the sun's light in the air by
// the way each pixel looks (shaders/haze.hlsl, haze_pass.hlsl): each frame it
// hands the colour grade's chain the camera's way to the sun, and the
// device's registers the air's light for the sky and the effects.
//
// What each area has of its own is written to the log as the party comes
// into it. A file beside the DLL, `outdoors.tune`, is a developer's: while the
// game runs it is read again whenever it is saved, `name = value` a line:
// `Haze = 0` for none,
// `HazeShow = 1` for the fog as white air; `SunlitFigures` 0 or 1 in place
// of the options screen's; `Fills`, how many times the area's own its fills
// are on figures, out of doors, in place of Improved lighting's rule; `Sun` and `SunOnFigures`, how many
// times the area's own the sun is on the ground and on figures; `sky.<a field
// of dao/core/Sky.h's kSkyFields, its name with _ for spaces>`, that value
// outright. Without the file nothing of the area is changed.
#pragma once

#include <array>
#include <string_view>

namespace outdoors
{

// The air's light as the sky and the clouds are to fog towards it (haze.hlsl):
// whether it is ours, the part the air's molecules scatter (Rayleigh's),
// the part its haze scatters (Mie's), and Mie's asymmetry, towards the sun.
struct SkyHaze
{
    bool on = false;
    std::array<float, 3> air{};
    std::array<float, 3> sun{};
    float g = 0;
};

// The air's light as it is now. Call on the game's thread.
SkyHaze Haze();

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

// A number of the developer's tuning file, or `otherwise` where it has none.
float Tune(std::string_view key, float otherwise);

// Logs the area's own light when it changes, keeps the air's light and the
// tuning file's. For dao::OnFrame: the atmosphere is read and set on the
// game's thread.
void Poll();

} // namespace outdoors
