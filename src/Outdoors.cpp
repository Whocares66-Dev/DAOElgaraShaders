#include "Outdoors.h"

#include "DayNight.h"
#include "LevelShadows.h"

#include "Probes.h"

#include "dao/Atmosphere.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
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
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace outdoors
{

namespace
{

constexpr std::string_view kTuneFile = "outdoors.tune";
// Frames between looks at the area and the file: half a second or so.
constexpr int kEvery = 30;

int g_frames = 0;

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

// Whether the player wants Improved lighting (the options screen's).
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

// Whether the area the party is in is out of doors and sunlit, and how strong
// its fills are on figures, as the option and the file say.
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
        spdlog::info("the area is {}", outdoors ? "out of doors" : "indoors");
    }
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

// Tells the level's shadows what the camera looks at, each frame: the maps
// are fitted around it (levelshadows::Focus).
void KeepFocus()
{
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    if (controller == nullptr)
    {
        levelshadows::Focus(std::nullopt);
        return;
    }
    const dao::Vector focus = controller->Focus();
    levelshadows::Focus(std::array<float, 3>{focus.x, focus.y, focus.z});
}

} // namespace

float Tune(std::string_view key, float otherwise)
{
    return Tuned(key).value_or(otherwise);
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
    daynight::TryFogMoon(Tuned("FogMoon"));
    daynight::TryDayHaze(Tuned("DawnHaze"), Tuned("DuskHaze"));
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
    KeepFocus();
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
