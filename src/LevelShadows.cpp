#include "LevelShadows.h"
#include "GpuTimer.h"
#include "KeptState.h"

#include "Cascades.h"
#include "EffectLights.h"
#include "Reflected.h"
#include "ShaderCode.h"

#include "dao/core/Erf.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace levelshadows
{

namespace
{

// The shadow variants' partition matrix (build_shaders.py's daoPartition),
// and the engine's own shadow pass's (pssmmesh.vsh's g_mPSSMPartitionMatrix),
// four registers each: the same matrix, the same way about.
constexpr UINT kPartitionRegister = 240;
constexpr UINT kMatrixRegisters = 4;
// The level's vertex shaders read constants up to c58 (spt_leafmeshes.vsh's
// leaf angles' scalars, after the tree's place at c56); a draw's are kept
// from c0 to here. Kept to c47, a tree's leaf meshes were replayed and
// measured by whatever the device last held there and known apart by the
// order they were drawn in alone: their bounds came to 1e21 m with the clock
// running, and the windmill's shadow flickered (2026-10-22).
constexpr UINT kConstants = 64;
constexpr UINT kStreams = 4;
// The engine's shadow pass's vertex shaders are known by the matrix they
// name in their constant table.
constexpr std::string_view kEnginePartitionName = "g_mPSSMPartitionMatrix";
constexpr std::wstring_view kVariantPrefix = L"daoshadow_";
constexpr std::wstring_view kCutVariantPrefix = L"daoshadowcut_";
constexpr std::wstring_view kCutShader = L"daoshadowcut.psh.fxo";
constexpr std::wstring_view kCopyVertexShader = L"daoshadowcopy.vsh.fxo";
constexpr std::wstring_view kCopyPixelShader = L"daoshadowcopy.psh.fxo";
constexpr UINT kAlphaReferenceRegister = 216; // shadow_cut.hlsl's fDaoAlphaReference
constexpr std::wstring_view kMeasurePrefix = L"daomeasure_";
constexpr std::wstring_view kCastShader = L"daoshadow.psh.fxo";
constexpr std::wstring_view kMeasureShader = L"daomeasure.psh.fxo";
// The measuring: the measuring variants' slot (build_shaders.py's
// daoMeasureSlot), how many draws a batch measures, a pixel each, and what the
// measured places are held above 0 by (shadow_measure.hlsl).
constexpr UINT kSlotRegister = 244;
constexpr UINT kMeasuredAtOnce = 256;
constexpr float kMeasureOffset = 65536.0F;

using shadercode::CodeOf;
using shadercode::Constant;
using shadercode::ConstantsOf;
using shadercode::RegisterOf;

constexpr std::uint64_t kFnvStart = 14695981039346656037ULL;

// FNV-1a over `size` bytes at `data`, from `hash`.
std::uint64_t FnvOn(std::uint64_t hash, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t at = 0; at < size; ++at)
    {
        hash = (hash ^ bytes[at]) * 1099511628211ULL;
    }
    return hash;
}

std::uint64_t Fnv(const std::vector<std::byte> &bytes)
{
    return FnvOn(kFnvStart, bytes.data(), bytes.size());
}

std::vector<std::byte> ReadFile(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
    {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

struct Stream
{
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0;
    UINT stride = 0;
};

// One of the level's draws, as the device had it.
struct LevelDraw
{
    IDirect3DVertexShader9 *variant = nullptr; // ours, for the draw's own
    IDirect3DVertexShader9 *measure = nullptr; // and the one its bounds are measured by
    std::uint64_t game = 0;                    // the hash of the game's shader it is drawn by
    // Its colours, the texture on sampler 0 (shadow_cast.hlsl); and a tree's
    // fronds and leaves are cut out by its alpha against the draw's own
    // reference (shadow_cut.hlsl).
    IDirect3DBaseTexture9 *texture = nullptr;
    bool cut = false;
    float alphaReference = 0;
    // Its bounds in the world, least x, y, z then greatest, once measured;
    // none known (drawn into every map) where nothing was measured.
    bool measured = false;
    std::optional<std::array<float, 6>> bounds;
    std::uint64_t measuring = 0; // the batch it is being measured in (MeasureSlot), 0 for none
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    std::array<Stream, kStreams> streams{};
    IDirect3DIndexBuffer9 *indices = nullptr;
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT least = 0;
    UINT vertices = 0;
    UINT first = 0;
    UINT count = 0;
    std::array<float, kConstants * 4> constants{};
    // Whether it turns where it stands, drawn into the maps as it is each
    // frame and kept out of the caches (Note).
    bool moving = false;
};

void Let(LevelDraw &draw)
{
    if (draw.declaration != nullptr)
    {
        draw.declaration->Release();
    }
    for (Stream &stream : draw.streams)
    {
        if (stream.buffer != nullptr)
        {
            stream.buffer->Release();
        }
    }
    if (draw.indices != nullptr)
    {
        draw.indices->Release();
    }
    if (draw.texture != nullptr)
    {
        draw.texture->Release();
    }
}

DrawIndexedFn g_draw = nullptr;
std::atomic<bool> g_wanted{false};
// The variants' byte code by the hash of the game's shader each is made from,
// and the variants made on the device by that hash.
std::unordered_map<std::uint64_t, std::vector<std::byte>> g_variantCode;
std::unordered_set<std::uint64_t> g_cutVariants;                // by the game's shader's hash
std::unordered_map<std::uint64_t, std::wstring> g_variantNames; // the game's shader's, by its hash
// A developer's: the kinds of draw left out of the maps (outdoors.tune's
// LevelShadowsSkip): 1 terrain, 2 buildings and furnishings, 4 trees'
// branches, 8 their fronds and leaves, 16 the figures (the engine's shadow
// pass's draws, replayed into each map).
std::atomic<unsigned> g_skipped{0};
constexpr unsigned kSkipFigures = 16;
std::vector<std::byte> g_cutCode;
IDirect3DPixelShader9 *g_cut = nullptr;
std::unordered_map<std::uint64_t, std::vector<std::byte>> g_measureCode;
std::unordered_map<std::uint64_t, IDirect3DVertexShader9 *> g_measures;
std::vector<std::byte> g_measurePixelCode;
IDirect3DPixelShader9 *g_measurePixel = nullptr;
// The batches of draws being measured, each into two targets, the greatest
// and the least, read back when the GPU has drawn them. Read as soon as drawn
// (GetRenderTargetData), the CPU waited on the GPU on almost every frame of a
// walk, and frames ran past 16.7 ms whenever the GPU had more to do (measured
// 2026-10-21): so the targets are lockable, locked without waiting
// (D3DLOCK_DONOTWAIT), and a few batches are in flight. A draw not yet
// measured is drawn into every map meanwhile (LevelDraw's bounds).
struct MeasureSlot
{
    std::array<IDirect3DSurface9 *, 2> targets{};
    std::vector<std::uint64_t> keys; // g_seen's, by slot
    std::uint64_t batch = 0;         // 0 for free
};
constexpr std::size_t kMeasureSlots = 3;
std::array<MeasureSlot, kMeasureSlots> g_measureSlots;
std::uint64_t g_measureBatches = 0;
std::unordered_map<std::uint64_t, IDirect3DVertexShader9 *> g_variants;
// The registers each variant reads of what the game's shader was given, its
// partition's left out (set once a map): a building's model matrix alone,
// a tree's wind and place. Set alone, they made a kept draw cheap enough to
// draw thousands of (2026-10-16: with all 48 registers set each, 2280 kept
// draws into both maps took Redcliffe's frame from 2.6 to 5.6 ms).
std::unordered_map<IDirect3DVertexShader9 *, std::vector<Constant>> g_variantReads;
// The level's shaders, by the game's: the hash it is known by, and the
// registers that say where a draw of it is in the world, its model's matrix
// (aur_mWorld) or a tree's place (g_vTreePos).
struct GameShader
{
    std::uint64_t hash = 0;
    UINT placed = 0;
    UINT placedCount = 0;
    // Its model's matrix to the screen, and to the world, where it has both:
    // the camera's is the first after the second's inverse.
    std::optional<UINT> toScreen;
    std::optional<UINT> toWorld;
};
std::unordered_map<IDirect3DVertexShader9 *, GameShader> g_gameShaders;
// The engine's shadow pass's, each with the register its partition's matrix is
// in (c4 in pssmmesh.vsh, after the bones in pssmskinmesh.vsh: taken for c4 at
// first, a bone's matrix was drawn by, 2026-10-16), its model's matrix's
// (aur_mWorld, the figure's place; a tree's shaders have none), and how many
// registers it reads from c0: all a replay of its draw sets (setting all 256
// for each of some 1100 replays a frame was a third of Denerim's market's
// cost on the game's thread, issue 30).
struct EngineShader
{
    UINT partition = 0;
    std::optional<UINT> world;
    UINT registers = 0;
};
std::unordered_map<IDirect3DVertexShader9 *, EngineShader> g_engineShadow;
std::vector<std::byte> g_castCode;
IDirect3DPixelShader9 *g_cast = nullptr;
IDirect3DStateBlock9 *g_saved = nullptr;
// Every draw of the level's seen since it was opened, by what is drawn and
// where: the level casts as it was last seen, what is behind the camera with
// the rest, once it has been on the screen (a tree behind the camera cast
// nothing when the frame's own draws were all, 2026-10-16). Let go when a
// level is opened (Forget).
std::unordered_map<std::uint64_t, LevelDraw> g_seen;
std::unordered_map<std::uint64_t, int> g_drawnThisFrame; // how often each key's mesh and place was, this frame
// Which kept draws there are, counted up as one is added or all let go: a
// tree's constants noted again each frame, its wind, do not count.
std::uint64_t g_seenGeneration = 0;
std::vector<const LevelDraw *> g_order; // g_seen's, grouped by what they share
bool g_seenChanged = false;
std::atomic<bool> g_forget{false};
int g_noted = 0; // this frame's
bool g_measuredThisFrame = false;
int g_measuredCount = 0;

void ForgetSeen()
{
    for (auto &[key, draw] : g_seen)
    {
        Let(draw);
    }
    g_seen.clear();
    g_order.clear();
    g_seenChanged = false;
    ++g_seenGeneration;
}
// The shadow map the engine is drawing into now, its partition's matrix, and
// whether it has drawn figures into it since it was set.
IDirect3DSurface9 *g_map = nullptr;
std::array<float, kMatrixRegisters * 4> g_partition{};
bool g_mapDrawn = false;
// The sun's shadows are the plugin's own: one atlas of four maps, each four
// times the last, fitted around what the camera looks at, its focus, so a
// point reads the finest that holds it (shadow.hlsl): texels of 0.8, 3.1,
// 12.5 and 50 cm on maps of 2048. Every map holds the whole level in its
// bounds, kept and drawn again only when it moves (Cache), and every figure
// the engine's own shadow pass draws, drawn into each map each frame. Every
// receiver, the level's and the figures', reads it on the sampler the engine
// gives its near map. The engine's own two maps are left to it: it fits them
// to its figures alone, draws a figure only into those its own split puts it
// in, skips one when it sees fit and blurs the near one into itself
// (traced 2026-10-16), and a figure's shaders have no sampler for more.
// Hosting the view's maps in them was a fault after a fault (the user,
// 2026-10-16: "do it properly").
constexpr std::array<float, 4> kRadii{3, 16, 80, 512};
// What share of its radius a map moves by: it is the same map while the focus
// walks about within it, and the level drawn into it is kept (Cache).
constexpr float kStepShare = 0.4F;

// The level as drawn into a map, kept while the map stays where it is: a
// texture of the map's own format, the depth it was drawn with, and what it
// was drawn for. Each frame its tile of the atlas is given it with one quad
// that writes its depth too (shadow_copy.hlsl), in place of the level drawn
// again (2026-10-16, Redcliffe: some 2870 draws a frame, 2.6 ms; the user:
// "caching the computations unless a light source changes").
struct Cache
{
    IDirect3DTexture9 *texture = nullptr;
    IDirect3DSurface9 *depth = nullptr;
    IDirect3DTexture9 *colours = nullptr; // the two finest maps': each texel's surface's colour, for its patches
    cascades::Matrix drawnBy{};
    std::uint64_t drawnWith = ~std::uint64_t{0}; // g_seenGeneration
    unsigned drawnSkipping = 0;                  // g_skipped, the developer's
};
std::array<Cache, 4> g_caches;

// The shadows' blend (the user, 2026-10-21: "shadows are flickering", "Do the
// shadow blend"). As the sun or the moon goes over, the maps are aimed anew in
// steps (Sun), and a step's maps lay their texels a little differently over
// the world, so every soft edge's fine pattern changed at once. So at a step
// the old aim's maps are kept: its level as it was cached (the caches' other
// set, which the new aim's do not draw into) and its figures drawn again each
// frame by the old aim into an atlas of their own; and the receivers read
// both and go from the old to the new over kFadeSeconds (shadow.hlsl's
// DaoShadowFade). The next step waits for the fade (Sun).
constexpr float kFadeSeconds = 1.0F;
std::array<Cache, 4> g_oldCaches;
std::array<std::optional<cascades::Fitted>, 4> g_oldCascades;
std::optional<std::array<cascades::Vector, 3>> g_oldAxes;
std::optional<std::array<cascades::Vector, 3>> g_fittedAxes; // the axes of the sun's own the maps were fitted by
std::optional<std::chrono::steady_clock::time_point> g_fadeFrom;
std::atomic<bool> g_fading{false}; // the drawing thread's word, read by Sun
float g_fadeShare = 0;             // how far this frame's fade is, 0 to 1; 0 for none
IDirect3DTexture9 *g_oldAtlas = nullptr;
// The registers the receivers read the old aim by (shadow.hlsl's
// daoShadowFade): the turn from the new aim's axes to the old's, by rows, and
// for each map its across and down offsets and its depth's scale and offset.
constexpr UINT kFadeRegister = 193;
constexpr UINT kFadeRegisters = 7;

// The level's sunlit patches read back from the two finest maps
// (Reflected.h): a texel in every kReflectStep of each cache's rows and
// columns drawn into a target of their own, copied to memory, and turned into
// patches at the frame's end. Only when a cache is drawn again: a map that
// stays put has the same.
constexpr int kReflectStep = 16;
// A sample of a cache, read back once the GPU has drawn it: lockable targets
// locked without waiting, two in flight a map, as the measuring's
// (MeasureSlot).
struct ReflectSlot
{
    IDirect3DSurface9 *target = nullptr;       // kReflectStep times smaller than the map
    IDirect3DSurface9 *colourTarget = nullptr; // and of the cache's colours
    std::optional<cascades::Matrix> waiting;   // the matrix the sample was drawn by, until it is read
    int texels = 0;                            // the map's
    std::uint64_t order = 0;                   // which was drawn first
};
struct Reflecting
{
    std::array<ReflectSlot, 2> slots;
};
std::uint64_t g_reflectOrder = 0;
std::array<Reflecting, 2> g_reflecting;
std::vector<std::byte> g_copyVertexCode;
std::vector<std::byte> g_copyPixelCode;
IDirect3DVertexShader9 *g_copyVertex = nullptr;
IDirect3DPixelShader9 *g_copyPixel = nullptr;
IDirect3DVertexDeclaration9 *g_copyDeclaration = nullptr;
int g_cacheDrawn = 0; // how often, for the log
cascades::Matrix g_toScreen{};
bool g_haveCamera = false;
std::array<std::optional<cascades::Fitted>, 4> g_cascades;
// Each map's depth along the sun, the least and most of the level in its
// bounds, and what it was reckoned for: the map's place and the level seen.
struct Along
{
    std::optional<std::array<float, 2>> span;
    std::array<float, 2> place{};
    cascades::Vector light{};
    std::uint64_t seen = ~std::uint64_t{0};
};
std::array<Along, 4> g_along;
std::optional<cascades::Vector> g_focus;        // under g_sunLock: Focus's
std::optional<cascades::Vector> g_fittedAround; // what this frame's maps were fitted around
std::mutex g_sunLock;
std::optional<std::array<cascades::Vector, 3>> g_sunAxes; // Sun's, under g_sunLock

// The atlas: the four maps in a square of two by two, map i at column i % 2
// and row i / 2, each kTile texels a side in the engine's maps' format (depth
// in red, 16 bits; shadow.hlsl's kDaoAtlasTile is the same), whatever size
// the game's shadow quality gives its own; its depth; and whether it was
// drawn this frame.
constexpr UINT kTile = 2048;
constexpr D3DFORMAT kMapFormat = D3DFMT_G16R16;
IDirect3DTexture9 *g_atlas = nullptr;
IDirect3DSurface9 *g_atlasDepth = nullptr;
UINT g_tile = 0;
bool g_atlasDrawn = false; // asked for this frame

// The effects' lights' shadows (effect_lights.hlsl's DaoEffectShadow): for
// each light handed this scene, six maps, a cube's faces, a row of six to a
// light, drawn with the figures within the light's reach. A face is
// kEffectTile square, a quarter turn wide, from kEffectNear to the light's
// reach; all of it the shaders' own reckoning, which these numbers must match.
constexpr UINT kEffectTile = 256; // effect_lights.hlsl's kDaoEffectTile; at 128 a figure's shadow from a fire was cut
                                  // in steps (the user, 2026-10-23: "shadows are very blocky")
constexpr UINT kEffectLightsMost = 16;
constexpr float kEffectNear = 0.05F;
std::atomic<bool> g_effectsWanted{false};
std::unordered_map<IDirect3DPixelShader9 *, DWORD> g_effectReceivers;
IDirect3DTexture9 *g_effectAtlas = nullptr;
IDirect3DSurface9 *g_effectDepth = nullptr;
// The level's own surfaces in each light's six maps, drawn once and copied
// into the atlas each frame, the figures drawn over them: the level's draws
// were five of the effects' maps' six milliseconds a frame at Redcliffe
// castle's gate (13 lights, 997 level draws; 2026-10-23), for maps that
// change only as the level seen does or the light moves. A row of the cache
// is a light's, by its place and reach, kept while the level seen is the same.
IDirect3DTexture9 *g_effectLevel = nullptr;
IDirect3DSurface9 *g_effectLevelDepth = nullptr;
struct EffectLevelRow
{
    std::array<float, 4> light{};
    std::uint64_t seen = ~std::uint64_t{0}; // g_seenGeneration it was drawn with
    unsigned skipping = 0;
    std::uint64_t used = 0; // the frame it was last copied in
};
std::array<EffectLevelRow, kEffectLightsMost> g_effectLevelRows;
std::uint64_t g_effectFrame = 0;
std::atomic<bool> g_effectCache{true}; // a developer's: outdoors.tune's EffectCache = 0 draws the level each frame
int g_effectLevelRowsDrawn = 0;        // a frame, for the log
bool g_effectsDrawn = false;           // asked for this frame
bool g_effectsReady = false;
bool g_atlasReady = false; // and drawn: the receivers read it, else the engine's own maps
// Whether the engine's shadow pass has drawn this frame, and last frame. The
// atlas takes its figures from the pass, so it is drawn after it: a receiver
// drawn before the pass (the game's own anti-aliasing draws some 55 things
// first) read an atlas with no figures in it, for the whole frame (the user's
// game, 2026-10-16). A frame with no pass, where the last had none either, has
// no figures to wait for.
bool g_passThisFrame = false;
bool g_passLastFrame = false;
// The receivers' pixel shaders, each with the sampler it reads the engine's
// near map on (its smpShadowPartition1, or a face's and hair's
// smpShadowPart1), which is given the atlas.
std::unordered_map<IDirect3DPixelShader9 *, DWORD> g_receivers;
std::unordered_map<IDirect3DPixelShader9 *, DWORD> g_receiversFar; // the engine's far map's sampler: the old aim's

// What a receiver's draw had on the sampler it is given the atlas on, given
// back when the draw is done (Drawn). The engine keeps the sampler states it
// set and does not set them again, so the atlas's point sampling, without
// mipmaps and clamped, was left on the slot for every draw after that reads
// a texture there: until 2026-10-16 figures' and props' textures were drawn
// blocky close up and sparkling in motion, and SMAA could not smooth them
// (the user: "grainy shimmering").
constexpr std::array<D3DSAMPLERSTATETYPE, 5> kLentStates{D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                                         D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV};
struct Lent
{
    bool lent = false;
    DWORD sampler = 0;
    IDirect3DBaseTexture9 *texture = nullptr;
    std::array<DWORD, kLentStates.size()> states{};
};
Lent g_lent;
Lent g_lentEffects;
Lent g_lentOld; // the old aim's atlas, on a receiver's far map's sampler while the shadows fade
SetSamplerStateFn g_setSampler = nullptr; // UseSamplerState's

void SetSampler(IDirect3DDevice9 *device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (g_setSampler != nullptr)
    {
        g_setSampler(device, sampler, type, value);
    }
    else
    {
        device->SetSamplerState(sampler, type, value);
    }
}

// One of the engine's draws of a figure into its shadow maps, as the device
// had it, to be drawn into each of the atlas's: the engine draws the same
// figure into both its maps, or into one, or none past its own reach.
// Where a figure's draw stands and how far about that it reaches.
struct Reach
{
    std::array<float, 3> centre{};
    float reach = 0;
};

struct FigureDraw
{
    std::uint64_t key = 0; // the buffers, the part and where it is
    IDirect3DVertexShader9 *vertex = nullptr;
    IDirect3DPixelShader9 *pixel = nullptr;
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    std::array<Stream, kStreams> streams{};
    IDirect3DIndexBuffer9 *indices = nullptr;
    IDirect3DBaseTexture9 *texture = nullptr;
    D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
    INT base = 0;
    UINT least = 0;
    UINT vertices = 0;
    UINT first = 0;
    UINT count = 0;
    UINT partition = 0;          // the register of the map's matrix
    UINT registers = 0;          // how many of vertexConstants its shader reads, from c0
    std::optional<Reach> bounds; // none where its shader names no place
    std::array<float, 256 * 4> vertexConstants{};
    std::array<float, 16 * 4> pixelConstants{};
    std::array<DWORD, 4> states{}; // alpha test, its reference and function, culling
};
constexpr std::array<D3DRENDERSTATETYPE, 4> kFigureStates{D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC,
                                                          D3DRS_CULLMODE};
std::vector<FigureDraw> g_figureDraws; // this frame's
std::unordered_set<std::uint64_t> g_figureKeys;
// The receivers' registers (shadow.hlsl's daoShadowData): each map's texture
// matrix's first three rows, whether they are ours, how far from the focus a
// map is read, and the focus.
constexpr UINT kReceiverRegister = 200;
constexpr UINT kReceiverRegisters = 14;

// What happened, for the log now and then.
int g_frames = 0;
int g_engineDraws = 0;
int g_drawnInto = 0;

// What the shadows cost a frame (issue 30, a player's frame rate in Denerim's
// market from 150 to 100 with Improved lighting): the figures replayed into
// the sun's maps and the effects' lights' maps, the most lights handed, and the
// time each part took over the log's interval on the game's thread
// (QueryPerformanceCounter, what the calls took to be made). The GPU's time
// was read by timestamp queries too, until the game crashed in Direct3D as it
// shut down with them made, every time (0.2.1, 2026-10-20).
enum Part : std::size_t
{
    kSunPart,     // DrawAtlas: the caches kept, each tile copied, what moves and the figures
    kEffectsPart, // DrawEffectShadows: each light's six faces
    kNotePart,    // NoteFigure and NoteLevel: the draws read off the device
    kParts
};
constexpr std::array<std::string_view, kParts> kPartNames{"sun's maps", "effects' maps", "noting"};
int g_figureReplays = 0;
int g_effectLevelDraws = 0;       // level draws into the effects' lights' maps, a frame
int g_effectFaces = 0;            // faces of the effects' lights' maps drawn, a frame
LONGLONG g_effectLevelTicks = 0;  // the effects' maps' level draws, since the last log
LONGLONG g_effectFigureTicks = 0; // and their figures'
int g_effectFigureReplays = 0;
std::size_t g_lightsMost = 0;
std::array<LONGLONG, kParts> g_partTicks{};

// A part's time on the game's thread, from its making to its end.
class Timed
{
  public:
    explicit Timed(Part part) : m_part(part)
    {
        QueryPerformanceCounter(&m_start);
    }
    ~Timed()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        g_partTicks.at(m_part) += now.QuadPart - m_start.QuadPart;
    }
    Timed(const Timed &) = delete;
    Timed &operator=(const Timed &) = delete;
    Timed(Timed &&) = delete;
    Timed &operator=(Timed &&) = delete;

  private:
    Part m_part;
    LARGE_INTEGER m_start{};
};

IDirect3DVertexShader9 *VariantOf(IDirect3DDevice9 *device, std::uint64_t hash)
{
    if (const auto made = g_variants.find(hash); made != g_variants.end())
    {
        return made->second;
    }
    IDirect3DVertexShader9 *variant = nullptr;
    const std::vector<std::byte> &code = g_variantCode.at(hash);
    if (FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD *>(code.data()), &variant)))
    {
        variant = nullptr;
    }
    g_variants[hash] = variant;
    if (variant != nullptr)
    {
        std::vector<Constant> reads;
        for (const Constant &constant : ConstantsOf(code))
        {
            if (constant.first != kPartitionRegister && constant.first + constant.count <= kConstants)
            {
                reads.push_back(constant);
            }
        }
        g_variantReads[variant] = reads;
    }
    return variant;
}

// The camera's matrix to the screen from a draw's to the screen and to the
// world, each four registers, by rows or by columns as the shader takes them
// (build_shaders.py's by_rows): a model's matrix to the world by rows ends in
// (0, 0, 0, 1); by columns, its fourth register is its place.
void NoteCamera(const float *toScreen, const float *toWorld)
{
    cascades::Matrix screen{};
    cascades::Matrix world{};
    std::memcpy(screen.data(), toScreen, sizeof screen);
    std::memcpy(world.data(), toWorld, sizeof world);
    const bool byRows = world[12] == 0 && world[13] == 0 && world[14] == 0 && world[15] == 1;
    const bool byColumns = world[3] == 0 && world[7] == 0 && world[11] == 0 && world[15] == 1;
    if (!byRows && byColumns)
    {
        for (int r = 0; r < 4; ++r)
        {
            for (int c = r + 1; c < 4; ++c)
            {
                std::swap(screen.at((r * 4) + c), screen.at((c * 4) + r));
                std::swap(world.at((r * 4) + c), world.at((c * 4) + r));
            }
        }
    }
    else if (!byRows)
    {
        return;
    }
    const std::optional<cascades::Matrix> fromWorld = cascades::Inverse(world);
    if (!fromWorld)
    {
        return;
    }
    cascades::Matrix camera{};
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int k = 0; k < 4; ++k)
            {
                camera.at((r * 4) + c) += screen.at((r * 4) + k) * fromWorld->at((k * 4) + c);
            }
        }
    }
    g_toScreen = camera;
    g_haveCamera = true;
}

IDirect3DVertexShader9 *MeasureOf(IDirect3DDevice9 *device, std::uint64_t hash)
{
    if (const auto made = g_measures.find(hash); made != g_measures.end())
    {
        return made->second;
    }
    IDirect3DVertexShader9 *measure = nullptr;
    const auto code = g_measureCode.find(hash);
    if (code == g_measureCode.end() || code->second.empty() ||
        FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD *>(code->second.data()), &measure)))
    {
        measure = nullptr;
    }
    g_measures[hash] = measure;
    return measure;
}

void Note(IDirect3DDevice9 *device, IDirect3DVertexShader9 *variant, const GameShader &game, D3DPRIMITIVETYPE type,
          INT base, UINT least, UINT vertices, UINT first, UINT count)
{
    LevelDraw draw;
    draw.variant = variant;
    draw.measure = MeasureOf(device, game.hash);
    draw.type = type;
    draw.base = base;
    draw.least = least;
    draw.vertices = vertices;
    draw.first = first;
    draw.count = count;
    draw.game = game.hash;
    draw.cut = g_cutVariants.contains(game.hash);
    device->GetTexture(0, &draw.texture);
    if (draw.cut)
    {
        DWORD reference = 0;
        device->GetRenderState(D3DRS_ALPHAREF, &reference);
        constexpr float kOfByte = 255;
        draw.alphaReference = static_cast<float>(reference & 0xFF) / kOfByte;
    }
    device->GetVertexDeclaration(&draw.declaration);
    for (UINT at = 0; at < kStreams; ++at)
    {
        Stream &stream = draw.streams.at(at);
        device->GetStreamSource(at, &stream.buffer, &stream.offset, &stream.stride);
    }
    device->GetIndices(&draw.indices);
    device->GetVertexShaderConstantF(0, draw.constants.data(), kConstants);
    if (game.toScreen && game.toWorld && *game.toScreen + 4 <= kConstants && *game.toWorld + 4 <= kConstants)
    {
        NoteCamera(draw.constants.data() + (static_cast<std::size_t>(*game.toScreen) * 4),
                   draw.constants.data() + (static_cast<std::size_t>(*game.toWorld) * 4));
    }
    // A draw is known by what is drawn, the buffers and the part of them,
    // and where it stands, its placement's translation, to the millimetre,
    // and which time this frame that was drawn: two of a mesh at one place
    // are two. How it is turned is not what it is but how it is now: a draw
    // that turns where it stands moves (Redcliffe's windmill, its sails at a
    // new angle each frame, every one kept as more of the level and the
    // caches drawn again each frame, 2026-10-16).
    std::vector<std::byte> key;
    const auto put = [&](const void *value, std::size_t size) {
        const std::size_t at = key.size();
        key.resize(at + size);
        std::memcpy(key.data() + at, value, size);
    };
    put(static_cast<const void *>(&draw.variant), sizeof(void *));
    put(static_cast<const void *>(&draw.streams[0].buffer), sizeof(void *));
    put(static_cast<const void *>(&draw.indices), sizeof(void *));
    for (const UINT part : {draw.streams[0].offset, static_cast<UINT>(base), least, vertices, first, count})
    {
        put(&part, sizeof part);
    }
    const float *placed = nullptr;
    if (game.placedCount != 0 && game.placed + game.placedCount <= kConstants)
    {
        placed = draw.constants.data() + (static_cast<std::size_t>(game.placed) * 4);
        // A model's matrix has its translation in its last column by rows or
        // its last row by columns, and 0 in the other; a tree's place is its
        // first three.
        const std::array<float, 3> place =
            game.placedCount == kMatrixRegisters
                ? std::array<float, 3>{placed[3] + placed[12], placed[7] + placed[13], placed[11] + placed[14]}
                : std::array<float, 3>{placed[0], placed[1], placed[2]};
        constexpr float kMillimetres = 1000;
        for (const float coordinate : place)
        {
            const long millimetres = std::lround(coordinate * kMillimetres);
            put(&millimetres, sizeof millimetres);
        }
    }
    const int time = g_drawnThisFrame[Fnv(key)]++;
    put(&time, sizeof time);
    const std::uint64_t hash = Fnv(key);
    if (const auto had = g_seen.find(hash); had != g_seen.end())
    {
        // The same: so are its bounds, but for a turn.
        LevelDraw &before = had->second;
        const bool turned =
            placed != nullptr && std::memcmp(before.constants.data() + (static_cast<std::size_t>(game.placed) * 4),
                                             placed, game.placedCount * 4 * sizeof(float)) != 0;
        draw.measured = before.measured && !turned;
        draw.bounds = turned ? std::nullopt : before.bounds;
        draw.measuring = turned ? 0 : before.measuring; // a turned draw's batch measured it where it was
        draw.moving = before.moving || turned;
        if (draw.moving && !before.moving)
        {
            ++g_seenGeneration; // out of the caches
            g_seenChanged = true;
        }
        Let(before);
        before = draw;
    }
    else
    {
        g_seen.emplace(hash, draw);
        g_seenChanged = true;
        ++g_seenGeneration;
    }
    ++g_noted;
}

void SetState(IDirect3DDevice9 *device)
{
    device->SetPixelShader(g_cast);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                       D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    device->SetRenderState(D3DRS_COLORWRITEENABLE1, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN |
                                                        D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
    // The colours read at their smallest picture (shadow_cast.hlsl), and a
    // cut texture's alpha as the level reads it.
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    device->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, 0);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device->SetVertexShaderConstantF(kPartitionRegister, g_partition.data(), kMatrixRegisters);
}

// How many points a draw's vertices are, drawn as points by its indices.
UINT PointsOf(const LevelDraw &draw)
{
    switch (draw.type)
    {
    case D3DPT_TRIANGLELIST:
        return draw.count * 3;
    case D3DPT_TRIANGLESTRIP:
        return draw.count + 2;
    default:
        return 0;
    }
}

bool MakeMeasuring(IDirect3DDevice9 *device)
{
    if (g_measurePixel == nullptr &&
        (g_measurePixelCode.empty() ||
         FAILED(
             device->CreatePixelShader(reinterpret_cast<const DWORD *>(g_measurePixelCode.data()), &g_measurePixel))))
    {
        g_measurePixel = nullptr;
        return false;
    }
    for (MeasureSlot &slot : g_measureSlots)
    {
        for (IDirect3DSurface9 *&target : slot.targets)
        {
            if (target == nullptr && FAILED(device->CreateRenderTarget(kMeasuredAtOnce, 1, D3DFMT_A32B32G32R32F,
                                                                       D3DMULTISAMPLE_NONE, 0, TRUE, &target, nullptr)))
            {
                target = nullptr;
                return false;
            }
        }
    }
    return true;
}

// A target's texels as floats, four a pixel; none while the GPU is still
// drawing it, and an empty one where it cannot be read.
std::optional<std::vector<float>> ReadFloats(IDirect3DSurface9 *target)
{
    D3DLOCKED_RECT locked{};
    const HRESULT result = target->LockRect(&locked, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
    if (result == D3DERR_WASSTILLDRAWING)
    {
        return std::nullopt;
    }
    if (FAILED(result))
    {
        return std::vector<float>{};
    }
    const auto *texels = static_cast<const float *>(locked.pBits);
    std::vector<float> read(texels, texels + (static_cast<std::size_t>(kMeasuredAtOnce) * 4));
    target->UnlockRect();
    return read;
}

// The batches the GPU has drawn read back: each draw's bounds from its pixel
// of the two targets. A draw replaced since, turned, is left to be measured
// again.
void CollectMeasured()
{
    for (MeasureSlot &slot : g_measureSlots)
    {
        if (slot.batch == 0)
        {
            continue;
        }
        const std::optional<std::vector<float>> most = ReadFloats(slot.targets[0]);
        if (!most)
        {
            continue;
        }
        const std::optional<std::vector<float>> least = ReadFloats(slot.targets[1]);
        if (!least)
        {
            continue;
        }
        const bool read = !most->empty() && !least->empty();
        for (std::size_t at = 0; at < slot.keys.size(); ++at)
        {
            const auto found = g_seen.find(slot.keys[at]);
            if (found == g_seen.end() || found->second.measuring != slot.batch)
            {
                continue;
            }
            LevelDraw &draw = found->second;
            draw.measuring = 0;
            if (!read)
            {
                continue;
            }
            draw.measured = true;
            const float *greatest = most->data() + (at * 4);
            const float *smallest = least->data() + (at * 4);
            // A point was drawn to it: the first target's alpha (the second's
            // is kept from writing while blending, Graphics.cpp's guard of the
            // second target's alpha).
            if (greatest[3] > 0)
            {
                draw.bounds = std::array<float, 6>{kMeasureOffset - smallest[0], kMeasureOffset - smallest[1],
                                                   kMeasureOffset - smallest[2], greatest[0] - kMeasureOffset,
                                                   greatest[1] - kMeasureOffset, greatest[2] - kMeasureOffset};
                ++g_measuredCount;
            }
        }
        slot.keys.clear();
        slot.batch = 0;
    }
}

// The kept draws not yet measured, up to a batch of them, measured: each drawn
// as points to its own pixel of the two targets, which keep the greatest and
// the least of its places in the world, and read back. The device's state is
// the caller's to keep; its targets and depth are kept here.
void Measure(IDirect3DDevice9 *device)
{
    CollectMeasured();
    MeasureSlot *free = nullptr;
    for (MeasureSlot &slot : g_measureSlots)
    {
        if (slot.batch == 0)
        {
            free = &slot;
            break;
        }
    }
    if (free == nullptr)
    {
        return;
    }
    std::vector<LevelDraw *> batch;
    for (auto &[key, draw] : g_seen)
    {
        if (!draw.measured && draw.measuring == 0 && batch.size() < kMeasuredAtOnce)
        {
            batch.push_back(&draw);
            free->keys.push_back(key);
        }
    }
    if (batch.empty())
    {
        return;
    }
    if (!MakeMeasuring(device))
    {
        static bool said = false;
        if (!said)
        {
            said = true;
            spdlog::warn("level shadows: no measuring: pixel shader {}, lockable targets {}",
                         static_cast<void *>(g_measurePixel), static_cast<void *>(g_measureSlots[0].targets[0]));
        }
        free->keys.clear();
        return;
    }
    std::array<IDirect3DSurface9 *, 2> targets{};
    IDirect3DSurface9 *depth = nullptr;
    device->GetRenderTarget(0, targets.data());
    device->GetRenderTarget(1, targets.data() + 1);
    device->GetDepthStencilSurface(&depth);
    device->SetRenderTarget(0, free->targets[0]);
    device->SetRenderTarget(1, free->targets[1]);
    device->SetDepthStencilSurface(nullptr);
    const D3DVIEWPORT9 viewport{0, 0, kMeasuredAtOnce, 1, 0, 1};
    device->SetViewport(&viewport);
    device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1, 0);
    device->SetPixelShader(g_measurePixel);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_MAX);
    device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
    // A point a pixel: the engine leaves its particles' sprites and sizes set,
    // and a measured draw's points spread into its neighbours' pixels (trees'
    // bounds of 280 m, alike for all, 2026-10-16).
    constexpr float kOnePixel = 1.0F;
    DWORD one = 0;
    std::memcpy(&one, &kOnePixel, sizeof one);
    device->SetRenderState(D3DRS_POINTSIZE, one);
    device->SetRenderState(D3DRS_POINTSIZE_MIN, one);
    device->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    device->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
    constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    device->SetVertexShaderConstantF(kPartitionRegister, kIdentity.data(), kMatrixRegisters);
    for (std::size_t slot = 0; slot < batch.size(); ++slot)
    {
        const LevelDraw &draw = *batch[slot];
        const UINT points = PointsOf(draw);
        if (draw.measure == nullptr || points == 0)
        {
            continue;
        }
        const std::array<float, 4> place{((2.0F * (static_cast<float>(slot) + 0.5F)) / kMeasuredAtOnce) - 1, 0, 0.5F,
                                         1};
        device->SetVertexShaderConstantF(kSlotRegister, place.data(), 1);
        device->SetVertexDeclaration(draw.declaration);
        for (UINT at = 0; at < kStreams; ++at)
        {
            device->SetStreamSource(at, draw.streams.at(at).buffer, draw.streams.at(at).offset,
                                    draw.streams.at(at).stride);
        }
        device->SetIndices(draw.indices);
        device->SetVertexShader(draw.measure);
        for (const Constant &read : g_variantReads[draw.variant])
        {
            device->SetVertexShaderConstantF(
                read.first, draw.constants.data() + (static_cast<std::size_t>(read.first) * 4), read.count);
        }
        g_draw(device, D3DPT_POINTLIST, draw.base, draw.least, draw.vertices, draw.first, points);
    }
    device->SetRenderTarget(0, targets[0]);
    device->SetRenderTarget(1, targets[1]);
    device->SetDepthStencilSurface(depth);
    for (IDirect3DSurface9 *held : {targets[0], targets[1], depth})
    {
        if (held != nullptr)
        {
            held->Release();
        }
    }
    free->batch = ++g_measureBatches;
    for (LevelDraw *draw : batch)
    {
        draw->measuring = free->batch;
    }
}

// The light an effect's light's maps are being drawn for, its place and
// reach: what is drawn into them comes within its reach (DrawEffectShadows).
std::optional<std::array<float, 4>> g_drawingFor;

// Whether a draw's bounds lie wholly outside the shadow map's slice: to one
// side of it, or beyond its far end; or, for an effect's light's maps, out of
// the light's reach. What lies nearer the sun than its near end is drawn,
// pressed onto that end, and so is a draw not measured. Each side is a plane
// of the map's clip space (x against -w and w, and so on), which a map's
// matrix with a perspective, an effect's light's, keeps as one: x over w is
// not, for a corner behind the light.
bool Outside(const LevelDraw &draw)
{
    if (!draw.bounds)
    {
        return false;
    }
    const std::array<float, 6> &b = *draw.bounds;
    if (g_drawingFor)
    {
        // The nearest point of the bounds to the light, against its reach.
        const std::array<float, 4> &light = *g_drawingFor;
        float away = 0;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            const float d = std::max({b.at(axis) - light.at(axis), 0.0F, light.at(axis) - b.at(axis + 3)});
            away += d * d;
        }
        if (away > light[3] * light[3])
        {
            return true;
        }
    }
    std::array<int, 5> beyond{}; // all corners: left of -1, right of 1, below, above, past the far end
    for (int corner = 0; corner < 8; ++corner)
    {
        const std::array<float, 4> p{b[(corner & 1) != 0 ? 3 : 0], b[(corner & 2) != 0 ? 4 : 1],
                                     b[(corner & 4) != 0 ? 5 : 2], 1};
        std::array<float, 4> q{};
        for (int row = 0; row < 4; ++row)
        {
            q.at(row) = (g_partition.at(row * 4) * p[0]) + (g_partition.at((row * 4) + 1) * p[1]) +
                        (g_partition.at((row * 4) + 2) * p[2]) + g_partition.at((row * 4) + 3);
        }
        const float w = q[3];
        beyond[0] += q[0] < -w ? 1 : 0;
        beyond[1] += q[0] > w ? 1 : 0;
        beyond[2] += q[1] < -w ? 1 : 0;
        beyond[3] += q[1] > w ? 1 : 0;
        beyond[4] += q[2] > w ? 1 : 0;
    }
    return std::ranges::any_of(beyond, [](int all) { return all == 8; });
}

// The kept draws in an order that keeps what draws share together, made again
// as they change.
void Order()
{
    if (!g_seenChanged)
    {
        return;
    }
    g_order.clear();
    for (const auto &[key, draw] : g_seen)
    {
        g_order.push_back(&draw);
    }
    std::ranges::sort(g_order, [](const LevelDraw *a, const LevelDraw *b) {
        return std::tie(a->variant, a->declaration, a->streams[0].buffer, a->indices) <
               std::tie(b->variant, b->declaration, b->streams[0].buffer, b->indices);
    });
    g_seenChanged = false;
}

// A kept draw's buffers, shader and constants on the device, what the last
// one drawn left as it is set again only where it differs.
void Bind(IDirect3DDevice9 *device, const LevelDraw &draw, const LevelDraw *last)
{
    if (last == nullptr || draw.declaration != last->declaration)
    {
        device->SetVertexDeclaration(draw.declaration);
    }
    for (UINT at = 0; at < kStreams; ++at)
    {
        const Stream &stream = draw.streams.at(at);
        if (last == nullptr || stream.buffer != last->streams.at(at).buffer ||
            stream.offset != last->streams.at(at).offset || stream.stride != last->streams.at(at).stride)
        {
            device->SetStreamSource(at, stream.buffer, stream.offset, stream.stride);
        }
    }
    if (last == nullptr || draw.indices != last->indices)
    {
        device->SetIndices(draw.indices);
    }
    if (last == nullptr || draw.variant != last->variant)
    {
        device->SetVertexShader(draw.variant);
    }
    if (last == nullptr || draw.cut != last->cut)
    {
        device->SetPixelShader(draw.cut ? g_cut : g_cast);
    }
    if (last == nullptr || draw.texture != last->texture)
    {
        device->SetTexture(0, draw.texture);
    }
    if (draw.cut)
    {
        const std::array<float, 4> reference{draw.alphaReference, 0, 0, 0};
        device->SetPixelShaderConstantF(kAlphaReferenceRegister, reference.data(), 1);
    }
    for (const Constant &read : g_variantReads[draw.variant])
    {
        device->SetVertexShaderConstantF(read.first, draw.constants.data() + (static_cast<std::size_t>(read.first) * 4),
                                         read.count);
    }
}

bool Skipped(const LevelDraw &draw)
{
    const unsigned skipped = g_skipped.load(std::memory_order_relaxed);
    if (skipped == 0)
    {
        return false;
    }
    const std::wstring &name = g_variantNames[draw.game];
    unsigned kind = 8U; // fronds and leaves
    if (name.starts_with(L"terrain"))
    {
        kind = 1U;
    }
    else if (name.starts_with(L"static"))
    {
        kind = 2U;
    }
    else if (name.starts_with(L"spt_branch"))
    {
        kind = 4U;
    }
    return (skipped & kind) != 0;
}

// A developer's look at a shadow map: its depth as a picture (PGM, a sample
// in two each way), written to the temporary folder when a file
// `dump-shadow-maps` is beside the DLL, which is then taken away.
std::filesystem::path g_dumpAsked;

void DumpMap(IDirect3DDevice9 *device, IDirect3DSurface9 *map, const std::string &name)
{
    D3DSURFACE_DESC desc;
    std::memset(&desc, 0, sizeof desc);
    map->GetDesc(&desc);
    IDirect3DSurface9 *copy = nullptr;
    if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &copy,
                                                   nullptr)))
    {
        return;
    }
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(device->GetRenderTargetData(map, copy)) &&
        SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
    {
        std::ofstream out(std::filesystem::temp_directory_path() / ("elgara-" + name + ".pgm"), std::ios::binary);
        constexpr char kLine = '\n';
        out << "P5" << kLine << desc.Width / 2 << ' ' << desc.Height / 2 << kLine << "255" << kLine;
        for (UINT y = 0; y < desc.Height; y += 2)
        {
            const auto *row = reinterpret_cast<const std::uint16_t *>(static_cast<const std::byte *>(locked.pBits) +
                                                                      (static_cast<std::size_t>(y) * locked.Pitch));
            for (UINT x = 0; x < desc.Width; x += 2)
            {
                out.put(static_cast<char>(row[static_cast<std::size_t>(2) * x] >> 8));
            }
        }
        copy->UnlockRect();
        spdlog::info("level shadows: a map written as elgara-{}.pgm", name);
    }
    copy->Release();
}

// The kept level draws, into the shadow map the device draws into.
// The kept draws into the map the device has now, by g_partition: those that
// stay where they are (a cache's) or those that move (each frame's).
void DrawLevel(IDirect3DDevice9 *device, bool moving)
{
    SetState(device);
    Order();
    const LevelDraw *last = nullptr;
    for (const LevelDraw *draw : g_order)
    {
        if (draw->moving != moving || Outside(*draw) || (draw->cut && g_cut == nullptr) || Skipped(*draw))
        {
            continue;
        }
        ++g_drawnInto;
        if (g_drawingFor)
        {
            ++g_effectLevelDraws;
        }
        Bind(device, *draw, last);
        g_draw(device, draw->type, draw->base, draw->least, draw->vertices, draw->first, draw->count);
        last = draw;
    }
}

} // namespace

void Start(const std::filesystem::path &override, const std::filesystem::path &game, DrawIndexedFn draw)
{
    g_draw = draw;
    g_castCode = ReadFile(override / kCastShader);
    g_dumpAsked = override.parent_path() / "dump-shadow-maps";
    g_measurePixelCode = ReadFile(override / kMeasureShader);
    g_cutCode = ReadFile(override / kCutShader);
    g_copyVertexCode = ReadFile(override / kCopyVertexShader);
    g_copyPixelCode = ReadFile(override / kCopyPixelShader);
    const std::optional<dao::core::erf::Archive> archive =
        dao::core::erf::Archive::Open(game / "packages" / "core" / "data" / "shaders.erf");
    std::error_code error;
    for (const auto &file : std::filesystem::directory_iterator(override, error))
    {
        const std::wstring name = file.path().filename().wstring();
        const bool cut = name.starts_with(kCutVariantPrefix);
        if ((!name.starts_with(kVariantPrefix) && !cut) || !name.ends_with(L".vsh.fxo") || !archive)
        {
            continue;
        }
        const std::wstring theirName = name.substr(cut ? kCutVariantPrefix.size() : kVariantPrefix.size());
        const std::optional<std::vector<std::byte>> theirs = archive->Read(theirName);
        if (theirs)
        {
            g_variantCode[Fnv(*theirs)] = ReadFile(file.path());
            g_variantNames[Fnv(*theirs)] = theirName;
            if (cut)
            {
                g_cutVariants.insert(Fnv(*theirs));
            }
            g_measureCode[Fnv(*theirs)] = ReadFile(override / (std::wstring(kMeasurePrefix) + theirName));
        }
    }
    spdlog::info("level shadows: {} shadow variants of the level's vertex shaders{}", g_variantCode.size(),
                 g_castCode.empty() ? ", and no pixel shader to draw them with" : "");
}

void Want(bool on)
{
    if (g_wanted.exchange(on) != on)
    {
        spdlog::info("level shadows: the level is {}drawn into the sun's shadow maps", on ? "" : "not ");
    }
}

void Created(const DWORD *code, IDirect3DVertexShader9 *shader)
{
    if (code == nullptr || shader == nullptr)
    {
        return;
    }
    // A shader's address is the last made there's alone: the game makes its
    // shaders again as the device is reset (a change of resolution), and one
    // of another kind can be made where one of ours was known, which left the
    // game's figures out of the sun's shadows (2026-10-16).
    g_gameShaders.erase(shader);
    g_engineShadow.erase(shader);
    const std::vector<std::byte> bytes = CodeOf(code);
    if (const std::uint64_t hash = Fnv(bytes); g_variantCode.contains(hash))
    {
        GameShader game;
        game.hash = hash;
        game.toScreen = RegisterOf(bytes, "aur_mWorldViewProj");
        game.toWorld = RegisterOf(bytes, "aur_mWorld");
        if (const std::optional<UINT> model = RegisterOf(bytes, "aur_mWorld"))
        {
            game.placed = *model;
            game.placedCount = kMatrixRegisters;
        }
        else if (const std::optional<UINT> tree = RegisterOf(bytes, "g_vTreePos"))
        {
            game.placed = *tree;
            game.placedCount = 1;
        }
        g_gameShaders[shader] = game;
    }
    if (const std::optional<UINT> partition = RegisterOf(bytes, kEnginePartitionName))
    {
        EngineShader engine;
        engine.partition = *partition;
        engine.world = RegisterOf(bytes, "aur_mWorld");
        engine.registers = *partition + kMatrixRegisters;
        for (const shadercode::Constant &constant : ConstantsOf(bytes))
        {
            engine.registers = std::max(engine.registers, constant.first + constant.count);
        }
        engine.registers = std::min<UINT>(engine.registers, 256);
        g_engineShadow[shader] = engine;
        spdlog::info("level shadows: an engine shadow pass shader, its partition at c{}, {} registers read", *partition,
                     engine.registers);
    }
}

static bool DrawAtlas(IDirect3DDevice9 *device);
static void EffectsDrawing(IDirect3DDevice9 *device);
static void SetReceivers(IDirect3DDevice9 *device);
static void SetReceiversFor(IDirect3DDevice9 *device, bool picture);

// Lets go of what a figure's draw held.
static void Let(FigureDraw &draw)
{
    for (IUnknown *held : std::initializer_list<IUnknown *>{
             draw.vertex, draw.pixel, draw.declaration, draw.indices, draw.texture, draw.streams[0].buffer,
             draw.streams[1].buffer, draw.streams[2].buffer, draw.streams[3].buffer})
    {
        if (held != nullptr)
        {
            held->Release();
        }
    }
}

// Where a figure's draw stands and how far it reaches about that: its model's
// matrix's translation (aur_mWorld, at `world`), and the farthest of its bones
// from there (bsm_bones, three registers a bone from c0 to its model's
// matrix), or for a draw with no bones (a weapon, a shield) a reach any such
// thing is within. Only which maps it is drawn into is decided by it, so it is
// taken wide.
static Reach FigureReach(const std::array<float, 256 * 4> &constants, UINT world)
{
    const std::size_t at = static_cast<std::size_t>(world) * 4;
    Reach bounds{{constants.at(at + 3), constants.at(at + 7), constants.at(at + 11)}, 0};
    constexpr float kThingReach = 2.5F; // metres: a weapon, a shield, a staff
    constexpr float kFlesh = 1.0F;      // metres past the farthest bone
    constexpr float kBoneFarthest = 30.0F;
    bounds.reach = kThingReach;
    for (std::size_t bone = 0; (bone * 3) + 3 <= world; ++bone)
    {
        const float x = constants.at((bone * 12) + 3);
        const float y = constants.at((bone * 12) + 7);
        const float z = constants.at((bone * 12) + 11);
        const float d = std::sqrt((x * x) + (y * y) + (z * z));
        if (std::isfinite(d) && d < kBoneFarthest)
        {
            bounds.reach = std::max(bounds.reach, d + kFlesh);
        }
    }
    return bounds;
}

// The engine's draw of a figure into one of its maps, kept as the device has
// it, once: the engine draws the same figure into both its maps, or into one.
// A draw is known by its buffers, the part of them and where it is, its
// model's matrix, the same in both maps; one whose shader names none (a
// tree's), by every register its shader reads but the map's matrix. It is
// known before anything else is read of it: a key of all 256 registers,
// hashed for each of the 259 draws a frame in Denerim's market, and each
// draw read whole before it was found to be the other map's, took some 4.7 ms
// of the game's thread (issue 30).
static void NoteFigure(IDirect3DDevice9 *device, const EngineShader &engine, D3DPRIMITIVETYPE type, INT base,
                       UINT least, UINT vertices, UINT first, UINT count)
{
    const Timed timed(kNotePart);
    IDirect3DVertexBuffer9 *buffer = nullptr;
    UINT offset = 0;
    UINT stride = 0;
    device->GetStreamSource(0, &buffer, &offset, &stride);
    IDirect3DIndexBuffer9 *indices = nullptr;
    device->GetIndices(&indices);
    std::uint64_t key = FnvOn(kFnvStart, static_cast<const void *>(&buffer), sizeof(void *));
    key = FnvOn(key, static_cast<const void *>(&indices), sizeof(void *));
    for (const UINT part : {static_cast<UINT>(base), first, count, offset})
    {
        key = FnvOn(key, &part, sizeof part);
    }
    for (IUnknown *held : {static_cast<IUnknown *>(buffer), static_cast<IUnknown *>(indices)})
    {
        if (held != nullptr)
        {
            held->Release();
        }
    }
    std::array<float, kMatrixRegisters * 4> world{};
    if (engine.world)
    {
        device->GetVertexShaderConstantF(*engine.world, world.data(), kMatrixRegisters);
        key = FnvOn(key, world.data(), sizeof world);
    }
    else
    {
        std::array<float, 256 * 4> constants{};
        device->GetVertexShaderConstantF(0, constants.data(), engine.registers);
        std::fill_n(constants.begin() + (static_cast<std::ptrdiff_t>(engine.partition) * 4), kMatrixRegisters * 4,
                    0.0F);
        key = FnvOn(key, constants.data(), engine.registers * 4 * sizeof(float));
    }
    if (!g_figureKeys.insert(key).second)
    {
        return;
    }
    FigureDraw &draw = g_figureDraws.emplace_back();
    draw.key = key;
    draw.partition = engine.partition;
    draw.registers = engine.registers;
    draw.type = type;
    draw.base = base;
    draw.least = least;
    draw.vertices = vertices;
    draw.first = first;
    draw.count = count;
    device->GetVertexShader(&draw.vertex);
    device->GetPixelShader(&draw.pixel);
    device->GetVertexDeclaration(&draw.declaration);
    for (UINT at = 0; at < kStreams; ++at)
    {
        Stream &stream = draw.streams.at(at);
        device->GetStreamSource(at, &stream.buffer, &stream.offset, &stream.stride);
    }
    device->GetIndices(&draw.indices);
    device->GetTexture(0, &draw.texture);
    device->GetVertexShaderConstantF(0, draw.vertexConstants.data(), draw.registers);
    device->GetPixelShaderConstantF(0, draw.pixelConstants.data(), 16);
    for (std::size_t at = 0; at < kFigureStates.size(); ++at)
    {
        device->GetRenderState(kFigureStates.at(at), &draw.states.at(at));
    }
    if (engine.world)
    {
        draw.bounds = FigureReach(draw.vertexConstants, *engine.world);
    }
}

// An engine's draw of its shadow pass: the map it draws into, its matrix (the
// sun's frame where the lighting has said none), and the figure kept.
static void EngineDrawing(IDirect3DDevice9 *device, const EngineShader &engine, D3DPRIMITIVETYPE type, INT base,
                          UINT least, UINT vertices, UINT first, UINT count)
{
    IDirect3DSurface9 *target = nullptr;
    device->GetRenderTarget(0, &target);
    if (target != g_map)
    {
        if (g_map != nullptr)
        {
            g_map->Release();
        }
        g_map = target;
    }
    else if (target != nullptr)
    {
        target->Release();
    }
    device->GetVertexShaderConstantF(engine.partition, g_partition.data(), kMatrixRegisters);
    if (g_wanted.load(std::memory_order_relaxed) || g_effectsWanted.load(std::memory_order_relaxed))
    {
        NoteFigure(device, engine, type, base, least, vertices, first, count);
    }
    g_mapDrawn = true;
    g_passThisFrame = true;
    ++g_engineDraws;
}

// A draw into the picture: if it receives the sun's shadow, the atlas drawn
// at the first such draw of the frame, after the engine's shadow pass, and
// given to it; if it is the level's own, the draw kept.
// The atlas lent to the draw about to be made on `sampler`, read point by
// point; Drawn gives back what was there.
static void Lend(IDirect3DDevice9 *device, DWORD sampler, Lent &lent = g_lent, IDirect3DTexture9 *texture = g_atlas)
{
    lent.lent = true;
    lent.sampler = sampler;
    device->GetTexture(sampler, &lent.texture);
    for (std::size_t at = 0; at < kLentStates.size(); ++at)
    {
        device->GetSamplerState(sampler, kLentStates.at(at), &lent.states.at(at));
    }
    device->SetTexture(sampler, texture);
    SetSampler(device, sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    SetSampler(device, sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    SetSampler(device, sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    SetSampler(device, sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    SetSampler(device, sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

// A draw of the level's own into the picture kept, to be drawn into the
// shadow maps.
static void NoteLevel(IDirect3DDevice9 *device, IDirect3DVertexShader9 *shader, D3DPRIMITIVETYPE type, INT base,
                      UINT least, UINT vertices, UINT first, UINT count)
{
    const Timed timed(kNotePart);
    if (const auto game = g_gameShaders.find(shader); game != g_gameShaders.end())
    {
        if (IDirect3DVertexShader9 *variant = VariantOf(device, game->second.hash))
        {
            Note(device, variant, game->second, type, base, least, vertices, first, count);
        }
    }
}

// A receiver about to be drawn into the picture is lent the atlas, and the
// old aim's while the shadows fade; the atlas drawn first at the frame's
// first.
static void Receiving(IDirect3DDevice9 *device)
{
    IDirect3DPixelShader9 *pixel = nullptr;
    device->GetPixelShader(&pixel);
    if (pixel != nullptr)
    {
        pixel->Release();
        if (const auto receiver = g_receivers.find(pixel); receiver != g_receivers.end())
        {
            if (!g_atlasDrawn && (g_passThisFrame || !g_passLastFrame))
            {
                g_atlasDrawn = true;
                const Timed timed(kSunPart);
                gputimer::Begin(device, "sun atlas");
                g_atlasReady = DrawAtlas(device);
                gputimer::End(device);
                SetReceivers(device);
            }
            if (g_atlasReady)
            {
                Lend(device, receiver->second);
                if (const auto oldSampler = g_receiversFar.find(pixel);
                    g_fadeShare > 0 && g_oldAtlas != nullptr && oldSampler != g_receiversFar.end())
                {
                    Lend(device, oldSampler->second, g_lentOld, g_oldAtlas);
                }
            }
        }
    }
}

static void PictureDrawing(IDirect3DDevice9 *device, IDirect3DVertexShader9 *shader, D3DPRIMITIVETYPE type, INT base,
                           UINT least, UINT vertices, UINT first, UINT count)
{
    Receiving(device);
    NoteLevel(device, shader, type, base, least, vertices, first, count);
}

void Drawing(IDirect3DDevice9 *device, bool picture, D3DPRIMITIVETYPE type, INT base, UINT least, UINT vertices,
             UINT first, UINT count)
{
    IDirect3DVertexShader9 *shader = nullptr;
    device->GetVertexShader(&shader);
    if (shader == nullptr)
    {
        return;
    }
    if (const auto engine = g_engineShadow.find(shader); engine != g_engineShadow.end())
    {
        EngineDrawing(device, engine->second, type, base, least, vertices, first, count);
    }
    else if (picture)
    {
        if (g_wanted.load(std::memory_order_relaxed))
        {
            PictureDrawing(device, shader, type, base, least, vertices, first, count);
        }
        else if (g_effectsWanted.load(std::memory_order_relaxed))
        {
            NoteLevel(device, shader, type, base, least, vertices, first, count);
        }
        EffectsDrawing(device);
    }
    shader->Release();
}

void DrawingPlain(IDirect3DDevice9 *device, bool picture)
{
    if (picture && g_wanted.load(std::memory_order_relaxed))
    {
        Receiving(device);
    }
}

void UseSamplerState(SetSamplerStateFn set)
{
    g_setSampler = set;
}

// What was lent a draw given back.
static void GiveBack(IDirect3DDevice9 *device, Lent &lent)
{
    if (!lent.lent)
    {
        return;
    }
    device->SetTexture(lent.sampler, lent.texture);
    for (std::size_t at = 0; at < kLentStates.size(); ++at)
    {
        SetSampler(device, lent.sampler, kLentStates.at(at), lent.states.at(at));
    }
    if (lent.texture != nullptr)
    {
        lent.texture->Release();
    }
    lent = Lent{};
}

void Drawn(IDirect3DDevice9 *device)
{
    GiveBack(device, g_lent);
    GiveBack(device, g_lentEffects);
    GiveBack(device, g_lentOld);
}

static bool g_drawsPictureNow = true; // the device's first target is the frame's own picture

// The old aim as the receivers read it (kFadeRegister): a point turned by Q
// and placed by a new map's rows is where the old map has it, but for an
// offset across and down and a scale and offset in depth. A map's rows are its
// axes scaled, square to the sun (Cascades.h), so with A the new axes and A'
// the old, by rows, Q = A^T A' turns each new row into the old one.
static std::array<float, kFadeRegisters * 4> FadeData()
{
    std::array<float, kFadeRegisters * 4> data{};
    if (!g_fittedAxes || !g_oldAxes)
    {
        return data;
    }
    const std::array<cascades::Vector, 3> &now = *g_fittedAxes;
    const std::array<cascades::Vector, 3> &old = *g_oldAxes;
    for (std::size_t r = 0; r < 3; ++r)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            float sum = 0;
            for (std::size_t k = 0; k < 3; ++k)
            {
                sum += now.at(k).at(r) * old.at(k).at(c);
            }
            data.at((r * 4) + c) = sum;
        }
    }
    for (std::size_t map = 0; map < g_cascades.size(); ++map)
    {
        const std::optional<cascades::Fitted> &fitted = g_cascades.at(map);
        const std::optional<cascades::Fitted> &was = g_oldCascades.at(map);
        if (!fitted || !was)
        {
            continue;
        }
        const cascades::Matrix &t = fitted->texture;
        const cascades::Matrix &o = was->texture;
        const float deep = std::hypot(t[8], t[9], t[10]);
        const float scale = deep > 0 ? std::hypot(o[8], o[9], o[10]) / deep : 1;
        float *shift = data.data() + ((3 + map) * 4);
        shift[0] = o[3] - t[3];
        shift[1] = o[7] - t[7];
        shift[2] = scale;
        shift[3] = o[11] - (scale * t[11]);
    }
    return data;
}

// The receivers' registers: ours where the maps are fitted to the view and
// what is drawn reads them for the frame's own picture (`picture`), else off.
static void SetReceiversFor(IDirect3DDevice9 *device, bool picture)
{
    std::array<float, kReceiverRegisters * 4> data{};
    if (g_wanted.load(std::memory_order_relaxed) && picture && g_atlasReady && g_cascades[0])
    {
        // Each map's texture matrix's first three rows, by rows, to its own
        // tile's 0 to 1: they are square to the sun, and their fourth row is
        // 0, 0, 0, 1.
        for (std::size_t map = 0; map < g_cascades.size(); ++map)
        {
            if (const std::optional<cascades::Fitted> &fitted = g_cascades.at(map))
            {
                std::memcpy(data.data() + (map * 12), fitted->texture.data(), 12 * sizeof(float));
            }
        }
        data[48] = 1;
        // How far from the focus each map is read, as a share of its width:
        // a map's box holds the focus and its radius less half a step about it.
        data[49] = 0.5F - (kStepShare / 4);
        data[50] = g_fadeShare;
        if (g_fadeShare > 0)
        {
            device->SetPixelShaderConstantF(kFadeRegister, FadeData().data(), kFadeRegisters);
        }
        if (g_fittedAround)
        {
            std::memcpy(data.data() + 52, g_fittedAround->data(), sizeof(cascades::Vector));
        }
    }
    device->SetPixelShaderConstantF(kReceiverRegister, data.data(), kReceiverRegisters);
}

// The receivers' registers as they are to be now.
static void SetReceivers(IDirect3DDevice9 *device)
{
    SetReceiversFor(device, g_drawsPictureNow);
}

// The maps fitted to the view for this frame, at its first draw of the
// engine's shadow pass, by the sun's frame of the engine's own matrix, now in
// g_partition; and this draw's map's put in its place.
// The copying quad's shaders and vertex layout, made once.
static bool MakeCopying(IDirect3DDevice9 *device)
{
    if (g_copyVertex == nullptr && !g_copyVertexCode.empty() &&
        FAILED(device->CreateVertexShader(reinterpret_cast<const DWORD *>(g_copyVertexCode.data()), &g_copyVertex)))
    {
        g_copyVertex = nullptr;
    }
    if (g_copyPixel == nullptr && !g_copyPixelCode.empty() &&
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD *>(g_copyPixelCode.data()), &g_copyPixel)))
    {
        g_copyPixel = nullptr;
    }
    if (g_cast == nullptr && !g_castCode.empty() &&
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD *>(g_castCode.data()), &g_cast)))
    {
        g_cast = nullptr;
    }
    if (g_cut == nullptr && !g_cutCode.empty() &&
        FAILED(device->CreatePixelShader(reinterpret_cast<const DWORD *>(g_cutCode.data()), &g_cut)))
    {
        g_cut = nullptr;
    }
    if (g_copyDeclaration == nullptr)
    {
        const std::array<D3DVERTEXELEMENT9, 3> elements{
            D3DVERTEXELEMENT9{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            D3DVERTEXELEMENT9{0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0}};
        if (FAILED(device->CreateVertexDeclaration(elements.data(), &g_copyDeclaration)))
        {
            g_copyDeclaration = nullptr;
        }
    }
    return g_copyVertex != nullptr && g_copyPixel != nullptr && g_copyDeclaration != nullptr && g_cast != nullptr;
}

// The cache's texture and depth made for a map, of its size and format, and
// a target of its colours if `coloured`.
static bool MakeCache(IDirect3DDevice9 *device, Cache &cache, bool coloured)
{
    if (cache.texture != nullptr && cache.depth != nullptr && (cache.colours != nullptr || !coloured))
    {
        return true;
    }
    if (FAILED(device->CreateTexture(kTile, kTile, 1, D3DUSAGE_RENDERTARGET, kMapFormat, D3DPOOL_DEFAULT,
                                     &cache.texture, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(kTile, kTile, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &cache.depth,
                                                 nullptr)) ||
        (coloured && FAILED(device->CreateTexture(kTile, kTile, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                                  D3DPOOL_DEFAULT, &cache.colours, nullptr))))
    {
        return false;
    }
    cache.drawnWith = ~std::uint64_t{0};
    return true;
}

// The level drawn into a cache by the map's matrix now in g_partition.
static void DrawCache(IDirect3DDevice9 *device, Cache &cache)
{
    IDirect3DSurface9 *surface = nullptr;
    if (FAILED(cache.texture->GetSurfaceLevel(0, &surface)))
    {
        return;
    }
    IDirect3DSurface9 *colours = nullptr;
    if (cache.colours != nullptr && FAILED(cache.colours->GetSurfaceLevel(0, &colours)))
    {
        colours = nullptr;
    }
    std::array<IDirect3DSurface9 *, 2> targets{};
    IDirect3DSurface9 *depth = nullptr;
    device->GetRenderTarget(0, targets.data());
    device->GetRenderTarget(1, targets.data() + 1);
    device->GetDepthStencilSurface(&depth);
    device->SetRenderTarget(0, surface);
    device->SetRenderTarget(1, colours);
    device->SetDepthStencilSurface(cache.depth);
    device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1, 0);
    DrawLevel(device, false);
    device->SetRenderTarget(0, targets[0]);
    device->SetRenderTarget(1, targets[1]);
    device->SetDepthStencilSurface(depth);
    for (IDirect3DSurface9 *held : {surface, colours, targets[0], targets[1], depth})
    {
        if (held != nullptr)
        {
            held->Release();
        }
    }
    cache.drawnBy = g_partition;
    cache.drawnWith = g_seenGeneration;
    cache.drawnSkipping = g_skipped.load(std::memory_order_relaxed);
    ++g_cacheDrawn;
}

// The corners of a quad over a whole target, each with where it reads a
// texture: Direct3D 9's pixel centres are on the texels' corners, so the quad
// is put half a target's pixel up and left, and its reading `half` of a
// texture's texel on, to read each texel at its middle.
namespace
{
struct Corner
{
    float px, py, pz, pw, u, v;
};
} // namespace
static std::array<Corner, 4> Quad(UINT width, UINT height, float half)
{
    const float x = 1.0F / static_cast<float>(width);
    const float y = 1.0F / static_cast<float>(height);
    return {Corner{-1 - x, 1 + y, 0.5F, 1, half, half}, Corner{1 - x, 1 + y, 0.5F, 1, 1 + half, half},
            Corner{-1 - x, -1 + y, 0.5F, 1, half, 1 + half}, Corner{1 - x, -1 + y, 0.5F, 1, 1 + half, 1 + half}};
}

// The copy's shaders, a texture point sampled on sampler 0, and no blending,
// testing or culling.
static void SetCopying(IDirect3DDevice9 *device, IDirect3DTexture9 *texture)
{
    device->SetVertexDeclaration(g_copyDeclaration);
    device->SetVertexShader(g_copyVertex);
    device->SetPixelShader(g_copyPixel);
    device->SetTexture(0, texture);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
}

// A cache's texture sampled at a texel in every kReflectStep into `target`:
// each pixel reads the map's texel kReflectStep * i + kReflectStep / 2, whose
// place Reflected.cpp's Make takes it at.
static void Sample(IDirect3DDevice9 *device, IDirect3DTexture9 *texture, UINT texels, IDirect3DSurface9 *target)
{
    const UINT size = texels / kReflectStep;
    device->SetRenderTarget(0, target);
    const D3DVIEWPORT9 viewport{0, 0, size, size, 0, 1};
    device->SetViewport(&viewport);
    SetCopying(device, texture);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    const std::array<Corner, 4> quad = Quad(size, size, 0.5F / static_cast<float>(texels));
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad.data(), sizeof(Corner));
}

// A cache just drawn, its depths and colours sampled into a free slot, for
// its patches to be made once the GPU has drawn them (MakePatches); with no
// slot free, not this time. The engine's
// maps are G16R16, depth in red; another format has no patches.
static void Reflect(IDirect3DDevice9 *device, const Cache &cache, Reflecting &reflecting)
{
    D3DSURFACE_DESC desc;
    std::memset(&desc, 0, sizeof desc);
    cache.texture->GetLevelDesc(0, &desc);
    if (cache.colours == nullptr || desc.Format != D3DFMT_G16R16 || desc.Width != desc.Height ||
        desc.Width % kReflectStep != 0)
    {
        return;
    }
    const UINT size = desc.Width / kReflectStep;
    ReflectSlot *slot = nullptr;
    for (ReflectSlot &each : reflecting.slots)
    {
        if (!each.waiting)
        {
            slot = &each;
            break;
        }
    }
    if (slot == nullptr)
    {
        return;
    }
    if (slot->target == nullptr && FAILED(device->CreateRenderTarget(size, size, desc.Format, D3DMULTISAMPLE_NONE, 0,
                                                                     TRUE, &slot->target, nullptr)))
    {
        slot->target = nullptr;
        return;
    }
    if (slot->colourTarget == nullptr &&
        FAILED(device->CreateRenderTarget(size, size, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                          &slot->colourTarget, nullptr)))
    {
        slot->colourTarget = nullptr;
        return;
    }
    std::array<IDirect3DSurface9 *, 2> targets{};
    IDirect3DSurface9 *depth = nullptr;
    device->GetRenderTarget(0, targets.data());
    device->GetRenderTarget(1, targets.data() + 1);
    device->GetDepthStencilSurface(&depth);
    device->SetRenderTarget(1, nullptr);
    device->SetDepthStencilSurface(nullptr);
    Sample(device, cache.texture, desc.Width, slot->target);
    Sample(device, cache.colours, desc.Width, slot->colourTarget);
    slot->waiting = cache.drawnBy;
    slot->texels = static_cast<int>(desc.Width);
    slot->order = ++g_reflectOrder;
    device->SetRenderTarget(0, targets[0]);
    device->SetRenderTarget(1, targets[1]);
    device->SetDepthStencilSurface(depth);
    for (IDirect3DSurface9 *held : {targets[0], targets[1], depth})
    {
        if (held != nullptr)
        {
            held->Release();
        }
    }
}

// The texels of a lockable target, each a 32-bit word, by rows; none while
// the GPU is still drawing it, and an empty one where it cannot be read.
static std::optional<std::vector<std::uint32_t>> Texels(IDirect3DSurface9 *target)
{
    D3DSURFACE_DESC desc;
    std::memset(&desc, 0, sizeof desc);
    target->GetDesc(&desc);
    D3DLOCKED_RECT locked;
    const HRESULT result = target->LockRect(&locked, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
    if (result == D3DERR_WASSTILLDRAWING)
    {
        return std::nullopt;
    }
    if (FAILED(result))
    {
        return std::vector<std::uint32_t>{};
    }
    std::vector<std::uint32_t> texels(static_cast<std::size_t>(desc.Width) * desc.Height);
    for (std::size_t row = 0; row < desc.Height; ++row)
    {
        std::memcpy(texels.data() + (row * desc.Width),
                    static_cast<const std::byte *>(locked.pBits) + (row * static_cast<std::size_t>(locked.Pitch)),
                    desc.Width * sizeof(std::uint32_t));
    }
    target->UnlockRect();
    return texels;
}

// A map's patches from a sample's depths and colours.
static void MakePatchesOf(std::size_t map, const std::vector<std::uint32_t> &depthTexels,
                          const std::vector<std::uint32_t> &colourTexels, int texels, const cascades::Matrix &clip)
{
    // G16R16: red, the depth, in the low half. A8R8G8B8: blue in the low byte.
    constexpr float kOfShort = 65535;
    constexpr float kOfByte = 255;
    std::vector<float> depths(depthTexels.size());
    std::vector<std::array<float, 3>> colours(colourTexels.size());
    for (std::size_t at = 0; at < depthTexels.size(); ++at)
    {
        depths[at] = static_cast<float>(depthTexels[at] & 0xFFFFU) / kOfShort;
        const std::uint32_t colour = colourTexels[at];
        colours[at] = {static_cast<float>((colour >> 16U) & 0xFFU) / kOfByte,
                       static_cast<float>((colour >> 8U) & 0xFFU) / kOfByte,
                       static_cast<float>(colour & 0xFFU) / kOfByte};
    }
    const auto size = static_cast<int>(std::lround(std::sqrt(static_cast<double>(depths.size()))));
    if (std::optional<reflected::Patches> patches = reflected::Make(depths, colours, size, kReflectStep, texels, clip))
    {
        reflected::Keep(map, std::make_shared<const reflected::Patches>(std::move(*patches)));
    }
}

// The patches of the samples the GPU has drawn, kept for the figures.
static void MakePatches()
{
    for (std::size_t map = 0; map < g_reflecting.size(); ++map)
    {
        // The slots drawn, oldest first: the newest read keeps its patches. One
        // not drawn yet stops the rest, drawn after it.
        std::vector<ReflectSlot *> waiting;
        for (ReflectSlot &slot : g_reflecting.at(map).slots)
        {
            if (slot.waiting)
            {
                waiting.push_back(&slot);
            }
        }
        std::ranges::sort(waiting, {}, &ReflectSlot::order);
        for (ReflectSlot *slot : waiting)
        {
            std::optional<std::vector<std::uint32_t>> depthTexels = Texels(slot->target);
            if (!depthTexels)
            {
                break;
            }
            std::optional<std::vector<std::uint32_t>> colourTexels = Texels(slot->colourTarget);
            if (!colourTexels)
            {
                break;
            }
            const cascades::Matrix clip = *slot->waiting;
            slot->waiting.reset();
            if (depthTexels->empty() || colourTexels->size() != depthTexels->size())
            {
                continue;
            }
            MakePatchesOf(map, *depthTexels, *colourTexels, slot->texels, clip);
        }
    }
}

// Each map's depth along the sun: the least and most of the level's measured
// bounds that fall in the map's square, reckoned again when the map moves or
// more of the level is seen, or the sun turns: under a day and night a map
// whose place comes out the same after a turn of the sun would otherwise keep
// the last way's depth (2026-10-22). A surface outside a
// map's depth is read from the next (shadow.hlsl), and one nearer the sun
// than all of it, read as lit, was a bright patch on a hillside (2026-10-16).
static std::optional<std::array<float, 2>> DepthAlong(std::size_t map, const cascades::Matrix &clip,
                                                      const cascades::Vector &light)
{
    Along &along = g_along.at(map);
    const std::array<float, 2> place{clip[3], clip[7]};
    if (along.place == place && along.light == light && along.seen == g_seenGeneration)
    {
        return along.span;
    }
    float least = std::numeric_limits<float>::infinity();
    float most = -std::numeric_limits<float>::infinity();
    for (const auto &[key, draw] : g_seen)
    {
        if (!draw.bounds)
        {
            continue;
        }
        const std::array<float, 6> &b = *draw.bounds;
        std::array<float, 6> span{1e30F, -1e30F, 1e30F, -1e30F, 1e30F, -1e30F}; // x, y in the map, way along the sun
        for (int corner = 0; corner < 8; ++corner)
        {
            const cascades::Vector p{b[(corner & 1) != 0 ? 3 : 0], b[(corner & 2) != 0 ? 4 : 1],
                                     b[(corner & 4) != 0 ? 5 : 2]};
            const cascades::Vector at = cascades::Transformed(clip, p);
            const float way = (light[0] * p[0]) + (light[1] * p[1]) + (light[2] * p[2]);
            span[0] = std::min(span[0], at[0]);
            span[1] = std::max(span[1], at[0]);
            span[2] = std::min(span[2], at[1]);
            span[3] = std::max(span[3], at[1]);
            span[4] = std::min(span[4], way);
            span[5] = std::max(span[5], way);
        }
        if (span[1] < -1 || span[0] > 1 || span[3] < -1 || span[2] > 1)
        {
            continue;
        }
        least = std::min(least, span[4]);
        most = std::max(most, span[5]);
    }
    along.place = place;
    along.light = light;
    along.seen = g_seenGeneration;
    along.span = least <= most ? std::optional<std::array<float, 2>>(std::array<float, 2>{least, most}) : std::nullopt;
    return along.span;
}

// The four maps for this frame, around the camera's focus (Focus), or its eye
// where it has none, square to the sun the lighting has (Sun): the engine's
// own maps are fitted to its figures afresh each frame, their frame off by
// some 1e-9 from one frame to the next as the figures move, and a map drawn by
// a matrix not the same as its cache's is drawn again (Redcliffe, each frame,
// 2026-10-16).
static bool FitMaps()
{
    std::optional<std::array<cascades::Vector, 3>> axes;
    std::optional<cascades::Vector> centre;
    {
        const std::scoped_lock lock(g_sunLock);
        axes = g_sunAxes;
        centre = g_focus;
    }
    // The sun aimed anew (Sun): the maps as they were, kept to fade from, and
    // their level's caches set aside for the new aim to draw the other set.
    const bool ours = axes.has_value();
    if (ours && g_fittedAxes && *g_fittedAxes != *axes && g_cascades[0] && !g_fading.load())
    {
        g_oldCascades = g_cascades;
        g_oldAxes = g_fittedAxes;
        std::swap(g_caches, g_oldCaches);
        g_fadeFrom = std::chrono::steady_clock::now();
        g_fading.store(true);
    }
    g_fittedAxes = ours ? axes : std::nullopt;
    if (!axes)
    {
        axes = cascades::AxesOf(g_partition);
    }
    if (!centre && g_haveCamera)
    {
        centre = cascades::Eye(g_toScreen);
    }
    g_fittedAround = centre;
    if (!centre || g_tile == 0)
    {
        return false;
    }
    for (std::size_t map = 0; map < g_cascades.size(); ++map)
    {
        const auto texels = static_cast<int>(g_tile);
        const std::optional<cascades::Fitted> square =
            cascades::FitAround(*centre, *axes, kRadii.at(map), texels, kStepShare);
        g_cascades.at(map) = square ? cascades::FitAround(*centre, *axes, kRadii.at(map), texels, kStepShare,
                                                          DepthAlong(map, square->clip, (*axes)[2]))
                                    : std::nullopt;
    }
    return g_cascades[0].has_value();
}

// The atlas and its depth, made once (and again after a device's reset).
static bool MakeAtlas(IDirect3DDevice9 *device)
{
    if (g_atlas != nullptr)
    {
        return true;
    }
    if (FAILED(device->CreateTexture(kTile * 2, kTile * 2, 1, D3DUSAGE_RENDERTARGET, kMapFormat, D3DPOOL_DEFAULT,
                                     &g_atlas, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(kTile * 2, kTile * 2, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                 &g_atlasDepth, nullptr)))
    {
        spdlog::warn("level shadows: the atlas of {0}x{0} could not be made", kTile * 2);
        for (IUnknown **held : {reinterpret_cast<IUnknown **>(&g_atlas), reinterpret_cast<IUnknown **>(&g_atlasDepth)})
        {
            if (*held != nullptr)
            {
                (*held)->Release();
                *held = nullptr;
            }
        }
        return false;
    }
    g_tile = kTile;
    return true;
}

// A figure's draw drawn again by a map's matrix.
static void Replay(IDirect3DDevice9 *device, const FigureDraw &draw, const cascades::Matrix &clip,
                   bool bothSides = false)
{
    device->SetVertexShader(draw.vertex);
    device->SetPixelShader(draw.pixel);
    device->SetVertexDeclaration(draw.declaration);
    for (UINT at = 0; at < kStreams; ++at)
    {
        const Stream &stream = draw.streams.at(at);
        device->SetStreamSource(at, stream.buffer, stream.offset, stream.stride);
    }
    device->SetIndices(draw.indices);
    device->SetTexture(0, draw.texture);
    device->SetVertexShaderConstantF(0, draw.vertexConstants.data(), draw.registers);
    device->SetVertexShaderConstantF(draw.partition, clip.data(), kMatrixRegisters);
    device->SetPixelShaderConstantF(0, draw.pixelConstants.data(), 16);
    for (std::size_t at = 0; at < kFigureStates.size(); ++at)
    {
        device->SetRenderState(kFigureStates.at(at), draw.states.at(at));
    }
    if (bothSides)
    {
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    }
    g_draw(device, draw.type, draw.base, draw.least, draw.vertices, draw.first, draw.count);
}

// Each map's cache drawn again where its map has moved or more is seen.
static void KeepCaches(IDirect3DDevice9 *device)
{
    for (std::size_t map = 0; map < g_caches.size(); ++map)
    {
        Cache &cache = g_caches.at(map);
        const std::optional<cascades::Fitted> &fitted = g_cascades.at(map);
        if (!fitted || !MakeCache(device, cache, map < g_reflecting.size()))
        {
            continue;
        }
        if (cache.drawnBy == fitted->clip && cache.drawnWith == g_seenGeneration &&
            cache.drawnSkipping == g_skipped.load(std::memory_order_relaxed))
        {
            continue;
        }
        if (!g_measuredThisFrame)
        {
            Measure(device);
            g_measuredThisFrame = true;
        }
        g_partition = fitted->clip;
        DrawCache(device, cache);
        if (map < g_reflecting.size())
        {
            Reflect(device, cache, g_reflecting.at(map));
        }
    }
}

// Whether a figure's bounds are wholly off a sun's map, across it: the map
// looks along the sun, so what is off its square casts nothing into it. Every
// figure was replayed into all four maps, the finest 6 m across (issue 30).
// Its depth is not tested: what is nearer the sun than the map's depth casts
// all the same.
static bool OffMap(const Reach &bounds, const cascades::Matrix &clip)
{
    const auto &[x, y, z] = bounds.centre;
    for (std::size_t row = 0; row < 2; ++row)
    {
        const float *r = clip.data() + (row * 4);
        const float at = (r[0] * x) + (r[1] * y) + (r[2] * z) + r[3];
        const float scale = std::sqrt((r[0] * r[0]) + (r[1] * r[1]) + (r[2] * r[2]));
        if (std::abs(at) > 1 + (bounds.reach * scale))
        {
            return true;
        }
    }
    return false;
}

// One tile of the atlas: its cache, with its depth, then what moves and every
// figure, by the map's matrix, where they are nearer the sun than what is there.
static void DrawTile(IDirect3DDevice9 *device, std::size_t map, const std::optional<cascades::Fitted> &fitted,
                     const Cache &cache)
{
    if (!fitted || cache.texture == nullptr)
    {
        return;
    }
    const D3DVIEWPORT9 viewport{
        static_cast<DWORD>((map % 2) * g_tile), static_cast<DWORD>((map / 2) * g_tile), g_tile, g_tile, 0, 1};
    device->SetViewport(&viewport);
    SetCopying(device, cache.texture);
    device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
    const std::array<Corner, 4> quad = Quad(g_tile, g_tile, 0);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad.data(), sizeof(Corner));
    g_partition = fitted->clip;
    DrawLevel(device, true);
    device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    if ((g_skipped.load(std::memory_order_relaxed) & kSkipFigures) != 0)
    {
        return;
    }
    for (const FigureDraw &draw : g_figureDraws)
    {
        if (!draw.bounds || !OffMap(*draw.bounds, fitted->clip))
        {
            Replay(device, draw, fitted->clip);
            ++g_figureReplays;
        }
    }
}

// The old aim's atlas drawn for this frame where the shadows fade: its level
// as cached and its figures by its own maps (g_oldCascades), on the atlas's
// depth, which the frame's own atlas is done with. The fade ended, the old
// aim is let go.
static void DrawOldAtlas(IDirect3DDevice9 *device)
{
    g_fadeShare = 0;
    if (!g_fading.load())
    {
        return;
    }
    const float seconds = g_fadeFrom
                              ? std::chrono::duration<float>(std::chrono::steady_clock::now() - *g_fadeFrom).count()
                              : kFadeSeconds;
    if (seconds >= kFadeSeconds || !g_oldCascades[0])
    {
        g_oldCascades = {};
        g_fading.store(false);
        return;
    }
    if (g_oldAtlas == nullptr && FAILED(device->CreateTexture(kTile * 2, kTile * 2, 1, D3DUSAGE_RENDERTARGET,
                                                              kMapFormat, D3DPOOL_DEFAULT, &g_oldAtlas, nullptr)))
    {
        g_oldAtlas = nullptr;
        return;
    }
    IDirect3DSurface9 *old = nullptr;
    if (FAILED(g_oldAtlas->GetSurfaceLevel(0, &old)))
    {
        return;
    }
    device->SetRenderTarget(0, old);
    device->SetDepthStencilSurface(g_atlasDepth);
    for (std::size_t map = 0; map < g_oldCascades.size(); ++map)
    {
        DrawTile(device, map, g_oldCascades.at(map), g_oldCaches.at(map));
    }
    old->Release();
    g_fadeShare = std::max(seconds / kFadeSeconds, 1e-3F);
}

// The atlas drawn for this frame, after the engine's shadow pass: the maps
// fitted, their caches kept, and each tile given its cache, what moves and
// the figures. False where there is nothing to draw it by.
static bool DrawAtlas(IDirect3DDevice9 *device)
{
    if (!MakeAtlas(device) || !MakeCopying(device) || !FitMaps())
    {
        return false;
    }
    const keptstate::Kept kept(device, g_saved);
    if (!kept)
    {
        return false;
    }
    KeepCaches(device);
    IDirect3DSurface9 *atlas = nullptr;
    if (SUCCEEDED(g_atlas->GetSurfaceLevel(0, &atlas)))
    {
        device->SetRenderTarget(0, atlas);
        device->SetRenderTarget(1, nullptr);
        device->SetDepthStencilSurface(g_atlasDepth);
        for (std::size_t map = 0; map < g_cascades.size(); ++map)
        {
            DrawTile(device, map, g_cascades.at(map), g_caches.at(map));
        }
        std::error_code error;
        if (!g_dumpAsked.empty() && std::filesystem::exists(g_dumpAsked, error))
        {
            DumpMap(device, atlas, "atlas");
            std::filesystem::remove(g_dumpAsked, error);
        }
        atlas->Release();
    }
    DrawOldAtlas(device);
    return true;
}

// The effects' atlas and its depth let go.
static void ReleaseEffectAtlas()
{
    g_effectLevelRows = {};
    for (IUnknown **held :
         {reinterpret_cast<IUnknown **>(&g_effectAtlas), reinterpret_cast<IUnknown **>(&g_effectDepth),
          reinterpret_cast<IUnknown **>(&g_effectLevel), reinterpret_cast<IUnknown **>(&g_effectLevelDepth)})
    {
        if (*held != nullptr)
        {
            (*held)->Release();
            *held = nullptr;
        }
    }
}

// The effects' atlas and its depth, made once (and again after a reset).
static bool MakeEffectAtlas(IDirect3DDevice9 *device)
{
    if (g_effectAtlas != nullptr)
    {
        return true;
    }
    if (FAILED(device->CreateTexture(kEffectTile * 6, kEffectTile * kEffectLightsMost, 1, D3DUSAGE_RENDERTARGET,
                                     kMapFormat, D3DPOOL_DEFAULT, &g_effectAtlas, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(kEffectTile * 6, kEffectTile * kEffectLightsMost, D3DFMT_D24X8,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &g_effectDepth, nullptr)) ||
        FAILED(device->CreateTexture(kEffectTile * 6, kEffectTile * kEffectLightsMost, 1, D3DUSAGE_RENDERTARGET,
                                     kMapFormat, D3DPOOL_DEFAULT, &g_effectLevel, nullptr)) ||
        FAILED(device->CreateDepthStencilSurface(kEffectTile * 6, kEffectTile * kEffectLightsMost, D3DFMT_D24X8,
                                                 D3DMULTISAMPLE_NONE, 0, TRUE, &g_effectLevelDepth, nullptr)))
    {
        spdlog::warn("effects' shadows: the atlas could not be made");
        ReleaseEffectAtlas();
        return false;
    }
    g_effectLevelRows = {};
    spdlog::info("effects' shadows: their atlas and its cache made, {} by {}", kEffectTile * 6,
                 kEffectTile * kEffectLightsMost);
    return true;
}

// The matrix of one face of a light's cube, by rows, as the engine's shadow
// pass takes it (g_mPSSMPartitionMatrix): x and y the world's other two axes
// in order, w the distance along the face's axis, z made a + b w so that the
// depth over w is a + b / w, 0 at kEffectNear and 1 at the reach.
static cascades::Matrix EffectFace(const std::array<float, 4> &light, std::size_t face)
{
    const std::size_t axis = face / 2;
    const float sign = face % 2 == 0 ? 1.0F : -1.0F;
    const float reachOf = light[3];
    const float a = reachOf / (reachOf - kEffectNear);
    const float b = -kEffectNear * a;
    // A row that reads component c of the way from the light: e_c, less the light's place.
    const auto along = [&](std::size_t c, float times) {
        std::array<float, 4> row{};
        row.at(c) = times;
        row[3] = -light.at(c) * times;
        return row;
    };
    constexpr std::array<std::array<std::size_t, 2>, 3> kAcross{{{1, 2}, {0, 2}, {0, 1}}};
    const std::array<std::size_t, 2> &across = kAcross.at(axis);
    const std::array<float, 4> x = along(across[0], 1);
    const std::array<float, 4> y = along(across[1], 1);
    const std::array<float, 4> w = along(axis, sign);
    std::array<float, 4> z{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        z.at(i) = a * w.at(i);
    }
    z[3] += b;
    cascades::Matrix clip{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        clip.at(i) = x.at(i);
        clip.at(4 + i) = y.at(i);
        clip.at(8 + i) = z.at(i);
        clip.at(12 + i) = w.at(i);
    }
    return clip;
}

// Whether a figure's bounds are wholly out of one face of a light's cube
// (EffectFace's): behind one of the face's four sides, each a quarter turn
// from its way. A figure was replayed into all six (issue 30).
static bool OffFace(const Reach &bounds, const std::array<float, 4> &light, std::size_t face)
{
    const std::size_t axis = face / 2;
    const float sign = face % 2 == 0 ? 1.0F : -1.0F;
    const std::array<float, 3> way{bounds.centre[0] - light[0], bounds.centre[1] - light[1],
                                   bounds.centre[2] - light[2]};
    const float ahead = sign * way.at(axis);
    // A side's plane through the light, its normal the face's way less the
    // other axis's, over the square root of 2: the centre's distance from it
    // against the reach.
    const float slack = bounds.reach * std::numbers::sqrt2_v<float>;
    for (std::size_t other = 0; other < 3; ++other)
    {
        if (other != axis && ahead - std::abs(way.at(other)) < -slack)
        {
            return true;
        }
    }
    return false;
}

// A face's place in a row of an atlas.
static D3DVIEWPORT9 EffectFaceView(std::size_t row, std::size_t face)
{
    return {
        static_cast<DWORD>(face * kEffectTile), static_cast<DWORD>(row * kEffectTile), kEffectTile, kEffectTile, 0, 1};
}

// One light's six maps of the level's own surfaces within its reach (a wall
// between the light and what is behind it), in a row of the level's cache,
// whose target is set.
static void DrawEffectLevel(IDirect3DDevice9 *device, std::size_t row, const std::array<float, 4> &place)
{
    LARGE_INTEGER start{};
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&start);
    for (std::size_t face = 0; face < 6; ++face)
    {
        const D3DVIEWPORT9 viewport = EffectFaceView(row, face);
        device->SetViewport(&viewport);
        device->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0F, 0);
        g_partition = EffectFace(place, face);
        g_drawingFor = place;
        DrawLevel(device, false);
        g_drawingFor.reset();
    }
    QueryPerformanceCounter(&end);
    g_effectLevelTicks += end.QuadPart - start.QuadPart;
    ++g_effectLevelRowsDrawn;
}

// A row of the level's cache copied into a light's row of the atlas, its
// depths into the atlas's depth as well, for the figures to be tested against.
static void CopyEffectLevel(IDirect3DDevice9 *device, std::size_t from, std::size_t to)
{
    const D3DVIEWPORT9 viewport{0, static_cast<DWORD>(to * kEffectTile), kEffectTile * 6, kEffectTile, 0, 1};
    device->SetViewport(&viewport);
    const float halfU = 0.5F / static_cast<float>(kEffectTile * 6);
    const float halfV = 0.5F / static_cast<float>(kEffectTile * kEffectLightsMost);
    const float top = static_cast<float>(from) / static_cast<float>(kEffectLightsMost);
    const float bottom = static_cast<float>(from + 1) / static_cast<float>(kEffectLightsMost);
    const float x = 1.0F / static_cast<float>(kEffectTile * 6);
    const float y = 1.0F / static_cast<float>(kEffectTile);
    const std::array<Corner, 4> quad{Corner{-1 - x, 1 + y, 0.5F, 1, halfU, top + halfV},
                                     Corner{1 - x, 1 + y, 0.5F, 1, 1 + halfU, top + halfV},
                                     Corner{-1 - x, -1 + y, 0.5F, 1, halfU, bottom + halfV},
                                     Corner{1 - x, -1 + y, 0.5F, 1, 1 + halfU, bottom + halfV}};
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad.data(), sizeof(Corner));
}

// One light's six maps' moving level draws and figures, over its level copied
// into its row: the figures whose bounds come within its reach and the face.
// A draw that names no place (a tree's) is left out.
static void DrawEffectLight(IDirect3DDevice9 *device, std::size_t light, const std::array<float, 4> &place)
{
    std::vector<const FigureDraw *> reached;
    for (const FigureDraw &draw : g_figureDraws)
    {
        if (!draw.bounds)
        {
            continue;
        }
        const float x = draw.bounds->centre[0] - place[0];
        const float y = draw.bounds->centre[1] - place[1];
        const float z = draw.bounds->centre[2] - place[2];
        const float within = place[3] + draw.bounds->reach;
        if ((x * x) + (y * y) + (z * z) <= within * within)
        {
            reached.push_back(&draw);
        }
    }
    for (std::size_t face = 0; face < 6; ++face)
    {
        const D3DVIEWPORT9 viewport = EffectFaceView(light, face);
        device->SetViewport(&viewport);
        ++g_effectFaces;
        const cascades::Matrix clip = EffectFace(place, face);
        g_partition = clip;
        g_drawingFor = place;
        LARGE_INTEGER start{};
        LARGE_INTEGER middle{};
        LARGE_INTEGER end{};
        QueryPerformanceCounter(&start);
        DrawLevel(device, true);
        g_drawingFor.reset();
        QueryPerformanceCounter(&middle);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
        for (const FigureDraw *draw : reached)
        {
            if (!OffFace(*draw->bounds, place, face))
            {
                Replay(device, *draw, clip, true);
                ++g_effectFigureReplays;
            }
        }
        QueryPerformanceCounter(&end);
        g_effectLevelTicks += middle.QuadPart - start.QuadPart;
        g_effectFigureTicks += end.QuadPart - middle.QuadPart;
    }
}

// The rows of the level's cache the lights are drawn from, each light's kept
// where the level seen is the same and drawn again where it is not, into a
// row no other light of the frame takes.
static std::vector<std::size_t> EffectLevelRows(IDirect3DDevice9 *device,
                                                const std::vector<std::array<float, 4>> &lights)
{
    ++g_effectFrame;
    const unsigned skipping = g_skipped.load(std::memory_order_relaxed);
    const std::size_t count = std::min(lights.size(), kEffectLightsMost);
    std::vector<std::size_t> rows(count, kEffectLightsMost);
    // Those kept.
    for (std::size_t light = 0; light < count; ++light)
    {
        for (std::size_t row = 0; row < g_effectLevelRows.size(); ++row)
        {
            EffectLevelRow &kept = g_effectLevelRows.at(row);
            if (kept.light == lights[light] && kept.seen == g_seenGeneration && kept.skipping == skipping &&
                kept.used != g_effectFrame && g_effectCache.load(std::memory_order_relaxed))
            {
                kept.used = g_effectFrame;
                rows[light] = row;
                break;
            }
        }
    }
    // The rest drawn, each into the row least lately used.
    IDirect3DSurface9 *level = nullptr;
    for (std::size_t light = 0; light < count; ++light)
    {
        if (rows[light] != kEffectLightsMost)
        {
            continue;
        }
        if (level == nullptr)
        {
            if (FAILED(g_effectLevel->GetSurfaceLevel(0, &level)))
            {
                return {};
            }
            device->SetRenderTarget(0, level);
            device->SetDepthStencilSurface(g_effectLevelDepth);
        }
        const auto oldest = std::ranges::min_element(g_effectLevelRows, {}, &EffectLevelRow::used);
        const auto row = static_cast<std::size_t>(oldest - g_effectLevelRows.begin());
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
        DrawEffectLevel(device, row, lights[light]);
        *oldest = {lights[light], g_seenGeneration, skipping, g_effectFrame};
        rows[light] = row;
    }
    if (level != nullptr)
    {
        level->Release();
    }
    return rows;
}

// The effects' lights' maps for this scene: each light handed, six faces,
// each with every figure that can stand within its reach. False where there
// is nothing to draw them by.
static bool DrawEffectShadows(IDirect3DDevice9 *device)
{
    const std::vector<std::array<float, 4>> &lights = effectlights::Handed();
    // The level is drawn by the sun's maps' own shaders (MakeCopying's cast
    // and cut), made there once whatever drew first.
    if (lights.empty() || !MakeEffectAtlas(device) || !MakeCopying(device))
    {
        return false;
    }
    const keptstate::Kept kept(device, g_saved);
    if (!kept)
    {
        return false;
    }
    if (!g_measuredThisFrame)
    {
        Measure(device);
        g_measuredThisFrame = true;
    }
    IDirect3DSurface9 *atlas = nullptr;
    if (SUCCEEDED(g_effectAtlas->GetSurfaceLevel(0, &atlas)))
    {
        device->SetRenderTarget(0, atlas);
        device->SetRenderTarget(1, nullptr);
        device->SetDepthStencilSurface(g_effectDepth);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
        const std::vector<std::size_t> rows = EffectLevelRows(device, lights);
        device->SetRenderTarget(0, atlas);
        device->SetDepthStencilSurface(g_effectDepth);
        // Each light's level, its depths too, then its figures over it.
        SetCopying(device, g_effectLevel);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
        for (std::size_t light = 0; light < rows.size(); ++light)
        {
            CopyEffectLevel(device, rows[light], light);
        }
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED);
        for (std::size_t light = 0; light < rows.size(); ++light)
        {
            DrawEffectLight(device, light, lights[light]);
        }
        std::error_code error;
        if (!g_dumpAsked.empty() && std::filesystem::exists(g_dumpAsked, error))
        {
            DumpMap(device, atlas, "effects");
            std::filesystem::remove(g_dumpAsked, error);
        }
        atlas->Release();
    }
    return true;
}

// A draw into the picture that reads the effects' shadows: the maps drawn at
// the first such draw of the scene, and lent to it.
static void EffectsDrawing(IDirect3DDevice9 *device)
{
    if (!g_effectsWanted.load(std::memory_order_relaxed))
    {
        return;
    }
    IDirect3DPixelShader9 *pixel = nullptr;
    device->GetPixelShader(&pixel);
    if (pixel == nullptr)
    {
        return;
    }
    pixel->Release();
    const auto receiver = g_effectReceivers.find(pixel);
    if (receiver == g_effectReceivers.end())
    {
        return;
    }
    if (!g_effectsDrawn)
    {
        g_effectsDrawn = true;
        g_lightsMost = std::max(g_lightsMost, effectlights::Handed().size());
        const Timed timed(kEffectsPart);
        gputimer::Begin(device, "effects' shadows");
        g_effectsReady = DrawEffectShadows(device);
        gputimer::End(device);
    }
    if (g_effectsReady)
    {
        Lend(device, receiver->second, g_lentEffects, g_effectAtlas);
    }
}

void WantEffectShadows(bool on)
{
    g_effectsWanted.store(on, std::memory_order_relaxed);
}

void CacheEffectShadows(bool on)
{
    g_effectCache.store(on, std::memory_order_relaxed);
}

void Leaving(IDirect3DDevice9 * /*device*/)
{
    g_mapDrawn = false;
}

void CreatedPixel(const DWORD *code, IDirect3DPixelShader9 *shader)
{
    if (code == nullptr || shader == nullptr)
    {
        return;
    }
    g_receivers.erase(shader); // the last made here's alone (Created)
    g_receiversFar.erase(shader);
    g_effectReceivers.erase(shader);
    if (const std::optional<UINT> sampler = RegisterOf(CodeOf(code), "smpDaoEffectShadows"))
    {
        g_effectReceivers[shader] = *sampler;
    }
    // The near map's name: the Character material's, or the Face's and the
    // HairAlpha's own (build_shaders.py's SHADOW_MAPS). Until 2026-10-16 only
    // the first was looked for, and every face and hair read whatever the
    // draw before had left on its sampler.
    for (const std::string_view name : {"smpShadowPartition1", "smpShadowPart1"})
    {
        if (const std::optional<UINT> sampler = RegisterOf(CodeOf(code), name))
        {
            g_receivers[shader] = *sampler;
            break;
        }
    }
    for (const std::string_view name : {"smpShadowPartition2", "smpShadowPart2"})
    {
        if (const std::optional<UINT> sampler = RegisterOf(CodeOf(code), name))
        {
            g_receiversFar[shader] = *sampler;
            break;
        }
    }
}

void Focus(const std::optional<std::array<float, 3>> &focus)
{
    const std::scoped_lock lock(g_sunLock);
    g_focus =
        focus ? std::optional<cascades::Vector>(cascades::Vector{(*focus)[0], (*focus)[1], (*focus)[2]}) : std::nullopt;
}

// The maps keep their aim until the light has turned this far from it. A map
// aimed afresh at a light that turns a little each frame (a day and a night's
// sun, 0.1 degree a second at an hour a day) lays its texels a little
// differently over the world each frame, and every shadow's edge shimmers:
// measured 2026-10-21 at night on the party, 1.4 to 3.7 of 255 changed from
// one half second to the next with the hour going on, none with it held (the
// user: "shadows are flickering", "characters are flickering at night too").
// A step of 0.2 degrees moves a ten-metre tree's shadow some 3 cm.
namespace
{
constexpr float kAimStep = 0.2F * std::numbers::pi_v<float> / 180;
std::optional<std::array<float, 3>> g_aimedAt; // the game thread's
} // namespace

void Sun(const std::array<float, 3> &going)
{
    if (g_fading.load())
    {
        return; // the next step after this one's fade
    }
    if (g_aimedAt)
    {
        const std::array<float, 3> &at = *g_aimedAt;
        const float along = std::clamp((at[0] * going[0]) + (at[1] * going[1]) + (at[2] * going[2]), -1.0F, 1.0F);
        if (std::acos(along) < kAimStep)
        {
            return;
        }
    }
    g_aimedAt = going;
    const std::optional<std::array<cascades::Vector, 3>> axes = cascades::AxesToward(going);
    const std::scoped_lock lock(g_sunLock);
    g_sunAxes = axes;
}

void Skip(unsigned kinds)
{
    g_skipped.store(kinds, std::memory_order_relaxed);
}

void Forget()
{
    g_forget = true;
    reflected::Forget();
}

void Receiving(IDirect3DDevice9 *device, bool picture)
{
    g_drawsPictureNow = picture;
    SetReceivers(device);
}

std::optional<std::array<float, 16>> ToScreen()
{
    return g_haveCamera ? std::optional<std::array<float, 16>>(g_toScreen) : std::nullopt;
}

void Presented()
{
    MakePatches();
    CollectMeasured();
    for (FigureDraw &draw : g_figureDraws)
    {
        Let(draw);
    }
    g_figureDraws.clear();
    g_figureKeys.clear();
    g_atlasDrawn = false;
    g_atlasReady = false;
    g_effectsDrawn = false;
    g_effectsReady = false;
    g_passLastFrame = g_passThisFrame;
    g_passThisFrame = false;
    g_drawnThisFrame.clear();
    constexpr int kEvery = 600;
    if (++g_frames % kEvery == 0)
    {
        spdlog::info("level shadows: {} of the game's shaders known, {} of the engine's shadow pass; a frame: {} level "
                     "draws noted, {} engine shadow draws, {} level draws into its maps; {} kept, {} measured; the "
                     "caches drawn {} times since",
                     g_gameShaders.size(), g_engineShadow.size(), g_noted, g_engineDraws, g_drawnInto, g_seen.size(),
                     g_measuredCount, g_cacheDrawn);
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        std::string parts;
        for (std::size_t part = 0; part < kParts; ++part)
        {
            const double cpu =
                1000.0 * static_cast<double>(g_partTicks.at(part)) / static_cast<double>(frequency.QuadPart) / kEvery;
            parts += fmt::format("{}{} {:.2f} ms", part == 0 ? "" : "; ", kPartNames.at(part), cpu);
        }
        spdlog::info("shadows' cost: a frame: {} figures replayed into the sun's maps, {} into the effects' lights' "
                     "maps, {} lights at the most; on the game's thread: {}",
                     g_figureReplays, g_effectFigureReplays, g_lightsMost, parts);
        const auto ms = [&](LONGLONG ticks) {
            return 1000.0 * static_cast<double>(ticks) / static_cast<double>(frequency.QuadPart) / kEvery;
        };
        spdlog::info("shadows' cost: the effects' maps a frame: {} faces, {} lights' level drawn again, {} level "
                     "draws ({:.2f} ms), the figures {:.2f} ms",
                     g_effectFaces, g_effectLevelRowsDrawn, g_effectLevelDraws, ms(g_effectLevelTicks),
                     ms(g_effectFigureTicks));
        g_effectLevelTicks = 0;
        g_effectFigureTicks = 0;
        g_partTicks = {};
        g_lightsMost = 0;
        std::vector<float> sizes;
        for (const auto &[key, draw] : g_seen)
        {
            if (draw.bounds)
            {
                const std::array<float, 6> &b = *draw.bounds;
                sizes.push_back(std::max({b[3] - b[0], b[4] - b[1], b[5] - b[2]}));
            }
        }
        std::ranges::sort(sizes);
        if (!sizes.empty())
        {
            spdlog::info("level shadows: kept draws' greatest extent: least {:.1f}, median {:.1f}, 90th {:.1f}, most "
                         "{:.1f} m",
                         sizes.front(), sizes[sizes.size() / 2], sizes[sizes.size() * 9 / 10], sizes.back());
        }
    }
    g_engineDraws = 0;
    g_drawnInto = 0;
    g_noted = 0;
    g_figureReplays = 0;
    g_effectFigureReplays = 0;
    g_effectLevelDraws = 0;
    g_effectFaces = 0;
    g_effectLevelRowsDrawn = 0;
    g_measuredThisFrame = false;
    if (g_forget.exchange(false))
    {
        ForgetSeen();
    }
}

namespace
{
// Lets a Direct3D object go, if there is one, and forgets it.
template <typename T> void Drop(T *&object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}
} // namespace

void Release()
{
    ForgetSeen();
    Drop(g_saved);
    for (Cache *cache : {g_caches.data(), g_caches.data() + 1, g_caches.data() + 2, g_caches.data() + 3,
                         g_oldCaches.data(), g_oldCaches.data() + 1, g_oldCaches.data() + 2, g_oldCaches.data() + 3})
    {
        Drop(cache->texture);
        Drop(cache->depth);
        Drop(cache->colours);
        *cache = Cache{};
    }
    for (Reflecting &reflecting : g_reflecting)
    {
        for (ReflectSlot &slot : reflecting.slots)
        {
            Drop(slot.target);
            Drop(slot.colourTarget);
            slot = ReflectSlot{};
        }
    }
    for (MeasureSlot &slot : g_measureSlots)
    {
        for (IDirect3DSurface9 *&target : slot.targets)
        {
            Drop(target);
        }
        slot.keys.clear();
        slot.batch = 0;
    }
    Drop(g_map);
    g_mapDrawn = false;
    for (FigureDraw &draw : g_figureDraws)
    {
        Let(draw);
    }
    g_figureDraws.clear();
    g_figureKeys.clear();
    Drop(g_atlas);
    Drop(g_atlasDepth);
    Drop(g_oldAtlas);
    g_oldCascades = {};
    g_fittedAxes.reset();
    g_fading.store(false);
    ReleaseEffectAtlas();
    g_tile = 0;
    g_along = {};
}

} // namespace levelshadows
