// A day and a night in one area: the sun going over and down, the moon after
// it, the sky, the fog and the level's baked light going from the one to the
// other by the hour (Improved lighting, Redcliffe's village and castle so far).
//
// The game has no time of day. A few places are two levels, one made for the
// day and one for the night, of the same ground: Redcliffe's village
// (lak100d and lak100n), Denerim's market (den200d, den200n) and hrt201. Each
// has its own sun or moon and its own sky and fog (its .arl), and its own
// sky's light (its layout's ambient light, which its lightmaps were baked
// with). The clock runs in the areas data/areas names, each on its
// day level, and takes of a night level its moon, its sky and its sky's
// light; Redcliffe's castle (lak200d) has no night level and takes the
// village's. The table says, by area, how its sun and moon go:
//
// - The day level's sun is an afternoon's of the area's season (its
//   declination, a summer's 18 degrees by default) at the hour it is that
//   high: the latitude is 90 degrees less its height (Redcliffe's 38 is 52,
//   England's) or the table's, and the sun goes round the sky by the hour on
//   that latitude's path, highest at the table's noon (13:00, a summer's
//   clock). The moon goes round the same sky, highest at midnight, at the
//   night level's moon or where the table puts it.
// - The sun's colour is the day's, less what the air takes out of it on the
//   longer way through it as the sun goes low (each channel by the area's own
//   scattering, Scattering, and the day's haze, thin after dawn and thicker by
//   dusk), and none under the horizon. The engine's one sun
//   is the moon once the sun is down, coming up from nothing as the sun goes
//   to 6 degrees under.
// - The sky and the clouds go from the day level's to the night's
//   (dao::core::Blend) as the sun goes from 6 degrees up to 12 under, the end
//   of nautical twilight. The level's bake goes, as the sky does, to the night
//   sky's even light (night_sky.hlsl), and its probes (Probes.cpp) and the
//   ambient colour the engine adds (dao::lights::OnAmbient) with it.
// - The fog's color is what the hour's sun or moon and sky light make of the
//   air, by how much of the light on it each level's own fog color is
//   (DayNight.cpp's LightAir).
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
#include <string>
#include <string_view>
#include <vector>

namespace daynight
{

// The light of the hour: the sun or the moon as the engine is to have it,
// the sky (its fog's color the hour's: LightAir), the sky's light over the
// level's own, the sun's light over the day level's (for its lamps), and the
// color the sky's disc is drawn in.
struct Light
{
    dao::core::Sunlight sun;
    dao::core::Sky sky;
    std::array<float, 3> baked{1, 1, 1};
    std::array<float, 3> sunOverDay{1, 1, 1};
    std::array<float, 3> disc{1, 1, 1};       // the sun's hue through the air, the moon's white
    bool moon = false;                        // the disc is the moon's: the engine's sun is the moon (SkyBodies.h)
    float nightShare = 0;                     // how far the hour is from the day's bake to the night sky's light
    std::array<float, 3> nightSky{};          // the night sky's light, as the lightmaps keep light (night_sky.hlsl)
    std::array<float, 2> water{1, 1};         // water's light over the level's own, and its colour kept (Water)
    std::array<float, 3> lit{1, 1, 1};        // the light on what is lit about, over the level's own (Lit)
    float evenShare = 0;                      // how far the bake gives way to the even light (EvenShare)
    std::array<float, 3> even{};              // that light, as the lightmaps keep light (EvenLight)
    std::array<float, 3> outdoor{};           // the sun's or the moon's light and the sky's together, out of doors
    std::array<float, 3> window{};            // an interior's: its windows' light a square metre (WindowLight)
    std::array<float, 4> bakeHue{1, 1, 1, 0}; // an interior's: its bake's hue, and how far each texel takes it
    std::array<float, 3> bakeWindow{1, 1, 1}; // an interior's: its bake where a window sees it, over the rest
    std::array<float, 3> glass{1, 1, 1};      // an interior's: its windows' glass by the hour (Glass)
    float relitNight = 0;                     // an interior's: how far its bake goes to its night lightmaps
};

// Reads data/areas (a file for each area the clock runs in, from the plugin's
// folder), and has the level's ambient colour, its lights and the figures'
// share of the sun follow the hour (dao::lights). Call once, as the plugin
// loads.
void Install();

// The night's light on what the level's bake lights (night_sky.hlsl): how
// far the hour is from the day's bake to the night sky's even light, 0 by day
// and 1 at night, as the sky goes between the two levels'; and that light,
// the night level's sky's, paled and as strong as Now's `night` says. 0 where
// the clock is not running. Any thread.
float NightShare();

// Whether the clock ran at the last Now. Any thread.
bool Running();
std::array<float, 3> NightSky();

// The bake's even light (night_sky.hlsl): how far the level's bake gives way
// to it, the night's share where the level's own is the day, 1 where it is
// the night (a borrowed day: its bake is the makers' night); and that light,
// the hour's sky's as the lightmaps keep light. 0 where the clock is not
// running. Any thread.
float EvenShare();
std::array<float, 3> EvenLight();

// The day level's sky light, as its lightmaps keep light, as of the last Now.
// Any thread.
std::array<float, 3> DaySky();

// Says a level was opened: `levelRim` its archive of rooms,
// packages\core\env\<level>\<level>.rim, or a mod's or an add-in's. The
// level's light (its sun, sky and sky light), and its other's where it has
// one, are read at the next Now, the .arl the copy the game takes (a mod's).
// Any thread. `coreEnv`: the game's own levels' folder, where the archive of a
// night an add-in's or a mod's level borrows is looked for when its own folder
// has none.
void Opened(const std::filesystem::path &levelRim, const std::filesystem::path &coreEnv);

// Sets the clock to an hour, 0 to 24. Call on the game's thread.
void SetHour(float hour);

// The clock's hour. Call on the game's thread.
float Hour();

// Whether a point light is a layout's lamp named as a fire: a room's
// (<level>_<x>_<y>_<d>_ll) whose name starts as data/areas/defaults.ini's
// `fires` say (torch, fire). Those keep their light by night, and the plugin
// lights by them itself (EffectLights.cpp). Any thread, once Install has run.
bool FireLamp(std::wstring_view model, std::wstring_view name);

// Whether a light's colour is a fire's: red the most, blue the least. A
// level's layouts light more than fires about a fire: the Circle tower's
// floors (lak503d) put the moonlight's blue bounce (bounce_RT, 0.34, 0.51,
// 0.99) and its pink (bounce_pink_RT), the books' violet glow (book_RT) and
// the windows' (window_001_RT, 0.69, 0.72, 0.79) among their candles and
// lanterns (candle_001_RT, Cndl_RT, lamp_001_RT, 0.91, 0.48, 0.15; read
// 2026-10-25). Indoors, where no sun is, a fire-coloured layout light is a
// fitting's and the rest are the makers' fill. Any thread.
bool FireColoured(const std::array<float, 3> &colour);

// How bright the plugin lights fires, a share of what the level or
// effects.ini says (data/areas/defaults.ini's `fire_strength`). Any thread,
// once Install has run.
float FireStrength();

// A lamp of the area's level its file moves (its `move_lamp` keys): from where
// the level puts it to where its light is to be, metres in the world.
struct LampMove
{
    std::array<float, 3> from{};
    std::array<float, 3> to{};
};

// The lamps the area the clock last ran in moves, none where it is not
// running. Any thread.
std::vector<LampMove> LampMoves();

// What an area's own level borrows: the other level, and whether the own is
// the night (a borrowed day) and not the day (a borrowed night); none where
// the clock does not run in it. Any thread.
struct Borrowed
{
    std::wstring other;
    bool ownIsNight = false;
};
std::optional<Borrowed> BorrowedBy(std::wstring_view level);

// Whether the clock runs in an area (its tag) out of doors: it has a file in
// data/areas that is not an interior's. Any thread, once Install has run.
bool Cycles(std::wstring_view area);

// Whether an area (its tag) is an interior the clock runs in: its file says
// the area out of doors its windows look on (`outside`). Its own level keeps
// its makers' light, a share of it (`fill`), by the hour's light out of doors
// over midnight's, and its windows are lights of the hour (WindowLight).
// Any thread, once Install has run.
bool Indoors(std::wstring_view area);

// The materials of a level's windows, as the file of an interior on it names
// them (`windows`); none where no interior's file names the level. Any
// thread, once Install has run.
std::vector<std::wstring> WindowMaterials(std::wstring_view level);

// The starts of the names of the beams of light a level's interior's file
// takes for openings (`beams`, defaults.ini's), none where no interior's file
// names the level. Any thread, once Install has run.
std::vector<std::wstring> BeamModels(std::wstring_view level);

// An interior's windows' light as of the last Now: a square metre of opening's
// colour, the area's `window` times the light out of doors (Light's outdoor),
// and how far it reaches, metres; nothing where the clock is not running
// indoors. Any thread.
struct Windows
{
    std::array<float, 3> colour{};
    float reach = 0;
};
Windows WindowLight();

// An interior's bake's mean colour at a luminance of 1, and how far each
// texel of its lightmaps takes that colour at its own luminance (night_sky.hlsl's
// daoBakeHue): at the other of day and night from its makers' wholly, so
// that the other's colour (by Baked) lights a violet moonlit stone and an
// orange candle's pool alike; at the makers' own none. 0 where the clock is not running indoors. Any
// thread.
std::array<float, 4> BakeHue();

// An interior's bake where a window sees it, over the rest of it
// (night_sky.hlsl's daoBakeWindow, effect_lights.hlsl's DaoWindowSeen): by
// day the light out of doors against the day level's own hour's, in each
// colour, so that the windows' share of the makers' day darkens and warms at
// dusk (the user, 2026-10-25: "Let's try with the windows carrying daylight");
// at night 1. 1 where the clock is not running indoors. Any thread.
std::array<float, 3> BakeWindow();

// An interior's night by its lamps (NightLightmaps.h, relit_night.hlsl): x
// how far its bake goes to the night lightmaps of the plugin's, as the
// daylight goes (1 less the daylight against noon's, IndoorsAt); y what their
// light is multiplied by. x 0 where the clock is not running indoors.
// Any thread.
std::array<float, 4> RelitNight();

// An interior's window glass by the hour (glass.hlsl): the light out of
// doors at the hour against the day's, each colour; 1 where the clock is not
// running indoors. Any thread.
std::array<float, 3> Glass();

// Whether a material object (its file's name, lower case, with or without
// .mao) is an interior's window pane that glows by daylight, as its file's
// `glass` says. Any thread.
bool IsGlassMaterial(std::wstring_view name);

// How far an interior's lamps are in its night lightmaps now (RelitNight's
// share where the level is drawn with them, else 0): its makers' fire lights,
// kept whole indoors, go by 1 less it. The drawing thread's word, a frame
// behind (NightLightmaps.h's Baked).
void LampsBaked(float share);

// An interior's share of its makers' light, its windows' light and their
// reach to try in place of its file's `fill`, `window` and `window_reach`
// (outdoors.tune's Fill, Window and WindowReach, a developer's), none for the
// file's; and the night lightmaps' strength (RelitNight), none for 1. Call on the game's thread, before Now.
void TryIndoors(std::optional<float> fill, std::optional<float> window, std::optional<float> reach,
                std::optional<float> relit = std::nullopt);

// A moon to try in place of the area's file's (outdoors.tune's MoonTurn and
// MoonHeight, a developer's): its turn and its height in degrees, none for the
// file's. Call on the game's thread, before Now.
void TryMoon(std::optional<float> turn, std::optional<float> height);

// A share of the night level's colour for the night to keep in place of the
// plugin's (outdoors.tune's NightColour, a developer's), none for the
// plugin's. Call on the game's thread, before Now.
void TryNightColour(std::optional<float> kept);

// A share of the moon's beam for the night's air to take in place of the
// plugin's (outdoors.tune's FogMoon, a developer's), none for the plugin's.
// Call on the game's thread, before Now.
void TryFogMoon(std::optional<float> share);

// A share of the moon's light for the area's water to take by night in place
// of its file's `water_moonlit` (outdoors.tune's WaterMoonlit, a
// developer's), none for the file's. Call on the game's thread, before Now.
void TryWaterMoonlit(std::optional<float> share);

// The day's haze's depth at dawn and at dusk to try in place of the
// plugin's (outdoors.tune's DawnHaze and DuskHaze, a developer's), none for
// the plugin's. Call on the game's thread, before Now.
void TryDayHaze(std::optional<float> dawn, std::optional<float> dusk);

// The light of the hour for the area as it is, or none where the clock does
// not run in `area` (its tag) or `running` is false. The level is the one
// whose sun is `ownSun`, the area's own; `ownSky` is the area's own sky, of
// which what the files do not hold is kept. The clock goes on by a day in `dayMinutes` of
// play while it runs there. `night` is how bright the moon is, a share of the
// night level's sun's; `fill`, where given, how much of the night level's sky
// light the night keeps beside it, in place of the area's (its file's
// `night_fill`); `haze`, where given, how much of the area's own haze its
// night's sky has, in place of its file's `night_haze`. Call on the game's
// thread.
std::optional<Light> Now(std::wstring_view area, const dao::core::Sunlight &ownSun, const dao::core::Sky &ownSky,
                         bool running, float dayMinutes, float night, std::optional<float> fill,
                         std::optional<float> haze);

// The sky light's multiplier as of the last Now, 1 where there is none. Any
// thread.
std::array<float, 3> Baked();

// The light on what is lit about (a fall's spray, a mist, smoke:
// effects.ini's `lit`), as of the last Now: the sun's or the moon's and the
// sky's together, over the level's own sun's and sky's, in each colour.
// 1 where the clock is not running. Any thread.
std::array<float, 3> Lit();

// Water's light as of the last Now: how strong, over the level's own by day,
// and how much of its colour is kept (as the night keeps of the moon's and
// the sky's). Water is lit by the sky's sun scattered (water_light.hlsl),
// not by the sun's own light, so the hour's light does not reach it by
// itself. 1 and 1 where the clock is not running. Any thread.
std::array<float, 2> Water();

// The colour the sky's disc is drawn in (Light's disc), or none where the
// clock is not running. Any thread.
std::optional<std::array<float, 3>> Disc();

// Whether the sky's disc is the moon, as of the last Now; false where the
// clock is not running. Any thread.
bool DiscIsMoon();

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
