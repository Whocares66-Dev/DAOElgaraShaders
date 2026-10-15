// Elgara Shaders: the game's own picture drawn better, with no renderer of our own.
//
// Nearly all of it is not code here. The build makes the game's own shaders
// again, changed (build_shaders.py), into this plugin's override folder,
// where the game finds a file before its own archives':
//
// - The shaders that receive the sun's shadows filter the shadow maps with
//   sixteen taps on a turned disc, not the game's four or five (shadow.hlsl),
//   and a character is shaded by them: the game reads no shadow map for it.
// - Armour, weapons, props and the level's surfaces mirror and scatter light
//   as what they are made of (materials.hlsl, data/materials.ini), and the
//   light baked into a level and a figure's ambient light are shaped by the
//   surface's normal map (bake.hlsl).
// - Before the game's colour grade, in the same chain of the game's, three
//   passes of anti-aliasing (smaa.py) and four of ambient occlusion
//   (occlusion.hlsl, occlusion_blur.hlsl), from the depth the game keeps
//   beside the picture; the grade reads the picture through them (grade.hlsl).
//
// What is code here:
//
// - The options screen's section on the Video tab: Improved materials,
//   Improved shadows and SMAA, each a checkbox, and the ambient occlusion's
//   strength and the anisotropic filtering's degree, each a slider. What is
//   chosen is kept in graphics.chosen beside the DLL. The shaders read the
//   checkboxes from registers this sets on the game's Direct3D device, and
//   the anti-aliasing and the occlusion from values of the game's
//   colour-grade chain, which this sets there.
// - The scene is drawn into a 16-bit target of our own, so that a surface
//   lit brighter than white is rolled off and not cut (grade.hlsl's
//   DaoShoulder), which the materials want.
// - The sun's shadow maps are drawn at kMapSize a side with Improved shadows
//   on, not the 512 or 1024 the game's shadow quality gives them: the engine
//   is asked for its two maps through one function, and this changes the
//   size on the way in.
// - Anisotropic filtering, which the game has no setting for.
//
// What it does goes to elgarashaders.log.
#include "MaterialPatch.h"
#include "ResourceDump.h"

#include "dao/Addresses.h"
#include "dao/Camera.h"
#include "dao/ClientExoApp.h"
#include "dao/Frame.h"
#include "dao/Framebuffer.h"
#include "dao/GameModes.h"
#include "dao/Hook.h"
#include "dao/Log.h"
#include "dao/Module.h"
#include "dao/Options.h"
#include "dao/Plugin.h"
#include "dao/World.h"

#include <spdlog/spdlog.h>

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace
{

constexpr std::int32_t kMapSize = 2048; // texels a side, with Improved shadows on

// The options as the player has them: the game thread's, as the options
// screen's callbacks and the frame's are. Each is read where it is needed
// by the drawing thread through what is handed on from it (g_look,
// g_anisotropy), and the shadow maps' size by the engine as it starts.
std::atomic<bool> g_materials{true};
std::atomic<bool> g_shadows{true};
bool g_smaa = true; // SMAA beside the game's own anti-aliasing, or not

// The two sliders, each a stop of its list (the user, 2026-10-12: "implement
// gtao, and add an option / slider for it. also add one for anisotropic
// filtering").
constexpr std::array<float, 4> kOcclusionStops{0.0F, 0.5F, 1.0F, 1.5F}; // Off, Low, Medium, High
constexpr std::array<const wchar_t *, 4> kOcclusionNames{L"Off", L"Low", L"Medium", L"High"};
constexpr std::array<int, 5> kAnisotropyStops{0, 2, 4, 8, 16};
constexpr std::array<const wchar_t *, 5> kAnisotropyNames{L"Off", L"2x", L"4x", L"8x", L"16x"};
std::atomic<int> g_occlusionStop{3};  // High
std::atomic<int> g_anisotropyStop{4}; // 16x

// One of the sun's shadow maps as the engine is asked for it.
// Notes on Partition: read 2026-10-10 in the function that asks (0x00455580):
// two of them on its stack, {10.0, size, format} and {sqrt of the 900 at
// 0x00BF20B4, size, format}, the size the global at 0x00BF20C4 that the
// shadow quality sets to 512 or 1024, the format 9 or 0x20 by a capability of
// the device. The function that takes them (0x00455420) copies each 12 bytes.
// It is asked once, as the game starts (seen in the log 2026-10-14), so the
// size Improved shadows gives is the one it had then.
struct Partition
{
    float distance;      // how far from the camera the map reaches, in metres
    std::int32_t size;   // the map's width and height
    std::int32_t format; // the engine's texture format
};

using SetPartitionsFn = void(__cdecl *)(const Partition *, std::uint32_t);
dao::Detour<SetPartitionsFn> g_setPartitions;

// The numbers the build's shaders read that are not the engine's to give:
// kLookRegisters float registers of the pixel shader from kLookRegister up,
// which none of the game's shaders reaches (their constants end under c70), so
// the engine never writes them and what is put there stays for every draw.
// build_shaders.py declares them as `daoLook` in each shader it rebuilds, and
// look.hlsl says which is which.
// Notes on the registers: the level's and the figures' shaders are handed
// what the engine has for them and nothing else. A number was first carried
// in the fourth of the sun's colour; an area with no sun (the Circle, the
// Dead Trenches) is given no sun's colour at all, so nothing arrived there
// (seen 2026-10-11). So they are set on the game's Direct3D device, taken as
// the game creates it, at the start of every scene, on the thread that draws.
constexpr UINT kLookRegister = 220;
constexpr std::size_t kLookRegisters = 4;
std::array<std::atomic<float>, kLookRegisters * 4> g_look{};
// Their places (look.hlsl).
constexpr std::size_t kRelief = 0;
constexpr std::size_t kMaterials = 1;
constexpr std::size_t kShadowOnFigures = 2;
constexpr std::size_t kShadows = 3;
constexpr std::size_t kShadowMapSize = 4;
constexpr std::size_t kUncut = 5;
// The last of them: whether the ambient share is written into what is drawn
// now (look.hlsl's DAO_SHARE_ASKED): where the device's first target is the
// frame's own picture, which is the drawing thread's to know and to say as
// the target changes (SetRenderTarget), and the ambient occlusion is on.
// The game's bloom carries the picture's alpha into its mask
// (illuminationmask.psh: its output's alpha is the picture's, scaled), so a
// share there is less bloom: with every option off, the lava falls of
// Orzammar's Commons and the figures' highlights were up to four levels of
// 255 dimmer than with no plugin (measured 2026-10-14). With the occlusion
// off the alpha is left the game's.
constexpr std::size_t kShareAsked = (kLookRegisters * 4) - 1;
bool g_drawsPicture = true;
std::atomic<bool> g_shareWanted{true}; // the game thread's word: the ambient occlusion is on

// Whether the share is written into what is drawn now.
bool ShareAsked()
{
    return g_drawsPicture && g_shareWanted.load(std::memory_order_relaxed);
}

void __cdecl SetPartitions(const Partition *partitions, std::uint32_t count)
{
    const bool ours = g_shadows.load(std::memory_order_relaxed);
    std::array<Partition, 2> asked{};
    if (count > asked.size())
    {
        g_setPartitions.Original()(partitions, count);
        return;
    }
    std::copy_n(partitions, count, asked.begin());
    for (std::uint32_t i = 0; i < count; ++i)
    {
        Partition &partition = asked.at(i);
        const std::int32_t size = ours ? kMapSize : partition.size;
        spdlog::info("shadow map to {} m: {} a side (the game's {}), format {}", partition.distance, size,
                     partition.size, partition.format);
        partition.size = size;
        g_look.at(kShadowMapSize).store(static_cast<float>(size), std::memory_order_relaxed);
    }
    g_setPartitions.Original()(asked.data(), count);
}

using CreateFn = IDirect3D9 *(__stdcall *)(UINT);
using CreateDeviceFn = HRESULT(__stdcall *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *,
                                            IDirect3DDevice9 **);
using BeginSceneFn = HRESULT(__stdcall *)(IDirect3DDevice9 *);
dao::Detour<CreateFn> g_create;
dao::Detour<CreateDeviceFn> g_createDevice;
dao::Detour<BeginSceneFn> g_beginScene;

// A COM object's method by its place in the vtable.
template <class Fn> Fn Method(void *object, std::size_t slot)
{
    return reinterpret_cast<Fn>((*static_cast<void ***>(object))[slot]);
}

constexpr std::size_t kCreateDeviceSlot = 16; // IDirect3D9::CreateDevice
constexpr std::size_t kBeginSceneSlot = 41;   // IDirect3DDevice9::BeginScene

// What the game draws into, logged as it is created, for a session that has a
// file named kLogTargetsFile beside the DLL: every texture made to be drawn
// into and every render target, its size, format (a D3DFORMAT's number: 21
// is A8R8G8B8, 113 A16B16G16R16F, 114 R32F) and anti-aliasing. It is how the
// question of a wider scene target is read, and changes nothing.
constexpr std::string_view kLogTargetsFile = "log-targets";
constexpr std::size_t kCreateTextureSlot = 23;      // IDirect3DDevice9::CreateTexture
constexpr std::size_t kCreateRenderTargetSlot = 28; // IDirect3DDevice9::CreateRenderTarget
using CreateTextureFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                             IDirect3DTexture9 **, HANDLE *);
using CreateRenderTargetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD,
                                                  BOOL, IDirect3DSurface9 **, HANDLE *);
dao::Detour<CreateTextureFn> g_createTexture;
dao::Detour<CreateRenderTargetFn> g_createRenderTarget;

// The drawing thread's own: whether targets are logged, the format our scene
// target has in place of the back buffer's (D3DFMT_UNKNOWN for the back
// buffer's own), and the back buffer's size.
bool g_logTargets = false;
D3DFORMAT g_wideFormat = D3DFMT_UNKNOWN;
UINT g_backWidth = 0;
UINT g_backHeight = 0;

// Whether a texture the game asks for is one the scene passes through on its
// way to the screen: one to draw into, of the back buffer's format, and of
// its size (the copies the effects read) or half of it (the bloom's).
bool CarriesTheScene(UINT width, UINT height, DWORD usage, D3DFORMAT format)
{
    const bool whole = width == g_backWidth && height == g_backHeight;
    const bool half = width == g_backWidth / 2 && height == g_backHeight / 2;
    return (usage & D3DUSAGE_RENDERTARGET) != 0 && format == D3DFMT_A8R8G8B8 && (whole || half);
}

HRESULT __stdcall CreateTexture(IDirect3DDevice9 *device, UINT width, UINT height, UINT levels, DWORD usage,
                                D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **texture, HANDLE *shared)
{
    // With a wide scene target, what the scene is copied into is as wide, or
    // the first copy would cut it back to eight bits. The depth the game
    // packs into a colour's bytes goes the same way and loses nothing: what
    // its shader writes are whole 256ths, which a 16-bit float holds exactly,
    // and unrounded they average truly where a pixel holds two surfaces.
    if (g_wideFormat != D3DFMT_UNKNOWN && CarriesTheScene(width, height, usage, format))
    {
        format = g_wideFormat;
    }
    if (g_logTargets && (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) != 0)
    {
        spdlog::info("a texture to draw into: {} by {}, format {}, {} levels, usage {:#x}", width, height,
                     static_cast<int>(format), levels, usage);
    }
    return g_createTexture.Original()(device, width, height, levels, usage, format, pool, texture, shared);
}

HRESULT __stdcall CreateRenderTarget(IDirect3DDevice9 *device, UINT width, UINT height, D3DFORMAT format,
                                     D3DMULTISAMPLE_TYPE samples, DWORD quality, BOOL lockable,
                                     IDirect3DSurface9 **surface, HANDLE *shared)
{
    if (g_logTargets)
    {
        spdlog::info("a render target: {} by {}, format {}, {} samples of quality {}", width, height,
                     static_cast<int>(format), static_cast<int>(samples), quality);
    }
    return g_createRenderTarget.Original()(device, width, height, format, samples, quality, lockable, surface, shared);
}

// One frame's drawing, target by target, for a session that has a file named
// kLogFrameFile beside the DLL: each change of render target with what the
// target is, each copy from one surface to another, each clear, and how many
// draws went to a target before the next change. One frame in kTraceEvery is
// logged, counted from the device's first Present. It is the map a scene
// target of our own has to follow, and changes nothing.
constexpr std::string_view kLogFrameFile = "log-frame";
constexpr int kTraceEvery = 600; // frames from one logged frame to the next
constexpr int kTraceFirst = 300;
constexpr std::size_t kPresentSlot = 17;         // IDirect3DDevice9::Present
constexpr std::size_t kStretchRectSlot = 34;     // IDirect3DDevice9::StretchRect
constexpr std::size_t kSetRenderTargetSlot = 37; // IDirect3DDevice9::SetRenderTarget
constexpr std::size_t kSetRenderStateSlot = 57;  // IDirect3DDevice9::SetRenderState
constexpr std::size_t kClearSlot = 43;           // IDirect3DDevice9::Clear
constexpr std::size_t kDrawSlot = 81;            // IDirect3DDevice9::DrawPrimitive
constexpr std::size_t kDrawIndexedSlot = 82;     // IDirect3DDevice9::DrawIndexedPrimitive
using PresentFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using StretchRectFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *,
                                           const RECT *, D3DTEXTUREFILTERTYPE);
using SetRenderTargetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 *);
using SetRenderStateFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);
using ClearFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD);
using DrawFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
dao::Detour<PresentFn> g_present;
dao::Detour<StretchRectFn> g_stretchRect;
dao::Detour<SetRenderTargetFn> g_setRenderTarget;
dao::Detour<SetRenderStateFn> g_setRenderState;
dao::Detour<ClearFn> g_clear;
dao::Detour<DrawFn> g_draw;
dao::Detour<DrawIndexedFn> g_drawIndexed;
// The drawing thread's own: the frames presented, whether this one is logged,
// and the draws since the last line.
int g_presented = 0;
bool g_traceAsked = false; // a file named kLogFrameFile is beside the DLL
bool g_tracing = false;
int g_draws = 0;

// The scene drawn into a render target of our own, 16-bit float. The game
// draws its depth pre-pass, its scene, its effects and its interface straight
// into the swap chain's back buffer, which is eight bits a channel and can be
// nothing else; a scene that holds more than white has to be drawn somewhere
// else. So wherever the game names the back buffer as a target, or as either
// end of a copy, it is given ours, of the same size and anti-aliasing, and as
// a frame is shown ours is resolved and drawn onto the real one.
// Notes on the scene target: seen in ten areas 2026-10-11, the plugin's
// options all off in it matching the picture without it, and across two
// Resets of the device (the anti-aliasing changed in the options screen and
// changed back), ours made again each time. A file named kOwnSceneFile beside
// the DLL says otherwise: holding `off` the scene stays in the back buffer,
// holding `8bit` ours is made in the back buffer's own format.
constexpr std::string_view kOwnSceneFile = "own-scene";
constexpr std::size_t kResetSlot = 16;           // IDirect3DDevice9::Reset
constexpr std::size_t kGetRenderTargetSlot = 38; // IDirect3DDevice9::GetRenderTarget
using ResetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using GetRenderTargetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, IDirect3DSurface9 **);
dao::Detour<ResetFn> g_reset;
dao::Detour<GetRenderTargetFn> g_getRenderTarget;

// The drawing thread's own.
struct Scene
{
    IDirect3DSurface9 *back = nullptr;       // the swap chain's, which the game takes for its target
    IDirect3DSurface9 *own = nullptr;        // ours, which it is given instead
    IDirect3DTexture9 *shown = nullptr;      // ours resolved, to be drawn onto the back buffer
    IDirect3DSurface9 *shownLevel = nullptr; // that texture's one level
    IDirect3DStateBlock9 *states = nullptr;  // the device's state, kept across our own draw
    UINT width = 0;
    UINT height = 0;
};
Scene g_scene;
IDirect3DDevice9 *g_device = nullptr; // the game's, for the swap chain's Present, which is handed none

template <class T> void Released(T *&object)
{
    if (object != nullptr)
    {
        object->Release();
        object = nullptr;
    }
}

// Lets go of everything of ours the device holds, as a Reset of it requires.
void DropScene()
{
    Released(g_scene.states);
    Released(g_scene.shownLevel);
    Released(g_scene.shown);
    Released(g_scene.own);
    Released(g_scene.back);
}

// Makes our target beside the back buffer and makes it the device's target,
// as the back buffer is of a new or a reset device.
void MakeScene(IDirect3DDevice9 *device)
{
    DropScene();
    g_drawsPicture = true;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_scene.back)))
    {
        return;
    }
    D3DSURFACE_DESC is{D3DFMT_UNKNOWN, D3DRTYPE_SURFACE, 0, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 0, 0};
    g_scene.back->GetDesc(&is);
    g_scene.width = g_backWidth = is.Width;
    g_scene.height = g_backHeight = is.Height;
    const D3DFORMAT format = g_wideFormat != D3DFMT_UNKNOWN ? g_wideFormat : is.Format;
    const bool made = SUCCEEDED(device->CreateRenderTarget(is.Width, is.Height, format, is.MultiSampleType,
                                                           is.MultiSampleQuality, FALSE, &g_scene.own, nullptr)) &&
                      SUCCEEDED(device->CreateTexture(is.Width, is.Height, 1, D3DUSAGE_RENDERTARGET, format,
                                                      D3DPOOL_DEFAULT, &g_scene.shown, nullptr)) &&
                      SUCCEEDED(g_scene.shown->GetSurfaceLevel(0, &g_scene.shownLevel)) &&
                      SUCCEEDED(device->CreateStateBlock(D3DSBT_ALL, &g_scene.states));
    if (!made)
    {
        DropScene();
        spdlog::warn("the scene stays in the back buffer: a target of our own was not made");
        return;
    }
    g_setRenderTarget.Original()(device, 0, g_scene.own);
    spdlog::info("the scene is drawn into a target of our own: {} by {}, format {}, {} samples", is.Width, is.Height,
                 static_cast<int>(format), static_cast<int>(is.MultiSampleType));
}

// The surface the device is to have where the game names `surface`.
IDirect3DSurface9 *Swapped(IDirect3DSurface9 *surface)
{
    return g_scene.own != nullptr && surface == g_scene.back ? g_scene.own : surface;
}

// Resolves our target and draws it over the whole of the back buffer, with
// the fixed pipeline and a state of our own, the device's state put back
// after. Called as a frame is shown.
void ShowScene(IDirect3DDevice9 *device)
{
    if (g_scene.own == nullptr)
    {
        return;
    }
    g_stretchRect.Original()(device, g_scene.own, nullptr, g_scene.shownLevel, nullptr, D3DTEXF_NONE);
    IDirect3DSurface9 *was = nullptr;
    g_getRenderTarget.Original()(device, 0, &was);
    g_scene.states->Capture();
    g_setRenderTarget.Original()(device, 0, g_scene.back);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(nullptr);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    constexpr std::array<std::pair<D3DRENDERSTATETYPE, DWORD>, 13> kStates{{
        {D3DRS_ZENABLE, FALSE},
        {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE},
        {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE},
        {D3DRS_SCISSORTESTENABLE, FALSE},
        {D3DRS_FOGENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE},
        {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_SRGBWRITEENABLE, FALSE},
        {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_FILLMODE, D3DFILL_SOLID},
        {D3DRS_COLORWRITEENABLE, 0xF},
    }};
    for (const auto &[state, value] : kStates)
    {
        device->SetRenderState(state, value);
    }
    device->SetTexture(0, g_scene.shown);
    device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    const D3DVIEWPORT9 whole{0, 0, g_scene.width, g_scene.height, 0.0F, 1.0F};
    device->SetViewport(&whole);
    // A pixel's centre is half a pixel in from its corner, and a texel's is
    // read at its own: the quad's corners stand half a pixel out.
    struct Corner
    {
        float x, y, z, w, u, v;
    };
    const float right = static_cast<float>(g_scene.width) - 0.5F;
    const float bottom = static_cast<float>(g_scene.height) - 0.5F;
    const std::array<Corner, 4> corners{{
        {-0.5F, -0.5F, 0, 1, 0, 0},
        {right, -0.5F, 0, 1, 1, 0},
        {-0.5F, bottom, 0, 1, 0, 1},
        {right, bottom, 0, 1, 1, 1},
    }};
    if (SUCCEEDED(device->BeginScene()))
    {
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, corners.data(), sizeof(Corner));
        device->EndScene();
    }
    g_scene.states->Apply();
    g_setRenderTarget.Original()(device, 0, was);
    Released(was);
}

// The game is never shown our target: where the device would hand it back,
// the game gets the back buffer it named.
HRESULT __stdcall GetRenderTarget(IDirect3DDevice9 *device, DWORD index, IDirect3DSurface9 **target)
{
    const HRESULT result = g_getRenderTarget.Original()(device, index, target);
    if (SUCCEEDED(result) && target != nullptr && g_scene.own != nullptr && *target == g_scene.own)
    {
        (*target)->Release();
        *target = g_scene.back;
        g_scene.back->AddRef();
    }
    return result;
}

HRESULT __stdcall Reset(IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *parameters)
{
    DropScene();
    const HRESULT result = g_reset.Original()(device, parameters);
    if (SUCCEEDED(result))
    {
        MakeScene(device);
    }
    return result;
}

// What a surface is, for a line of the trace.
std::string Described(IDirect3DDevice9 *device, IDirect3DSurface9 *surface)
{
    if (surface == nullptr)
    {
        return "none";
    }
    D3DSURFACE_DESC is{D3DFMT_UNKNOWN, D3DRTYPE_SURFACE, 0, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 0, 0};
    surface->GetDesc(&is);
    IDirect3DSurface9 *back = nullptr;
    const bool isBack = SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back == surface;
    if (back != nullptr)
    {
        back->Release();
    }
    std::string_view which;
    if (isBack)
    {
        which = "the back buffer, ";
    }
    else if (surface == g_scene.own)
    {
        which = "our scene target, ";
    }
    return fmt::format("{}{} by {}, format {}, {} samples, at {}", which, is.Width, is.Height,
                       static_cast<int>(is.Format), static_cast<int>(is.MultiSampleType),
                       static_cast<const void *>(surface));
}

// The draws gathered since the last line, as a line of their own.
void LogDraws()
{
    if (g_draws != 0)
    {
        spdlog::info("    {} draws", g_draws);
        g_draws = 0;
    }
}

// A frame is shown: the end of one frame's lines and, at the frames to log,
// the start of the next's.
void FramePresented(std::string_view by)
{
    if (g_tracing)
    {
        LogDraws();
        spdlog::info("  Present by {}: the end of frame {}", by, g_presented);
    }
    ++g_presented;
    // Only where it is asked for: the scene target stands on the same
    // detours, and without this the trace went to every player's log, sixty
    // lines every ten seconds (seen 2026-10-14).
    g_tracing = g_traceAsked && g_presented % kTraceEvery == kTraceFirst;
    if (g_tracing)
    {
        spdlog::info("frame {}, as it is drawn:", g_presented);
    }
}

HRESULT __stdcall Present(IDirect3DDevice9 *device, const RECT *from, const RECT *to, HWND window, const RGNDATA *dirty)
{
    ShowScene(device);
    FramePresented("the device");
    return g_present.Original()(device, from, to, window, dirty);
}

// The game shows its frames by its swap chain, not the device (seen
// 2026-10-11: with the device's Present alone detoured no frame was counted).
constexpr std::size_t kChainPresentSlot = 3; // IDirect3DSwapChain9::Present
using ChainPresentFn = HRESULT(__stdcall *)(IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *,
                                            DWORD);
dao::Detour<ChainPresentFn> g_chainPresent;

HRESULT __stdcall ChainPresent(IDirect3DSwapChain9 *chain, const RECT *from, const RECT *to, HWND window,
                               const RGNDATA *dirty, DWORD flags)
{
    if (g_device != nullptr)
    {
        ShowScene(g_device);
    }
    FramePresented("the swap chain");
    return g_chainPresent.Original()(chain, from, to, window, dirty, flags);
}

HRESULT __stdcall StretchRect(IDirect3DDevice9 *device, IDirect3DSurface9 *from, const RECT *part,
                              IDirect3DSurface9 *to, const RECT *place, D3DTEXTUREFILTERTYPE filter)
{
    from = Swapped(from);
    to = Swapped(to);
    if (g_tracing)
    {
        LogDraws();
        spdlog::info("  copy [{}] to [{}]", Described(device, from), Described(device, to));
    }
    return g_stretchRect.Original()(device, from, part, to, place, filter);
}

// The alpha's own blend, as the game last asked for it. Only our states ask:
// a pass that blends is given one that leaves in the picture's alpha what
// share of the surface under it still shows (build_shaders.py's
// with_share_kept: nothing of the drawn thing's alpha, and the alpha there
// by what the thing leaves clear). That is for the frame's own picture. In a
// target the interface lays on the screen by its alpha it rubs a blended
// thing out: Cailan's helm, blended whole for its plume, was not in the
// inventory's figure, and the head under it hidden as it is under any helm
// (the user, 2026-10-12: "alistair's head is missing in char page"; seen
// with the helm put on and taken off, twice). So it is in force only while
// the share is written (ShareAsked), and otherwise the alpha blends as the
// colour does, which is the game's way.
DWORD g_separateAlphaAsked = FALSE;

HRESULT __stdcall SetRenderState(IDirect3DDevice9 *device, D3DRENDERSTATETYPE state, DWORD value)
{
    if (state == D3DRS_SEPARATEALPHABLENDENABLE)
    {
        g_separateAlphaAsked = value;
        value = ShareAsked() ? value : FALSE;
    }
    return g_setRenderState.Original()(device, state, value);
}

// Whether a surface is the frame's own picture: our target, or the back
// buffer where none of ours was made.
bool IsPicture(IDirect3DDevice9 *device, IDirect3DSurface9 *surface)
{
    if (g_scene.own != nullptr)
    {
        return surface == g_scene.own;
    }
    IDirect3DSurface9 *back = nullptr;
    const bool is = SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back == surface;
    Released(back);
    return is;
}

// The registers' last as the drawing thread has it now: its three numbers
// and whether the frame's own picture is drawn into.
void HandShareAsked(IDirect3DDevice9 *device)
{
    std::array<float, 4> last{};
    for (std::size_t i = 0; i + 1 < last.size(); ++i)
    {
        last.at(i) = g_look.at(kShareAsked - 3 + i).load(std::memory_order_relaxed);
    }
    last.back() = ShareAsked() ? 1.0F : 0.0F;
    device->SetPixelShaderConstantF(kLookRegister + kLookRegisters - 1, last.data(), 1);
}

HRESULT __stdcall SetRenderTarget(IDirect3DDevice9 *device, DWORD index, IDirect3DSurface9 *target)
{
    target = index == 0 ? Swapped(target) : target;
    if (index == 0 && target != nullptr)
    {
        // A portrait and the record's and the inventory's figures are drawn
        // into targets of their own (the record's: 270 by 459, format 21,
        // first in the frame; traced 2026-10-12), and the scene, with the
        // game's anti-aliasing and without, into the frame's own.
        if (const bool picture = IsPicture(device, target); picture != g_drawsPicture)
        {
            g_drawsPicture = picture;
            HandShareAsked(device);
            if (g_setRenderState.IsAttached())
            {
                g_setRenderState.Original()(device, D3DRS_SEPARATEALPHABLENDENABLE,
                                            ShareAsked() ? g_separateAlphaAsked : FALSE);
            }
        }
    }
    if (g_tracing)
    {
        LogDraws();
        spdlog::info("  target {}: [{}]", index, Described(device, target));
    }
    return g_setRenderTarget.Original()(device, index, target);
}

HRESULT __stdcall Clear(IDirect3DDevice9 *device, DWORD count, const D3DRECT *rects, DWORD flags, D3DCOLOR colour,
                        float depth, DWORD stencil)
{
    if (g_tracing)
    {
        LogDraws();
        spdlog::info("  clear, flags {:#x}, colour {:#010x}", flags, colour);
    }
    return g_clear.Original()(device, count, rects, flags, colour, depth, stencil);
}

HRESULT __stdcall Draw(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, UINT first, UINT count)
{
    g_draws += g_tracing ? 1 : 0;
    return g_draw.Original()(device, type, first, count);
}

HRESULT __stdcall DrawIndexed(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT least, UINT vertices,
                              UINT first, UINT count)
{
    g_draws += g_tracing ? 1 : 0;
    return g_drawIndexed.Original()(device, type, base, least, vertices, first, count);
}

// Places the detours the frame's trace and our own scene target both stand
// on, and makes that target if it is asked for.
void WatchFramesOf(IDirect3DDevice9 *device)
{
    std::string word;
    std::ifstream(dao::Module::OwnFolder() / kOwnSceneFile) >> word;
    const bool ownScene = word != "off";
    std::error_code error;
    g_traceAsked = std::filesystem::exists(dao::Module::OwnFolder() / kLogFrameFile, error);
    if (!ownScene && !g_traceAsked)
    {
        return;
    }
    g_device = device;
    if (ownScene)
    {
        g_wideFormat = word == "8bit" ? D3DFMT_UNKNOWN : D3DFMT_A16B16G16R16F;
        g_createTexture.AttachTo(Method<CreateTextureFn>(device, kCreateTextureSlot), &CreateTexture);
    }
    g_present.AttachTo(Method<PresentFn>(device, kPresentSlot), &Present);
    IDirect3DSwapChain9 *chain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &chain)) && chain != nullptr)
    {
        g_chainPresent.AttachTo(Method<ChainPresentFn>(chain, kChainPresentSlot), &ChainPresent);
        chain->Release();
    }
    g_stretchRect.AttachTo(Method<StretchRectFn>(device, kStretchRectSlot), &StretchRect);
    g_setRenderTarget.AttachTo(Method<SetRenderTargetFn>(device, kSetRenderTargetSlot), &SetRenderTarget);
    g_setRenderState.AttachTo(Method<SetRenderStateFn>(device, kSetRenderStateSlot), &SetRenderState);
    g_clear.AttachTo(Method<ClearFn>(device, kClearSlot), &Clear);
    g_draw.AttachTo(Method<DrawFn>(device, kDrawSlot), &Draw);
    g_drawIndexed.AttachTo(Method<DrawIndexedFn>(device, kDrawIndexedSlot), &DrawIndexed);
    if (ownScene)
    {
        g_getRenderTarget.AttachTo(Method<GetRenderTargetFn>(device, kGetRenderTargetSlot), &GetRenderTarget);
        g_reset.AttachTo(Method<ResetFn>(device, kResetSlot), &Reset);
        MakeScene(device);
    }
}

void LogTargetsOf(IDirect3DDevice9 *device, const D3DPRESENT_PARAMETERS *parameters)
{
    std::error_code error;
    if (!std::filesystem::exists(dao::Module::OwnFolder() / kLogTargetsFile, error))
    {
        return;
    }
    if (parameters != nullptr)
    {
        spdlog::info("the back buffer: {} by {}, format {}, {} samples of quality {}", parameters->BackBufferWidth,
                     parameters->BackBufferHeight, static_cast<int>(parameters->BackBufferFormat),
                     static_cast<int>(parameters->MultiSampleType), parameters->MultiSampleQuality);
    }
    g_logTargets = true;
    g_createTexture.AttachTo(Method<CreateTextureFn>(device, kCreateTextureSlot), &CreateTexture);
    g_createRenderTarget.AttachTo(Method<CreateRenderTargetFn>(device, kCreateRenderTargetSlot), &CreateRenderTarget);
}

// Anisotropic filtering, which the game has no setting for: a texture seen at
// a slant (the ground ahead, a wall along a street) is read through a
// stretched footprint instead of a square one, and keeps its detail. The game
// asks for plain linear filtering; where it asks that of a sampler whose
// texture has mipmaps, it is given anisotropic filtering of the slider's
// degree instead. A sampler with no mipmaps is a render target, a shadow map
// or a lookup, and is left as asked. The game's own asking is remembered for
// each sampler, so the degree can change, or go, while the game runs.
constexpr std::size_t kSamplers = 16;            // the pixel shader's
constexpr std::size_t kSetSamplerStateSlot = 69; // IDirect3DDevice9::SetSamplerState
using SetSamplerStateFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
dao::Detour<SetSamplerStateFn> g_setSamplerState;
std::atomic<int> g_anisotropy{0}; // the slider's degree, 0 or 1 for the game's own filtering
// The drawing thread's own: what the game last asked of each sampler
// (Direct3D's defaults until it does), and the degree in force.
std::array<DWORD, kSamplers> g_askedMin = [] {
    std::array<DWORD, kSamplers> all{};
    all.fill(D3DTEXF_POINT);
    return all;
}();
std::array<DWORD, kSamplers> g_askedMip{}; // D3DTEXF_NONE
int g_anisotropyInForce = 0;

// Gives one sampler the minification filter it is to have.
void FilterSampler(IDirect3DDevice9 *device, std::size_t sampler)
{
    const auto index = static_cast<DWORD>(sampler);
    const bool stretched =
        g_anisotropyInForce > 1 && g_askedMin.at(sampler) == D3DTEXF_LINEAR && g_askedMip.at(sampler) != D3DTEXF_NONE;
    if (stretched)
    {
        g_setSamplerState.Original()(device, index, D3DSAMP_MAXANISOTROPY, static_cast<DWORD>(g_anisotropyInForce));
    }
    g_setSamplerState.Original()(device, index, D3DSAMP_MINFILTER,
                                 stretched ? D3DTEXF_ANISOTROPIC : g_askedMin.at(sampler));
}

HRESULT __stdcall SetSamplerState(IDirect3DDevice9 *device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (sampler >= kSamplers || (type != D3DSAMP_MINFILTER && type != D3DSAMP_MIPFILTER))
    {
        return g_setSamplerState.Original()(device, sampler, type, value);
    }
    (type == D3DSAMP_MINFILTER ? g_askedMin : g_askedMip).at(sampler) = value;
    const HRESULT result =
        type == D3DSAMP_MIPFILTER ? g_setSamplerState.Original()(device, sampler, type, value) : D3D_OK;
    FilterSampler(device, sampler);
    return result;
}

HRESULT __stdcall BeginScene(IDirect3DDevice9 *device)
{
    if (const int wanted = g_anisotropy.load(std::memory_order_relaxed);
        wanted != g_anisotropyInForce && g_setSamplerState.IsAttached())
    {
        g_anisotropyInForce = wanted;
        for (std::size_t sampler = 0; sampler < kSamplers; ++sampler)
        {
            FilterSampler(device, sampler);
        }
    }
    std::array<float, kLookRegisters * 4> values{};
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        values.at(i) = g_look.at(i).load(std::memory_order_relaxed);
    }
    values.at(kShareAsked) = ShareAsked() ? 1.0F : 0.0F;
    device->SetPixelShaderConstantF(kLookRegister, values.data(), kLookRegisters);
    return g_beginScene.Original()(device);
}

HRESULT __stdcall CreateDevice(IDirect3D9 *direct3d, UINT adapter, D3DDEVTYPE type, HWND window, DWORD behaviour,
                               D3DPRESENT_PARAMETERS *parameters, IDirect3DDevice9 **device)
{
    const HRESULT result = g_createDevice.Original()(direct3d, adapter, type, window, behaviour, parameters, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr && !g_beginScene.IsAttached())
    {
        const bool placed = g_beginScene.AttachTo(Method<BeginSceneFn>(*device, kBeginSceneSlot), &BeginScene);
        spdlog::info("the game's Direct3D device: {}",
                     placed ? "the options' registers are set every scene" : "no detour, the registers stay 0");
        g_setSamplerState.AttachTo(Method<SetSamplerStateFn>(*device, kSetSamplerStateSlot), &SetSamplerState);
        LogTargetsOf(*device, parameters);
        WatchFramesOf(*device);
    }
    return result;
}

IDirect3D9 *__stdcall Create(UINT version)
{
    IDirect3D9 *direct3d = g_create.Original()(version);
    if (direct3d != nullptr && !g_createDevice.IsAttached() &&
        !g_createDevice.AttachTo(Method<CreateDeviceFn>(direct3d, kCreateDeviceSlot), &CreateDevice))
    {
        spdlog::warn("Direct3D's CreateDevice: no detour, the registers stay 0");
    }
    return direct3d;
}

// Watches for the game's Direct3D device from Direct3DCreate9 on; called at
// load, before the game has made it.
void WatchForDevice()
{
    const HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (d3d9 == nullptr ||
        !g_create.AttachTo(reinterpret_cast<CreateFn>(GetProcAddress(d3d9, "Direct3DCreate9")), &Create))
    {
        spdlog::warn("Direct3DCreate9: no detour, the registers stay 0");
    }
}

constexpr std::wstring_view kChain = L"da_hsvmatrix";  // the chain build_shaders.py puts our values in
constexpr int kKeepEvery = 30;                         // frames between looks at the chain
constexpr const char *kChosenFile = "graphics.chosen"; // what is chosen, kept between sessions
constexpr std::wstring_view kSmaa = L"DaoSmaa";        // the chain's switch for its anti-aliasing (smaa.py)

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

// A stop read from graphics.chosen, held to its list.
int StopOf(std::string_view value, std::size_t stops)
{
    const std::string number(value);
    return std::clamp(static_cast<int>(std::strtol(number.c_str(), nullptr, 10)), 0, static_cast<int>(stops) - 1);
}

// What was chosen last session; what it does not say keeps its default.
void ReadChosen(const std::filesystem::path &folder)
{
    std::ifstream in(folder / kChosenFile);
    for (std::string line; std::getline(in, line);)
    {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos)
        {
            continue;
        }
        const std::string_view key = Trimmed(std::string_view(line).substr(0, equals));
        const std::string_view value = Trimmed(std::string_view(line).substr(equals + 1));
        if (key == "ImprovedMaterials")
        {
            g_materials.store(value == "On", std::memory_order_relaxed);
        }
        if (key == "ImprovedShadows")
        {
            g_shadows.store(value == "On", std::memory_order_relaxed);
        }
        if (key == "AntiAliasing")
        {
            g_smaa = value == "SMAA";
        }
        if (key == "AmbientOcclusion")
        {
            g_occlusionStop.store(StopOf(value, kOcclusionStops.size()), std::memory_order_relaxed);
        }
        if (key == "AnisotropicFiltering")
        {
            g_anisotropyStop.store(StopOf(value, kAnisotropyStops.size()), std::memory_order_relaxed);
        }
    }
}

// Keeps what is chosen for the next session.
void KeepChosen()
{
    const auto said = [](bool on) { return on ? "On" : "Off"; };
    std::ofstream out(dao::Module::OwnFolder() / kChosenFile, std::ios::trunc);
    out << "ImprovedMaterials = " << said(g_materials.load(std::memory_order_relaxed))
        << "\nImprovedShadows = " << said(g_shadows.load(std::memory_order_relaxed))
        << "\nAntiAliasing = " << (g_smaa ? "SMAA" : "Off")
        << "\nAmbientOcclusion = " << g_occlusionStop.load(std::memory_order_relaxed)
        << "\nAnisotropicFiltering = " << g_anisotropyStop.load(std::memory_order_relaxed) << '\n';
    if (!out)
    {
        spdlog::warn("{} was not written: the choice lasts this session", kChosenFile);
    }
}

// Sets one value of the chain if it is not that already. A chain that has no
// such value (a build without the pass that reads it) is left alone.
void Keep(std::wstring_view name, float value, std::string_view key)
{
    const std::optional<float> now = dao::framebuffer::Value(kChain, name);
    if (now && std::abs(*now - value) > 0.0001F)
    {
        dao::framebuffer::SetValue(kChain, name, value);
        spdlog::info("the chain's {} is {}", key, value);
    }
}

// One of the chain's values kept at what it is to be, with nothing said: for
// those that move every frame.
void KeepQuietly(std::wstring_view name, float value)
{
    if (const std::optional<float> has = dao::framebuffer::Value(kChain, name); has && std::abs(*has - value) > 1e-5F)
    {
        dao::framebuffer::SetValue(kChain, name, value);
    }
}

// Hands the build's shaders the options that are theirs (look.hlsl), and the
// anisotropic filtering its degree. The materials and a surface left uncut
// want a scene that holds more than white (materials.hlsl, look.hlsl's
// DaoCut), and are held at 0 without one.
void KeepLookRegisters()
{
    const bool materials = g_materials.load(std::memory_order_relaxed);
    const bool wide = materials && g_wideFormat != D3DFMT_UNKNOWN;
    const float shadows = g_shadows.load(std::memory_order_relaxed) ? 1.0F : 0.0F;
    g_look.at(kRelief).store(materials ? 1.0F : 0.0F, std::memory_order_relaxed);
    g_look.at(kMaterials).store(wide ? 1.0F : 0.0F, std::memory_order_relaxed);
    g_look.at(kUncut).store(wide ? 1.0F : 0.0F, std::memory_order_relaxed);
    g_look.at(kShadows).store(shadows, std::memory_order_relaxed);
    g_look.at(kShadowOnFigures).store(shadows, std::memory_order_relaxed);
    g_shareWanted.store(g_occlusionStop.load(std::memory_order_relaxed) > 0, std::memory_order_relaxed);
    g_anisotropy.store(kAnisotropyStops.at(static_cast<std::size_t>(g_anisotropyStop.load(std::memory_order_relaxed))),
                       std::memory_order_relaxed);
}

// The focal length of the picture, in pixels: what makes the angle between
// the rays through two pixels the angle between the two ways in the world
// that were drawn there. `a` and `b` are the pixels from the picture's
// middle, `cosine` that of the angle between the ways. The rays draw
// together as the length grows, so it is found by halving.
float FocalLength(const std::array<float, 2> &a, const std::array<float, 2> &b, float cosine)
{
    float low = 16;
    float high = 65536;
    for (int step = 0; step < 40; ++step)
    {
        const float f = std::sqrt(low * high);
        const float between =
            ((a[0] * b[0]) + (a[1] * b[1]) + (f * f)) /
            std::sqrt(((a[0] * a[0]) + (a[1] * a[1]) + (f * f)) * ((b[0] * b[0]) + (b[1] * b[1]) + (f * f)));
        (between < cosine ? low : high) = f;
    }
    return std::sqrt(low * high);
}

// A way in the world as the picture has it.
struct SeenWay
{
    std::array<float, 3> way;
    std::array<float, 2> at; // pixels from the middle
    float off;
};

// Where the picture has a way in the world: a point far off along it from
// the eye, asked its place on the screen. Nothing for a way behind the
// camera.
// @param middle the picture's middle, in pixels
std::optional<SeenWay> SeeWay(const dao::Vector &eye, const std::array<float, 2> &middle,
                              const std::array<float, 3> &way)
{
    constexpr float kFar = 20000.0F; // metres: far enough that where the camera is does not matter
    const std::optional<dao::world::ScreenPoint> point =
        dao::world::ToScreen({eye.x + (kFar * way[0]), eye.y + (kFar * way[1]), eye.z + (kFar * way[2])});
    if (!point || !point->inFront)
    {
        return std::nullopt;
    }
    const std::array<float, 2> at{point->x - middle[0], point->y - middle[1]};
    return SeenWay{way, at, std::hypot(at[0], at[1])};
}

// The two of fourteen ways about that are drawn nearest the middle: the
// nearest, and the next.
std::pair<std::optional<SeenWay>, std::optional<SeenWay>> NearestTheMiddle(const dao::Vector &eye,
                                                                           const std::array<float, 2> &middle)
{
    std::optional<SeenWay> first;
    std::optional<SeenWay> second;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int z = -1; z <= 1; ++z)
            {
                const int axes = std::abs(x) + std::abs(y) + std::abs(z);
                if (axes != 1 && axes != 3)
                {
                    continue;
                }
                const float length = std::sqrt(static_cast<float>(axes));
                std::optional<SeenWay> seen = SeeWay(
                    eye, middle,
                    {static_cast<float>(x) / length, static_cast<float>(y) / length, static_cast<float>(z) / length});
                if (seen && (!first || seen->off < first->off))
                {
                    second = first;
                    first = seen;
                }
                else if (seen && (!second || seen->off < second->off))
                {
                    second = seen;
                }
            }
        }
    }
    return {first, second};
}

// Tells the ambient occlusion its lens, each frame (occlusion.hlsl): the
// picture's width and height on a plane one unit before the eye, by the
// focal length two ways near the picture's middle give. Nothing of the
// camera is read but where it is: a point far off along a way in the world
// is asked its place on the screen (dao::world::ToScreen), which is right
// whatever camera draws, a conversation's too. In a frame with nothing to
// tell it by, the values stay.
void KeepLens()
{
    const std::optional<dao::world::ViewSize> size = dao::world::ScreenSize();
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    const dao::SceneCamera *camera = controller != nullptr ? controller->Scene() : nullptr;
    if (!size || camera == nullptr || size->width <= 0 || size->height <= 0)
    {
        return;
    }
    const dao::Vector eye = camera->Position();
    const std::array<float, 2> middle{static_cast<float>(size->width) / 2, static_cast<float>(size->height) / 2};
    const auto [first, second] = NearestTheMiddle(eye, middle);
    if (!first || !second || std::hypot(first->at[0] - second->at[0], first->at[1] - second->at[1]) < 8)
    {
        return;
    }
    const float cosine =
        (first->way[0] * second->way[0]) + (first->way[1] * second->way[1]) + (first->way[2] * second->way[2]);
    const float focal = FocalLength(first->at, second->at, cosine);
    if (!(focal > 0))
    {
        return;
    }
    KeepQuietly(L"DaoLensX", static_cast<float>(size->width) / focal);
    KeepQuietly(L"DaoLensY", static_cast<float>(size->height) / focal);
}

// Keeps the chain's values at the options', and the registers. The chain is
// asked each time, since a new game or a reload of the chain would put its
// values back to their own.
void ApplyOptions()
{
    static int frames = 0;
    if (++frames % kKeepEvery != 0)
    {
        return;
    }
    // The highlights' roll-off is for a scene that holds more than white:
    // in the back buffer's eight bits it would only dim what is nearly white.
    const bool highlights = g_materials.load(std::memory_order_relaxed) && g_wideFormat != D3DFMT_UNKNOWN;
    Keep(L"DaoHighlights", highlights ? 1.0F : 0.0F, "Highlights");
    Keep(L"DaoOcclusion", kOcclusionStops.at(static_cast<std::size_t>(g_occlusionStop.load(std::memory_order_relaxed))),
         "Occlusion");
    Keep(kSmaa, g_smaa ? 1.0F : 0.0F, "Smaa");
    KeepLookRegisters();
}

// The options screen's section, on the Video tab's right column, which holds
// a heading and twelve rows of 35 px (DAOLib #82), a slider 41 px (#84):
// three checkboxes and two sliders. The checkboxes and the sliders are the
// first release's (issue 23); the lighting's are on the dev branch.
constexpr const wchar_t *kOptionsTitle = L"Elgara Shaders";

// A checkbox kept in graphics.chosen.
dao::options::Toggle Checkbox(std::atomic<bool> &on, const wchar_t *name, const wchar_t *label, const wchar_t *tooltip,
                              const char *said)
{
    dao::options::Toggle toggle;
    toggle.name = name;
    toggle.label = label;
    toggle.tooltip = tooltip;
    toggle.get = [&on] { return on.load(std::memory_order_relaxed); };
    toggle.set = [&on, said](bool wanted) {
        on.store(wanted, std::memory_order_relaxed);
        spdlog::info("{}: {}", said, wanted ? "on" : "off");
        KeepLookRegisters();
        KeepChosen();
        return wanted;
    };
    return toggle;
}

// A slider over a list of stops kept in graphics.chosen.
template <std::size_t N>
dao::options::Choice StopSlider(std::atomic<int> &stop, const std::array<const wchar_t *, N> &names,
                                const wchar_t *name, const wchar_t *label, const wchar_t *tooltip, const char *said)
{
    dao::options::Choice slider;
    slider.name = name;
    slider.label = label;
    slider.tooltip = tooltip;
    slider.values.assign(names.begin(), names.end());
    slider.get = [&stop] { return static_cast<std::int32_t>(stop.load(std::memory_order_relaxed)); };
    slider.set = [&stop, said](std::int32_t wanted) {
        stop.store(std::clamp(wanted, 0, static_cast<std::int32_t>(N) - 1), std::memory_order_relaxed);
        spdlog::info("{}: stop {}", said, stop.load(std::memory_order_relaxed));
        KeepLookRegisters();
        KeepChosen();
        return wanted;
    };
    return slider;
}

void AddToOptions()
{
    dao::options::Toggle smaa;
    smaa.name = L"GraphicsAntiAliasing";
    smaa.label = L"SMAA";
    smaa.tooltip = L"Smooths the edges the game's own anti-aliasing misses; works with it or without";
    smaa.get = [] { return g_smaa; };
    smaa.set = [](bool wanted) {
        g_smaa = wanted;
        spdlog::info("SMAA: {}", wanted ? "on" : "off");
        KeepChosen();
        return wanted;
    };
    if (!dao::options::AddSection(
            kOptionsTitle,
            {Checkbox(g_materials, L"GraphicsMaterials", L"Improved materials",
                      L"Metal, gilt and polished stone reflect as what they are made of, and normal maps shape the "
                      L"light in shade and indoors",
                      "improved materials"),
             Checkbox(g_shadows, L"GraphicsShadows", L"Improved shadows",
                      L"Softer, finer shadows, which fall on characters too; their finer maps are made as the game "
                      L"starts",
                      "improved shadows"),
             smaa},
            {StopSlider(g_occlusionStop, kOcclusionNames, L"GraphicsOcclusion", L"Ambient occlusion",
                        L"Shade in corners and creases and under things (GTAO)", "ambient occlusion"),
             StopSlider(g_anisotropyStop, kAnisotropyNames, L"GraphicsAnisotropic", L"Anisotropic filtering",
                        L"Keeps textures sharp where they are seen at a slant", "anisotropic filtering")},
            dao::options::Column::Right, dao::options::Tab::Video))
    {
        spdlog::warn("the options screen did not take the section");
    }
}

// Says what became of the section once the options screen has been opened:
// it can be left out for want of room, which nothing else reports.
void LogOptionsSection()
{
    static dao::options::SectionState said = dao::options::SectionState::Waiting;
    const dao::options::SectionState now = dao::options::StateOf(kOptionsTitle);
    if (now == said)
    {
        return;
    }
    said = now;
    switch (now)
    {
    case dao::options::SectionState::Shown:
        spdlog::info("the options screen's Video tab shows the plugin's section");
        break;
    case dao::options::SectionState::NoRoom:
        spdlog::warn("the options screen has no room for the plugin's section: other sections took the Video tab's "
                     "rows");
        break;
    case dao::options::SectionState::Failed:
        spdlog::warn("the options screen was not as expected, and has no section of the plugin's");
        break;
    default:
        break;
    }
}

} // namespace

extern "C" __declspec(dllexport) bool __cdecl DAOLib_Load(const dao::LoaderInfo *info)
{
    dao::OpenLog("elgarashaders");
    if (info == nullptr || info->apiVersion != dao::kPluginApiVersion || !info->knownExecutable)
    {
        spdlog::error("not the loader or the game this was built for");
        return false;
    }
    ReadChosen(dao::Module::OwnFolder());
    materialpatch::Start(dao::Module::OwnFolder());
    KeepLookRegisters();
    if (!g_setPartitions.Attach(dao::addr::kShadowMaps_SetPartitions, &SetPartitions))
    {
        spdlog::warn("the shadow maps keep the game's size: no detour");
    }
    WatchForDevice();
    AddToOptions();
    dao::OnFrame(&LogOptionsSection);
    dao::OnFrame(&KeepLens);
    dao::OnFrame(&ApplyOptions);
    dao::OnFrame(&resourcedump::Poll);
    dao::OnFrame(&materialpatch::LogCounts);
    return true;
}
