#include "DayNight.h"

#include "dao/Lights.h"
#include "dao/core/Erf.h"
#include "dao/core/Gff4.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
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

struct Pair
{
    LevelLight day;
    LevelLight night;
};

std::wstring Lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

std::optional<std::vector<std::byte>> FileBytes(const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        return std::nullopt;
    }
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(chars.size());
    std::memcpy(bytes.data(), chars.data(), chars.size());
    return bytes;
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

// A level's sun and sky, from its .arl.
bool ReadArl(const std::filesystem::path &arl, LevelLight &light)
{
    const std::optional<std::vector<std::byte>> bytes = FileBytes(arl);
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

std::optional<LevelLight> ReadLevel(const std::filesystem::path &env, const std::wstring &level)
{
    LevelLight light;
    if (!ReadArl(env / (level + L".arl"), light) || !ReadLayouts(env / level / (level + L".rim"), level, light))
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
// The sun's declination: an early summer's. The day level's noon sun (38
// degrees up) gives the latitude as an equinox's noon (52 degrees), and an
// equinox's sun set at 18:00: at 19:00 it was 9 degrees under, the level in
// deep dusk, "way too dark for 7:00 PM" (the user, 2026-10-22, who chose
// summer days). At 18 degrees it rises about 04:20, sets about 19:40 and
// dusk lasts until about 20:30; at noon it is 56 degrees up, higher than the
// day level's.
constexpr float kSunDeclination = 18 * kDegree;
// The hour the sun is highest: a summer's clock, an hour on from the sun's
// (as clocks keep summer time). At noon it rose about 04:20, and at 4:00 the
// morning was light already (the user, 2026-10-22: "sun rises too early");
// at 13:00 it rises about 05:20 and sets about 20:40.
constexpr float kSolarNoon = 13;
// The day levels the clock runs in (Opened).
constexpr std::array<std::wstring_view, 1> kCycled{L"lak100d"};
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
// "light should be more of a pale blue white, not nearly so garish").
constexpr float kNightColour = 0.35F;

// A colour with `kept` of its saturation: 0 its grey of the same luminance.
std::array<float, 3> Paler(const std::array<float, 3> &colour, float kept)
{
    const float grey = (0.2126F * colour[0]) + (0.7152F * colour[1]) + (0.0722F * colour[2]);
    return {std::lerp(grey, colour[0], kept), std::lerp(grey, colour[1], kept), std::lerp(grey, colour[2], kept)};
}

// The sun's path through the day level's own sun: that sun is the summer's
// (kSunDeclination) of an afternoon, at the hour it stands as high, and the
// path is turned about the upright so that at that hour it is where the
// level's is. Noon is higher, and the level's own light, its bake's, is the
// afternoon's (the user, 2026-10-22: "perhaps the default isn't noon, but
// something more like 10:AM or 3:PM"). The latitude is the one at which the
// level's sun would be an equinox's noon. Returns the path's transit and the
// level's sun's hour angle (radians after noon).
std::pair<Vec3, float> SunPathThrough(const Vec3 &level, float latitude)
{
    const float height = Height(level);
    const float cosine = (std::sin(height) - (std::sin(latitude) * std::sin(kSunDeclination))) /
                         (std::cos(latitude) * std::cos(kSunDeclination));
    const float after = std::acos(std::clamp(cosine, -1.0F, 1.0F));
    const Vec3 then = OnPath(level, latitude, kSunDeclination, after);
    const float turned = std::atan2(level[1], level[0]) - std::atan2(then[1], then[0]);
    const float c = std::cos(turned);
    const float s = std::sin(turned);
    return {Vec3{(c * level[0]) - (s * level[1]), (s * level[0]) + (c * level[1]), level[2]}, after};
}

Light LightAt(const Pair &pair, const dao::core::Sky &own, float hour, float night, float fill)
{
    const dao::core::Sky daySky = SkyOf(own, pair.day);
    const dao::core::Sky nightSky = SkyOf(own, pair.night);
    const Vec3 levelSun{-pair.day.sun.direction[0], -pair.day.sun.direction[1], -pair.day.sun.direction[2]};
    const Vec3 midnight{-pair.night.sun.direction[0], -pair.night.sun.direction[1], -pair.night.sun.direction[2]};
    const float noonHeight = Height(levelSun); // the level's sun's, as an equinox's noon
    const float latitude = (std::numbers::pi_v<float> / 2) - noonHeight;
    const float turn = (hour - kSolarNoon) * 15 * kDegree;
    const Vec3 toSun = OnPath(SunPathThrough(levelSun, latitude).first, latitude, kSunDeclination, turn);
    const float moonTilt = Height(midnight) - noonHeight;
    const Vec3 toMoon = OnPath(midnight, latitude, moonTilt, turn + std::numbers::pi_v<float>);
    const float sunHeight = Height(toSun);
    const float moonHeight = Height(toMoon);

    const float day = SmoothStep(kNightFrom, kDayFrom, sunHeight);
    Light light;
    light.sky = dao::core::Blend(nightSky, daySky, day);
    light.nightShare = 1 - day;
    // At night the moon alone lights what the bake lit by day, from where it
    // is: the bake, a level's probes and its ambient go to the night sky's
    // light, kept to `fill` of the night level's (the user, 2026-10-21: "fade
    // out the baked lighting and just strengthen the moonlight so it lights
    // things properly from its position").
    light.nightSky = Paler(pair.night.skyLight, kNightColour);
    for (float &channel : light.nightSky)
    {
        channel *= fill;
    }

    // The light through the air: the day's or the night's at its highest, less
    // what the longer way takes out, by the air of the sky of the hour.
    const std::array<float, 3> depth = Depth(light.sky);
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
        std::array<float, 3> paler = Paler(colour, kNightColour);
        for (float &channel : paler)
        {
            channel *= night;
        }
        return paler;
    };
    const std::array<float, 3> moon =
        ofNight({pair.night.sun.color[0], pair.night.sun.color[1], pair.night.sun.color[2]});
    const std::array<float, 3> moonOnFigures =
        ofNight({pair.night.sun.characterColor[0], pair.night.sun.characterColor[1], pair.night.sun.characterColor[2]});
    const std::array<float, 3> nightSkyLight = light.nightSky;
    const std::array<float, 3> &ownSkyLight = pair.day.skyLight;
    for (std::size_t i = 0; i < 3; ++i)
    {
        // No more than the level's own: a summer's sun higher than the day
        // level's noon is as bright as that noon (kSunDeclination).
        const float air = std::exp(-depth.at(i) * std::max(mass - atHighest, 0.0F));
        const float through = air * up;
        light.disc.at(i) = sunLights ? air : 1;
        light.sun.color.at(i) = (sunLights ? pair.day.sun.color.at(i) : moon.at(i)) * through;
        light.sun.characterColor.at(i) =
            (sunLights ? pair.day.sun.characterColor.at(i) : moonOnFigures.at(i)) * through;
        light.sun.direction.at(i) = -toward.at(i);
        light.sunOverDay.at(i) = sunLights ? through : 0;
        const float skyLight = std::lerp(nightSkyLight.at(i), pair.day.skyLight.at(i), day);
        light.baked.at(i) = ownSkyLight.at(i) > 0 ? skyLight / ownSkyLight.at(i) : 1;
    }
    light.sun.color[3] = light.sun.characterColor[3] = light.sun.direction[3] = 1;
    // The disc: the sun's the hue of its light through the air, as bright as
    // ever (a low sun is still the brightest thing in the sky); its strength
    // (the sky's moon alpha, the disc's whole brightness) the day's or the
    // night's own, going as the body crosses the horizon and, for the moon,
    // with twilight.
    const float brightest = std::max({light.disc[0], light.disc[1], light.disc[2]});
    for (float &channel : light.disc)
    {
        channel = brightest > 0 ? channel / brightest : 1;
    }
    light.sky.moonAlpha = sunLights ? daySky.moonAlpha * SmoothStep(kOver, -kOver, sunHeight) : nightSky.moonAlpha * up;
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

// Every level of a pair opened, by its name, and the levels looked at, under
// g_lock.
std::mutex g_lock;
std::map<std::wstring, std::shared_ptr<const Pair>> g_pairs;
std::set<std::wstring> g_asked;
// What the hour's light hands the render thread: the sky light's multiplier,
// the disc's colour (under g_lock), and the day level's sunlight lamps with
// what they are multiplied by.
std::array<std::atomic<float>, 3> g_baked{1.0F, 1.0F, 1.0F};
std::atomic<float> g_nightShare{0};
std::array<std::atomic<float>, 3> g_nightSky{0.0F, 0.0F, 0.0F};
std::optional<std::array<float, 3>> g_disc;
std::atomic<bool> g_running{false}; // the clock runs in a level with a night
std::array<std::atomic<float>, 3> g_layoutLightsBy{1.0F, 1.0F, 1.0F};

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
    g_nightShare.store(0, std::memory_order_relaxed);
    g_running.store(false, std::memory_order_relaxed);
    const std::scoped_lock lock(g_lock);
    g_disc.reset();
}

} // namespace

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

void Opened(const std::filesystem::path &levelRim)
{
    const std::wstring level = Lower(levelRim.stem().wstring());
    {
        const std::scoped_lock lock(g_lock);
        if (g_asked.contains(level))
        {
            return;
        }
    }
    // A level with a day and a night is two, named alike but for the last
    // letter: d for the day's, n for the night's (lak100d and lak100n). The
    // clock runs in the day level of those named in kCycled alone: the night
    // level is the story's night, and the other places are left until the
    // story's times are known (the user, 2026-10-22: "certain areas / times
    // should not change for story reasons").
    {
        const std::scoped_lock lock(g_lock);
        g_asked.insert(level);
    }
    if (std::ranges::find(kCycled, level) == kCycled.end())
    {
        return;
    }
    const std::filesystem::path env = levelRim.parent_path().parent_path();
    const std::wstring other = level.substr(0, level.size() - 1) + L"n";
    // Some small files (the two .arl and the layouts), read as the level is
    // opened.
    std::optional<LevelLight> day = ReadLevel(env, level);
    std::optional<LevelLight> night = ReadLevel(env, other);
    if (!day || !night)
    {
        spdlog::warn("day and night: {} or {} could not be read", std::filesystem::path(level).string(),
                     std::filesystem::path(other).string());
        return;
    }
    auto pair = std::make_shared<Pair>();
    pair->day = std::move(*day);
    pair->night = std::move(*night);
    {
        const Vec3 levelSun{-pair->day.sun.direction[0], -pair->day.sun.direction[1], -pair->day.sun.direction[2]};
        const float latitude = (std::numbers::pi_v<float> / 2) - Height(levelSun);
        const float at = kSolarNoon + (SunPathThrough(levelSun, latitude).second / (15 * kDegree));
        spdlog::info("day and night: the day level's sun is the summer's at {:02d}:{:02d}", static_cast<int>(at),
                     static_cast<int>((at - std::floor(at)) * 60));
    }
    spdlog::info("day and night: {} and {}, their skies' light {:.3f} {:.3f} {:.3f} and {:.3f} {:.3f} {:.3f}",
                 std::filesystem::path(level).string(), std::filesystem::path(other).string(), pair->day.skyLight[0],
                 pair->day.skyLight[1], pair->day.skyLight[2], pair->night.skyLight[0], pair->night.skyLight[1],
                 pair->night.skyLight[2]);
    const std::scoped_lock lock(g_lock);
    g_pairs[level] = std::move(pair);
}

float NightShare()
{
    return g_nightShare.load(std::memory_order_relaxed);
}

std::array<float, 3> NightSky()
{
    return {g_nightSky[0].load(std::memory_order_relaxed), g_nightSky[1].load(std::memory_order_relaxed),
            g_nightSky[2].load(std::memory_order_relaxed)};
}

void SetHour(float hour)
{
    g_hour = std::fmod(std::fmod(hour, 24.0F) + 24, 24.0F);
}

float Hour()
{
    return g_hour;
}

std::optional<Light> Now(const dao::core::Sunlight &ownSun, const dao::core::Sky &ownSky, bool running,
                         float dayMinutes, float night, float fill)
{
    std::shared_ptr<const Pair> pair;
    {
        const std::scoped_lock lock(g_lock);
        for (const auto &[level, each] : g_pairs)
        {
            if (Same(each->day.sun, ownSun))
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
    const Light light = LightAt(*pair, ownSky, g_hour, night, fill);
    for (std::size_t i = 0; i < 3; ++i)
    {
        g_baked.at(i).store(light.baked.at(i), std::memory_order_relaxed);
        g_layoutLightsBy.at(i).store(light.sunOverDay.at(i), std::memory_order_relaxed);
    }
    g_nightShare.store(light.nightShare, std::memory_order_relaxed);
    for (std::size_t i = 0; i < 3; ++i)
    {
        g_nightSky.at(i).store(light.nightSky.at(i), std::memory_order_relaxed);
    }
    g_running.store(true, std::memory_order_relaxed);
    const std::scoped_lock lock(g_lock);
    g_disc = light.disc;
    return light;
}

std::array<float, 3> Baked()
{
    return {g_baked[0].load(std::memory_order_relaxed), g_baked[1].load(std::memory_order_relaxed),
            g_baked[2].load(std::memory_order_relaxed)};
}

std::optional<std::array<float, 3>> Disc()
{
    const std::scoped_lock lock(g_lock);
    return g_disc;
}

void Install()
{
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
        // 2026-10-21). The figures' (<...>_lc) light figures alone, a fill:
        // they go with the sky's light, as the bake and the probes do, or a
        // figure in the moon's shadow was dark against the fill-lit ground
        // (the user, 2026-10-22). An effect's light, a fire's, is its own.
        if (!g_running.load(std::memory_order_relaxed))
        {
            return;
        }
        const std::wstring_view model = light.model;
        const auto endsWith = [&](std::wstring_view end) {
            return model.size() >= end.size() &&
                   _wcsnicmp(model.data() + model.size() - end.size(), end.data(), end.size()) == 0;
        };
        if (endsWith(L"_ll"))
        {
            for (std::size_t i = 0; i < 3; ++i)
            {
                light.color.at(i) *= g_layoutLightsBy.at(i).load(std::memory_order_relaxed);
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
