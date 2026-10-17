#include "EffectLights.h"

#include "dao/Addresses.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/GameModes.h"
#include "dao/Hook.h"
#include "dao/Module.h"
#include "dao/Objects.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace effectlights
{
namespace
{

// The registers (effect_lights.hlsl): two a light, then the count.
constexpr UINT kLightsRegister = 160;
constexpr std::size_t kMostLights = 16;
constexpr UINT kCountRegister = kLightsRegister + (2 * kMostLights);

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
std::array<float, (2 * kMostLights + 1) * 4> g_registers{};
std::vector<std::array<float, 4>> g_handed;

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
    std::array<float, (2 * kMostLights + 1) * 4> registers{};
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
    std::array<float, (2 * kMostLights + 1) * 4> registers{};
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
