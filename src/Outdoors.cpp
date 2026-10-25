#include "Outdoors.h"

#include "DayNight.h"
#include "LevelShadows.h"

#include "Probes.h"

#include "dao/Atmosphere.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/Framebuffer.h"
#include "dao/GameModes.h"
#include "dao/Module.h"
#include "dao/Objects.h"
#include "dao/World.h"
#include "dao/core/Sky.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace outdoors
{

namespace
{

constexpr std::string_view kTuneFile = "outdoors.tune";
constexpr std::wstring_view kChain = L"da_hsvmatrix"; // the chain build_shaders.py puts our values in
// Frames between looks at the area and the file: half a second or so.
constexpr int kEvery = 30;

int g_frames = 0;

using Vec3 = std::array<float, 3>;
using Mat3 = std::array<Vec3, 3>; // by rows

float Dot(const Vec3 &a, const Vec3 &b)
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Vec3 Times(const Mat3 &m, const Vec3 &v)
{
    return {Dot(m[0], v), Dot(m[1], v), Dot(m[2], v)};
}

std::string_view Trimmed(std::string_view text)
{
    constexpr std::string_view kSpace = " \t\r";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

std::string Underscored(std::string_view name)
{
    std::string out(name);
    for (char &c : out)
    {
        c = c == ' ' ? '_' : c;
    }
    return out;
}

void LogArea(const dao::atmosphere::Sunlight &sun, const dao::atmosphere::Sky &sky)
{
    spdlog::info(
        "the area's sun: ground {:.3f} {:.3f} {:.3f}, figures {:.3f} {:.3f} {:.3f}, going {:.2f} {:.2f} {:.2f}",
        sun.color[0], sun.color[1], sun.color[2], sun.characterColor[0], sun.characterColor[1], sun.characterColor[2],
        sun.direction[0], sun.direction[1], sun.direction[2]);
    std::string line;
    for (const dao::core::SkyField &field : dao::core::kSkyFields)
    {
        line += std::string(line.empty() ? "" : ", ") + Underscored(field.name) + " " +
                std::to_string(sky.*field.member).substr(0, 6);
    }
    spdlog::info("the area's sky: {}", line);
}

// The area's own light, logged when it is another's.
void WatchArea()
{
    static std::optional<dao::atmosphere::Sunlight> logged;
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = dao::atmosphere::AreaOwnSky();
    if (!sun || !sky || sun == logged)
    {
        return;
    }
    logged = sun;
    LogArea(*sun, *sky);
}

// The tuning file as last read, and when it was written.
std::map<std::string, float, std::less<>> g_tune;
std::filesystem::file_time_type g_tuneWritten{};
bool g_skySet = false; // a sky of ours is on the area now: the hour's or the file's

void ReadTune(const std::filesystem::path &file)
{
    g_tune.clear();
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
    {
        const std::size_t equals = line.find('=');
        if (line.starts_with(';') || equals == std::string::npos)
        {
            continue;
        }
        const std::string_view key = Trimmed(std::string_view(line).substr(0, equals));
        const std::string_view value = Trimmed(std::string_view(line).substr(equals + 1));
        float number = 0;
        if (std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc{})
        {
            g_tune[std::string(key)] = number;
        }
    }
    std::string said;
    for (const auto &[key, value] : g_tune)
    {
        said += std::string(said.empty() ? "" : ", ") + key + " " + std::to_string(value).substr(0, 6);
    }
    spdlog::info("{}: {}", kTuneFile, said.empty() ? "nothing" : said);
}

std::optional<float> Tuned(std::string_view key)
{
    const auto found = g_tune.find(key);
    return found != g_tune.end() ? std::optional<float>(found->second) : std::nullopt;
}

// Whether the player wants the air lit (the options screen's Improved lighting).
bool g_wanted = false;

// The light of the hour where the area's level has a day and a night
// (DayNight.h), as of this frame; none elsewhere.
std::optional<daynight::Light> g_hourLight;
// Whether the options screen's Day / night cycle is on (WantClock).
bool g_clockOn = false;

// The sun and the sky the plugin lights the area by: the hour's, or the
// area's own.
std::optional<dao::atmosphere::Sunlight> BaseSunlight()
{
    return g_hourLight ? std::optional(g_hourLight->sun) : dao::atmosphere::AreaOwnSunlight();
}
std::optional<dao::atmosphere::Sky> BaseSky()
{
    return g_hourLight ? std::optional(g_hourLight->sky) : dao::atmosphere::AreaOwnSky();
}

SkyHaze g_haze;
std::array<float, 20> g_hazeView{}; // HazeView's, set by KeepHaze
float g_fills = 1;

// The one sun on figures (Improved lighting): out of doors under a sun or a moon, a figure is lit as the
// ground it stands on is, by the one sun in the ground's colour and strength
// and the area's ambient light, and the area's fills, three far lights only
// figures have, are taken away (the user, 2026-10-16: "One sun, one color,
// one strength. We should also try getting rid of fill lights and just using
// sun directly"). An area gives figures a sun of their own: 0.89 times the
// ground's in Redcliffe, 1.43 in the Dalish camp, pale yellow on figures and
// deep orange on the ground in the Korcari Wilds, gray and cyan in Haven.
// (Two rules before this one kept the fills at a half, with the sun on figures
// at a fixed 1.3, which left figures 13 to 29 in 100 darker where the sun is
// weak, or given what the fills lost.) Half of them was given back for an
// afternoon of 2026-10-17, and in a building's shadow in the Dalish camp
// Alistair was 54.7 of 255 where the ground about his feet was 41.1; with no
// fills and no sun on him, 41.0 against 41.0: the probes light a figure as
// the bake lights the ground, and nothing can shade a fill.
constexpr float kSunlitFills = 0;
bool g_wantedSunlit = false;

// Whether figures take the one sun (Improved lighting): the player's word, or the tuning file's
// `SunlitFigures`, to compare without the options screen.
bool SunlitFigures()
{
    return Tuned("SunlitFigures").value_or(g_wantedSunlit ? 1 : 0) > 0;
}
bool g_outdoors = false;
bool g_sunlit = false; // out of doors with a sun on the ground (Sunlit)

// The air's light from the area's own. Its fog colour is the air's light as
// its makers saw it, the same every way. The air is a scatterer of the sun's
// light: in each channel a share of the colour is scattered by the air's
// molecules (Rayleigh's, as bright towards the sun as away from it, darkest
// across) and the rest by its haze (Mie's, bright towards the sun). The two
// shares are the area's own, reckoned as the engine reckons them for its sky
// (lib-agent, read in the sky's recompute 0x0055BFA0, 2026-10-16): Rayleigh's
// rayleighMultiplier times (6.95, 11.8, 24.4) per million metres, Mie's
// mieMultiplier times Preetham's 0.434 c pi (2 pi / lambda)^2 0.67, c of the
// turbidity, at 650, 570 and 475 nm; and Mie's phase is Henyey-Greenstein's
// with the area's hg. On the mean over all ways the haze is the area's own
// colour: nothing is added, and the light only goes where the air sends it.
// Only the lobe's way is not the engine's: its sky takes hg against the way
// to the sun, and every area's hg is negative (-0.5 the default), which puts
// the haze's glow behind the eye, where no haze sends it; Preetham's model
// and the sky over us send it forward, so the size of hg is taken and the
// lobe faces the sun. An area with no sun keeps its fog as it is.
// (Two forms before this one had a share of my own and a phase of my own;
// the first added the sun's light on top of the colour, and far trees towards
// the sun went white in the Dalish camp, whose fog is brighter than its sun;
// the user, 2026-10-16: "Are we just applying hacks on top of a problem we
// don't understand".)
// Under a day and a night the sky is the hour's, and the haze's part is the
// sun's light scattered on, so it goes with the sun's colour over the day's
// (`sunOverDay`); the moon's is left in the sky's own fog colour.
// The haze's part is of the luminance its share of the colour has, in the
// fog's own hue (below). Haven's fog is nearly all haze
// (its Mie multiplier 0.046 against Rayleigh's 0.028) of a deep teal, and
// with the lobe towards the sun its far mountains went a flat neon cyan
// brighter than its sky (the user, 2026-10-23: "fog w/ improved lighting
// makes mountains look too intense", issue 53; its daytime "is cool, not
// warm": the haze is not given the sun's colour either).
SkyHaze HazeOf(const dao::atmosphere::Sunlight &sun, const dao::atmosphere::Sky &sky, bool outdoors,
               const std::array<float, 3> &sunOverDay)
{
    SkyHaze haze;
    const bool sunny = std::max({sun.color[0], sun.color[1], sun.color[2]}) > 0;
    haze.on = outdoors && sunny && g_wanted && Tuned("Haze").value_or(1) > 0;
    const daynight::Scattered scattered = daynight::Scattering(sky);
    const std::array<float, 3> fog{sky.fogRed, sky.fogGreen, sky.fogBlue};
    constexpr std::array<double, 3> kLuminance{0.2126, 0.7152, 0.0722};
    double hazeLuminance = 0;
    for (std::size_t i = 0; i < 3; ++i)
    {
        const double rayleigh = scattered.air.at(i);
        const double mie = scattered.haze.at(i);
        const double mieShare = rayleigh + mie > 0 ? mie / (rayleigh + mie) : 0;
        haze.air.at(i) = static_cast<float>((1 - mieShare) * fog.at(i));
        hazeLuminance += kLuminance.at(i) * mieShare * fog.at(i);
    }
    // In the fog's own hue, at the luminance the haze's share of it has: the
    // makers' colour of the air, not its brightness. In grey the Dalish camp's
    // and the Brecilian Forest's gold went a cool grey (the user, 2026-10-24,
    // asked the makers' colour back); Haven's neon (issue 53) was the haze's
    // own brightness, and at this luminance its far mountains are as they were
    // in grey (94, 118, 135 of 255 against 92, 118, 136).
    const double fogLuminance = (kLuminance[0] * fog[0]) + (kLuminance[1] * fog[1]) + (kLuminance[2] * fog[2]);
    for (std::size_t i = 0; i < 3; ++i)
    {
        const double tint = fogLuminance > 0 ? fog.at(i) / fogLuminance : 1.0;
        haze.sun.at(i) = static_cast<float>(hazeLuminance * tint) * sunOverDay.at(i);
    }
    haze.g = std::min(std::abs(sky.hg), 0.95F);
    return haze;
}

void KeepTune()
{
    const std::filesystem::path file = dao::Module::OwnFolder() / kTuneFile;
    std::error_code error;
    if (!std::filesystem::exists(file, error))
    {
        if (g_tuneWritten != std::filesystem::file_time_type{})
        {
            g_tune.clear();
            g_tuneWritten = {};
            spdlog::info("{} gone: the area's own light", kTuneFile);
        }
        return;
    }
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, error);
    if (!error && written != g_tuneWritten)
    {
        g_tuneWritten = written;
        ReadTune(file);
    }
}

// The sky on the area: the hour's or its own, with the file's values over it
// for a developer's trying of them.
void KeepSky()
{
    const std::optional<dao::atmosphere::Sky> own = dao::atmosphere::AreaOwnSky();
    std::optional<dao::atmosphere::Sky> sky = BaseSky();
    if (!own || !sky)
    {
        return;
    }
    for (const dao::core::SkyField &field : dao::core::kSkyFields)
    {
        if (const std::optional<float> value = Tuned("sky." + Underscored(field.name)))
        {
            (*sky).*field.member = *value;
        }
    }
    if (*sky == *own)
    {
        if (g_skySet)
        {
            dao::atmosphere::ResetSky();
            g_skySet = false;
        }
        return;
    }
    if (*sky != dao::atmosphere::CurrentSky())
    {
        dao::atmosphere::SetSky(*sky);
    }
    g_skySet = true;
}

// The air's light for the area the party is in, as the option and the file
// say.
// Whether the area is out of doors: a level of terrain, whose rooms are its
// cells (dao::world::Room's state 6), not a level of rooms. An area's own sun
// says nothing of it: 64 of the 132 layouts with a sun and a fog (counted with
// ghilana, 2026-10-16) include Redcliffe castle's floors, Flemeth's hut and
// Vigil's Keep's basements, where the sun reaches the air only through
// windows, and air lit towards it all about a room is lit by nothing there.
// An area the day and night cycle runs in is out of doors by its file
// (data/areas), whatever its level is made of: Vigil's Keep by day and the
// Wending Wood (Awakening) are levels of rooms, and the clock, waiting for
// the out of doors, never ran there (2026-10-24).
std::wstring AreaTag();

bool OutOfDoors()
{
    constexpr std::int32_t kTerrainCell = 6;
    const std::vector<dao::world::Room> rooms = dao::world::Rooms();
    if (std::ranges::any_of(rooms, [](const dao::world::Room &room) { return room.state == kTerrainCell; }))
    {
        return true;
    }
    const std::wstring area = AreaTag();
    if (!daynight::Cycles(area))
    {
        return false;
    }
    static std::wstring said;
    if (said != area)
    {
        said = area;
        spdlog::info("the area {} is out of doors by its day and night file, its level being of rooms",
                     std::filesystem::path(area).string());
    }
    return true;
}

void KeepAir()
{
    // Whether the area has a sun is its own word: under a day and a night the
    // hour's is none at dusk, and the fills were back on figures there.
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = BaseSky();
    const bool outdoors = OutOfDoors();
    static std::optional<bool> said;
    if (sun && sky && said != outdoors)
    {
        said = outdoors;
        spdlog::info("the area is {}",
                     outdoors ? "out of doors: its air is lit by its sun" : "indoors: its fog as it is");
    }
    g_haze = sun && sky
                 ? HazeOf(*sun, *sky, outdoors, g_hourLight ? g_hourLight->sunOverDay : std::array<float, 3>{1, 1, 1})
                 : SkyHaze{};
    g_outdoors = sun && sky && outdoors;
    const bool sunny = sun && std::max({sun->color[0], sun->color[1], sun->color[2]}) > 0;
    g_sunlit = g_outdoors && sunny;
    const float rule = SunlitFigures() && sunny ? kSunlitFills : 1;
    g_fills = g_outdoors ? std::clamp(Tuned("Fills").value_or(rule), 0.0F, 1.0F) : 1;
    probes::Want(g_outdoors && SunlitFigures() && Tuned("Probes").value_or(1) > 0);
}

// The area's sun: on figures the ground's, with Improved lighting on out of
// doors; and as the tuning file says, on the ground times its `Sun`, on
// figures times its `SunOnFigures`.
// The sun's colour on the ground, for the bounce from the level's sunlit
// patches (Probes.h), each giving back its own colour's share of it; out of
// doors, and as the tuning file's `Bounce` says, a developer's switch.
void KeepSunBounce(const dao::atmosphere::Sunlight &sun)
{
    std::array<float, 3> sent{};
    if (g_outdoors)
    {
        const float bounce = Tuned("Bounce").value_or(1);
        sent = {sun.color[0] * bounce, sun.color[1] * bounce, sun.color[2] * bounce};
    }
    probes::SunBounce(sent);
}

void KeepSun()
{
    const std::optional<dao::atmosphere::Sunlight> own = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sunlight> base = BaseSunlight();
    if (!own || !base)
    {
        return;
    }
    dao::atmosphere::Sunlight sun = *base;
    const float ground = Tuned("Sun").value_or(1);
    const float figures = Tuned("SunOnFigures").value_or(1);
    const bool oneSun = g_outdoors && SunlitFigures();
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        sun.color.at(channel) *= ground;
        sun.characterColor.at(channel) = (oneSun ? sun.color.at(channel) : sun.characterColor.at(channel)) * figures;
    }
    KeepSunBounce(sun);
    levelshadows::Sun({sun.direction[0], sun.direction[1], sun.direction[2]});
    if (sun == dao::atmosphere::CurrentSunlight())
    {
        return;
    }
    if (sun == *own)
    {
        dao::atmosphere::ResetSunlight();
    }
    else
    {
        dao::atmosphere::SetSunlight(sun);
    }
}

void Set(std::wstring_view name, float value)
{
    if (const std::optional<float> has = dao::framebuffer::Value(kChain, name); has && std::abs(*has - value) > 1e-6F)
    {
        dao::framebuffer::SetValue(kChain, name, value);
    }
}

// The camera's turn: the matrix that takes a way in the world into the
// camera's axes (x right, y down the picture, z ahead). Nothing of the
// camera is read but where it is: points far off along twenty-six ways are
// asked their place on the screen (dao::world::ToScreen), which is right
// whatever camera draws (a conversation's, photo cam's), and the turn that
// best takes the ways to where they are drawn is found as the rotation
// nearest their sum (the polar decomposition, by Higham's iteration).
// `focal` is the picture's focal length in pixels.
// The ways about the eye, each times where it is drawn, summed; and how many
// were drawn.
std::pair<Mat3, int> DrawnWays(const dao::Vector &eye, float width, float height, float focal)
{
    constexpr float kFar = 20000.0F; // metres: far enough that where the camera is does not matter
    Mat3 sum{};
    int seen = 0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int z = -1; z <= 1; ++z)
            {
                if (x == 0 && y == 0 && z == 0)
                {
                    continue;
                }
                const float length = std::sqrt(static_cast<float>((x * x) + (y * y) + (z * z)));
                const Vec3 way{static_cast<float>(x) / length, static_cast<float>(y) / length,
                               static_cast<float>(z) / length};
                const std::optional<dao::world::ScreenPoint> point =
                    dao::world::ToScreen({eye.x + (kFar * way[0]), eye.y + (kFar * way[1]), eye.z + (kFar * way[2])});
                if (!point || !point->inFront)
                {
                    continue;
                }
                Vec3 drawn{(point->x - (width / 2)) / focal, (point->y - (height / 2)) / focal, 1};
                const float norm = std::sqrt(Dot(drawn, drawn));
                // A way far to the side is drawn far from the middle, where a
                // pixel's error is a large angle: weighed by how far ahead it is.
                const float weight = 1 / (norm * norm);
                for (std::size_t r = 0; r < 3; ++r)
                {
                    for (std::size_t c = 0; c < 3; ++c)
                    {
                        sum.at(r).at(c) += weight * (drawn.at(r) / norm) * way.at(c);
                    }
                }
                ++seen;
            }
        }
    }
    return {sum, seen};
}

// The rotation nearest `sum` (the polar decomposition, by Higham's
// iteration), or none where it has no inverse.
std::optional<Mat3> Nearest(const Mat3 &sum)
{
    Mat3 turn = sum;
    for (int step = 0; step < 12; ++step)
    {
        // R <- (R + R^-T) / 2; R^-T is the cofactor matrix over the determinant.
        const Mat3 &m = turn;
        const Mat3 cofactor{{
            {(m[1][1] * m[2][2]) - (m[1][2] * m[2][1]), (m[1][2] * m[2][0]) - (m[1][0] * m[2][2]),
             (m[1][0] * m[2][1]) - (m[1][1] * m[2][0])},
            {(m[0][2] * m[2][1]) - (m[0][1] * m[2][2]), (m[0][0] * m[2][2]) - (m[0][2] * m[2][0]),
             (m[0][1] * m[2][0]) - (m[0][0] * m[2][1])},
            {(m[0][1] * m[1][2]) - (m[0][2] * m[1][1]), (m[0][2] * m[1][0]) - (m[0][0] * m[1][2]),
             (m[0][0] * m[1][1]) - (m[0][1] * m[1][0])},
        }};
        const float det = Dot(m[0], cofactor[0]);
        if (std::abs(det) < 1e-12F)
        {
            return std::nullopt;
        }
        Mat3 next{};
        for (std::size_t r = 0; r < 3; ++r)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                next.at(r).at(c) = 0.5F * (m.at(r).at(c) + (cofactor.at(r).at(c) / det));
            }
        }
        turn = next;
    }
    return turn;
}

std::optional<Mat3> CameraTurn(const dao::Vector &eye, float width, float height, float focal)
{
    const auto [sum, seen] = DrawnWays(eye, width, height, focal);
    if (seen < 3)
    {
        return std::nullopt;
    }
    return Nearest(sum);
}

// Hands the grade what the air's light is reckoned by (grade.hlsl's
// DaoHaze), each frame: the camera's way moves the sun about the picture.
void KeepHaze()
{
    const bool hazed = g_haze.on;
    Set(L"DaoHaze", hazed ? 1.0F : 0.0F);
    g_hazeView = {};
    if (!hazed)
    {
        return;
    }
    const std::optional<dao::atmosphere::Sunlight> sun = BaseSunlight();
    const std::optional<dao::atmosphere::Sky> sky = BaseSky();
    const std::optional<dao::world::ViewSize> size = dao::world::ScreenSize();
    const std::optional<float> lens = dao::framebuffer::Value(kChain, L"DaoLensX");
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    const dao::SceneCamera *camera = controller != nullptr ? controller->Scene() : nullptr;
    if (controller != nullptr)
    {
        const dao::Vector focus = controller->Focus();
        levelshadows::Focus(std::array<float, 3>{focus.x, focus.y, focus.z});
    }
    else
    {
        levelshadows::Focus(std::nullopt);
    }
    if (!sun || !sky || !size || !lens || *lens <= 0 || camera == nullptr || size->width <= 0)
    {
        return;
    }
    const auto width = static_cast<float>(size->width);
    const auto height = static_cast<float>(size->height);
    const std::optional<Mat3> turn = CameraTurn(camera->Position(), width, height, width / *lens);
    if (!turn)
    {
        return;
    }
    const Vec3 toSun = Times(*turn, {-sun->direction[0], -sun->direction[1], -sun->direction[2]});
    Set(L"DaoSunX", toSun[0]);
    Set(L"DaoSunY", toSun[1]);
    Set(L"DaoSunZ", toSun[2]);
    // The camera's place and its axes in the world (the turn's rows), for the
    // sun's shadows in the air (haze_pass.hlsl's DaoSunInAir).
    const dao::Vector eye = camera->Position();
    Set(L"DaoEyeX", eye.x);
    Set(L"DaoEyeY", eye.y);
    Set(L"DaoEyeZ", eye.z);
    Set(L"DaoRightX", (*turn)[0][0]);
    Set(L"DaoRightY", (*turn)[0][1]);
    Set(L"DaoRightZ", (*turn)[0][2]);
    Set(L"DaoDownX", (*turn)[1][0]);
    Set(L"DaoDownY", (*turn)[1][1]);
    Set(L"DaoDownZ", (*turn)[1][2]);
    Set(L"DaoAheadX", (*turn)[2][0]);
    Set(L"DaoAheadY", (*turn)[2][1]);
    Set(L"DaoAheadZ", (*turn)[2][2]);
    const std::optional<float> lensY = dao::framebuffer::Value(kChain, L"DaoLensY");
    std::array<float, 20> view{};
    for (std::size_t axis = 0; axis < 3; ++axis)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            view.at((4 * axis) + c) = (*turn).at(axis).at(c);
        }
    }
    view[3] = 1 / width;
    view[7] = 1 / height;
    view[11] = *lens;
    view[12] = -sun->direction[0];
    view[13] = -sun->direction[1];
    view[14] = -sun->direction[2];
    view[15] = lensY.value_or(*lens * height / width);
    const SkyHaze haze = g_haze;
    // The colour the engine fogs surfaces towards, which the pass takes away
    // again where it gives the air's light, and how fast it thickens.
    if (const std::optional<dao::atmosphere::Sky> now = dao::atmosphere::CurrentSky())
    {
        Set(L"DaoFogR", now->fogRed);
        Set(L"DaoFogG", now->fogGreen);
        Set(L"DaoFogB", now->fogBlue);
        Set(L"DaoFogIntensity", now->fogIntensity);
        Set(L"DaoFogCap", now->fogCap);
        Set(L"DaoFogZenith", now->fogZenith);
        view[16] = now->fogCap;
        view[17] = now->fogZenith * std::numbers::pi_v<float> / 180;
        view[18] = now->fogIntensity;
    }
    Set(L"DaoAirR", haze.air[0]);
    Set(L"DaoAirG", haze.air[1]);
    Set(L"DaoAirB", haze.air[2]);
    Set(L"DaoSunR", haze.sun[0]);
    Set(L"DaoSunG", haze.sun[1]);
    Set(L"DaoSunB", haze.sun[2]);
    Set(L"DaoHazeG", haze.g);
    Set(L"DaoHazeShow", Tuned("HazeShow").value_or(0));
    view[19] = 1;
    g_hazeView = view;
}

} // namespace

float Tune(std::string_view key, float otherwise)
{
    return Tuned(key).value_or(otherwise);
}

SkyHaze Haze()
{
    return g_haze;
}

std::array<float, 20> HazeView()
{
    return g_hazeView;
}

float Fills()
{
    return g_fills;
}

void WantSunlitFigures(bool on)
{
    if (on != g_wantedSunlit)
    {
        g_wantedSunlit = on;
        KeepAir();
        KeepSun();
    }
}

bool Sunlit()
{
    return g_sunlit;
}

void WantClock(bool on)
{
    g_clockOn = on;
}

void Want(bool on)
{
    if (on != g_wanted)
    {
        g_wanted = on;
        KeepAir();
    }
}

// Whether the scene was out of doors at the last look (OutOfDoors), how many
// times that has changed, and whether the scene has just changed (the game's
// mode, an area loaded: SceneChanged), so that the next frame looks at once.
// The party picker is a scene of its own, with no terrain, opened over a camp
// out of doors, and for the half second to the next look its figures were lit
// as the camp's, by the moon with no fills, near black, and came up in two
// steps as the looks caught up (seen 2026-10-17; the user: "at first
// everything looks dark before the lighting is applied").
namespace
{
std::optional<bool> g_seenOutdoors;
std::atomic<int> g_changes{0};
bool g_lookNow = false;

// How many minutes of play a day of the cycle takes (the user, 2026-10-21:
// "should be 1 hour = 1 day"), where the tuning file's `DayMinutes` does not
// say.
constexpr float kDayMinutes = 60;
// How bright the moon is, as a share of the night level's sun's (DayNight.h's
// Now): the night level's own.
constexpr float kNight = 1;

// The player's area's tag, asked of the scripts.
std::wstring AreaTag()
{
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    dao::CCModule *module = app != nullptr ? app->GetModule() : nullptr;
    dao::CCArea *area = module != nullptr ? module->CurrentArea() : nullptr;
    return area != nullptr ? dao::script::api::GetTag(area->Id()) : std::wstring();
}

// The light of the hour, each frame, where the clock runs in the area
// (DayNight.h) and the options screen's Day / night cycle runs it: out of
// doors with Improved lighting on.
void KeepHour()
{
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = dao::atmosphere::AreaOwnSky();
    const bool running = sun && sky && g_wanted && g_outdoors && g_clockOn;
    const bool had = g_hourLight.has_value();
    daynight::TryMoon(Tuned("MoonTurn"), Tuned("MoonHeight"));
    daynight::TryNightColour(Tuned("NightColour"));
    g_hourLight = sun && sky ? daynight::Now(AreaTag(), *sun, *sky, running, Tuned("DayMinutes").value_or(kDayMinutes),
                                             Tuned("Night").value_or(kNight), Tuned("NightFill"), Tuned("NightHaze"))
                             : std::nullopt;
    if (had != g_hourLight.has_value())
    {
        spdlog::info("day and night: {}", g_hourLight ? "the hour's light" : "the area's own light");
    }
}
} // namespace

void Poll()
{
    KeepHour();
    // The hour's light moves each frame: its air, its sky (the sky's disc is
    // placed as the sky is set) and its sun with it.
    if (g_hourLight)
    {
        KeepAir();
        KeepSky();
        KeepSun();
    }
    KeepHaze();
    if (++g_frames % kEvery != 0 && !g_lookNow)
    {
        return;
    }
    const bool asked = g_lookNow;
    g_lookNow = false;
    WatchArea();
    KeepTune();
    KeepAir();
    KeepSky();
    KeepSun();
    const bool outdoors = OutOfDoors();
    if (asked || g_seenOutdoors != outdoors)
    {
        g_seenOutdoors = outdoors;
        g_changes.fetch_add(1, std::memory_order_relaxed);
    }
}

void SceneChanged()
{
    g_lookNow = true;
}

int Changes()
{
    return g_changes.load(std::memory_order_relaxed);
}

} // namespace outdoors
