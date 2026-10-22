// A day and a night in one area: the sun going over and down, the moon after
// it, the sky, the fog and the level's baked light going from the one to the
// other by the hour (Improved lighting, Redcliffe's village alone so far).
//
// The game has no time of day. A few places are two levels, one made for the
// day and one for the night, of the same ground: Redcliffe's village
// (lak100d and lak100n), Denerim's market (den200d, den200n) and hrt201. Each
// has its own sun or moon and its own sky and fog (its .arl), and its own
// sky's light (its layout's ambient light, which its lightmaps were baked
// with). The clock runs in the day level, and takes of the night level its
// moon, its sky and its sky's light:
//
// - The day level's sun is a summer afternoon's, at 15:22 by a clock an hour
//   on from the sun's (kSolarNoon): the latitude is 90 degrees less its
//   height (Redcliffe's 38 is 52, England's), and the sun goes round the sky
//   by the hour on that latitude's path at 18 degrees' declination, through
//   the level's own sun. The moon goes round the same sky, at the night
//   level's moon at midnight.
// - The sun's colour is the day's, less what the air takes out of it on the
//   longer way through it as the sun goes low (each channel by the area's own
//   scattering, Scattering), and none under the horizon. The engine's one sun
//   is the moon once the sun is down, coming up from nothing as the sun goes
//   to 6 degrees under.
// - The sky, the fog and the clouds go from the day level's to the night's
//   (dao::core::Blend) as the sun goes from 6 degrees up to 12 under, the end
//   of nautical twilight. The level's bake goes, as the sky does, to the night
//   sky's even light (night_sky.hlsl), and its probes (Probes.cpp) and the
//   ambient colour the engine adds (dao::lights::OnAmbient) with it.
// - The layouts' lights (_ll: the makers' sunlight about the village) go with
//   the sun's light, the figures' fills (_lc) with the sky's
//   (dao::lights::OnPointLight); and the engine's grid of where a figure is
//   out of the level's one sun is let go (dao::lights::OnSunShare).
//
// The options screen's Day / night cycle runs it, and its Time of day reads
// and sets the hour (Graphics.cpp); the developer's tuning file's
// `DayMinutes` (Outdoors.h) is how many minutes of play a day takes (an hour
// if not said, 0 to hold the hour).
#pragma once

#include "dao/core/Sky.h"

#include <array>
#include <filesystem>
#include <optional>

namespace daynight
{

// The light of the hour: the sun or the moon as the engine is to have it,
// the sky, the sky's light over the level's own, the sun's light over the
// day level's (for its lamps and the haze's share of it), and the colour the
// sky's disc is drawn in.
struct Light
{
    dao::core::Sunlight sun;
    dao::core::Sky sky;
    std::array<float, 3> baked{1, 1, 1};
    std::array<float, 3> sunOverDay{1, 1, 1};
    std::array<float, 3> disc{1, 1, 1}; // the sun's hue through the air, the moon's white
    float nightShare = 0;               // how far the hour is from the day's bake to the night sky's light
    std::array<float, 3> nightSky{};    // the night sky's light, as the lightmaps keep light (night_sky.hlsl)
};

// Has the level's ambient colour, its lights and the figures' share of the
// sun follow the hour (dao::lights). Call once, as the plugin loads.
void Install();

// The night's light on what the level's bake lights (night_sky.hlsl): how
// far the hour is from the day's bake to the night sky's even light, 0 by day
// and 1 at night, as the sky goes between the two levels'; and that light,
// the night level's sky's, paled and as strong as Now's `night` says. 0 where
// the clock is not running. Any thread.
float NightShare();
std::array<float, 3> NightSky();

// Says a level was opened: `levelRim` its archive of rooms,
// packages\core\env\<level>\<level>.rim. Reads the level's and its other's
// light where it has one. Any thread.
void Opened(const std::filesystem::path &levelRim);

// Sets the clock to an hour, 0 to 24. Call on the game's thread.
void SetHour(float hour);

// The clock's hour. Call on the game's thread.
float Hour();

// The light of the hour for the area as it is, or none where its level has no
// day and night or `running` is false. The level is the one whose sun is
// `ownSun`, the area's own; `ownSky` is the area's own sky, of which what the
// files do not hold is kept. The clock goes on by a day in `dayMinutes` of
// play while it runs there. `night` is how bright the moon is, a share of the
// night level's sun's; `fill` how much of the night level's sky light the
// night keeps beside it. Call on the game's thread.
std::optional<Light> Now(const dao::core::Sunlight &ownSun, const dao::core::Sky &ownSky, bool running,
                         float dayMinutes, float night, float fill);

// The sky light's multiplier as of the last Now, 1 where there is none. Any
// thread.
std::array<float, 3> Baked();

// The colour the sky's disc is drawn in (Light's disc), or none where the
// clock is not running. Any thread.
std::optional<std::array<float, 3>> Disc();

// How much the air of a sky scatters, each channel, per metre: its molecules
// (Rayleigh's (6.95, 11.8, 24.4) per million metres times the sky's rayleigh
// multiplier) and its haze (Preetham's, by the sky's mie multiplier and
// turbidity), as the engine reckons them for its sky (lib-agent, read in the
// sky's recompute 0x0055BFA0, 2026-10-16), at 650, 570 and 475 nm.
struct Scattered
{
    std::array<double, 3> air{};
    std::array<double, 3> haze{};
};
Scattered Scattering(const dao::core::Sky &sky);

} // namespace daynight
