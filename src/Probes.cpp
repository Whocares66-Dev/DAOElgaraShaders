#include "Probes.h"

#include "GameFolder.h"
#include "LevelShadows.h"
#include "ProbeBaker.h"
#include "Reflected.h"

#include "dao/Addresses.h"
#include "dao/Hook.h"
#include "dao/Log.h"
#include "dao/core/LightProbes.h"
#include "dao/core/Msvc8.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace probes
{

namespace
{

// Where a figure's probe light goes, read in the executable 2026-10-11 (the
// dev branch's Graphics.cpp has the whole of it): a figure's lights are chosen
// into its light state (the groups at +0, +0x10, +0x20 for probes, +0x30), and
// the state's own function (0x0046B260, __thiscall with one argument, `ret 4`)
// fills from them what the shaders are handed when a material first asks in a
// frame: the probe's three matrices at +0x130, +0x170 and +0x1B0
// (g_mLightParams, which a figure's vertex shader makes its ambient light of
// with the area's ambient colour), copied from the probe's blueprint (the
// node's +0x174, there at +0x160) only when the probe chosen is not the one
// of last time, which the state keeps at +0x110 and nothing reads but to
// compare. So the mix is written over the three after that function has run,
// each time, and to give a state its probe's own light back the kept probe is
// made one no probe is, and the function copies again.
constexpr std::size_t kLightsProbes = 0x3C;     // the manager's list of light probes
constexpr std::size_t kStateProbes = 0x20;      // in a light state: the group of probes chosen
constexpr std::size_t kStateProbeKept = 0x110;  // the probe its matrices were last copied from
constexpr std::size_t kStateIrradiance = 0x130; // the three matrices
constexpr std::size_t kProbeBlueprint = 0x174;  // in a probe's node
constexpr std::size_t kBlueprintIrradiance = 0x160;
constexpr std::size_t kLightNodeScore = 0x184; // a float, what the light's score leaves
constexpr std::uintptr_t kNoProbe = 1;         // kept in a state to have its probe's light copied again
constexpr float kProbeKeyWithin = 0.05F;       // metres: how near a level's probe is to a room's key to be it
// How near a level's probe's light is to a room's for the room to be its:
// the level's files and the engine's copy of them are the same numbers.
constexpr float kProbeLightWithin = 1e-4F;
// Where a light is, as its score asks: the second virtual of the light the
// node has at +0x180 (0x0053D423 in the probe's score). The node's own
// virtual answers nothing for a probe: every probe of a level was at 0, 0, 0
// by it (logged 2026-10-11).
constexpr std::size_t kNodeLight = 0x180;
constexpr std::size_t kLightPlaceSlot = 2;

using Irradiance = dao::core::probes::Irradiance;
using LightPlaceFn = void(__fastcall *)(void *, int, float *);
using ChooseForMovingFn = std::uintptr_t(__fastcall *)(void *, int, void *, void *, void *, std::uintptr_t);
using ChooseFromListFn = std::uintptr_t(__fastcall *)(void *, int, void *, float, void *, std::uintptr_t,
                                                      std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                                      void *);
using LightScoreFn = std::uintptr_t(__fastcall *)(void *, int, const float *, float, std::uintptr_t, std::uintptr_t);
using StateFillFn = std::uintptr_t(__fastcall *)(void *, int, std::uintptr_t);
// The engine making a source of an archive (ECResMan::ResMan::MakeSource),
// read 2026-10-11: __thiscall, the archive's path as the engine's wide
// string, the source's kind, its priority and two more, `ret 0x14`.
using OpenArchiveFn = std::uintptr_t(__fastcall *)(void *, int, const dao::core::WStringRep *, std::uintptr_t,
                                                   std::uintptr_t, std::uintptr_t, std::uintptr_t);

dao::Detour<ChooseForMovingFn> g_chooseForMoving;
dao::Detour<ChooseFromListFn> g_chooseFromList;
dao::Detour<LightScoreFn> g_probeScore;
dao::Detour<StateFillFn> g_stateFill;
dao::Detour<OpenArchiveFn> g_openArchive;

std::atomic<bool> g_wanted{false};
// The sun's colour on the ground, the game thread's word (SunBounce). Under a
// lock: read as a light state fills.
std::mutex g_sunLock;
std::array<float, 3> g_sunSent{};
// How far from a figure the level's sunlit patches are gathered, in metres:
// ground past it, under 6 degrees below the horizon of a figure's middle a
// metre up, lights only its sides, and those little.
constexpr float kBounceReach = 10;

struct ScoredProbe
{
    void *node;
    float score;
};

thread_local void *t_figuresManager = nullptr; // the manager choosing a figure's lights, on this thread
thread_local std::vector<ScoredProbe> *t_probesScored = nullptr;

std::mutex g_blendsLock;
// A light state's mix, where its figure was, and the light the level's
// sunlit patches send it there, with what that was gathered from: the
// patches' generation and the sun as sent.
struct Blend
{
    Irradiance light{};
    std::array<float, 3> place{};
    Irradiance bounce{};
    std::uint64_t bounceOf = ~std::uint64_t{0};
    std::array<float, 3> bounceSent{};
};
std::unordered_map<void *, Blend> g_blends; // by light state, under g_blendsLock
std::unordered_set<void *> g_statesWritten; // the light states whose matrices are ours now

float Distance(const float *a, const float *b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// Where a light is, by the engine's own word.
std::array<float, 4> PlaceOfLight(void *node)
{
    alignas(16) std::array<float, 4> at{};
    void *light = static_cast<std::byte *>(node) + kNodeLight;
    (*static_cast<LightPlaceFn **>(light))[kLightPlaceSlot](light, 0, at.data());
    return at;
}

// The light a probe of the level's has, as the engine keeps it, or null.
const float *LightOfProbe(const void *node)
{
    const std::byte *blueprint = nullptr;
    std::memcpy(static_cast<void *>(&blueprint), static_cast<const std::byte *>(node) + kProbeBlueprint,
                sizeof blueprint);
    return blueprint != nullptr ? reinterpret_cast<const float *>(blueprint + kBlueprintIrradiance) : nullptr;
}

// The baked room of this probe of the level's, or null: the one whose key is
// where the probe is and whose own light is the probe's. The light tells two
// levels with a probe at one place apart, a village by day and by night.
const probebaker::Room *RoomOf(const probebaker::Rooms &rooms, void *node)
{
    const std::array<float, 4> at = PlaceOfLight(node);
    const float *light = LightOfProbe(node);
    if (light == nullptr)
    {
        return nullptr;
    }
    for (const std::shared_ptr<const probebaker::Room> &room : rooms)
    {
        if (Distance(room->key.data(), at.data()) <= kProbeKeyWithin &&
            std::equal(room->own.begin(), room->own.end(), light,
                       [](float ours, float theirs) { return std::abs(ours - theirs) <= kProbeLightWithin; }))
        {
            return room.get();
        }
    }
    return nullptr;
}

std::uintptr_t __fastcall ProbeScore(void *node, int unused, const float *place, float size, std::uintptr_t room,
                                     std::uintptr_t other)
{
    const std::uintptr_t result = g_probeScore.Original()(node, unused, place, size, room, other);
    if (t_probesScored != nullptr)
    {
        float score = 0;
        std::memcpy(&score, static_cast<const std::byte *>(node) + kLightNodeScore, sizeof score);
        if (score > 0)
        {
            t_probesScored->push_back({node, score});
        }
    }
    return result;
}

// Keeps the mix for the light state whose probes were just chosen into
// `group`, for a figure at `place`: the nearest baked probes of each room of
// the level's own probes that scored for it, mixed. With none scored and a
// probe chosen all the same, the choice was not made again and what was kept
// stands; with no baked room, the state has its own probe's light back.
void KeepBlend(void *group, const std::vector<ScoredProbe> &scored, const float *place)
{
    std::array<const std::byte *, 2> chosen{}; // the group's vector: its first and its end
    std::memcpy(static_cast<void *>(chosen.data()), static_cast<const std::byte *>(group) + sizeof(void *),
                sizeof chosen);
    if (scored.empty() && chosen[0] != chosen[1])
    {
        return;
    }
    thread_local std::vector<dao::core::probes::Near> lights;
    lights.clear();
    // The rooms as they are now, held while their probes' light is pointed at.
    const std::shared_ptr<const probebaker::Rooms> rooms = probebaker::AllRooms();
    if (rooms)
    {
        for (const ScoredProbe &probe : scored)
        {
            if (const probebaker::Room *room = RoomOf(*rooms, probe.node); room != nullptr && !room->probes.empty())
            {
                probebaker::Nearest(*room, place, lights);
            }
        }
    }
    const std::optional<Irradiance> mix = lights.empty() ? std::nullopt : dao::core::probes::Mix(lights);
    void *state = static_cast<std::byte *>(group) - kStateProbes;
    const std::scoped_lock lock(g_blendsLock);
    if (mix)
    {
        g_blends[state] = Blend{.light = *mix, .place = {place[0], place[1], place[2]}};
    }
    else
    {
        g_blends.erase(state);
    }
}

std::uintptr_t __fastcall ChooseForMoving(void *manager, int unused, void *node, void *sun, void *colour,
                                          std::uintptr_t more)
{
    t_figuresManager = manager;
    const std::uintptr_t result = g_chooseForMoving.Original()(manager, unused, node, sun, colour, more);
    t_figuresManager = nullptr;
    return result;
}

std::uintptr_t __fastcall ChooseFromList(void *list, int unused, void *place, float size, void *group,
                                         std::uintptr_t changed, std::uintptr_t owned, std::uintptr_t a7,
                                         std::uintptr_t a8, std::uintptr_t a9, void *only)
{
    const bool probes = t_figuresManager != nullptr &&
                        list == static_cast<std::byte *>(t_figuresManager) + kLightsProbes &&
                        g_wanted.load(std::memory_order_relaxed);
    thread_local std::vector<ScoredProbe> scored;
    if (probes)
    {
        scored.clear();
        t_probesScored = &scored;
    }
    const std::uintptr_t result =
        g_chooseFromList.Original()(list, unused, place, size, group, changed, owned, a7, a8, a9, only);
    if (probes)
    {
        t_probesScored = nullptr;
        KeepBlend(group, scored, static_cast<const float *>(place));
    }
    return result;
}

// The light the level's sunlit patches send a figure at `place`: from the
// near map's patches, or the far one's where the near one does not reach.
// The engine's own bake has none of this: the sun is laid on surfaces as they
// are drawn, and the level's probes were made from its baked light alone.
Irradiance Bounce(const std::array<float, 3> &place, const std::array<float, 3> &sent)
{
    dao::core::probes::Coefficients light{};
    // The second map first: the first's 8 m are narrower than the reach.
    for (const std::size_t map : {std::size_t{1}, std::size_t{0}})
    {
        const std::shared_ptr<const reflected::Patches> patches = reflected::Kept(map);
        if (patches && reflected::Gather(*patches, place, kBounceReach, sent, light))
        {
            break;
        }
    }
    return dao::core::probes::Matrices(light);
}

// A blend's bounce, gathered again if the patches or the sun have changed.
void KeepBounce(Blend &blend)
{
    std::array<float, 3> sent{};
    {
        const std::scoped_lock lock(g_sunLock);
        sent = g_sunSent;
    }
    const std::uint64_t generation = reflected::Generation();
    if (blend.bounceOf != generation || blend.bounceSent != sent)
    {
        blend.bounce = Bounce(blend.place, sent);
        blend.bounceOf = generation;
        blend.bounceSent = sent;
    }
}

std::uintptr_t __fastcall StateFill(void *state, int unused, std::uintptr_t argument)
{
    std::optional<Irradiance> mix;
    {
        const std::scoped_lock lock(g_blendsLock);
        if (const auto found = g_blends.find(state);
            found != g_blends.end() && g_wanted.load(std::memory_order_relaxed))
        {
            KeepBounce(found->second);
            mix = found->second.light;
            for (std::size_t at = 0; at < mix->size(); ++at)
            {
                mix->at(at) += found->second.bounce.at(at);
            }
        }
        else if (g_statesWritten.erase(state) != 0)
        {
            // Matrices of ours left in the state are the engine's to write again.
            std::memcpy(static_cast<std::byte *>(state) + kStateProbeKept, &kNoProbe, sizeof kNoProbe);
        }
    }
    const std::uintptr_t result = g_stateFill.Original()(state, unused, argument);
    if (mix)
    {
        std::memcpy(static_cast<std::byte *>(state) + kStateIrradiance, mix->data(), sizeof *mix);
        const std::scoped_lock lock(g_blendsLock);
        g_statesWritten.insert(state);
    }
    return result;
}

std::uintptr_t __fastcall OpenArchive(void *self, int unused, const dao::core::WStringRep *path, std::uintptr_t a2,
                                      std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5)
{
    const std::uintptr_t result = g_openArchive.Original()(self, unused, path, a2, a3, a4, a5);
    // The engine names a level's archives by its own name for the levels'
    // folder: ENV:lak303d\lak303d.rim (logged 2026-10-11). A level's rooms
    // are in the archive of its folder that has the folder's name.
    constexpr std::wstring_view kLevels = L"ENV:";
    if (path != nullptr && path->size > kLevels.size() && path->size < 1024 && path->View().starts_with(kLevels))
    {
        const std::filesystem::path file(path->View().substr(kLevels.size()));
        std::wstring extension = file.extension().wstring();
        std::ranges::transform(extension, extension.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension == L".rim" && _wcsicmp(file.stem().c_str(), file.parent_path().c_str()) == 0)
        {
            probebaker::Want(GameFolder() / "packages" / "core" / "env" / file);
            levelshadows::Forget();
        }
    }
    return result;
}

} // namespace

bool Install()
{
    if (!g_chooseForMoving.Attach(dao::addr::kLightManager_ChooseForMoving, &ChooseForMoving) ||
        !g_chooseFromList.Attach(dao::addr::kLightList_ChooseFor, &ChooseFromList) ||
        !g_probeScore.Attach(dao::addr::kObjectNodeLightProbe_Score, &ProbeScore) ||
        !g_stateFill.Attach(dao::addr::kLightState_Fill, &StateFill) ||
        !g_openArchive.Attach(dao::addr::kResMan_MakeSource, &OpenArchive))
    {
        spdlog::warn("light probes: a detour could not be placed; figures keep their level's own probe");
        return false;
    }
    probebaker::Start(dao::LogDirectory() / "elgarashaders" / "probes", GameFolder(), {});
    return true;
}

void SunBounce(const std::array<float, 3> &sent)
{
    const std::scoped_lock lock(g_sunLock);
    g_sunSent = sent;
}

void Want(bool on)
{
    if (g_wanted.exchange(on, std::memory_order_relaxed) != on)
    {
        spdlog::info("light probes: figures take {}", on ? "the baked probes" : "the level's own probe");
    }
}

} // namespace probes
