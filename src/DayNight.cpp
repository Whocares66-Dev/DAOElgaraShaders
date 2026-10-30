#include "DayNight.h"

#include "dao/Lights.h"
#include "dao/Module.h"
#include "dao/Resources.h"
#include "dao/core/Dds.h"
#include "dao/core/Erf.h"
#include "dao/core/Gff4.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace daynight
{

namespace
{

using Vec3 = std::array<float, 3>;
namespace gff4 = dao::core::gff4;

constexpr float kDegree = std::numbers::pi_v<float> / 180;

// The GFF4 labels of an .arl's light and a layout's nodes (DAOLib's
// dev/gff4_fields.csv).
constexpr std::uint32_t kSunlightColorChar = 3149;
constexpr std::uint32_t kSunlightDirection = 3150;
constexpr std::uint32_t kSunlightColor = 3152;
constexpr std::uint32_t kAtmoData = 22500;
constexpr std::uint32_t kAtmoSunColor = 22519;
constexpr std::uint32_t kAtmoSunIntensity = 22520;
constexpr std::uint32_t kAtmoTurbidity = 22521;
constexpr std::uint32_t kAtmoEarthReflectance = 22522;
constexpr std::uint32_t kAtmoMieMultiplier = 22523;
constexpr std::uint32_t kAtmoRayleighMultiplier = 22524;
constexpr std::uint32_t kAtmoDistanceMultiplier = 22526;
constexpr std::uint32_t kAtmoAlpha = 22528;
constexpr std::uint32_t kAtmoFogColor = 22529;
constexpr std::uint32_t kAtmoFogIntensity = 22530;
constexpr std::uint32_t kAtmoFogCap = 22531;
constexpr std::uint32_t kAtmoFogZenith = 22532;
constexpr std::uint32_t kCloudData = 22600;
constexpr std::uint32_t kCloudDensity = 22620;
constexpr std::uint32_t kCloudSharpness = 22621;
constexpr std::uint32_t kCloudDepth = 22622;
constexpr std::uint32_t kCloudRangeMultiplier1 = 22623;
constexpr std::uint32_t kCloudRangeMultiplier2 = 22624;
constexpr std::uint32_t kCloudColor = 22625;
constexpr std::uint32_t kMoonScale = 22700;
constexpr std::uint32_t kMoonAlpha = 22701;
constexpr std::uint32_t kAmbientColour = 6010;
constexpr std::uint32_t kChildren = 6999;
constexpr std::size_t kNodesDeep = 8;

// What a level's files say of its light: its sun or moon; of its sky each
// value its .arl has, over the area's own where it has none (the haze's hg is
// in none of Redcliffe's: the engine keeps an .arl's values as they are, its
// fog zenith 42.4 is 42.4 in the engine's block, logged 2026-10-17); the
// sky's light its lightmaps were baked with.
struct LevelLight
{
    dao::core::Sunlight sun;
    std::vector<std::pair<float dao::core::Sky::*, float>> sky;
    std::array<float, 3> skyLight{};
};

// What an interior's file says of its own light (data/areas/README.md).
struct IndoorLight
{
    float fill = 1;                    // how much of its makers' unmotivated light is kept (`fill`)
    float window = 0;                  // the windows' light a square metre, a share of the outdoor light (`window`)
    float windowReach = 0;             // metres (`window_reach`)
    std::vector<std::wstring> windows; // the materials of its windows' models (`windows`)
    std::vector<std::wstring> beams;   // the starts of its beams' models' names (`beams`)
    // Which its makers lit it as (`baked = day`, else a night), and, for a
    // night's bake, the level whose rooms' fill is its day's colour (`day`)
    // and that colour (ReadIndoorColours).
    bool ownIsNight = true;
    std::wstring other;
    std::optional<std::array<float, 3>> dayFill;
    std::array<float, 3> bakeHue{1, 1, 1}; // its bake's mean colour at a luminance of 1 (ReadIndoorColours)
    float bakeLuminance = 0;               // its bake's mean luminance (ReadIndoorColours)
    // For a day's bake, the level its makers lit the same rooms by night
    // (`night`), and its bake's mean colour (ReadIndoorColours): its night's
    // colour and brightness.
    std::wstring night;
    std::optional<std::array<float, 3>> nightBake;
};

struct Pair
{
    LevelLight day;
    LevelLight night;
    // Whether the area's own level, its bake's, is the night (its file's
    // `day`, a borrowed day) and not the day (its file's `night`).
    bool ownIsNight = false;
    std::wstring area;               // the area the clock runs in (its area's file)
    Vec3 midnight{};                 // the way to the moon at midnight (its area's file)
    Vec3 ownMoon{};                  // the way to the night level's own moon
    std::optional<float> moonHeight; // degrees, none for the night level's own (its area's file)
    float moonTurn = 0;              // degrees about the upright from the night level's own (its area's file)
    float latitude = 0;              // radians (its area's file)
    float declination = 0;           // the sun's, radians (its area's file)
    float noon = 0;                  // the hour the sun is highest (its area's file)
    float nightFill = 0;             // the share of the night sky's light kept by night (its area's file)
    float nightHaze = 0;             // how much of the area's own haze its night's sky has (its area's file)
    float effectsMoonlit = 0;        // how much of the moon's light its spray and mist take (its area's file)
    float waterMoonlit = 1;          // how much of the moon's light its water takes (its area's file)
    std::vector<std::pair<float dao::core::Sky::*, float>> dayClouds; // its file's `day_clouds` level's clouds
    std::vector<LampMove> lampMoves;                                  // its file's `move_lamp`s
    std::optional<float> heldHour; // the hour its light is held at (its file's `hour`), none for the clock's
    bool heldOwn = false;          // held at the level's own hour (its file's `hour = own`)
    float ownHour = 0;             // the hour the level's own sun stands where it is
    std::vector<std::pair<std::wstring, std::int32_t>> heldUntil; // plots' flags any of which lets the clock run
    // An interior (its file's `outside`): the fields above are the outside
    // area's, whose clock and sky its windows see; its own light is its
    // level's, the makers', by the hour (IndoorsAt).
    std::optional<IndoorLight> indoors;
};

std::wstring Lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

void Put(std::vector<std::pair<float dao::core::Sky::*, float>> &sky, const gff4::Struct &in, std::uint32_t label,
         std::initializer_list<float dao::core::Sky::*> members)
{
    const std::vector<float> values = in.Floats(label);
    std::size_t i = 0;
    for (float dao::core::Sky::*member : members)
    {
        if (i < values.size())
        {
            sky.emplace_back(member, values[i++]);
        }
    }
}

// A level's sun and sky, from its .arl: the copy the game takes, a mod's
// where one has it (Dragon Age Evolved's lak200d.arl in the player's
// override, #66). The game thread's.
bool ReadArl(const std::wstring &level, LevelLight &light)
{
    const std::optional<std::vector<std::byte>> bytes = dao::resources::Read(level + L".arl");
    const std::optional<gff4::File> file = bytes ? gff4::File::Parse(*bytes) : std::nullopt;
    const std::optional<gff4::Struct> root = file ? file->Root() : std::nullopt;
    if (!root)
    {
        return false;
    }
    const std::vector<float> colour = root->Floats(kSunlightColor);
    const std::vector<float> onFigures = root->Floats(kSunlightColorChar);
    const std::vector<float> direction = root->Floats(kSunlightDirection);
    if (colour.size() < 3 || onFigures.size() < 3 || direction.size() < 3)
    {
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        light.sun.color.at(i) = colour[i];
        light.sun.characterColor.at(i) = onFigures[i];
        light.sun.direction.at(i) = direction[i];
    }
    light.sun.color[3] = light.sun.characterColor[3] = light.sun.direction[3] = 1;
    using S = dao::core::Sky;
    if (const std::optional<gff4::Struct> atmo = root->Child(kAtmoData))
    {
        Put(light.sky, *atmo, kAtmoSunColor, {&S::sunRed, &S::sunGreen, &S::sunBlue});
        Put(light.sky, *atmo, kAtmoSunIntensity, {&S::sunIntensity});
        Put(light.sky, *atmo, kAtmoTurbidity, {&S::turbidity});
        Put(light.sky, *atmo, kAtmoEarthReflectance, {&S::earthReflectance});
        Put(light.sky, *atmo, kAtmoMieMultiplier, {&S::mieMultiplier});
        Put(light.sky, *atmo, kAtmoRayleighMultiplier, {&S::rayleighMultiplier});
        Put(light.sky, *atmo, kAtmoDistanceMultiplier, {&S::distanceMultiplier});
        Put(light.sky, *atmo, kAtmoAlpha, {&S::atmosphereAlpha});
        Put(light.sky, *atmo, kAtmoFogColor, {&S::fogRed, &S::fogGreen, &S::fogBlue});
        Put(light.sky, *atmo, kAtmoFogIntensity, {&S::fogIntensity});
        Put(light.sky, *atmo, kAtmoFogCap, {&S::fogCap});
        Put(light.sky, *atmo, kAtmoFogZenith, {&S::fogZenith});
        Put(light.sky, *atmo, kMoonScale, {&S::moonScale});
        Put(light.sky, *atmo, kMoonAlpha, {&S::moonAlpha});
    }
    if (const std::optional<gff4::Struct> clouds = root->Child(kCloudData))
    {
        Put(light.sky, *clouds, kCloudDensity, {&S::cloudDensity});
        Put(light.sky, *clouds, kCloudSharpness, {&S::cloudSharpness});
        Put(light.sky, *clouds, kCloudDepth, {&S::cloudDepth});
        Put(light.sky, *clouds, kCloudRangeMultiplier1, {&S::cloudRangeMultiplier1});
        Put(light.sky, *clouds, kCloudRangeMultiplier2, {&S::cloudRangeMultiplier2});
        Put(light.sky, *clouds, kCloudColor, {&S::cloudRed, &S::cloudGreen, &S::cloudBlue});
    }
    return true;
}

// The ambient light (a node `nalt`) under a layout's node.
void Under(const gff4::Struct &node, std::size_t deep, std::optional<std::array<float, 3>> &ambient)
{
    if (deep > kNodesDeep || node.Shape().Find(kChildren) == nullptr)
    {
        return;
    }
    for (const gff4::Struct &child : node.Structs(kChildren))
    {
        if (child.Label() == "nalt" && !ambient)
        {
            if (const std::vector<float> colour = child.Floats(kAmbientColour); colour.size() >= 3)
            {
                ambient = std::array<float, 3>{colour[0], colour[1], colour[2]};
            }
        }
        Under(child, deep + 1, ambient);
    }
}

// A level's layouts, from its archive of rooms: the sky's light it was baked
// with, its layout's ambient light (<level>_ll.mmh, beside its light probe;
// Redcliffe's day 0.16, 0.20, 0.16 and night 0.03, 0.098, 0.21, the night's
// 0.19, 0.49 and 1.31 times the day's, where the mean of the two levels'
// lightmaps' used texels is 0.25, 0.51 and 1.35 times: measured 2026-10-21).
bool ReadLayouts(const std::filesystem::path &rim, const std::wstring &level, LevelLight &light)
{
    const std::optional<dao::core::erf::Archive> archive = dao::core::erf::Archive::Open(rim);
    if (!archive || !archive->Readable())
    {
        return false;
    }
    const std::wstring layout = level + L"_ll.mmh";
    std::optional<std::array<float, 3>> skyLight;
    for (const dao::core::erf::Entry &entry : archive->Entries())
    {
        if (Lower(entry.name) != layout)
        {
            continue;
        }
        const std::optional<std::vector<std::byte>> bytes = archive->Read(entry);
        const std::optional<gff4::File> file = bytes ? gff4::File::Parse(*bytes) : std::nullopt;
        const std::optional<gff4::Struct> root = file ? file->Root() : std::nullopt;
        if (root)
        {
            Under(*root, 0, skyLight);
        }
        break;
    }
    if (!skyLight)
    {
        return false;
    }
    light.skyLight = *skyLight;
    return true;
}

// A level's light: its .arl's and its archive of rooms' (`rim`). The game
// thread's.
std::optional<LevelLight> ReadLevel(const std::filesystem::path &rim, const std::wstring &level)
{
    LevelLight light;
    if (!ReadArl(level, light) || !ReadLayouts(rim, level, light))
    {
        return std::nullopt;
    }
    return light;
}

Vec3 Cross(const Vec3 &a, const Vec3 &b)
{
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}

// The way to a body of the sky at hour angle `turn` (radians from its
// highest), on a path whose highest is at `transit`'s height and way round,
// `latitude` (radians) and the body's declination `tilt`. Up is z. The
// path's highest is cos(latitude - tilt) up: the noon of an equinox, tilt 0,
// is at 90 degrees less the latitude.
Vec3 OnPath(const Vec3 &transit, float latitude, float tilt, float turn)
{
    const float flat = std::hypot(transit[0], transit[1]);
    const Vec3 toward = flat > 1e-6F ? Vec3{transit[0] / flat, transit[1] / flat, 0} : Vec3{1, 0, 0};
    const Vec3 up{0, 0, 1};
    // Going on from the highest: the side the sky turns to, which the level
    // does not say; across the transit's way from up.
    const Vec3 across = Cross(up, toward);
    Vec3 way{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const float equator = (std::sin(latitude) * toward.at(i)) + (std::cos(latitude) * up.at(i));
        const float pole = (-std::cos(latitude) * toward.at(i)) + (std::sin(latitude) * up.at(i));
        way.at(i) =
            (std::cos(tilt) * ((std::cos(turn) * equator) - (std::sin(turn) * across.at(i)))) + (std::sin(tilt) * pole);
    }
    return way;
}

// How much air the light of a body `height` (radians) up comes through, in
// air masses straight up (Kasten and Young, 1989); finite at the horizon.
float AirMass(float height)
{
    const float degrees = std::max(height / kDegree, 0.0F);
    return 1 / (std::sin(std::max(height, 0.0F)) + (0.50572F * std::pow(degrees + 6.07995F, -1.6364F)));
}

// The air's depth for the light of each channel straight up: its molecules'
// scattering over the air's 8 km of scale height, its haze's over 1.2 km.
std::array<float, 3> Depth(const dao::core::Sky &sky)
{
    constexpr double kAirHeight = 8000;
    constexpr double kHazeHeight = 1200;
    const Scattered scattered = Scattering(sky);
    std::array<float, 3> depth{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        depth.at(i) = static_cast<float>((scattered.air.at(i) * kAirHeight) + (scattered.haze.at(i) * kHazeHeight));
    }
    return depth;
}

// The haze the air holds by the hour, as its depth straight up at 550 nm: the
// day's dust, smoke and damp, settled by a still night and raised through the
// day, least after dawn and most by dusk; a low sun through it is pale at
// dawn and orange to red at dusk (the user, 2026-10-24: "Sunsets are often a
// reddish orange ... Sunrises I think are a bit different. They tend to be
// paler"). A clear day's aerosol is some 0.05 to 0.3 deep; the levels' own
// haze (their Mie multiplier, Depth's) is 0.004 to 0.013 deep at 570 nm, from
// Redcliffe's to the farm's, too clear to redden a sun. It is in the sun's way
// alone, so the ground, the disc and the air are lit by the same reddened sun
// (the review of the haze, 2026-10-25).
constexpr float kDawnHaze = 0.05F;
// 0.25 at dusk until 2026-10-24, when 0.25 and 0.5 were seen at the farm and
// in Redcliffe at 20:00 and 20:30 (the user: "0.5 is too red. Let's try 0.2").
// 0.2 until 2026-10-25: it left a sun 5 degrees up a quarter of its own
// light (0.24, 0.18, 0.10), and the farm's air, whose blue is nearly all its
// sky light's, went navy at 20:00 (its far mountains 43, 51, 118 of 255). At
// 0.1 that sun keeps 0.46, 0.38, 0.26 of its light, its disc (1, 0.83, 0.58),
// and the mountains were 81, 83, 132 (with the plugin's haze of the time,
// gone on 2026-10-27).
constexpr float kDuskHaze = 0.1F;
// outdoors.tune's DawnHaze and DuskHaze, a developer's, to try others live
// (TryDayHaze). The game thread's.
std::optional<float> g_tryDawnHaze;
std::optional<float> g_tryDuskHaze;

// The day's haze's depth in each channel (650, 570 and 475 nm, as
// Scattering's), `turn` the sun's hour angle from noon: dawn's six hours
// before noon, dusk's six after, by Angstrom's exponent 1.3 of a continental
// haze.
std::array<float, 3> DayHaze(float turn)
{
    constexpr std::array<float, 3> kOverGreen{0.805F, 0.954F, 1.210F}; // (550 / lambda)^1.3
    const float share = std::clamp(0.5F + (turn / std::numbers::pi_v<float>), 0.0F, 1.0F);
    const float deep =
        std::max(std::lerp(g_tryDawnHaze.value_or(kDawnHaze), g_tryDuskHaze.value_or(kDuskHaze), share), 0.0F);
    return {deep * kOverGreen[0], deep * kOverGreen[1], deep * kOverGreen[2]};
}

// The air's depth in each channel with the hour's haze (DayHaze) added.
std::array<float, 3> WithDayHaze(const std::array<float, 3> &clear, float turn)
{
    const std::array<float, 3> haze = DayHaze(turn);
    return {clear[0] + haze[0], clear[1] + haze[1], clear[2] + haze[2]};
}

float SmoothStep(float from, float to, float x)
{
    const float t = std::clamp((x - from) / (to - from), 0.0F, 1.0F);
    return t * t * (3 - (2 * t));
}

float Height(const Vec3 &way)
{
    return std::asin(std::clamp(way[2], -1.0F, 1.0F));
}

dao::core::Sky SkyOf(const dao::core::Sky &own, const LevelLight &level)
{
    dao::core::Sky sky = own;
    for (const auto &[member, value] : level.sky)
    {
        sky.*member = value;
    }
    return sky;
}

// The sun's height at which the day's sky and sky light have gone to the
// night's, and at which they begin to: the end of nautical twilight, and 6
// degrees up.
constexpr float kNightFrom = -12 * kDegree;
constexpr float kDayFrom = 6 * kDegree;
// The sun's height under the horizon at which the moon is the engine's sun at
// its whole: the end of civil twilight.
constexpr float kMoonFrom = -6 * kDegree;
// An area the clock runs in, as its file in data/areas says it (its comments say
// why each is as it is). Angles in degrees.
struct Cycled
{
    std::wstring area;
    std::wstring own;   // the area's own level, its file's `level`
    std::wstring other; // the level it borrows the other of its day and night from
    bool ownIsNight = false;
    std::optional<float> moonHeight; // none for the night level's own
    float moonTurn = 0;
    std::optional<float> latitude; // none: the latitude of `latitudeOf`'s sun
    std::wstring latitudeOf;       // empty for the day level
    float declination = 0;
    float noon = 0;
    float nightFill = 0;
    float nightHaze = 0;
    float effectsMoonlit = 0;
    float waterMoonlit = 1;
    std::vector<std::pair<float dao::core::Sky::*, float>> dayClouds; // its file's `day_clouds`, data/clouds
    std::vector<LampMove> lampMoves;
    std::optional<float> heldHour; // the hour its light is held at (its file's `hour`), none for the clock's
    bool heldOwn = false;          // held at the level's own hour (its file's `hour = own`)
    std::vector<std::pair<std::wstring, std::int32_t>> heldUntil; // plots' flags any of which lets the clock run
    std::wstring outside;               // an interior's: the area whose clock and sky its windows see
    std::optional<IndoorLight> indoors; // an interior's own light
};
// The sun's and the moon's radius as seen, radians: a body is gone when its
// disc is.
constexpr float kDiscRadius = 0.267F * kDegree;
// The sun's height at which the engine's sun goes from the sun to the moon:
// the sun's disc a diameter under the horizon, where neither gives light. The
// sky has one disc, drawn where the engine's sun comes from (lib-agent-3,
// read in the sky's recompute 0x0055BFA0, 2026-10-21), so the disc goes over
// with the light: the sun's fades as it sets, the moon's comes from nothing.
constexpr float kOver = -2 * kDiscRadius;
// How much of the night level's colour its moon and its sky's light keep: the
// night level's moon is a deep blue (0.27, 0.41, 0.79), and the blue of a
// night is the eye's, moonlight the sun's reflected (the user, 2026-10-21:
// "light should be more of a pale blue white, not nearly so garish"); 0.35
// until 2026-10-24, when midnight at 0.35, 0.5 and 0.65 was seen in Redcliffe
// village and the West Brecilian Forest (15, 16, 28 and 13, 27, 36 of 255 at
// 0.5, about as bright, bluer) and the user chose 0.5: "0.5 is fine".
constexpr float kNightColour = 0.5F;
// outdoors.tune's NightColour, a developer's, to try another share live
// (TryNightColour). The game thread's.
std::optional<float> g_tryColour;

// How much of the moon's beam the night's air takes beside the night sky's
// even light (LightAir): none left the Dalish camp's fog unseen against the
// dark trees, the whole moon made it a bright cyan sheet (2026-10-27).
// outdoors.tune's FogMoon tries others live, a developer's (TryFogMoon).
constexpr float kFogMoon = 0.25F;
std::optional<float> g_tryFogMoon;
float FogMoon()
{
    return std::clamp(g_tryFogMoon.value_or(kFogMoon), 0.0F, 1.0F);
}

// outdoors.tune's WaterMoonlit, in place of the area's file's
// `water_moonlit`, a developer's (TryWaterMoonlit). The game thread's.
std::optional<float> g_tryWaterMoonlit;
float WaterMoonlit(const Pair &pair)
{
    return std::clamp(g_tryWaterMoonlit.value_or(pair.waterMoonlit), 0.0F, 1.0F);
}
float NightColour()
{
    return std::clamp(g_tryColour.value_or(kNightColour), 0.0F, 1.0F);
}

// A colour's luminance.
float Luminance(const std::array<float, 3> &colour)
{
    return (0.2126F * colour[0]) + (0.7152F * colour[1]) + (0.0722F * colour[2]);
}

// A colour with `kept` of its saturation: 0 its grey of the same luminance.
std::array<float, 3> Paler(const std::array<float, 3> &colour, float kept)
{
    const float grey = (0.2126F * colour[0]) + (0.7152F * colour[1]) + (0.0722F * colour[2]);
    return {std::lerp(grey, colour[0], kept), std::lerp(grey, colour[1], kept), std::lerp(grey, colour[2], kept)};
}

// The sun's path through the day level's own sun: that sun is the season's
// (`declination`) of an afternoon, at the hour it stands as high, and the
// path is turned about the upright so that at that hour it is where the
// level's is. Noon is higher, and the level's own light, its bake's, is the
// afternoon's (the user, 2026-10-22: "perhaps the default isn't noon, but
// something more like 10:AM or 3:PM"). The latitude is the one at which the
// level's sun would be an equinox's noon. Returns the path's transit and the
// level's sun's hour angle (radians after noon).
std::pair<Vec3, float> SunPathThrough(const Vec3 &level, float latitude, float declination)
{
    const float height = Height(level);
    const float cosine = (std::sin(height) - (std::sin(latitude) * std::sin(declination))) /
                         (std::cos(latitude) * std::cos(declination));
    const float after = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const Vec3 then = OnPath(level, latitude, declination, after);
    const float turned = std::atan2(level[1], level[0]) - std::atan2(then[1], then[0]);
    const float c = std::cos(turned);
    const float s = std::sin(turned);
    return {Vec3{(c * level[0]) - (s * level[1]), (s * level[0]) + (c * level[1]), level[2]}, after};
}

// A color's red, green and blue.
std::array<float, 3> Rgb(const std::array<float, 4> &colour)
{
    return {colour[0], colour[1], colour[2]};
}

std::array<float, 3> FogOf(const dao::core::Sky &sky)
{
    return {sky.fogRed, sky.fogGreen, sky.fogBlue};
}

// How much of the light on the air a level's fog color is, in each color: the
// fog color over the level's sun and its sky's light, which a surface facing
// the sun has from them. The fog color is the air as the level's makers lit
// it, chosen by eye and the same every way; this reads it as what that light
// makes of the air, so that any other light, the hour's, makes of it what it
// would (review of the haze, 2026-10-25). A channel with no light has no fog.
std::array<float, 3> FogOver(const std::array<float, 3> &fog, const std::array<float, 3> &sun,
                             const std::array<float, 3> &skyLight)
{
    std::array<float, 3> by{};
    for (std::size_t i = 0; i < by.size(); ++i)
    {
        const float light = sun.at(i) + skyLight.at(i);
        by.at(i) = light > 0 ? fog.at(i) / light : 0;
    }
    return by;
}

// The area's own level's light: the bake's, and the sun the area has.
const LevelLight &Own(const Pair &pair)
{
    return pair.ownIsNight ? pair.night : pair.day;
}

// A colour times `by`.
std::array<float, 3> Scaled(std::array<float, 3> colour, float by)
{
    for (float &channel : colour)
    {
        channel *= by;
    }
    return colour;
}

// The night's haze: `haze` of the way from the night level's air to the
// area's own, its scattering and its clouds; the night level's moon, its
// light and its fog either way. Its scattering is the sky dome's and the
// moon's way through the air (Depth): the fog's color is the hour's light
// (LightAir), whatever the dome's numbers. Redcliffe's night is a clear
// sky's deep blue, and Haven's hazy gray day went to it (the user,
// 2026-10-23: "the daytime is pretty cloudy & dim, the nighttime shouldn't
// be so blue"). The day's fog's reach is not taken: it fogged Haven's
// midnight sky whole, in the night's fog's colour, at 71, 92, 105 of 255.
void HazeNight(dao::core::Sky &nightSky, const dao::core::Sky &daySky, const Pair &pair, float night, float haze)
{
    if (haze <= 0)
    {
        return;
    }
    // As bright as the night level's: the scattered light goes as the
    // sky's sun's strength times how much the air scatters, and Haven's
    // hazy air drew its midnight sky at 71, 92, 105 of 255 against
    // Redcliffe's air's 20, 28, 74.
    const auto scattered = [](const dao::core::Sky &sky) {
        const Scattered by = Scattering(sky);
        return (0.2126 * (by.air[0] + by.haze[0])) + (0.7152 * (by.air[1] + by.haze[1])) +
               (0.0722 * (by.air[2] + by.haze[2]));
    };
    const double nightScatters = scattered(nightSky);
    for (float dao::core::Sky::*member :
         {&dao::core::Sky::turbidity, &dao::core::Sky::mieMultiplier, &dao::core::Sky::rayleighMultiplier,
          &dao::core::Sky::hg, &dao::core::Sky::cloudDensity, &dao::core::Sky::cloudSharpness,
          &dao::core::Sky::cloudDepth})
    {
        nightSky.*member = std::lerp(nightSky.*member, daySky.*member, haze);
    }
    // The day's clouds lit by the moon as the land is, the day's colour
    // as much dimmer as the moon is than the day's sun: in the day's own
    // colour Haven's midnight clouds were a lit gray (63, 82, 94 of 255),
    // and in the night level's, near black, the sky was (7, 7, 10).
    const auto grey = [](const std::array<float, 4> &c) {
        return (0.2126F * c[0]) + (0.7152F * c[1]) + (0.0722F * c[2]);
    };
    const std::array<float, 3> moonPaled = Scaled(
        Paler({pair.night.sun.color[0], pair.night.sun.color[1], pair.night.sun.color[2]}, NightColour()), night);
    const float daySun = grey(pair.day.sun.color);
    const float cloudBy = daySun > 0 ? grey({moonPaled[0], moonPaled[1], moonPaled[2], 1}) / daySun : 0;
    nightSky.cloudRed = std::lerp(nightSky.cloudRed, daySky.cloudRed * cloudBy, haze);
    nightSky.cloudGreen = std::lerp(nightSky.cloudGreen, daySky.cloudGreen * cloudBy, haze);
    nightSky.cloudBlue = std::lerp(nightSky.cloudBlue, daySky.cloudBlue * cloudBy, haze);
    if (const double hazed = scattered(nightSky); hazed > 0)
    {
        nightSky.sunIntensity *= static_cast<float>(nightScatters / hazed);
    }
}

// The way to the moon at midnight: the night level's own turned `turn`
// degrees about the upright, `height` degrees up where given.
Vec3 MidnightOf(const Vec3 &own, float turn, std::optional<float> height)
{
    const float round = std::atan2(own[1], own[0]) + (turn * kDegree);
    const float up = height ? *height * kDegree : Height(own);
    return {std::cos(up) * std::cos(round), std::cos(up) * std::sin(round), std::sin(up)};
}

// outdoors.tune's MoonTurn and MoonHeight, a developer's, to try a moon live
// in place of the area's file's (TryMoon). The game thread's.
std::optional<float> g_tryTurn;
std::optional<float> g_tryHeight;

// The hour's light on water and on what is lit about (Water, Lit), against
// the own level's, as they were made for.
void WaterAndSpray(Light &light, const Pair &pair, const dao::core::Sky &daySky, const dao::core::Sky &nightSky,
                   float day, bool sunLights)
{
    // Water: the water shaders light it by the sky's sun, scattered (the
    // sky's sun colour and intensity), where everything else has the sun's
    // light. The night level's sky's sun is as strong as the day's and warm
    // (0.60, 0.43, 0.40 at 14.5 against 0.74, 0.32, 0.14 at 17.4, logged
    // 2026-10-22), and Redcliffe's falls stayed a lit cream at 01:00 (the
    // user: "should be a dimmer white"). So its strength goes as the hour's
    // light against the day level's, over the hour's sky's sun against the
    // day sky's; its colour is paled as the night's light is.
    const auto grey = [](float r, float g, float b) { return (0.2126F * r) + (0.7152F * g) + (0.0722F * b); };
    const float skySun = grey(light.sky.sunRed, light.sky.sunGreen, light.sky.sunBlue) * light.sky.sunIntensity;
    // The own level's sky and sun, as its water and spray were made for.
    const dao::core::Sky &ownSky = pair.ownIsNight ? nightSky : daySky;
    const float daySkySun = grey(ownSky.sunRed, ownSky.sunGreen, ownSky.sunBlue) * ownSky.sunIntensity;
    const float daySun = grey(Own(pair).sun.color[0], Own(pair).sun.color[1], Own(pair).sun.color[2]);
    const float sun = grey(light.sun.color[0], light.sun.color[1], light.sun.color[2]);
    // What is lit about, a spray or a mist, by the light on it from above:
    // dimmed by the sky's light alone it was darker than the moonlit rock
    // beside it (2026-10-22), where white water by the moon is a dimmer
    // white.
    std::array<float, 3> sky{};
    for (std::size_t i = 0; i < sky.size(); ++i)
    {
        sky.at(i) = std::lerp(light.nightSky.at(i), pair.day.skyLight.at(i), day);
    }
    const std::array<float, 3> &ownSkyLight = Own(pair).skyLight;
    // By night, as much of the moon as the area's spray stands in (its file's
    // `effects_moonlit`): the plugin shadows the moon on surfaces, not on a
    // particle, and under the West Brecilian Forest's canopy its falls took
    // the whole moon, 0.56 of their noon brightness at midnight beside ground
    // all but black (2026-10-24).
    // In each colour, over the own level's: a mist made for a night (the
    // Circle's docks, blue-white) is white under a borrowed day, and one made
    // for a day takes the evening's warmth and the moon's pale blue as the
    // ground beside it does (2026-10-24; its colour once turned the falls ink
    // blue, 2026-10-22, before the night's colour was paled).
    // By night over the own level's light's grey rather than each colour,
    // and the night's light's brightness without its colour: white water by
    // the moon is a dimmer white (the user, 2026-10-22: "we want it to be a
    // less bright white, not blue"). A spray drawn white under the Brecilian
    // forests' gold day light (0.61, 0.40, 0.28) took the night's light over
    // that in each colour and went deep blue, 20, 57, 127 of 255 at the West
    // forest's falls at midnight; 34, 54, 70 now, beside rock at 36, 63, 66
    // (#63). Water keeps its light's colour: without it (its sky's sun's,
    // warm) the stream went bluer, 89, 101, 119 to 80, 103, 128. A night the
    // own level (the Circle's docks) keeps its own.
    const float sprayBy = sunLights ? 1 : pair.effectsMoonlit;
    const std::array<float, 3> ownLight{Own(pair).sun.color[0] + ownSkyLight[0],
                                        Own(pair).sun.color[1] + ownSkyLight[1],
                                        Own(pair).sun.color[2] + ownSkyLight[2]};
    const float ownGrey = grey(ownLight[0], ownLight[1], ownLight[2]);
    const float toGrey = pair.ownIsNight ? 0 : light.nightShare;
    for (std::size_t i = 0; i < light.lit.size(); ++i)
    {
        const float own = std::lerp(ownLight.at(i), ownGrey, toGrey);
        light.lit.at(i) = own > 0 ? ((light.sun.color.at(i) * sprayBy) + sky.at(i)) / own : 1;
    }
    light.lit = Paler(light.lit, 1 - toGrey);
    // By night, the share of the moon's and the sky's light that reaches the
    // area's water (its file's `water_moonlit`): as with its spray, the moon
    // the plugin shadows on the ground lit the water under the canopy whole,
    // and the West Brecilian Forest's stream ran white past moss all but
    // black at midnight (#63).
    const float waterBy = sunLights ? 1 : WaterMoonlit(pair);
    const float reaches =
        grey(light.sun.color[0], light.sun.color[1], light.sun.color[2]) + grey(sky[0], sky[1], sky[2]);
    const float reached = (sun * waterBy) + grey(sky[0], sky[1], sky[2]);
    light.water = {(skySun > 0 && daySun > 0 ? (sun / daySun) * (daySkySun / skySun) : 1) *
                       (reaches > 0 ? reached / reaches : 1),
                   std::lerp(1.0F, NightColour(), light.nightShare)};
}

// The fog's color by the hour: the hour's sun or moon and sky light on the
// air, by the day level's FogOver and the night level's mixed as the sky is.
// At the day level's own light it is its own fog color exactly, and at the
// night level's its night fog as the plugin pales it. The engine fogs every
// surface and the sky toward it, so the far landscape follows the hour, warm
// at dusk and cool at night.
void LightAir(Light &light, const Pair &pair, const dao::core::Sky &daySky, const dao::core::Sky &nightSky, float day,
              bool sunLights)
{
    const std::array<float, 3> dayBy = FogOver(FogOf(daySky), Rgb(pair.day.sun.color), pair.day.skyLight);
    // The night level's light paled as the night's is (its fog was paled by
    // LightAt), so that its own moon and sky light make its paled fog.
    const std::array<float, 3> nightBy = FogOver(FogOf(nightSky), Paler(Rgb(pair.night.sun.color), NightColour()),
                                                 Paler(pair.night.skyLight, NightColour()));
    std::array<float, 3> by{};
    std::array<float, 3> skyLight{};
    for (std::size_t i = 0; i < by.size(); ++i)
    {
        // How much of its light the air gives back is the place's own, as
        // its thickness is (LightAt), at every hour: from the night level
        // the Dalish camp's thick gold fog took Redcliffe's clear night
        // air's share and was as dark as the trees behind it by midnight
        // (the user, 2026-10-27: "fog still seems to disappear at night").
        by.at(i) = pair.ownIsNight ? nightBy.at(i) : dayBy.at(i);
        skyLight.at(i) = std::lerp(light.nightSky.at(i), pair.day.skyLight.at(i), day);
    }
    // By night the air takes the night sky's even light alone, what lights
    // the shade, and not the moon's beam: lit by the whole moon, the game's
    // night lit for play at half the day's light, the Dalish camp's fog was a
    // bright cyan sheet over the dark trees at midnight (the user,
    // 2026-10-27: "fog is way too bright ... tone it down, don't want it
    // neon").
    const std::array<float, 3> sun = sunLights ? Rgb(light.sun.color) : Scaled(Rgb(light.sun.color), FogMoon());
    light.sky.fogRed = by[0] * (sun[0] + skyLight[0]);
    light.sky.fogGreen = by[1] * (sun[1] + skyLight[1]);
    light.sky.fogBlue = by[2] * (sun[2] + skyLight[2]);
}

Light LightAt(const Pair &pair, const dao::core::Sky &own, float hour, float night, float fill, float haze)
{
    dao::core::Sky daySky = SkyOf(own, pair.day);
    // By day the clouds of the level its file names (`day_clouds`), lit as
    // that level's makers lit them; the night keeps its own.
    for (const auto &[member, value] : pair.dayClouds)
    {
        daySky.*member = value;
    }
    dao::core::Sky nightSky = SkyOf(own, pair.night);
    // The night's fog is the moon's light scattered, paled as that light
    // is: its own deep blue was what a mist and a fall's spray fogged to,
    // and they went blue as they were dimmed (the user, 2026-10-22: "we
    // want it to be a less bright white, not blue").
    const std::array<float, 3> fog = Paler({nightSky.fogRed, nightSky.fogGreen, nightSky.fogBlue}, NightColour());
    nightSky.fogRed = fog[0];
    nightSky.fogGreen = fog[1];
    nightSky.fogBlue = fog[2];
    HazeNight(nightSky, daySky, pair, night, haze);
    const Vec3 levelSun{-pair.day.sun.direction[0], -pair.day.sun.direction[1], -pair.day.sun.direction[2]};
    const Vec3 &midnight = pair.midnight;
    const float noonHeight = Height(levelSun); // the level's sun's, as its colour was made for
    const float latitude = pair.latitude;
    const float turn = (hour - pair.noon) * 15 * kDegree;
    const Vec3 toSun =
        OnPath(SunPathThrough(levelSun, latitude, pair.declination).first, latitude, pair.declination, turn);
    const float moonTilt = Height(midnight) - ((std::numbers::pi_v<float> / 2) - latitude);
    const Vec3 toMoon = OnPath(midnight, latitude, moonTilt, turn + std::numbers::pi_v<float>);
    const float sunHeight = Height(toSun);
    const float moonHeight = Height(toMoon);

    const float day = SmoothStep(kNightFrom, kDayFrom, sunHeight);
    Light light;
    light.sky = dao::core::Blend(nightSky, daySky, day);
    // How thick the air is, its fog's intensity, cap and zenith, is the
    // place's own at every hour, its own level's; the hour gives the fog its
    // colour alone. Blended to the night level's, Haven's thick fog thinned
    // to Redcliffe's clear night air and went by midnight (the user,
    // 2026-10-27: "the fog seems to disappear at night?").
    {
        const dao::core::Sky &place = pair.ownIsNight ? nightSky : daySky;
        light.sky.fogIntensity = place.fogIntensity;
        light.sky.fogCap = place.fogCap;
        light.sky.fogZenith = place.fogZenith;
    }
    light.nightShare = 1 - day;
    // At night the moon alone lights what the bake lit by day, from where it
    // is: the bake, a level's probes and its ambient go to the night sky's
    // light, kept to `fill` of the night level's (the user, 2026-10-21: "fade
    // out the baked lighting and just strengthen the moonlight so it lights
    // things properly from its position").
    light.nightSky = Scaled(Paler(pair.night.skyLight, NightColour()), fill);

    // The light through the air: the day's or the night's at its highest, less
    // what the longer way takes out, by the air of the sky of the hour and the
    // day's haze by the hour.
    const std::array<float, 3> depth = WithDayHaze(Depth(light.sky), turn);
    const bool sunLights = sunHeight > kOver;
    const float up = sunLights
                         ? SmoothStep(-kDiscRadius, kDiscRadius, sunHeight)
                         : SmoothStep(-kDiscRadius, kDiscRadius, moonHeight) * SmoothStep(kOver, kMoonFrom, sunHeight);
    const float atHighest = AirMass(sunLights ? noonHeight : Height(midnight));
    const float mass = AirMass(sunLights ? sunHeight : moonHeight);
    const Vec3 &toward = sunLights ? toSun : toMoon;
    // The night's light, as bright as `night` of the night level's: that
    // level's light is about half the day's (its lightmaps', 2026-10-21), a
    // night lit for play, and the user found it "too bright at midnight".
    const auto ofNight = [night](const std::array<float, 3> &colour) {
        return Scaled(Paler(colour, NightColour()), night);
    };
    const std::array<float, 3> moon =
        ofNight({pair.night.sun.color[0], pair.night.sun.color[1], pair.night.sun.color[2]});
    const std::array<float, 3> moonOnFigures =
        ofNight({pair.night.sun.characterColor[0], pair.night.sun.characterColor[1], pair.night.sun.characterColor[2]});
    const std::array<float, 3> nightSkyLight = light.nightSky;
    const std::array<float, 3> &ownSkyLight = Own(pair).skyLight;
    for (std::size_t i = 0; i < 3; ++i)
    {
        // No more than the level's own: a summer's sun higher than the day
        // level's noon is as bright as that noon (the area's declination).
        const float air = std::exp(-depth.at(i) * std::max(mass - atHighest, 0.0F));
        const float through = air * up;
        light.disc.at(i) = sunLights ? air : 1;
        light.sun.color.at(i) = (sunLights ? pair.day.sun.color.at(i) : moon.at(i)) * through;
        light.sun.characterColor.at(i) =
            (sunLights ? pair.day.sun.characterColor.at(i) : moonOnFigures.at(i)) * through;
        light.sun.direction.at(i) = -toward.at(i);
        // The own level's layouts' lights, its day's sun's bounce and fills,
        // or its night's moon's where its own level is the night.
        light.sunOverDay.at(i) = sunLights != pair.ownIsNight ? through : 0;
        const float skyLight = std::lerp(nightSkyLight.at(i), pair.day.skyLight.at(i), day);
        light.baked.at(i) = ownSkyLight.at(i) > 0 ? skyLight / ownSkyLight.at(i) : 1;
        light.outdoor.at(i) = light.sun.color.at(i) + skyLight;
        light.even.at(i) = pair.ownIsNight ? skyLight : nightSkyLight.at(i);
    }
    // The bake gives way to the sky's even light by night (night_sky.hlsl),
    // and always where the bake is a night's: by day a night's bake kept the
    // makers' moonlight where it fell, under the borrowed sun.
    light.evenShare = pair.ownIsNight ? 1 : light.nightShare;
    light.sun.color[3] = light.sun.characterColor[3] = light.sun.direction[3] = 1;
    // The disc: the sun's the hue of its light through the air, as bright as
    // ever (a low sun is still the brightest thing in the sky); its strength
    // (the sky's moon alpha, the disc's whole brightness) the day's or the
    // night's own, going as the body crosses the horizon and, for the moon,
    // with twilight.
    light.moon = !sunLights;
    const float brightest = std::max({light.disc[0], light.disc[1], light.disc[2]});
    for (float &channel : light.disc)
    {
        channel = brightest > 0 ? channel / brightest : 1;
    }
    light.sky.moonAlpha = sunLights ? daySky.moonAlpha * SmoothStep(kOver, -kOver, sunHeight) : nightSky.moonAlpha * up;
    WaterAndSpray(light, pair, daySky, nightSky, day, sunLights);
    LightAir(light, pair, daySky, nightSky, day, sunLights);
    return light;
}

// outdoors.tune's Fill, Window and WindowReach (TryIndoors). The game thread's.
std::optional<float> g_tryFill;
std::optional<float> g_tryWindow;
std::optional<float> g_tryReach;
std::optional<float> g_tryRelit;

// An interior's light by the hour (its file's `outside`): its level is lit
// by its makers as a night (the Broken Circle's tower floors) or as a day (the
// mage origin's and Witch Hunt's tower, lak303d), and every light of theirs,
// its bake and ambient colour, its probes, its layouts' lamps that are not a
// fitting's, its figures' fills and key light and its fog, is whole by day in
// the daylight's colour and kept to `fill` of it by night in the night's,
// going between them as the light out of doors does; the candles and lamps
// light the night. The colour at the makers' own end is their bake's as made,
// at the other a night's bake's day level's fill colour (its file's `day`) or
// a day's bake's night the light out of doors at midnight. Where a window
// sees the bake (DaoWindowSeen), its daylight goes by the light out of doors
// of the hour (BakeWindow). The sun's or the moon's colour comes in by the
// windows alone (WindowLight), and the beams of light its makers placed (effects.ini's
// `lit`) are the windows' and go by the light out of doors against their
// makers' hour's. `fill` was first the share kept at every hour, the makers'
// own as bright as made and the other end by the light out of doors against
// theirs: Witch Hunt's library was half its vanilla brightness at noon and
// blue (the user, 2026-10-25: "Should be darker at night. Shouldn't be blue
// during the day"). Taken in the sun's colour, each channel by the light out
// of doors at noon over midnight's (2.28, 1.63, 0.74), the Broken Circle's
// violet bake (lak503d's lightmaps' mean 0.096, 0.105, 0.182) went a
// red-brown at noon, read as firelight, not daylight.
Light IndoorsAt(const Pair &pair, const IndoorLight &indoors, const dao::core::Sunlight &ownSun,
                const dao::core::Sky &ownSky, float hour, float night, float nightFill, float haze)
{
    const float fill = std::max(g_tryFill.value_or(indoors.fill), 0.0F);
    const Light out = LightAt(pair, ownSky, hour, night, nightFill, haze);
    // The light out of doors at midnight and at the day level's own hour; the
    // makers' own hour, which their beams of light were made for, is the one
    // they baked.
    const Light atNight = LightAt(pair, ownSky, 0, night, nightFill, haze);
    const Light atDay = LightAt(pair, ownSky, pair.ownHour, night, nightFill, haze);
    const Light &midnight = indoors.ownIsNight ? atNight : atDay;
    // How far the hour is from night to day, as the sky and the bake out of
    // doors go (LightAt's: from the sun 12 degrees under to 6 up). By the
    // light out of doors' brightness between midnight's and the day's it was
    // night by 19:30, the sun 10 degrees up: the game's night is lit for play,
    // 0.48 of white to the day's 0.78, and 19:30's 0.50 was 7 in 100 of the
    // way (Witch Hunt's library blue, 29.9, 21.4, 43.3 of 255; 2026-10-25).
    const float day = 1 - out.nightShare;
    // How bright the makers' light is by day: as the daylight on open ground
    // is against noon's, the sun's by the sine of its height and the day
    // sky's share. Out of doors the sun's light varies hour by hour by the
    // way it meets the ground, which the shaders take; indoors nothing of the
    // sun meets it, and by `day` alone, whole from the sun 6 degrees up, a
    // room was as bright at 19:30 as at noon and went to night within the
    // hour (the user, 2026-10-29: "Most of the day is equally bright, the
    // night is equally dark. Outdoors with the Sun it varies more from hour
    // to hour"). The colour still goes by `day`, as the sky's does.
    const auto daylight = [&](const Light &at) {
        const float sunUp = at.moon ? 0 : std::max(-at.sun.direction[2], 0.0F);
        return (Luminance({at.sun.color[0], at.sun.color[1], at.sun.color[2]}) * sunUp) +
               ((1 - at.nightShare) * Luminance(pair.day.skyLight));
    };
    const float atNoon = daylight(LightAt(pair, ownSky, pair.noon, night, nightFill, haze));
    const float bright = atNoon > 0 ? std::clamp(daylight(out) / atNoon, 0.0F, 1.0F) : day;
    // The other end's colour: a night's bake's day takes its day level's
    // fill (lak303d's gray), a day's bake's night the light out of doors at
    // midnight, the moon's and the night sky's, paled again as the night's
    // fog is (NightColour): taken as it is, the moon's 1 : 1.25 : 1.85, Witch
    // Hunt's library at midnight was 27.3, 16.8, 36.9 of 255, and 29.8, 16.7,
    // 29.8 paled (2026-10-25). In the
    // Broken Circle's own deep blue fill (lak503d's rooms', 0.015, 0.045,
    // 0.095), paled as the night's, Witch Hunt's library at midnight was
    // blue-violet (27.1, 19.1, 41.9 of 255, 2026-10-25).
    // A day's bake with its rooms' night bake named (`night`) takes that
    // bake's colour by night, and its brightness against its own: the makers'
    // night. Witch Hunt's tower, a day's bake, in the light out of doors at
    // midnight paled, was brighter and less blue than the Broken Circle's
    // night floors (the user, 2026-10-30: "The witch one version doesn't
    // really look blue at night compared to the vanilla game. It also looks a
    // lot brighter").
    const std::optional<std::array<float, 3>> otherColour =
        indoors.ownIsNight ? indoors.dayFill
                           : std::optional(indoors.nightBake.value_or(Paler(atNight.outdoor, NightColour())));
    const float nightBright = !indoors.ownIsNight && indoors.nightBake && indoors.bakeLuminance > 0
                                  ? Luminance(*indoors.nightBake) / indoors.bakeLuminance
                                  : 1.0F;
    Light light;
    light.sun = ownSun;
    light.sky = ownSky;
    light.outdoor = out.outdoor;
    const std::array<float *, 3> fog{&light.sky.fogRed, &light.sky.fogGreen, &light.sky.fogBlue};
    // How far the hour is from the makers' own end to the other.
    const float other = indoors.ownIsNight ? day : 1 - day;
    light.bakeHue[3] = other;
    light.relitNight = 1 - bright;
    for (std::size_t i = 0; i < 3; ++i)
    {
        const float by = midnight.outdoor.at(i) > 0 ? out.outdoor.at(i) / midnight.outdoor.at(i) : 1;
        const float tint = otherColour && Luminance(*otherColour) > 0 && indoors.bakeHue.at(i) > 0
                               ? (otherColour->at(i) / Luminance(*otherColour)) / indoors.bakeHue.at(i)
                               : 1;
        const float kept = std::lerp(fill, 1.0F, bright) * std::lerp(1.0F, tint * nightBright, other);
        light.sun.characterColor.at(i) *= kept;
        light.baked.at(i) = kept;
        light.sunOverDay.at(i) = kept;
        *fog.at(i) *= kept;
        // Where a window sees the bake, its daylight goes by the light out of
        // doors against the day's own, the sun's colour by dusk.
        light.bakeWindow.at(i) =
            std::lerp(1.0F, atDay.outdoor.at(i) > 0 ? out.outdoor.at(i) / atDay.outdoor.at(i) : 1.0F, day);
        light.lit.at(i) = by;
        light.bakeHue.at(i) = indoors.bakeHue.at(i);
        // The windows' own light is kept by night as the makers' is: one eye
        // for the room (the user, 2026-10-25, of Witch Hunt's dormitory: "The
        // dorm looks very blue"). The Broken Circle's dormitory at midnight
        // went from 48.6, 22.0, 34.2 of 255 to 37.4, 13.7, 23.1 (vanilla's 48.5,
        // 21.9, 35.8). Most of that dormitory's blue was Finn's Cleansing Aura,
        // which his tactics turn back on as the party walks (2026-10-25).
        light.window.at(i) =
            std::max(g_tryWindow.value_or(indoors.window), 0.0F) * out.outdoor.at(i) * std::lerp(fill, 1.0F, bright);
    }
    // Said as the hour or the tuning moves it by more than a little.
    static std::array<float, 3> said{};
    if (std::abs(light.lit[0] - said[0]) + std::abs(light.lit[1] - said[1]) + std::abs(light.lit[2] - said[2]) > 0.05F)
    {
        said = light.lit;
        spdlog::info("day and night: indoors at {:.2f}, the light out of doors {:.3f} {:.3f} {:.3f}, {:.2f} {:.2f} "
                     "{:.2f} times its makers' hour's, the daylight {:.2f} of noon's; their light kept at {:.3f} "
                     "{:.3f} {:.3f}",
                     hour, out.outdoor[0], out.outdoor[1], out.outdoor[2], light.lit[0], light.lit[1], light.lit[2],
                     bright, light.baked[0], light.baked[1], light.baked[2]);
    }
    return light;
}

// Whether the area's own sun is a level's: the engine keeps its file's
// numbers, so an area of a level has that level's sun exactly. A level is
// found so, not by its name being opened: the engine opens levels ahead of
// the party (DAOLib's fast load opens the indoor ones beside an area).
bool Same(const dao::core::Sunlight &a, const dao::core::Sunlight &b)
{
    constexpr float kWithin = 1e-4F; // a float's last digits, which the engine's copy may round
    for (std::size_t i = 0; i < 3; ++i)
    {
        if (std::abs(a.color.at(i) - b.color.at(i)) > kWithin ||
            std::abs(a.characterColor.at(i) - b.characterColor.at(i)) > kWithin ||
            std::abs(a.direction.at(i) - b.direction.at(i)) > kWithin)
        {
            return false;
        }
    }
    return true;
}

// The areas of data/areas (Install), every area of a level opened, by its
// tag, and the levels looked at, under g_lock.
std::mutex g_lock;
std::vector<Cycled> g_cycled;
// The starts of the names of layouts' lights that are fires (data/areas'
// defaults.ini's `fires`), set once in Install before any light is seen.
std::vector<std::wstring> g_fireLights;
float g_fireStrength = 1; // defaults.ini's `fire_strength`, set once in Install
std::map<std::wstring, std::shared_ptr<const Pair>> g_pairs;
std::set<std::wstring> g_asked;
// The levels opened whose pairs are yet to be read (Opened's): each its
// archive of rooms and the game's levels' folder.
std::vector<std::pair<std::filesystem::path, std::filesystem::path>> g_opened;
// What the hour's light hands the render thread: the sky light's multiplier,
// the disc's colour (under g_lock), and the day level's sunlight lamps with
// what they are multiplied by.
std::array<std::atomic<float>, 3> g_baked{1.0F, 1.0F, 1.0F};
std::array<std::atomic<float>, 2> g_water{1.0F, 1.0F};
std::array<std::atomic<float>, 3> g_lit{1.0F, 1.0F, 1.0F};
std::atomic<float> g_nightShare{0};
std::array<std::atomic<float>, 3> g_nightSky{0.0F, 0.0F, 0.0F};
std::atomic<float> g_evenShare{0};
std::array<std::atomic<float>, 3> g_even{0.0F, 0.0F, 0.0F};
std::array<std::atomic<float>, 3> g_daySky{0.0F, 0.0F, 0.0F};
std::optional<std::array<float, 3>> g_disc;
std::atomic<bool> g_discIsMoon{false};
std::vector<LampMove> g_lampMoves;  // the running area's, under g_lock
std::atomic<bool> g_running{false}; // the clock runs in a level with a night
std::atomic<bool> g_indoors{false}; // and in an interior
std::array<std::atomic<float>, 3> g_layoutLightsBy{1.0F, 1.0F, 1.0F};
// An interior's windows' light (WindowLight).
std::array<std::atomic<float>, 3> g_window{0.0F, 0.0F, 0.0F};
std::atomic<float> g_windowReach{0};
// An interior's bake's own hue and how much each texel takes it (BakeHue),
// and where a window sees it (BakeWindow).
std::array<std::atomic<float>, 4> g_bakeHue{1.0F, 1.0F, 1.0F, 0.0F};
std::array<std::atomic<float>, 3> g_bakeWindow{1.0F, 1.0F, 1.0F};
std::atomic<float> g_relitNight{0};
std::atomic<float> g_relitStrength{1};
std::atomic<float> g_lampsBaked{0};

// The clock: the hour, and when it was last moved on.
float g_hour = 12;
std::optional<std::chrono::steady_clock::time_point> g_ticked;
// The most a frame moves the clock on, seconds: a loading screen or a pause
// between two frames is not play.
constexpr float kMostFrame = 0.25F;

void Stopped()
{
    for (std::atomic<float> &channel : g_baked)
    {
        channel.store(1, std::memory_order_relaxed);
    }
    for (std::atomic<float> &value : g_water)
    {
        value.store(1, std::memory_order_relaxed);
    }
    for (std::atomic<float> &value : g_lit)
    {
        value.store(1, std::memory_order_relaxed);
    }
    g_nightShare.store(0, std::memory_order_relaxed);
    g_evenShare.store(0, std::memory_order_relaxed);
    g_running.store(false, std::memory_order_relaxed);
    g_indoors.store(false, std::memory_order_relaxed);
    for (std::atomic<float> &value : g_window)
    {
        value.store(0, std::memory_order_relaxed);
    }
    g_windowReach.store(0, std::memory_order_relaxed);
    g_bakeHue[3].store(0, std::memory_order_relaxed);
    g_relitNight.store(0, std::memory_order_relaxed);
    for (std::atomic<float> &value : g_bakeWindow)
    {
        value.store(1, std::memory_order_relaxed);
    }
    const std::scoped_lock lock(g_lock);
    g_disc.reset();
    g_discIsMoon.store(false, std::memory_order_relaxed);
    g_lampMoves.clear();
}

// A line's text without its comment and the spaces about it.
std::string Trimmed(std::string line)
{
    line.erase(std::min(line.find(';'), line.size()));
    const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!line.empty() && space(line.back()))
    {
        line.pop_back();
    }
    const auto first = std::ranges::find_if_not(line, space);
    return {first, line.end()};
}

std::optional<float> Number(const std::string &text)
{
    std::istringstream in(text);
    float value = 0;
    return in >> value && (in >> std::ws).eof() ? std::optional<float>(value) : std::nullopt;
}

// What the areas' files say (data/areas/README.md says what each key is):
// every file but defaults.ini an area, named by its tag, with defaults.ini's
// values where it says none, and defaults.ini's fires.
struct Table
{
    std::vector<Cycled> areas;
    std::vector<std::wstring> fires;
    float fireStrength = 1;
};

using Keys = std::map<std::string, std::string>;

// A file's `key = value` lines.
Keys ReadKeys(const std::filesystem::path &file)
{
    Keys keys;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        line = Trimmed(line);
        if (const std::size_t equals = line.find('='); equals != std::string::npos)
        {
            keys[Trimmed(line.substr(0, equals))] = Trimmed(line.substr(equals + 1));
        }
    }
    return keys;
}

// An area's `move_lamp<anything> = x y z > x y z`: a lamp the level puts at
// the first place lit at the second.
std::vector<LampMove> LampMovesOf(const Keys &keys)
{
    std::vector<LampMove> moves;
    for (const auto &[key, value] : keys)
    {
        std::istringstream in(value);
        LampMove move;
        char to = 0;
        if (key.starts_with("move_lamp") && in >> move.from[0] >> move.from[1] >> move.from[2] >> to && to == '>' &&
            in >> move.to[0] >> move.to[1] >> move.to[2])
        {
            moves.push_back(move);
        }
    }
    return moves;
}

// A key's value in an area's file, or defaults.ini's where it says none.
std::optional<std::string> Said(const Keys &keys, const Keys &defaults, const std::string &key)
{
    if (const auto value = keys.find(key); value != keys.end())
    {
        return value->second;
    }
    if (const auto value = defaults.find(key); value != defaults.end())
    {
        return value->second;
    }
    return std::nullopt;
}

std::wstring Wide(const std::string &text)
{
    return Lower(std::wstring(text.begin(), text.end()));
}

// An area's `hour` and `hour_until`, its own file's alone. `hour`: the hour
// the area's light is held at, the story's (Soldier's Peak's night before its
// quest is done, the user, 2026-10-25: "before you beat the quest and claim
// the keep for your own, it should be night. After that it can cycle
// normally"); `hour = own`: the level's own hour, its makers' light
// (Redcliffe's day before the night's siege). `hour_until`: `<plot> <flag>`,
// several by commas, any of which, the party's, lets the clock run.
void ReadHold(const Keys &keys, Cycled &area)
{
    const std::string hour = keys.contains("hour") ? keys.at("hour") : "";
    area.heldOwn = hour == "own";
    area.heldHour = area.heldOwn ? std::optional<float>(0.0F) : Number(hour);
    if (keys.contains("hour_until"))
    {
        std::istringstream all(keys.at("hour_until"));
        for (std::string one; std::getline(all, one, ',');)
        {
            std::istringstream until(one);
            std::string plot;
            std::int32_t flag = -1;
            if (until >> plot >> flag && flag >= 0)
            {
                area.heldUntil.emplace_back(Wide(plot), flag);
            }
        }
    }
}

// An interior's file (its `outside`): its level, the area whose clock and
// sky its windows see, how much of its makers' light it keeps and how its
// windows light it; none where it lacks one of them.
std::optional<Cycled> IndoorsOf(const std::string &tag, const Keys &keys, const Keys &defaults)
{
    Cycled area;
    area.area = Wide(tag);
    area.own = Wide(Said(keys, defaults, "level").value_or(""));
    area.outside = Wide(Said(keys, defaults, "outside").value_or(""));
    IndoorLight indoors;
    const std::optional<float> fill = Number(Said(keys, defaults, "fill").value_or(""));
    const std::optional<float> window = Number(Said(keys, defaults, "window").value_or(""));
    const std::optional<float> reach = Number(Said(keys, defaults, "window_reach").value_or(""));
    const std::string day = Said(keys, defaults, "day").value_or("");
    indoors.ownIsNight = keys.contains("baked") ? keys.at("baked") != "day" : true;
    indoors.other = Wide(day);
    indoors.night = Wide(Said(keys, defaults, "night").value_or(""));
    std::istringstream beams(Said(keys, defaults, "beams").value_or(""));
    for (std::string model; beams >> model;)
    {
        indoors.beams.push_back(Wide(model));
    }
    std::istringstream windows(Said(keys, defaults, "windows").value_or(""));
    for (std::string material; windows >> material;)
    {
        indoors.windows.push_back(Wide(material));
    }
    if (area.own.empty() || area.outside.empty() || !fill || !window || !reach || (!day.empty() && !indoors.ownIsNight))
    {
        spdlog::warn("day and night: areas/{}.ini, an interior, lacks its level, its outside, its fill or its windows' "
                     "light or reach, or names a day for a day's bake",
                     tag);
        return std::nullopt;
    }
    indoors.fill = std::max(*fill, 0.0F);
    indoors.window = std::max(*window, 0.0F);
    indoors.windowReach = std::max(*reach, 0.0F);
    area.indoors = std::move(indoors);
    ReadHold(keys, area);
    return area;
}

// One area's file, with defaults.ini's values where it says none; none
// where it lacks what the clock needs.
std::optional<Cycled> AreaOf(const std::string &tag, const Keys &keys, const Keys &defaults)
{
    if (keys.contains("outside"))
    {
        return IndoorsOf(tag, keys, defaults);
    }
    const auto said = [&](const std::string &key) { return Said(keys, defaults, key); };
    Cycled area;
    area.area = Wide(tag);
    area.own = Wide(said("level").value_or(""));
    // `night`: the night borrowed, the level the day; `day`: the day
    // borrowed, the level the night (the Circle's docks). Not both.
    const std::string borrowedNight = said("night").value_or("");
    const std::string borrowedDay = said("day").value_or("");
    area.ownIsNight = !borrowedDay.empty();
    area.other = Wide(area.ownIsNight ? borrowedDay : borrowedNight);
    const std::optional<float> declination = Number(said("declination").value_or(""));
    const std::optional<float> noon = Number(said("noon").value_or(""));
    const std::optional<float> nightFill = Number(said("night_fill").value_or(""));
    const std::optional<float> nightHaze = Number(said("night_haze").value_or(""));
    const std::optional<float> effectsMoonlit = Number(said("effects_moonlit").value_or(""));
    const std::optional<float> waterMoonlit = Number(said("water_moonlit").value_or(""));
    if (area.own.empty() || area.other.empty() || (!borrowedNight.empty() && !borrowedDay.empty()) || !declination ||
        !noon || !nightFill || !nightHaze || !effectsMoonlit || !waterMoonlit)
    {
        spdlog::warn("day and night: areas/{}.ini lacks its level, one of its night or its day, the declination, "
                     "noon, the night's fill, its haze or its effects' or water's moonlight",
                     tag);
        return std::nullopt;
    }
    area.nightFill = *nightFill;
    area.nightHaze = std::clamp(*nightHaze, 0.0F, 1.0F);
    area.effectsMoonlit = std::clamp(*effectsMoonlit, 0.0F, 1.0F);
    area.waterMoonlit = std::clamp(*waterMoonlit, 0.0F, 1.0F);
    area.lampMoves = LampMovesOf(keys);
    area.declination = *declination;
    area.noon = *noon;
    // `own`: the night level's own moon's height, where defaults.ini gives another.
    const std::string moonHeight = said("moon_height").value_or("own");
    area.moonHeight = moonHeight == "own" ? std::nullopt : Number(moonHeight);
    area.moonTurn = Number(said("moon_turn").value_or("")).value_or(0);
    ReadHold(keys, area);
    const std::string latitude = said("latitude").value_or("");
    area.latitude = Number(latitude);
    if (!area.latitude)
    {
        area.latitudeOf = Wide(latitude);
    }
    return area;
}

// A sky's clouds by a file of data/clouds (`name`.ini): `density`,
// `sharpness`, `depth`, `range1`, `range2` and `color`, the engine's cloud
// fields; none where it has no such file. An area's `day_clouds` names one,
// for a day of other weather than its borrowed day's: the unrepaired Vigil's
// Keep rains by day too, under the clear day it borrows (vgk101d, its sun at
// full strength over scattered clouds), and takes `overcast` (the user,
// 2026-10-27: "how about not changing the disk and just adding more clouds
// during the keep day (but not night)", "shouldn't we make clouds its own
// file?"). The keep's own storm clouds, its night level's, darkened the noon
// sky to night (29, 37, 45 of 255): their colour is a night's.
std::vector<std::pair<float dao::core::Sky::*, float>> CloudsOf(const std::filesystem::path &folder,
                                                                const std::string &name)
{
    std::vector<std::pair<float dao::core::Sky::*, float>> clouds;
    std::error_code error;
    const std::filesystem::path file = folder / (name + ".ini");
    if (name.empty() || !std::filesystem::exists(file, error))
    {
        return clouds;
    }
    const Keys keys = ReadKeys(file);
    const auto one = [&](const char *key, float dao::core::Sky::*member) {
        if (const auto value = keys.find(key); value != keys.end())
        {
            if (const std::optional<float> number = Number(value->second))
            {
                clouds.emplace_back(member, *number);
            }
        }
    };
    one("density", &dao::core::Sky::cloudDensity);
    one("sharpness", &dao::core::Sky::cloudSharpness);
    one("depth", &dao::core::Sky::cloudDepth);
    one("range1", &dao::core::Sky::cloudRangeMultiplier1);
    one("range2", &dao::core::Sky::cloudRangeMultiplier2);
    if (const auto colour = keys.find("color"); colour != keys.end())
    {
        std::istringstream rgb(colour->second);
        float r = 0;
        float g = 0;
        float b = 0;
        if (rgb >> r >> g >> b)
        {
            clouds.emplace_back(&dao::core::Sky::cloudRed, r);
            clouds.emplace_back(&dao::core::Sky::cloudGreen, g);
            clouds.emplace_back(&dao::core::Sky::cloudBlue, b);
        }
    }
    return clouds;
}

Table ReadTable(const std::filesystem::path &folder)
{
    std::vector<std::pair<std::string, Keys>> sections;
    std::error_code error;
    for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(folder, error))
    {
        if (entry.path().extension() == ".ini")
        {
            sections.emplace_back(entry.path().stem().string(), ReadKeys(entry.path()));
        }
    }
    Keys defaults;
    if (const auto found = std::ranges::find(sections, std::string("defaults"), &decltype(sections)::value_type::first);
        found != sections.end())
    {
        defaults = found->second;
    }
    Table table;
    if (const auto strength = defaults.find("fire_strength"); strength != defaults.end())
    {
        table.fireStrength = Number(strength->second).value_or(1);
    }
    if (const auto fires = defaults.find("fires"); fires != defaults.end())
    {
        std::istringstream names(fires->second);
        for (std::string name; names >> name;)
        {
            table.fires.push_back(Wide(name));
        }
    }
    for (const auto &[tag, keys] : sections)
    {
        if (tag == "defaults")
        {
            continue;
        }
        if (std::optional<Cycled> area = AreaOf(tag, keys, defaults))
        {
            if (const auto clouds = keys.find("day_clouds"); clouds != keys.end())
            {
                area->dayClouds = CloudsOf(folder.parent_path() / "clouds", clouds->second);
            }
            table.areas.push_back(std::move(*area));
        }
    }
    return table;
}

} // namespace

bool FireColoured(const std::array<float, 3> &colour)
{
    return colour[0] >= colour[1] && colour[1] >= colour[2];
}

float FireStrength()
{
    return g_fireStrength;
}

std::vector<LampMove> LampMoves()
{
    const std::scoped_lock lock(g_lock);
    return g_lampMoves;
}

bool FireLamp(std::wstring_view model, std::wstring_view name)
{
    const auto lower = [](wchar_t c) { return std::towlower(c); };
    const bool layout = model.size() >= 3 &&
                        std::ranges::equal(model.substr(model.size() - 3), std::wstring_view(L"_ll"), {}, lower, lower);
    const auto named = [&](std::wstring_view start) {
        return name.size() >= start.size() && std::ranges::equal(name.substr(0, start.size()), start, {}, lower, lower);
    };
    return layout && std::ranges::any_of(g_fireLights, named);
}

Scattered Scattering(const dao::core::Sky &sky)
{
    constexpr std::array<double, 3> kRayleigh{6.95e-6, 1.18e-5, 2.44e-5};                 // per metre
    constexpr std::array<double, 3> kInverseWavelength{1538461.5, 1754386.0, 2105263.25}; // per metre
    constexpr double kPi = std::numbers::pi;
    // Preetham's concentration factor, as the engine has it.
    constexpr double kConcentration = 0.434; // NOLINT(modernize-use-std-numbers): not log10(e)
    const double c = std::max((6.544 * sky.turbidity) - 6.51, 0.0) * 1e-17;
    Scattered scattered;
    for (std::size_t i = 0; i < 3; ++i)
    {
        scattered.air.at(i) = std::max(sky.rayleighMultiplier, 0.0F) * kRayleigh.at(i);
        scattered.haze.at(i) = std::max(sky.mieMultiplier, 0.0F) * 0.67 * kConcentration * 4 * kPi * kPi * kPi * c *
                               kInverseWavelength.at(i) * kInverseWavelength.at(i);
    }
    return scattered;
}

namespace
{

// The light of an area's day and night: `own` its level's, `other` the
// level it borrows the other of them from (Cycled's).
std::shared_ptr<Pair> MakePair(const Cycled &cycled, const LevelLight &own, const LevelLight &other)
{
    auto pair = std::make_shared<Pair>();
    pair->ownIsNight = cycled.ownIsNight;
    pair->day = cycled.ownIsNight ? other : own;
    pair->night = cycled.ownIsNight ? own : other;
    pair->area = cycled.area;
    pair->declination = cycled.declination * kDegree;
    pair->noon = cycled.noon;
    pair->nightFill = cycled.nightFill;
    pair->nightHaze = cycled.nightHaze;
    pair->effectsMoonlit = cycled.effectsMoonlit;
    pair->waterMoonlit = cycled.waterMoonlit;
    pair->dayClouds = cycled.dayClouds;
    pair->lampMoves = cycled.lampMoves;
    pair->heldHour = cycled.heldHour;
    pair->heldOwn = cycled.heldOwn;
    pair->heldUntil = cycled.heldUntil;
    pair->ownMoon = {-pair->night.sun.direction[0], -pair->night.sun.direction[1], -pair->night.sun.direction[2]};
    pair->moonHeight = cycled.moonHeight;
    pair->moonTurn = cycled.moonTurn;
    pair->midnight = MidnightOf(pair->ownMoon, cycled.moonTurn, cycled.moonHeight);
    LevelLight of;
    // The day level's sun, the area's own or the borrowed day's.
    const std::wstring &dayLevel = cycled.ownIsNight ? cycled.other : cycled.own;
    const std::wstring &latitudeOf = cycled.latitudeOf.empty() ? dayLevel : cycled.latitudeOf;
    const dao::core::Sunlight &by =
        latitudeOf == dayLevel || !ReadArl(latitudeOf, of) ? pair->day.sun : of.sun;
    pair->latitude = cycled.latitude ? *cycled.latitude * kDegree
                                     : (std::numbers::pi_v<float> / 2) -
                                           Height({-by.direction[0], -by.direction[1], -by.direction[2]});
    const Vec3 levelSun{-pair->day.sun.direction[0], -pair->day.sun.direction[1], -pair->day.sun.direction[2]};
    pair->ownHour = pair->noon + (SunPathThrough(levelSun, pair->latitude, pair->declination).second / (15 * kDegree));
    spdlog::info("day and night: {}'s level's sun is the season's at {:02d}:{:02d}, at {:.1f} degrees' latitude",
                 std::filesystem::path(cycled.area).string(), static_cast<int>(pair->ownHour),
                 static_cast<int>((pair->ownHour - std::floor(pair->ownHour)) * 60), pair->latitude / kDegree);
    spdlog::info("day and night: {} by day and {} by night, their skies' light {:.3f} {:.3f} {:.3f} and {:.3f} "
                 "{:.3f} {:.3f}",
                 std::filesystem::path(cycled.ownIsNight ? cycled.other : cycled.own).string(),
                 std::filesystem::path(cycled.ownIsNight ? cycled.own : cycled.other).string(), pair->day.skyLight[0],
                 pair->day.skyLight[1], pair->day.skyLight[2], pair->night.skyLight[0], pair->night.skyLight[1],
                 pair->night.skyLight[2]);
    return pair;
}

// Each file of a level's folder's archives that `wanted` says, by its name in
// lower case, handed to `take` until it answers true.
void EachFile(const std::filesystem::path &folder, const std::function<bool(const std::wstring &)> &wanted,
              const std::function<bool(const std::vector<std::byte> &)> &take)
{
    std::error_code error;
    for (const std::filesystem::directory_entry &file : std::filesystem::directory_iterator(folder, error))
    {
        if (Lower(file.path().extension().wstring()) != L".rim")
        {
            continue;
        }
        const std::optional<dao::core::erf::Archive> archive = dao::core::erf::Archive::Open(file.path());
        if (!archive || !archive->Readable())
        {
            continue;
        }
        for (const dao::core::erf::Entry &entry : archive->Entries())
        {
            if (!wanted(Lower(entry.name)))
            {
                continue;
            }
            if (const std::optional<std::vector<std::byte>> bytes = archive->Read(entry); bytes && take(*bytes))
            {
                return;
            }
        }
    }
}

// The ambient light (a node `nalt`) of a level of rooms, its first room
// layout's (<level>_<room>_ll.mmh, in its rooms' archives): the light its
// makers filled its rooms with. The mage origin's tower, lit as a day
// (lak303d), fills every room with 0.104, 0.112, 0.108, its GreyRT and GreyBK
// lights alike gray (0.39, 0.42, 0.40); the Broken Circle's night floors with
// 0.015, 0.045, 0.095 (read 2026-10-25).
std::optional<std::array<float, 3>> RoomsAmbient(const std::filesystem::path &folder, const std::wstring &level)
{
    std::optional<std::array<float, 3>> ambient;
    EachFile(
        folder, [&](const std::wstring &name) { return name.starts_with(level + L"_") && name.ends_with(L"_ll.mmh"); },
        [&](const std::vector<std::byte> &bytes) {
            const std::optional<gff4::File> parsed = gff4::File::Parse(bytes);
            if (const std::optional<gff4::Struct> root = parsed ? parsed->Root() : std::nullopt)
            {
                Under(*root, 0, ambient);
            }
            return ambient.has_value();
        });
    return ambient;
}

// The mean colour of a level's lightmaps (<level>_room*.dds, in its
// archives) as stored, over the texels with any light, every fourth a side:
// the colour of its makers' bake. lak503d's is 0.096, 0.105, 0.182 (measured
// 2026-10-25), a violet.
std::optional<std::array<float, 3>> BakeColour(const std::filesystem::path &folder, const std::wstring &level)
{
    std::array<double, 3> sum{};
    std::size_t counted = 0;
    EachFile(
        folder, [&](const std::wstring &name) { return name.starts_with(level + L"_room") && name.ends_with(L".dds"); },
        [&](const std::vector<std::byte> &bytes) {
            const std::optional<dao::core::dds::Image> image = dao::core::dds::Decode(bytes);
            constexpr std::size_t kEvery = 4;
            for (std::size_t y = 0; image && y < image->height; y += kEvery)
            {
                for (std::size_t x = 0; x < image->width; x += kEvery)
                {
                    const std::uint8_t *pixel = &image->rgba[((y * image->width) + x) * 4];
                    if (pixel[0] + pixel[1] + pixel[2] <= 3)
                    {
                        continue;
                    }
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        sum.at(c) += pixel[c] / 255.0;
                    }
                    ++counted;
                }
            }
            return false;
        });
    if (counted == 0)
    {
        return std::nullopt;
    }
    const auto mean = [&](std::size_t c) { return static_cast<float>(sum.at(c) / static_cast<double>(counted)); };
    return std::array<float, 3>{mean(0), mean(1), mean(2)};
}

// An interior's bake's own colour at a luminance of 1 (BakeColour), and, for
// a night's bake, the colour its day's fill takes (its file's `day`, a level
// its makers lit by day: that level's rooms' fill). Left as made where either
// could not be read.
void ReadIndoorColours(IndoorLight &indoors, const std::filesystem::path &folder, const std::wstring &own,
                       const std::function<std::filesystem::path(const std::wstring &)> &folderOf)
{
    const std::optional<std::array<float, 3>> bake = BakeColour(folder, own);
    if (!bake || Luminance(*bake) <= 0)
    {
        return;
    }
    for (std::size_t c = 0; c < 3; ++c)
    {
        indoors.bakeHue.at(c) = bake->at(c) / Luminance(*bake);
    }
    indoors.bakeLuminance = Luminance(*bake);
    if (const std::optional<std::array<float, 3>> night =
            indoors.night.empty() ? std::nullopt : BakeColour(folderOf(indoors.night), indoors.night);
        night && Luminance(*night) > 0)
    {
        indoors.nightBake = *night;
        spdlog::info("day and night: {}'s night as {}'s bake, {:.3f} {:.3f} {:.3f}", std::filesystem::path(own).string(),
                     std::filesystem::path(indoors.night).string(), night->at(0), night->at(1), night->at(2));
    }
    const std::wstring &day = indoors.other;
    if (const std::optional<std::array<float, 3>> fill =
            day.empty() ? std::nullopt : RoomsAmbient(folderOf(day), day);
        fill && Luminance(*fill) > 0)
    {
        indoors.dayFill = *fill;
    }
    spdlog::info("day and night: {}'s bake {:.3f} {:.3f} {:.3f}{}", std::filesystem::path(own).string(), bake->at(0),
                 bake->at(1), bake->at(2),
                 indoors.dayFill ? ", its day's fill " + std::filesystem::path(day).string() + "'s" : std::string());
}

// The areas the clock runs in on a level, each out of doors, and each
// interior with the area out of doors its windows look on. Under g_lock.
void AreasOn(const std::wstring &level, std::vector<Cycled> &areas, std::vector<std::pair<Cycled, Cycled>> &interiors)
{
    for (const Cycled &cycled : g_cycled)
    {
        if (cycled.own != level)
        {
            continue;
        }
        if (!cycled.indoors)
        {
            areas.push_back(cycled);
            continue;
        }
        const auto outside = std::ranges::find(g_cycled, cycled.outside, &Cycled::area);
        if (outside == g_cycled.end() || outside->indoors)
        {
            spdlog::warn("day and night: {}'s outside, {}, has no file of an area out of doors",
                         std::filesystem::path(cycled.area).string(), std::filesystem::path(cycled.outside).string());
            continue;
        }
        interiors.emplace_back(cycled, *outside);
    }
}

} // namespace

void Opened(const std::filesystem::path &levelRim, const std::filesystem::path &coreEnv)
{
    const std::wstring level = Lower(levelRim.stem().wstring());
    // A level with a day and a night is two, named alike but for the last
    // letter: d for the day's, n for the night's (lak100d and lak100n). The
    // clock runs in the areas data/areas names alone: a night level is the
    // story's night, and the other places are left until the story's times
    // are known. An interior is lit by the hour of the area its windows look
    // out on, whose levels are read with its own.
    const std::scoped_lock lock(g_lock);
    if (!g_asked.insert(level).second ||
        std::ranges::none_of(g_cycled, [&](const Cycled &cycled) { return cycled.own == level; }))
    {
        return;
    }
    // Read at the next Now: the engine is opening an archive here, and its
    // files are read through it (ReadArl).
    g_opened.push_back({levelRim, coreEnv});
}

namespace
{

// The pairs of a level opened (Opened's): its areas' days and nights, read
// from some small files (the .arl and the layouts). The game thread's.
void ReadPairs(const std::filesystem::path &levelRim, const std::filesystem::path &coreEnv)
{
    const std::wstring level = Lower(levelRim.stem().wstring());
    std::vector<Cycled> areas;
    std::vector<std::pair<Cycled, Cycled>> interiors;
    {
        const std::scoped_lock lock(g_lock);
        g_asked.insert(level);
        AreasOn(level, areas, interiors);
    }
    // A level's archive of rooms is in its folder in its own levels' folder:
    // an add-in's, a mod's (Dragon Age Evolved's in an override folder) or
    // the game's. Another level is looked for beside it, then in the game's:
    // an add-in's or a mod's level borrows the game's night (Awakening's,
    // Redcliffe's).
    const std::filesystem::path env = levelRim.parent_path().parent_path();
    const auto rimOf = [&](const std::wstring &name) {
        std::error_code error;
        const std::filesystem::path beside = env / name / (name + L".rim");
        return std::filesystem::exists(beside, error) ? beside : coreEnv / name / (name + L".rim");
    };
    const std::optional<LevelLight> own = ReadLevel(levelRim, level);
    for (const Cycled &cycled : areas)
    {
        std::optional<LevelLight> other = ReadLevel(rimOf(cycled.other), cycled.other);
        if (!own || !other)
        {
            spdlog::warn("day and night: {} or {} could not be read", std::filesystem::path(level).string(),
                         std::filesystem::path(cycled.other).string());
            continue;
        }
        std::shared_ptr<Pair> pair = MakePair(cycled, *own, *other);
        const std::scoped_lock lock(g_lock);
        g_pairs[cycled.area] = std::move(pair);
    }
    for (const auto &[inside, outside] : interiors)
    {
        const std::optional<LevelLight> outsideOwn = ReadLevel(rimOf(outside.own), outside.own);
        const std::optional<LevelLight> outsideOther = ReadLevel(rimOf(outside.other), outside.other);
        if (!outsideOwn || !outsideOther)
        {
            spdlog::warn("day and night: {} or {}, {}'s outside, could not be read",
                         std::filesystem::path(outside.own).string(), std::filesystem::path(outside.other).string(),
                         std::filesystem::path(inside.area).string());
            continue;
        }
        // The outside's clock and sky, held as the interior's own file says.
        std::shared_ptr<Pair> pair = MakePair(outside, *outsideOwn, *outsideOther);
        pair->area = inside.area;
        pair->heldHour = inside.heldHour;
        pair->heldOwn = inside.heldOwn;
        pair->heldUntil = inside.heldUntil;
        pair->lampMoves = inside.lampMoves;
        pair->indoors = inside.indoors;
        if (pair->indoors)
        {
            ReadIndoorColours(*pair->indoors, levelRim.parent_path(), level,
                              [&](const std::wstring &name) { return rimOf(name).parent_path(); });
        }
        spdlog::info("day and night: {} indoors, its windows on {}'s sky, its makers' light kept at {:.2f}",
                     std::filesystem::path(inside.area).string(), std::filesystem::path(outside.area).string(),
                     inside.indoors.value_or(IndoorLight{}).fill);
        const std::scoped_lock lock(g_lock);
        g_pairs[inside.area] = std::move(pair);
    }
}

} // namespace

float NightShare()
{
    return g_nightShare.load(std::memory_order_relaxed);
}

bool Running()
{
    return g_running.load(std::memory_order_relaxed);
}

std::array<float, 3> NightSky()
{
    return {g_nightSky[0].load(std::memory_order_relaxed), g_nightSky[1].load(std::memory_order_relaxed),
            g_nightSky[2].load(std::memory_order_relaxed)};
}

float EvenShare()
{
    return g_evenShare.load(std::memory_order_relaxed);
}

std::array<float, 3> EvenLight()
{
    return {g_even[0].load(std::memory_order_relaxed), g_even[1].load(std::memory_order_relaxed),
            g_even[2].load(std::memory_order_relaxed)};
}

std::array<float, 3> DaySky()
{
    return {g_daySky[0].load(std::memory_order_relaxed), g_daySky[1].load(std::memory_order_relaxed),
            g_daySky[2].load(std::memory_order_relaxed)};
}

void SetHour(float hour)
{
    g_hour = std::fmod(std::fmod(hour, 24.0F) + 24, 24.0F);
}

float Hour()
{
    return g_hour;
}

bool Cycles(std::wstring_view area)
{
    const std::wstring tag = Lower(std::wstring(area));
    const std::scoped_lock lock(g_lock);
    return std::ranges::any_of(g_cycled,
                               [&](const Cycled &cycled) { return !cycled.indoors && Lower(cycled.area) == tag; });
}

bool Indoors(std::wstring_view area)
{
    const std::wstring tag = Lower(std::wstring(area));
    const std::scoped_lock lock(g_lock);
    return std::ranges::any_of(g_cycled,
                               [&](const Cycled &cycled) { return cycled.indoors && Lower(cycled.area) == tag; });
}

std::vector<std::wstring> WindowMaterials(std::wstring_view level)
{
    const std::wstring name = Lower(std::wstring(level));
    const std::scoped_lock lock(g_lock);
    std::vector<std::wstring> materials;
    for (const Cycled &cycled : g_cycled)
    {
        if (cycled.indoors && cycled.own == name)
        {
            materials.insert(materials.end(), cycled.indoors->windows.begin(), cycled.indoors->windows.end());
        }
    }
    return materials;
}

std::array<float, 3> BakeWindow()
{
    return {g_bakeWindow[0].load(std::memory_order_relaxed), g_bakeWindow[1].load(std::memory_order_relaxed),
            g_bakeWindow[2].load(std::memory_order_relaxed)};
}

void LampsBaked(float share)
{
    g_lampsBaked.store(share, std::memory_order_relaxed);
}

std::array<float, 4> RelitNight()
{
    return {g_relitNight.load(std::memory_order_relaxed), g_relitStrength.load(std::memory_order_relaxed), 0, 0};
}

std::array<float, 4> BakeHue()
{
    return {g_bakeHue[0].load(std::memory_order_relaxed), g_bakeHue[1].load(std::memory_order_relaxed),
            g_bakeHue[2].load(std::memory_order_relaxed), g_bakeHue[3].load(std::memory_order_relaxed)};
}

std::vector<std::wstring> BeamModels(std::wstring_view level)
{
    const std::wstring name = Lower(std::wstring(level));
    const std::scoped_lock lock(g_lock);
    std::vector<std::wstring> starts;
    for (const Cycled &cycled : g_cycled)
    {
        if (cycled.indoors && cycled.own == name)
        {
            starts.insert(starts.end(), cycled.indoors->beams.begin(), cycled.indoors->beams.end());
        }
    }
    return starts;
}

Windows WindowLight()
{
    return {{g_window[0].load(std::memory_order_relaxed), g_window[1].load(std::memory_order_relaxed),
             g_window[2].load(std::memory_order_relaxed)},
            g_windowReach.load(std::memory_order_relaxed)};
}

void TryIndoors(std::optional<float> fill, std::optional<float> window, std::optional<float> reach,
                std::optional<float> relit)
{
    g_tryFill = fill;
    g_tryWindow = window;
    g_tryReach = reach;
    g_tryRelit = relit;
}

std::optional<Borrowed> BorrowedBy(std::wstring_view level)
{
    const std::scoped_lock lock(g_lock);
    const auto cycled =
        std::ranges::find_if(g_cycled, [&](const Cycled &each) { return !each.indoors && each.own == level; });
    return cycled != g_cycled.end() ? std::optional<Borrowed>(Borrowed{cycled->other, cycled->ownIsNight})
                                    : std::nullopt;
}

void TryFogMoon(std::optional<float> share)
{
    g_tryFogMoon = share;
}

void TryWaterMoonlit(std::optional<float> share)
{
    g_tryWaterMoonlit = share;
}

void TryNightColour(std::optional<float> kept)
{
    g_tryColour = kept;
}

void TryDayHaze(std::optional<float> dawn, std::optional<float> dusk)
{
    g_tryDawnHaze = dawn;
    g_tryDuskHaze = dusk;
}

void TryMoon(std::optional<float> turn, std::optional<float> height)
{
    g_tryTurn = turn;
    g_tryHeight = height;
}

namespace
{

// The hour an area's light is held at, where its file holds it and the
// party has not the flag that lets its clock run; none for the clock's. The
// flag is asked of the scripts once a second. The game thread's.
std::optional<float> HeldHour(const Pair &pair)
{
    if (!pair.heldHour)
    {
        return std::nullopt;
    }
    const float held = pair.heldOwn ? pair.ownHour : *pair.heldHour;
    if (pair.heldUntil.empty())
    {
        return held;
    }
    static std::wstring asked; // the area last asked for
    static bool set = false;
    static std::chrono::steady_clock::time_point at;
    const auto now = std::chrono::steady_clock::now();
    if (asked != pair.area || now - at > std::chrono::seconds(1))
    {
        namespace api = dao::script::api;
        const bool was = set && asked == pair.area;
        const dao::ObjectId party = api::GetParty(api::GetHero());
        set = std::ranges::any_of(pair.heldUntil, [&](const auto &until) {
            const std::wstring guid = api::GetPlotGUID(until.first);
            return !guid.empty() && api::GetPartyPlotFlag(party, guid, until.second) != 0;
        });
        if (set != was || asked != pair.area)
        {
            spdlog::info("day and night: {}'s light {}", std::filesystem::path(pair.area).string(),
                         set ? "goes with the clock, its plot's flag set" : "held, no flag of its plots set");
        }
        asked = pair.area;
        at = now;
    }
    return set ? std::nullopt : std::optional(held);
}

} // namespace

std::optional<Light> Now(std::wstring_view area, const dao::core::Sunlight &ownSun, const dao::core::Sky &ownSky,
                         bool running, float dayMinutes, float night, std::optional<float> fill,
                         std::optional<float> haze)
{
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> opened;
    {
        const std::scoped_lock lock(g_lock);
        opened.swap(g_opened);
    }
    for (const auto &[levelRim, coreEnv] : opened)
    {
        ReadPairs(levelRim, coreEnv);
    }
    std::shared_ptr<const Pair> pair;
    {
        const std::scoped_lock lock(g_lock);
        for (const auto &[tag, each] : g_pairs)
        {
            // An interior is its own level's whatever its sun; an area out of
            // doors is the one of its levels the area's sun says.
            if (tag == Lower(std::wstring(area)) && (each->indoors || Same(Own(*each).sun, ownSun)))
            {
                pair = each;
                break;
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_ticked && dayMinutes > 0 && running && pair)
    {
        const float seconds = std::min(std::chrono::duration<float>(now - *g_ticked).count(), kMostFrame);
        g_hour = std::fmod(g_hour + (seconds * 24 / (dayMinutes * 60)), 24.0F);
    }
    g_ticked = now;
    if (!pair || !running)
    {
        Stopped();
        return std::nullopt;
    }
    Pair tried;
    const Pair *lit = pair.get();
    if (g_tryTurn || g_tryHeight)
    {
        tried = *pair;
        tried.midnight =
            MidnightOf(pair->ownMoon, g_tryTurn.value_or(pair->moonTurn), g_tryHeight ? g_tryHeight : pair->moonHeight);
        lit = &tried;
    }
    const float hour = HeldHour(*pair).value_or(g_hour);
    const float nightFill = fill.value_or(pair->nightFill);
    const float nightHaze = std::clamp(haze.value_or(pair->nightHaze), 0.0F, 1.0F);
    const Light light = pair->indoors
                            ? IndoorsAt(*lit, *pair->indoors, ownSun, ownSky, hour, night, nightFill, nightHaze)
                            : LightAt(*lit, ownSky, hour, night, nightFill, nightHaze);
    for (std::size_t i = 0; i < 3; ++i)
    {
        g_baked.at(i).store(light.baked.at(i), std::memory_order_relaxed);
        g_layoutLightsBy.at(i).store(light.sunOverDay.at(i), std::memory_order_relaxed);
    }
    g_nightShare.store(light.nightShare, std::memory_order_relaxed);
    g_water[0].store(light.water[0], std::memory_order_relaxed);
    g_water[1].store(light.water[1], std::memory_order_relaxed);
    for (std::size_t i = 0; i < g_lit.size(); ++i)
    {
        g_lit.at(i).store(light.lit.at(i), std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < 3; ++i)
    {
        g_nightSky.at(i).store(light.nightSky.at(i), std::memory_order_relaxed);
        g_even.at(i).store(light.even.at(i), std::memory_order_relaxed);
        g_daySky.at(i).store(pair->day.skyLight.at(i), std::memory_order_relaxed);
    }
    g_evenShare.store(light.evenShare, std::memory_order_relaxed);
    for (std::size_t i = 0; i < g_window.size(); ++i)
    {
        g_window.at(i).store(light.window.at(i), std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < g_bakeHue.size(); ++i)
    {
        g_bakeHue.at(i).store(light.bakeHue.at(i), std::memory_order_relaxed);
    }
    for (std::size_t i = 0; i < g_bakeWindow.size(); ++i)
    {
        g_bakeWindow.at(i).store(light.bakeWindow.at(i), std::memory_order_relaxed);
    }
    g_relitNight.store(light.relitNight, std::memory_order_relaxed);
    g_relitStrength.store(std::max(g_tryRelit.value_or(1.0F), 0.0F), std::memory_order_relaxed);
    g_windowReach.store(pair->indoors ? std::max(g_tryReach.value_or(pair->indoors->windowReach), 0.0F) : 0.0F,
                        std::memory_order_relaxed);
    g_indoors.store(pair->indoors.has_value(), std::memory_order_relaxed);
    g_running.store(true, std::memory_order_relaxed);
    const std::scoped_lock lock(g_lock);
    g_disc = light.disc;
    g_discIsMoon.store(light.moon, std::memory_order_relaxed);
    g_lampMoves = pair->lampMoves;
    return light;
}

std::array<float, 3> Baked()
{
    return {g_baked[0].load(std::memory_order_relaxed), g_baked[1].load(std::memory_order_relaxed),
            g_baked[2].load(std::memory_order_relaxed)};
}

std::array<float, 2> Water()
{
    return {g_water[0].load(std::memory_order_relaxed), g_water[1].load(std::memory_order_relaxed)};
}

std::array<float, 3> Lit()
{
    return {g_lit[0].load(std::memory_order_relaxed), g_lit[1].load(std::memory_order_relaxed),
            g_lit[2].load(std::memory_order_relaxed)};
}

std::optional<std::array<float, 3>> Disc()
{
    const std::scoped_lock lock(g_lock);
    return g_disc;
}

bool DiscIsMoon()
{
    return g_discIsMoon.load(std::memory_order_relaxed);
}

void Install()
{
    {
        Table table = ReadTable(dao::Module::OwnFolder() / "areas");
        spdlog::info("day and night: {} areas and {} names of fires by the areas' files", table.areas.size(),
                     table.fires.size());
        g_fireLights = std::move(table.fires);
        g_fireStrength = table.fireStrength;
        const std::scoped_lock lock(g_lock);
        g_cycled = std::move(table.areas);
    }
    // The ambient colour the engine adds to a level's surfaces and props: the
    // level's sky light, which goes with the hour as its bake does.
    dao::lights::OnAmbient([](dao::lights::Ambient &ambient) {
        for (std::size_t i = 0; i < 3; ++i)
        {
            ambient.color.at(i) *= g_baked.at(i).load(std::memory_order_relaxed);
        }
    });
    // The engine's own shade on figures: a grid over the area says, cell by
    // cell, whether a figure there is out of the level's sun, and gives it a
    // tenth of the sun's light if so (DAOLib's Grid.h, dao::lights::SunShare).
    // The grid was made for the level's one sun; while the hour moves the sun
    // and the moon it shaded Lyna under a cliff at noon and at midnight alike,
    // the sun's strength on her doing nothing there (the user, 2026-10-22).
    // The plugin's maps shadow figures as the light is; the grid is let go.
    dao::lights::OnSunShare([](dao::lights::SunShare &sun) {
        if (g_running.load(std::memory_order_relaxed))
        {
            sun.share = 1.0F;
        }
    });
    dao::lights::OnPointLight([](dao::lights::PointLight &light) {
        // A level's layouts' lights are its day's. A room's (<level>_<x>_<y>_<d>_ll):
        // the sun's bounce ("Hotspots", the sun's colour) and fills, which go
        // with the sun, as the moon alone lights the level at night. Read from
        // the level's archive alone, only 20 of Redcliffe's were dimmed, and
        // the rooms' own lit the houses warm at midnight (the user,
        // 2026-10-21). Not those named as fires (data/areas/defaults.ini's `fires`): the castle's
        // layouts have its torches' lights among them, which went out with
        // the sun and left its torches' alcoves black at night (issue 35). The figures' (<...>_lc) light figures alone,
        // a fill: they go with the sky's light, as the bake and the probes do, or a figure in the moon's shadow was
        // dark against the fill-lit ground (the user, 2026-10-22). An effect's light, a fire's, is its own.
        if (!g_running.load(std::memory_order_relaxed))
        {
            return;
        }
        const std::wstring_view model = light.model;
        const auto endsWith = [&](std::wstring_view end) {
            return model.size() >= end.size() &&
                   _wcsnicmp(model.data() + model.size() - end.size(), end.data(), end.size()) == 0;
        };
        // Indoors a fire-coloured one is a lantern's or a candle's and keeps
        // its light (FireColoured); the rest are its makers' fill.
        const bool fitting =
            g_indoors.load(std::memory_order_relaxed) && FireColoured({light.color[0], light.color[1], light.color[2]});
        if (endsWith(L"_ll") && !FireLamp(model, light.name) && !fitting)
        {
            for (std::size_t i = 0; i < 3; ++i)
            {
                light.color.at(i) *= g_layoutLightsBy.at(i).load(std::memory_order_relaxed);
            }
        }
        else if (fitting)
        {
            // Its light in the night lightmaps and their probes by night.
            const float kept = 1 - g_lampsBaked.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < 3; ++i)
            {
                light.color.at(i) *= kept;
            }
        }
        else if (endsWith(L"_lc"))
        {
            for (std::size_t i = 0; i < 3; ++i)
            {
                light.color.at(i) *= g_baked.at(i).load(std::memory_order_relaxed);
            }
        }
    });
}

} // namespace daynight
