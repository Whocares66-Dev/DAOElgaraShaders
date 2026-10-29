#include "DrawDistance.h"

#include "dao/Addresses.h"
#include "dao/ClientExoApp.h"
#include "dao/Hook.h"
#include "dao/Module.h"

#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace
{

// The address library's IDs of what is read here (DrawDistance.h), not yet
// named in DAOLib's Addresses.h: to be asked of the address library's session.
// `python extern/DAOLib/tools/address_ids.py <id>` gives each one's address.
constexpr dao::AddressId kTile_ChooseDetail{10319};   // 0x005CCFE0, __thiscall (tile, viewer, layout), ret 8
constexpr dao::AddressId kLodManager_Distance{11669}; // 0x006170F0, LodManager::BandDistance
constexpr dao::AddressId kTreeDistanceFactor{95759};  // 0x00BF22D8, float
constexpr std::array<dao::AddressId, 3> kEffectFadeDistances{dao::AddressId{95756}, dao::AddressId{95757},
                                                             dao::AddressId{95758}}; // 0x00BF22CC.., floats, m
constexpr dao::AddressId kApp_RemakeTreeBounds{10551};                               // 0x005D6C20, __thiscall (app)

// A tile's distance in metres at which it changes from its near detail to
// its far (0x005CBD00 writes it as the tile is made).
constexpr std::size_t kTileFarDetail = 0x2A8;
// The detail a tile draws now: 0 near, 1 far, -1 none yet.
constexpr std::size_t kTileDetail = 0x2BC;
// BandDistance's answer for an object of another area or with no model.
constexpr float kNowhere = 1000.0F;

std::atomic<float> g_factor{1.0F};
// Which of the parts the factor scales (WantParts).
std::atomic<bool> g_scaleTiles{true};
std::atomic<bool> g_scaleObjects{true};
std::atomic<bool> g_scaleTrees{true};
std::atomic<bool> g_scaleEffects{true};
// The tiles chosen for since the last frame, and how many of them draw their
// near detail: the log says when that has changed, at most every
// kTilesSaidEvery frames (it changes as the party walks). Ostagar's 1673
// were 342 near at 1x and all of them at 8x (2026-10-29).
constexpr int kTilesSaidEvery = 1800;
std::atomic<int> g_tiles{0};
std::atomic<int> g_nearTiles{0};

using ChooseDetailFn = void(__fastcall *)(void *tile, int edx, const float *viewer, void *layout);
using DistanceFn = float(__fastcall *)(void *manager, int edx, void *object, int anyArea);
using RemakeTreeBoundsFn = void(__fastcall *)(void *app, int edx);
dao::Detour<ChooseDetailFn> g_chooseDetail;
dao::Detour<DistanceFn> g_distance;

// The tile's own distance scaled for the choice alone, and given back after:
// the tile keeps the engine's. The choice can load the near detail at once
// and set the distance anew (0x005CC9E0, a model flagged 0x400), which is kept.
void __fastcall ChooseDetail(void *tile, int edx, const float *viewer, void *layout)
{
    const float factor = g_factor.load(std::memory_order_relaxed);
    g_tiles.fetch_add(1, std::memory_order_relaxed);
    if (*reinterpret_cast<const std::int32_t *>(static_cast<const std::byte *>(tile) + kTileDetail) == 0)
    {
        g_nearTiles.fetch_add(1, std::memory_order_relaxed);
    }
    if (factor == 1.0F || !g_scaleTiles.load(std::memory_order_relaxed))
    {
        g_chooseDetail.Original()(tile, edx, viewer, layout);
        return;
    }
    auto *atDistance = reinterpret_cast<float *>(static_cast<std::byte *>(tile) + kTileFarDetail);
    const float own = *atDistance;
    const float ours = own * factor;
    *atDistance = ours;
    g_chooseDetail.Original()(tile, edx, viewer, layout);
    if (*atDistance == ours)
    {
        *atDistance = own;
    }
}

// An object as near as it is over the factor, for its band and its showing.
float __fastcall Distance(void *manager, int edx, void *object, int anyArea)
{
    const float distance = g_distance.Original()(manager, edx, object, anyArea);
    const float factor =
        g_scaleObjects.load(std::memory_order_relaxed) ? g_factor.load(std::memory_order_relaxed) : 1.0F;
    return distance >= kNowhere ? distance : distance / factor;
}

// One of the game's globals kept at its own value times the factor: what it
// holds that is not what was last written is the game's own anew.
struct Scaled
{
    dao::AddressId id;
    float own = 0.0F;
    float written = 0.0F;
    bool read = false;

    // Returns whether it was written.
    bool Keep(float factor)
    {
        auto *value = dao::At<float>(id);
        if (value == nullptr)
        {
            return false;
        }
        if (!read || *value != written)
        {
            own = *value;
            read = true;
        }
        const float wanted = own * factor;
        if (*value == wanted)
        {
            written = wanted;
            return false;
        }
        *value = wanted;
        written = wanted;
        return true;
    }
};

Scaled g_trees{kTreeDistanceFactor};
std::array<Scaled, 3> g_effects{Scaled{kEffectFadeDistances[0]}, Scaled{kEffectFadeDistances[1]},
                                Scaled{kEffectFadeDistances[2]}};

} // namespace

namespace drawdistance
{

void Install()
{
    if (!g_chooseDetail.Attach(kTile_ChooseDetail, &ChooseDetail))
    {
        spdlog::warn("draw distance: the terrain keeps vanilla's detail, no detour");
    }
    if (!g_distance.Attach(kLodManager_Distance, &Distance))
    {
        spdlog::warn("draw distance: creatures and objects keep vanilla's distances, no detour");
    }
}

void Want(float factor)
{
    g_factor.store(factor > 0.0F ? factor : 1.0F, std::memory_order_relaxed);
}

void WantParts(bool tiles, bool objects, bool trees, bool effects)
{
    g_scaleTiles.store(tiles, std::memory_order_relaxed);
    g_scaleObjects.store(objects, std::memory_order_relaxed);
    g_scaleTrees.store(trees, std::memory_order_relaxed);
    g_scaleEffects.store(effects, std::memory_order_relaxed);
}

void Keep()
{
    const float factor = g_factor.load(std::memory_order_relaxed);
    static int saidTiles = -1;
    static int saidNear = -1;
    static int sinceSaid = kTilesSaidEvery;
    ++sinceSaid;
    if (const int tiles = g_tiles.exchange(0), nearTiles = g_nearTiles.exchange(0);
        tiles != 0 && sinceSaid >= kTilesSaidEvery && (tiles != saidTiles || nearTiles != saidNear))
    {
        spdlog::info("draw distance: {} of {} tiles draw their near detail", nearTiles, tiles);
        saidTiles = tiles;
        saidNear = nearTiles;
        sinceSaid = 0;
    }
    for (Scaled &effect : g_effects)
    {
        effect.Keep(g_scaleEffects.load(std::memory_order_relaxed) ? factor : 1.0F);
    }
    if (g_trees.Keep(g_scaleTrees.load(std::memory_order_relaxed) ? factor : 1.0F))
    {
        // The trees' bounds are made by the factor as a tile's trees are, and
        // made again by the detail setting as it changes the factor.
        if (void *app = dao::CClientExoApp::Get(); app != nullptr)
        {
            if (const std::uintptr_t remake = dao::Module::Address(kApp_RemakeTreeBounds); remake != 0)
            {
                reinterpret_cast<RemakeTreeBoundsFn>(remake)(app, 0);
            }
        }
        spdlog::info("draw distance: the trees' factor {} (the game's {})", g_trees.written, g_trees.own);
    }
}

} // namespace drawdistance
