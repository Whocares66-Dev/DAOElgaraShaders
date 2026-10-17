#include "Outdoors.h"

#include "dao/Atmosphere.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/Framebuffer.h"
#include "dao/GameModes.h"
#include "dao/Module.h"
#include "dao/World.h"
#include "dao/core/Sky.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
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
bool g_tuned = false; // something of the file's is on the area now

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

// Whether the player wants the air lit (the options screen's Sunlit haze).
bool g_wanted = false;

SkyHaze g_haze;
float g_fills = 1;

// Sunlit figures: out of doors under a sun or a moon, a figure is lit as the
// ground it stands on is, by the one sun in the ground's colour and strength
// and the area's ambient light, and the area's fills, three far lights only
// figures have, are taken away (the user, 2026-10-16: "One sun, one color,
// one strength. We should also try getting rid of fill lights and just using
// sun directly"). An area gives figures a sun of their own: 0.89 times the
// ground's in Redcliffe, 1.43 in the Dalish camp, pale yellow on figures and
// deep orange on the ground in the Korcari Wilds, gray and cyan in Haven.
// (Two rules before this one kept the fills at a half, with the sun on figures
// at a fixed 1.3, which left figures 13 to 29 in 100 darker where the sun is
// weak, or given what the fills lost.)
constexpr float kSunlitFills = 0;
bool g_wantedSunlit = false;

// Whether Sunlit figures is on: the player's word, or the tuning file's
// `SunlitFigures`, to compare without the options screen.
bool SunlitFigures()
{
    return Tuned("SunlitFigures").value_or(g_wantedSunlit ? 1 : 0) > 0;
}
bool g_outdoors = false;

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
SkyHaze HazeOf(const dao::atmosphere::Sunlight &sun, const dao::atmosphere::Sky &sky, bool outdoors)
{
    SkyHaze haze;
    const bool sunny = std::max({sun.color[0], sun.color[1], sun.color[2]}) > 0;
    haze.on = outdoors && sunny && g_wanted && Tuned("Haze").value_or(1) > 0;
    constexpr std::array<float, 3> kRayleigh{6.95e-6F, 1.18e-5F, 2.44e-5F};               // per metre
    constexpr std::array<double, 3> kInverseWavelength{1538461.5, 1754386.0, 2105263.25}; // per metre
    constexpr double kPi = std::numbers::pi;
    // Preetham's concentration factor, as the engine has it.
    constexpr double kConcentration = 0.434; // NOLINT(modernize-use-std-numbers): not log10(e)
    const double c = std::max((6.544 * sky.turbidity) - 6.51, 0.0) * 1e-17;
    const std::array<float, 3> fog{sky.fogRed, sky.fogGreen, sky.fogBlue};
    for (std::size_t i = 0; i < 3; ++i)
    {
        const double rayleigh = std::max(sky.rayleighMultiplier, 0.0F) * kRayleigh.at(i);
        const double mie = std::max(sky.mieMultiplier, 0.0F) * 0.67 * kConcentration * 4 * kPi * kPi * kPi * c *
                           kInverseWavelength.at(i) * kInverseWavelength.at(i);
        const double mieShare = rayleigh + mie > 0 ? mie / (rayleigh + mie) : 0;
        haze.air.at(i) = static_cast<float>((1 - mieShare) * fog.at(i));
        haze.sun.at(i) = static_cast<float>(mieShare * fog.at(i));
    }
    haze.g = std::min(std::abs(sky.hg), 0.95F);
    return haze;
}

// The file's sun and sky put on the area, for a developer's trying of them.
void ApplyTune()
{
    const std::optional<dao::atmosphere::Sunlight> ownSun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> ownSky = dao::atmosphere::AreaOwnSky();
    if (!ownSun || !ownSky)
    {
        return;
    }
    dao::atmosphere::Sky sky = *ownSky;
    for (const dao::core::SkyField &field : dao::core::kSkyFields)
    {
        if (const std::optional<float> value = Tuned("sky." + Underscored(field.name)))
        {
            sky.*field.member = *value;
        }
    }
    if (sky != dao::atmosphere::CurrentSky())
    {
        dao::atmosphere::SetSky(sky);
    }
    g_tuned = true;
}

void KeepTune()
{
    const std::filesystem::path file = dao::Module::OwnFolder() / kTuneFile;
    std::error_code error;
    if (!std::filesystem::exists(file, error))
    {
        if (g_tuned)
        {
            dao::atmosphere::ResetSky();
            g_tuned = false;
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
    ApplyTune();
}

// The air's light for the area the party is in, as the option and the file
// say.
// Whether the area is out of doors: a level of terrain, whose rooms are its
// cells (dao::world::Room's state 6), not a level of rooms. An area's own sun
// says nothing of it: 64 of the 132 layouts with a sun and a fog (counted with
// ghilana, 2026-10-16) include Redcliffe castle's floors, Flemeth's hut and
// Vigil's Keep's basements, where the sun reaches the air only through
// windows, and air lit towards it all about a room is lit by nothing there.
bool OutOfDoors()
{
    constexpr std::int32_t kTerrainCell = 6;
    const std::vector<dao::world::Room> rooms = dao::world::Rooms();
    return std::ranges::any_of(rooms, [](const dao::world::Room &room) { return room.state == kTerrainCell; });
}

void KeepAir()
{
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = dao::atmosphere::AreaOwnSky();
    const bool outdoors = OutOfDoors();
    static std::optional<bool> said;
    if (sun && sky && said != outdoors)
    {
        said = outdoors;
        spdlog::info("the area is {}",
                     outdoors ? "out of doors: its air is lit by its sun" : "indoors: its fog as it is");
    }
    g_haze = sun && sky ? HazeOf(*sun, *sky, outdoors) : SkyHaze{};
    g_outdoors = sun && sky && outdoors;
    const bool sunny = sun && std::max({sun->color[0], sun->color[1], sun->color[2]}) > 0;
    const float rule = SunlitFigures() && sunny ? kSunlitFills : 1;
    g_fills = g_outdoors ? std::clamp(Tuned("Fills").value_or(rule), 0.0F, 1.0F) : 1;
}

// The area's sun: on figures the ground's, with Sunlit figures on out of
// doors; and as the tuning file says, on the ground times its `Sun`, on
// figures times its `SunOnFigures`.
void KeepSun()
{
    const std::optional<dao::atmosphere::Sunlight> own = dao::atmosphere::AreaOwnSunlight();
    if (!own)
    {
        return;
    }
    dao::atmosphere::Sunlight sun = *own;
    const float ground = Tuned("Sun").value_or(1);
    const float figures = Tuned("SunOnFigures").value_or(1);
    const bool oneSun = g_outdoors && SunlitFigures();
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        sun.characterColor.at(channel) = (oneSun ? sun.color.at(channel) : sun.characterColor.at(channel)) * figures;
        sun.color.at(channel) *= ground;
    }
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
    if (!hazed)
    {
        return;
    }
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = dao::atmosphere::AreaOwnSky();
    const std::optional<dao::world::ViewSize> size = dao::world::ScreenSize();
    const std::optional<float> lens = dao::framebuffer::Value(kChain, L"DaoLensX");
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    const dao::SceneCamera *camera = controller != nullptr ? controller->Scene() : nullptr;
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
    const SkyHaze haze = g_haze;
    // The colour the engine fogs surfaces towards, which the pass takes away
    // again where it gives the air's light.
    if (const std::optional<dao::atmosphere::Sky> now = dao::atmosphere::CurrentSky())
    {
        Set(L"DaoFogR", now->fogRed);
        Set(L"DaoFogG", now->fogGreen);
        Set(L"DaoFogB", now->fogBlue);
    }
    Set(L"DaoAirR", haze.air[0]);
    Set(L"DaoAirG", haze.air[1]);
    Set(L"DaoAirB", haze.air[2]);
    Set(L"DaoSunR", haze.sun[0]);
    Set(L"DaoSunG", haze.sun[1]);
    Set(L"DaoSunB", haze.sun[2]);
    Set(L"DaoHazeG", haze.g);
    Set(L"DaoHazeShow", Tuned("HazeShow").value_or(0));
}

} // namespace

SkyHaze Haze()
{
    return g_haze;
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

void Want(bool on)
{
    if (on != g_wanted)
    {
        g_wanted = on;
        KeepAir();
    }
}

void Poll()
{
    KeepHaze();
    if (++g_frames % kEvery != 0)
    {
        return;
    }
    WatchArea();
    KeepTune();
    KeepAir();
    KeepSun();
}

} // namespace outdoors
