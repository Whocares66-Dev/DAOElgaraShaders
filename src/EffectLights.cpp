#include "EffectLights.h"

#include "DayNight.h"
#include "Glowing.h"
#include "LevelFiles.h"
#include "LevelShadows.h"

#include "dao/Addresses.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/GameModes.h"
#include "dao/Hook.h"
#include "dao/Lights.h"
#include "dao/Module.h"
#include "dao/Objects.h"
#include "dao/Vfx.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace effectlights
{
namespace
{

// The registers (effect_lights.hlsl): two a light, then the count.
constexpr UINT kLightsRegister = 160;
constexpr std::size_t kMostLights = 16;
constexpr UINT kCountRegister = kLightsRegister + (2 * kMostLights);
// The registers' floats: two registers a light and one for the count, four floats each.
constexpr std::size_t kRegisterFloats = ((2 * kMostLights) + 1) * 4;

// The light manager's lists of the lights that have no flag in their
// blueprint, the figures' and the level's (DAOLib's notes on
// kLightManager_AddLights), the most an object takes of a list (+4 of it,
// 0x00461F10), and the scene a manager is of (+8; its base's constructor,
// 0x0046C4F0).
constexpr std::size_t kFiguresUnflagged = 0x84;
constexpr std::size_t kLevelsUnflagged = 0xC;
constexpr std::size_t kListMost = 4;
constexpr std::size_t kManagerScene = 0x8;

// A point light's node as LightState::Fill (0x0046B260) makes what a shader
// is handed of it, read 2026-10-17: its place, the translation of the
// matrix at +0x60 (the same as its light's own answer, logged); its reach,
// 0x0053C870's; its colour, 0x0053CD90's: its own at +0x1A0, times its
// owner's tint (the owner at +0x154, the tint at +0x2D4) where its byte at
// +0x196 says so; times +0x1B4 and +0x1BC (the flicker). Fill also takes in
// a fade of the object's own choice (0x0046A470), which an effect's light
// lit on everything has no part in.
constexpr std::size_t kNodePlaceX = 0x6C;
constexpr std::size_t kNodePlaceY = 0x7C;
constexpr std::size_t kNodePlaceZ = 0x8C;
constexpr std::size_t kNodeReach = 0x1B0;
constexpr std::size_t kNodeColour = 0x1A0;
constexpr std::size_t kNodeTinted = 0x196;
constexpr std::size_t kNodeOwner = 0x154;
constexpr std::size_t kOwnerTint = 0x2D4;
constexpr std::size_t kNodeStrength = 0x1B4;
constexpr std::size_t kNodeFlicker = 0x1BC;

using ManagerLightsFn = std::uintptr_t(__fastcall *)(void *, int, void *);
using ListFn = std::uintptr_t(__fastcall *)(void *, int, void *);
using ManagerChangedFn = void(__fastcall *)(void *, int);
dao::Detour<ManagerLightsFn> g_managerAdd;
dao::Detour<ManagerLightsFn> g_managerRemove;
dao::Detour<ListFn> g_listInsert;
dao::Detour<ListFn> g_listErase;

thread_local void *t_manager = nullptr; // the manager taking lights in or letting them go, on this thread

std::atomic<bool> g_wanted{false};

// The level's unflagged lights, each with its manager, and each manager's
// own most for the two lists, kept to be given back. In the order of their
// addresses, so that going through them is the same each time.
std::mutex g_lock;
std::map<void *, void *> g_lights;
std::map<void *, std::array<std::uint32_t, 2>> g_ownMost;

// The lights as last read, for the render thread, and as last handed.
std::mutex g_readLock;
std::array<float, kRegisterFloats> g_registers{};
// The lights the scene took as it first drew into the picture, and whether it
// has: the render thread's alone.
std::array<float, kRegisterFloats> g_sceneRegisters{};
bool g_sceneTook = false;
std::vector<std::array<float, 4>> g_handed;

// The light of a fire whose model has none: Redcliffe's pyre is
// fxe_fire_m_ns_p, played by a script on a placeable, and lit nothing at
// night (the user, 2026-10-22), its glow by day being the level's bake. Its
// lit twin, fxe_fire_m_d_p, has a point light, which the engine lights the
// scene with; data/effects.ini gives a lightless fire the light of its twin,
// by hand: `<model> = glow <red> <green> <blue> <reach> <height> ; ...`.
struct FireLight
{
    std::array<float, 3> colour{};
    float reach = 0;
    float height = 0; // above the effect's place, its owner's origin
};
std::unordered_map<std::wstring, FireLight> g_fires; // by model name, lower case; read once, at Install

// The effects that scatter the light about them (water, smoke, dust: LIT in
// data/effects.ini), which take the sky's light as the hour has it. Their
// sprites are drawn in their own colour, lit by nothing, and Redcliffe's
// waterfalls shone white at midnight (the user, 2026-10-22: "Water is still
// too bright"). By model name, lower case; read once, at Install.
std::unordered_set<std::wstring> g_lit;

std::wstring Lower(std::wstring_view text)
{
    std::wstring lower(text);
    for (wchar_t &c : lower)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return lower;
}

// The table's fires with a light and its LIT effects.
std::unordered_map<std::wstring, FireLight> ReadTable(const std::filesystem::path &file,
                                                      std::unordered_set<std::wstring> &lit)
{
    std::unordered_map<std::wstring, FireLight> fires;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == ';' || equals == std::string::npos)
        {
            continue;
        }
        std::istringstream value(line.substr(equals + 1, line.find(';') - equals - 1));
        std::string name = line.substr(0, equals);
        std::erase_if(name, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        const std::wstring model = Lower(std::wstring(name.begin(), name.end()));
        std::string kind;
        FireLight fire;
        value >> kind;
        if (kind == "lit")
        {
            lit.insert(model);
        }
        else if (kind == "glow" &&
                 value >> fire.colour[0] >> fire.colour[1] >> fire.colour[2] >> fire.reach >> fire.height &&
                 fire.reach > 0)
        {
            fires[model] = fire;
        }
    }
    return fires;
}

float FloatAt(const void *base, std::size_t offset)
{
    float value = 0;
    std::memcpy(&value, static_cast<const std::byte *>(base) + offset, sizeof value);
    return value;
}

std::uint32_t &MostOf(void *manager, std::size_t list)
{
    return *reinterpret_cast<std::uint32_t *>(static_cast<std::byte *>(manager) + list + kListMost);
}

// The manager's most for its unflagged lists as the plugin wants it now,
// called while the manager is taking lights in, so alive. True if changed.
bool KeepMost(void *manager)
{
    const std::scoped_lock lock(g_lock);
    const bool wanted = g_wanted.load(std::memory_order_relaxed);
    auto own = g_ownMost.find(manager);
    if (wanted && own == g_ownMost.end())
    {
        g_ownMost[manager] = {MostOf(manager, kFiguresUnflagged), MostOf(manager, kLevelsUnflagged)};
        MostOf(manager, kFiguresUnflagged) = 0;
        MostOf(manager, kLevelsUnflagged) = 0;
        return true;
    }
    if (!wanted && own != g_ownMost.end())
    {
        MostOf(manager, kFiguresUnflagged) = own->second[0];
        MostOf(manager, kLevelsUnflagged) = own->second[1];
        g_ownMost.erase(own);
        return true;
    }
    return false;
}

std::uintptr_t __fastcall ManagerAdd(void *manager, int unused, void *object)
{
    const bool changed = KeepMost(manager);
    t_manager = manager;
    const std::uintptr_t result = g_managerAdd.Original()(manager, unused, object);
    t_manager = nullptr;
    if (changed)
    {
        // Every object chooses its lights again, as when the manager's lists change.
        reinterpret_cast<ManagerChangedFn>(dao::Module::Address(dao::addr::kLightManager_MarkChanged))(manager, 0);
    }
    return result;
}

std::uintptr_t __fastcall ManagerRemove(void *manager, int unused, void *object)
{
    t_manager = manager;
    const std::uintptr_t result = g_managerRemove.Original()(manager, unused, object);
    t_manager = nullptr;
    return result;
}

std::uintptr_t __fastcall ListInsert(void *list, int unused, void *node)
{
    const std::uintptr_t result = g_listInsert.Original()(list, unused, node);
    if (t_manager != nullptr && node != nullptr && list == static_cast<std::byte *>(t_manager) + kLevelsUnflagged)
    {
        const std::scoped_lock lock(g_lock);
        g_lights.insert_or_assign(node, t_manager);
    }
    return result;
}

std::uintptr_t __fastcall ListErase(void *list, int unused, void *node)
{
    if (node != nullptr)
    {
        const std::scoped_lock lock(g_lock);
        g_lights.erase(node);
    }
    return g_listErase.Original()(list, unused, node);
}

// What a shader is handed of a light: its place and reach, its colour.
struct Light
{
    std::array<float, 4> place{};
    std::array<float, 4> colour{};
};

Light LightOf(const void *node)
{
    Light light;
    light.place = {FloatAt(node, kNodePlaceX), FloatAt(node, kNodePlaceY), FloatAt(node, kNodePlaceZ),
                   FloatAt(node, kNodeReach)};
    std::array<float, 4> colour{};
    std::memcpy(colour.data(), static_cast<const std::byte *>(node) + kNodeColour, sizeof colour);
    if (*(static_cast<const std::byte *>(node) + kNodeTinted) != std::byte{0})
    {
        const void *owner = nullptr;
        std::memcpy(static_cast<void *>(&owner), static_cast<const std::byte *>(node) + kNodeOwner, sizeof owner);
        if (owner != nullptr)
        {
            for (std::size_t i = 0; i < colour.size(); ++i)
            {
                colour.at(i) *= FloatAt(owner, kOwnerTint + (i * sizeof(float)));
            }
        }
    }
    const float strength = FloatAt(node, kNodeStrength) * FloatAt(node, kNodeFlicker);
    light.colour = {colour[0] * strength, colour[1] * strength, colour[2] * strength, colour[3]};
    return light;
}

// The level's lamps named as fires (daynight::FireLamp), lit by the plugin,
// each pixel, as an effect's light is, in place of the engine's. The engine
// gives a level's lamps to the objects it chooses them for and reckons them
// by the vertex, inside the sum the sun's shadow is laid on: Lothering's
// bonfire (fire_big_001_RT, 12 m) lit the ground in a pool cut off where a
// hill's shadow from the moon lay and at the meshes it was not chosen for
// (the user, 2026-10-23: "fire light seems to stop on some textures?", "the
// fire should be overriding the shadows"). They are read from the level's
// layouts as it is opened, every one: taken from the engine as it handed
// them, Redcliffe castle's torches were lit only once near, the engine
// handing a lamp for the objects about the camera alone ("torches don't
// emit light from a distance ... but they do when closer").
std::atomic<bool> g_lampsWanted{false};
// How bright fires and lamps are lit, a share of their own light (FireStrength).
std::atomic<float> g_fireShare{1};
std::atomic<bool> g_culled{true};
std::mutex g_lampLock;
// Each level's, by its name, and the level whose layout lights the engine
// last handed: the one the player is in. Under g_lampLock.
struct LevelFires;
std::map<std::wstring, std::shared_ptr<const LevelFires>> g_levelLamps;
std::wstring g_lampLevel;
std::vector<std::future<void>> g_lampReads; // the loading thread's

// How near a lamp must be to the place an area's file names for it, metres:
// a lamp is named by where the level puts it, read to a tenth.
constexpr float kLampMatch = 0.5F;
// How near a flame must be to where a lamp is moved for the lamp to light it:
// the lamp is put a little out of its lantern (Lothering's, half a metre).
constexpr float kLampNear = 1.0F;

bool Near(const std::array<float, 3> &a, const std::array<float, 3> &b, float within)
{
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return (x * x) + (y * y) + (z * z) < within * within;
}

// How near a layout's lamp must be to a fire its room places to be the
// fire's light, metres: Haven's mountain top's light_619, warm, 5 m, is its
// temple gate's torch's.
constexpr float kAtFire = 2.0F;

// The fires a room places that effects.ini gives a light, each where its
// light is, above its place.
std::vector<Light> PlacedFires(levelfiles::Level &level, const std::wstring &id)
{
    std::vector<Light> fires;
    const levelfiles::level::Room *room = level.RoomOf(id);
    if (room == nullptr)
    {
        return fires;
    }
    for (const levelfiles::level::Placed &placed : room->models)
    {
        const auto fire = g_fires.find(Lower(placed.file));
        if (fire == g_fires.end())
        {
            continue;
        }
        const FireLight &f = fire->second;
        const levelfiles::core::Point at = levelfiles::level::InLevel(*room, placed, {0, 0, 0});
        fires.push_back({{at[0], at[1], at[2] + f.height, f.reach}, {f.colour[0], f.colour[1], f.colour[2], 1}});
    }
    return fires;
}

// A level's fires, lit by the plugin from the moment the level opens: the
// engine runs a room's effects only near the camera, and Haven's mountain
// top's torches at the temple's gate went dark at night from across the
// plateau (the user, 2026-10-23: "at 21:00 we cannot [see them] until we
// move closer").
struct LevelFires
{
    std::vector<Light> lights;
    // The layouts' lamps that are a fire's light though not named as one,
    // "<layout>/<name>", lower case: the engine's own copies are put out.
    std::vector<std::wstring> atFires;
    std::size_t glowing = 0; // of `lights`, the glowing ground's (Glowing.h)
};

// The lamps of every layout of a level that are fires' (named as fires, or
// at a fire the room places), each fire with none lit as effects.ini says,
// in the world.
LevelFires ReadLamps(const std::filesystem::path &levelRim)
{
    LevelFires read;
    std::vector<Light> &lamps = read.lights;
    std::optional<levelfiles::Archive> rooms = levelfiles::Archive::Open(levelRim);
    if (!rooms)
    {
        return read;
    }
    const std::wstring name = Lower(levelRim.stem().wstring());
    levelfiles::Level level(levelRim.parent_path(), name, std::move(*rooms), nullptr, nullptr);
    for (const std::wstring &room : level.Rooms())
    {
        std::wstring layout = name;
        layout += L"_";
        layout += room;
        layout += L"_ll";
        std::vector<Light> fires = PlacedFires(level, room);
        for (const levelfiles::Lamp &lamp : level.LampsOf(room))
        {
            const std::array<float, 3> at{lamp.at[0], lamp.at[1], lamp.at[2]};
            const auto fire = std::ranges::find_if(fires, [&](const Light &light) {
                return Near({light.place[0], light.place[1], light.place[2]}, at, kAtFire);
            });
            if (lamp.reach <= 0 || (!daynight::FireLamp(layout, lamp.name) && fire == fires.end()))
            {
                continue;
            }
            // The makers' own light for the fire, not effects.ini's.
            if (fire != fires.end())
            {
                fires.erase(fire);
                if (!daynight::FireLamp(layout, lamp.name))
                {
                    read.atFires.push_back(layout + L"/" + Lower(lamp.name));
                }
            }
            // Two lamps at one place are one light: Redcliffe castle's torches
            // are each a torch_baked_ and a torch_animated_ (lak200d), and as
            // two they took two of the shaders' sixteen, the farther torches
            // left out.
            const auto same = std::ranges::find_if(lamps, [&](const Light &light) {
                return Near({light.place[0], light.place[1], light.place[2]}, at, kLampMatch);
            });
            if (same != lamps.end())
            {
                for (std::size_t c = 0; c < 3; ++c)
                {
                    same->colour.at(c) += lamp.colour.at(c);
                }
                same->place[3] = std::max(same->place[3], lamp.reach);
                continue;
            }
            lamps.push_back({{at[0], at[1], at[2], lamp.reach}, {lamp.colour[0], lamp.colour[1], lamp.colour[2], 1}});
        }
        lamps.insert(lamps.end(), fires.begin(), fires.end());
    }
    // And the light of the ground that glows, lava's (Glowing.h).
    for (const glowing::Light &glow : glowing::LightsOf(level))
    {
        lamps.push_back(
            {{glow.at[0], glow.at[1], glow.at[2], glow.reach}, {glow.colour[0], glow.colour[1], glow.colour[2], 1}});
        ++read.glowing;
    }
    return read;
}

// A level's lamps read and kept, off the loading thread.
void KeepLamps(const std::filesystem::path &levelRim, const std::wstring &level)
{
    auto lamps = std::make_shared<const LevelFires>(ReadLamps(levelRim));
    spdlog::info("effects' lights: {} fires in {}'s layouts and rooms, {} of the layouts' lamps at a fire, "
                 "{} of glowing ground",
                 lamps->lights.size(), std::filesystem::path(level).string(), lamps->atFires.size(), lamps->glowing);
    const std::scoped_lock lock(g_lampLock);
    g_levelLamps[level] = std::move(lamps);
}

// The lamps of the level the player is in added to `lights`, each where the
// area's file moves it.
void AddLamps(std::vector<Light> &lights)
{
    if (!g_lampsWanted.load(std::memory_order_relaxed))
    {
        return;
    }
    std::shared_ptr<const LevelFires> lamps;
    {
        const std::scoped_lock lock(g_lampLock);
        if (const auto found = g_levelLamps.find(g_lampLevel); found != g_levelLamps.end())
        {
            lamps = found->second;
        }
    }
    if (!lamps)
    {
        return;
    }
    const std::vector<daynight::LampMove> moves = daynight::LampMoves();
    for (Light lamp : lamps->lights)
    {
        // Where the area's file moves it: Lothering's Dane's Refuge lamp is
        // put 3.2 m from its lantern, and lit each pixel its pool lay beside
        // the lantern (the user, 2026-10-23: "lamp light seems off center").
        for (const daynight::LampMove &move : moves)
        {
            if (Near({lamp.place[0], lamp.place[1], lamp.place[2]}, move.from, kLampMatch))
            {
                lamp.place = {move.to[0], move.to[1], move.to[2], lamp.place[3]};
            }
        }
        for (std::size_t c = 0; c < 3; ++c)
        {
            lamp.colour.at(c) *= g_fireShare.load(std::memory_order_relaxed);
        }
        lights.push_back(lamp);
    }
}

// The lightless fires effects.ini gives a light, each at its effect's place;
// `engines`: how many of `lights` are the engine's, before the level's own.
void AddFires(std::vector<Light> &lights, std::size_t engines)
{
    if (g_fires.empty())
    {
        return;
    }
    const std::vector<daynight::LampMove> moves = daynight::LampMoves();
    // The level's lamps and fires (AddLamps), after the engine's lights.
    const std::vector<Light> levelLit(lights.begin() + static_cast<std::ptrdiff_t>(engines), lights.end());
    for (const dao::vfx::RunningEffect &effect : dao::vfx::RunningEffects())
    {
        if (effect.modelHasLight || !effect.position)
        {
            continue;
        }
        // A flame a lamp is moved onto is lit by the lamp: one light, the
        // level makers'.
        const std::array<float, 3> at{effect.position->x, effect.position->y, effect.position->z};
        if (std::ranges::any_of(moves, [&](const daynight::LampMove &move) { return Near(at, move.to, kLampNear); }))
        {
            continue;
        }
        const auto fire = g_fires.find(Lower(effect.model));
        if (fire == g_fires.end())
        {
            continue;
        }
        const FireLight &f = fire->second;
        // A fire the level places is lit already, as the level opens
        // (AddPlacedFires): one light, not two.
        const std::array<float, 3> lit{at[0], at[1], at[2] + f.height};
        if (std::ranges::any_of(levelLit, [&](const Light &light) {
                return Near({light.place[0], light.place[1], light.place[2]}, lit, kLampMatch);
            }))
        {
            continue;
        }
        const float share = g_fireShare.load(std::memory_order_relaxed);
        lights.push_back(
            {{lit[0], lit[1], lit[2], f.reach}, {f.colour[0] * share, f.colour[1] * share, f.colour[2] * share, 1}});
    }
}

// The scene the player's area is drawn as, and where the camera is.
void *AreasScene()
{
    dao::CClientExoApp *app = dao::CClientExoApp::Get();
    dao::CCModule *module = app != nullptr ? app->GetModule() : nullptr;
    dao::CCArea *area = module != nullptr ? module->CurrentArea() : nullptr;
    return area != nullptr ? area->Scene() : nullptr;
}

std::array<float, 3> Eye()
{
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    const dao::SceneCamera *camera = controller != nullptr ? controller->Scene() : nullptr;
    if (camera == nullptr)
    {
        return {};
    }
    const dao::Vector eye = camera->Position();
    return {eye.x, eye.y, eye.z};
}

// Whether a light's reach, a ball about its place, lies wholly outside the
// camera's view: beyond one of the six planes of its clip space (Direct3D's:
// x and y within w, z from 0 to w), each a sum of the matrix's rows, measured
// against the ball's radius.
bool OffScreen(const std::array<float, 16> &m, const std::array<float, 4> &ball)
{
    const auto row = [&](std::size_t r, std::size_t c) { return m.at((r * 4) + c); };
    for (int plane = 0; plane < 6; ++plane)
    {
        std::array<float, 4> p{};
        for (std::size_t c = 0; c < 4; ++c)
        {
            const float w = row(3, c);
            switch (plane)
            {
            case 0:
                p.at(c) = w + row(0, c);
                break;
            case 1:
                p.at(c) = w - row(0, c);
                break;
            case 2:
                p.at(c) = w + row(1, c);
                break;
            case 3:
                p.at(c) = w - row(1, c);
                break;
            case 4:
                p.at(c) = row(2, c);
                break;
            default:
                p.at(c) = w - row(2, c);
                break;
            }
        }
        const float length = std::sqrt((p[0] * p[0]) + (p[1] * p[1]) + (p[2] * p[2]));
        if (length > 0 && (p[0] * ball[0]) + (p[1] * ball[1]) + (p[2] * ball[2]) + p[3] < -ball[3] * length)
        {
            return true;
        }
    }
    return false;
}

// How many lights the view left of those there were, said once a scene, a
// second after it is first seen to have any (a developer's).
void NoteChoice(std::size_t had, std::size_t kept)
{
    static void *said = nullptr;
    static std::optional<std::chrono::steady_clock::time_point> first;
    void *scene = AreasScene();
    if (scene == said || had == 0)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!first)
    {
        first = now;
        return;
    }
    if (now - *first < std::chrono::seconds(1))
    {
        return;
    }
    spdlog::info("effects' lights: of {}, {} could light the view, {} out of it", had, kept, had - kept);
    said = scene;
    first.reset();
}

} // namespace

bool Install()
{
    if (!g_managerAdd.Attach(dao::addr::kLightManager_AddLights, &ManagerAdd) ||
        !g_managerRemove.Attach(dao::addr::kLightManager_RemoveLights, &ManagerRemove) ||
        !g_listInsert.Attach(dao::addr::kLightList_Insert, &ListInsert) ||
        !g_listErase.Attach(dao::addr::kLightList_Erase, &ListErase))
    {
        spdlog::warn("effects' lights: a detour could not be placed; the engine chooses them as it does");
        return false;
    }
    g_fires = ReadTable(dao::Module::OwnFolder() / "effects.ini", g_lit);
    spdlog::info("effects' lights: {} lightless fires given a light and {} effects lit by the sky, by effects.ini",
                 g_fires.size(), g_lit.size());
    dao::lights::OnPointLight([](dao::lights::PointLight &light) {
        if (!g_wanted.load(std::memory_order_relaxed) || !g_lampsWanted.load(std::memory_order_relaxed))
        {
            return;
        }
        // A room's layout (<level>_<x>_<y>_<d>_ll): the level the player is in.
        const std::wstring model = Lower(light.model);
        if (const std::size_t end = model.find(L'_'); end != std::wstring::npos && model.ends_with(L"_ll"))
        {
            const std::scoped_lock lock(g_lampLock);
            if (model.substr(0, end) != g_lampLevel)
            {
                g_lampLevel = model.substr(0, end);
                const auto found = g_levelLamps.find(g_lampLevel);
                spdlog::info("effects' lights: in {}, {} of its lamps lit by the plugin",
                             std::filesystem::path(g_lampLevel).string(),
                             found != g_levelLamps.end() ? found->second->lights.size() : 0);
            }
            // A lamp at a fire the level places, lit by the plugin (ReadLamps).
            if (const auto found = g_levelLamps.find(g_lampLevel);
                found != g_levelLamps.end() &&
                std::ranges::find(found->second->atFires, model + L"/" + Lower(light.name)) !=
                    found->second->atFires.end())
            {
                light.color = {0, 0, 0, light.color[3]};
                return;
            }
        }
        if (daynight::FireLamp(light.model, light.name))
        {
            light.color = {0, 0, 0, light.color[3]};
        }
    });
    dao::vfx::OnEmitterColor([](dao::vfx::EmitterColor &emitter) {
        if (!emitter.modelHasLight && g_lit.contains(Lower(emitter.model)))
        {
            // By how bright the light on it is, not its color: the night sky's deep
            // blue turned the falls' white spray ink blue (the user, 2026-10-22).
            const float bright = daynight::Lit();
            emitter.color.r *= bright;
            emitter.color.g *= bright;
            emitter.color.b *= bright;
        }
    });
    return true;
}

void LevelOpened(const std::filesystem::path &levelRim)
{
    const std::wstring level = Lower(levelRim.stem().wstring());
    {
        const std::scoped_lock lock(g_lampLock);
        if (g_levelLamps.contains(level))
        {
            return;
        }
        g_levelLamps[level] = std::make_shared<const LevelFires>();
        std::erase_if(g_lampReads, [](const std::future<void> &read) {
            return read.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
        g_lampReads.push_back(std::async(std::launch::async, KeepLamps, levelRim, level));
    }
}

void FireStrength(float share)
{
    g_fireShare.store(share, std::memory_order_relaxed);
}

void WantCulled(bool on)
{
    g_culled.store(on, std::memory_order_relaxed);
}

void WantLamps(bool on)
{
    g_lampsWanted.store(on, std::memory_order_relaxed);
}

void Want(bool on)
{
    g_wanted.store(on, std::memory_order_relaxed);
}

void Read()
{
    std::vector<Light> lights;
    if (g_wanted.load(std::memory_order_relaxed))
    {
        void *scene = AreasScene();
        const std::scoped_lock lock(g_lock);
        for (const auto &[node, manager] : g_lights)
        {
            void *of = nullptr;
            std::memcpy(static_cast<void *>(&of), static_cast<const std::byte *>(manager) + kManagerScene, sizeof of);
            if (scene == nullptr || of != scene)
            {
                continue; // another scene's: a portrait's, or an area kept by fast load
            }
            const Light light = LightOf(node);
            if (light.place[3] > 0 && std::max({light.colour[0], light.colour[1], light.colour[2]}) > 0)
            {
                lights.push_back(light);
            }
        }
        const std::size_t engines = lights.size();
        AddLamps(lights);
        AddFires(lights, engines);
    }
    // What lights nothing on the screen left out, then, of more than the
    // shaders take, those that light the most about the camera: each by its
    // brightness over its distance squared. By nearness alone, lights behind
    // the camera took the place of those before it (the user, 2026-10-23: "if
    // the light is out of viewport and its light reach is beyond the
    // viewport, we don't need it"), and the mountain top's dim patches of
    // lava those of the temple gate's torches across the plateau.
    const std::size_t before = lights.size();
    if (const std::optional<std::array<float, 16>> toScreen = levelshadows::ToScreen();
        toScreen && g_culled.load(std::memory_order_relaxed))
    {
        std::erase_if(lights, [&](const Light &light) { return OffScreen(*toScreen, light.place); });
    }
    NoteChoice(before, lights.size());
    if (lights.size() > kMostLights)
    {
        const std::array<float, 3> eye = Eye();
        const auto lighting = [&](const Light &light) {
            const float x = light.place[0] - eye[0];
            const float y = light.place[1] - eye[1];
            const float z = light.place[2] - eye[2];
            const float bright = std::max({light.colour[0], light.colour[1], light.colour[2]});
            return bright / std::max((x * x) + (y * y) + (z * z), 1.0F);
        };
        std::ranges::nth_element(lights, lights.begin() + kMostLights,
                                 [&](const Light &a, const Light &b) { return lighting(a) > lighting(b); });
        lights.resize(kMostLights);
    }
    std::array<float, kRegisterFloats> registers{};
    for (std::size_t i = 0; i < lights.size(); ++i)
    {
        std::memcpy(registers.data() + (i * 8), lights[i].place.data(), sizeof lights[i].place);
        std::memcpy(registers.data() + (i * 8) + 4, lights[i].colour.data(), sizeof lights[i].colour);
    }
    registers.at(2 * kMostLights * 4) = static_cast<float>(lights.size());
    const std::scoped_lock lock(g_readLock);
    g_registers = registers;
}

void Hand(IDirect3DDevice9 *device, bool picture, bool sceneStarts)
{
    // The scene's lights, not the last read: Read runs on the game's thread
    // while the scene is drawn, so a target changed back to the picture
    // could take a newer list than the one the effects' shadow maps had been
    // drawn for (the maps are drawn once a scene, light i into row i). In
    // the Ruined Temple (lak401d), with the game's anti-aliasing, stretches
    // of snow and the statues beyond an arch went black as the camera turned
    // (the user, 2026-10-23: "the statue thing depends on angle"); with the
    // list taken once a scene they did not, in four views where they had
    // (2026-10-24). Taken as the scene first draws into the picture, not as
    // it begins: with the game's anti-aliasing the scene begins on another
    // target, and taken at the start the lights were none.
    if (sceneStarts)
    {
        g_sceneTook = false;
    }
    if (picture && !g_sceneTook)
    {
        const std::scoped_lock lock(g_readLock);
        g_sceneRegisters = g_registers;
        g_sceneTook = true;
    }
    std::array<float, kRegisterFloats> registers{};
    if (picture)
    {
        registers = g_sceneRegisters;
    }
    static_assert(kCountRegister == kLightsRegister + (2 * kMostLights));
    device->SetPixelShaderConstantF(kLightsRegister, registers.data(), static_cast<UINT>(2 * kMostLights) + 1);
    if (!picture)
    {
        return; // the lights the shadow maps are drawn for are the frame's still
    }
    const auto count = static_cast<std::size_t>(registers.at(2 * kMostLights * 4));
    g_handed.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        std::memcpy(g_handed[i].data(), registers.data() + (i * 8), sizeof g_handed[i]);
    }
}

const std::vector<std::array<float, 4>> &Handed()
{
    return g_handed;
}

} // namespace effectlights
