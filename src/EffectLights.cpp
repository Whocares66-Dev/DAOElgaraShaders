#include "EffectLights.h"

#include "DayNight.h"

#include "dao/Addresses.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/GameModes.h"
#include "dao/Hook.h"
#include "dao/Module.h"
#include "dao/Objects.h"
#include "dao/Vfx.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
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
        for (const dao::vfx::RunningEffect &effect :
             g_fires.empty() ? std::vector<dao::vfx::RunningEffect>{} : dao::vfx::RunningEffects())
        {
            if (effect.modelHasLight || !effect.position)
            {
                continue;
            }
            const auto fire = g_fires.find(Lower(effect.model));
            if (fire != g_fires.end())
            {
                const FireLight &f = fire->second;
                lights.push_back({{effect.position->x, effect.position->y, effect.position->z + f.height, f.reach},
                                  {f.colour[0], f.colour[1], f.colour[2], 1}});
            }
        }
    }
    // More than the shaders take: the nearest the camera.
    if (lights.size() > kMostLights)
    {
        const std::array<float, 3> eye = Eye();
        const auto away = [&](const Light &light) {
            const float x = light.place[0] - eye[0];
            const float y = light.place[1] - eye[1];
            const float z = light.place[2] - eye[2];
            return (x * x) + (y * y) + (z * z);
        };
        std::ranges::nth_element(lights, lights.begin() + kMostLights,
                                 [&](const Light &a, const Light &b) { return away(a) < away(b); });
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

void Hand(IDirect3DDevice9 *device)
{
    std::array<float, kRegisterFloats> registers{};
    {
        const std::scoped_lock lock(g_readLock);
        registers = g_registers;
    }
    static_assert(kCountRegister == kLightsRegister + (2 * kMostLights));
    device->SetPixelShaderConstantF(kLightsRegister, registers.data(), static_cast<UINT>(2 * kMostLights) + 1);
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
