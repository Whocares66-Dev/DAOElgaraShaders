// Elgara Shaders: the game's own picture drawn better, with no renderer of our own.
//
// Nearly all of it is not code here. The build makes the game's own shaders
// again, changed (build_shaders.py), into this plugin's override folder,
// where the game finds a file before its own archives':
//
// - The shaders that receive the sun's shadows filter the shadow maps with
//   sixteen taps on a turned disc, not the game's four or five (shadow.hlsl).
// - A character is lit by the sun the shadows are cast by, and shaded by
//   them: the game lights it from another side and reads no shadow map.
// - The last pass over the picture, the game's colour grade, also sharpens
//   it, gives its colours and middle tones more, and dithers it (grade.hlsl).
// - Before that pass, in the same chain of the game's, three passes of
//   anti-aliasing (smaa.py) and three of ambient occlusion (occlusion.hlsl,
//   occlusion_blur.hlsl), from the depth the game keeps beside the picture.
//
// What is code here:
//
// - The look is chosen in the game and made in a file. graphics.ini, beside
//   the DLL, has named looks, each how much of every change there is
//   (Sharpen, Vibrance, Contrast, Occlusion, OcclusionRadius, Sun: how much
//   stronger the area's sun is on its ground and buildings, and Relief,
//   Materials and ShadowOnFigures, which the level's and the figures' shaders
//   read from registers this sets on the game's Direct3D device), and the
//   options screen has a slider with a stop for each, under a dropdown that
//   turns the chain's anti-aliasing on. The shaders read those
//   as values of the game's own colour-grade chain, as it keeps an area's hue
//   and saturation, and this sets them there; it reads the file again when
//   it changes, so a look is tuned with the game running.
// - The sun's shadow maps are drawn at kMapSize a side, not the 512 or 1024
//   the game's shadow quality gives them: the engine is asked for its two
//   maps through one function, and this changes the size on the way in.
// - The scene's lights and its sunlight go to the log when they change.
//
// What it does goes to elgarashaders.log.
#include "ProbeBaker.h"
#include "ResourceDump.h"

#include "dao/Addresses.h"
#include "dao/Atmosphere.h"
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
#include "dao/core/LightProbes.h"
#include "dao/core/Msvc8.h"
#include "dao/generated/ScriptApi.h"

#include <spdlog/spdlog.h>

#include <d3d9.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

constexpr std::int32_t kMapSize = 2048; // texels a side; build_shaders.py is told the same

// One of the sun's shadow maps as the engine is asked for it.
// Notes on Partition: read 2026-10-10 in the function that asks (0x00455580):
// two of them on its stack, {10.0, size, format} and {sqrt of the 900 at
// 0x00BF20B4, size, format}, the size the global at 0x00BF20C4 that the
// shadow quality sets to 512 or 1024, the format 9 or 0x20 by a capability of
// the device. The function that takes them (0x00455420) copies each 12 bytes.
struct Partition
{
    float distance;      // how far from the camera the map reaches, in metres
    std::int32_t size;   // the map's width and height
    std::int32_t format; // the engine's texture format
};

using SetPartitionsFn = void(__cdecl *)(const Partition *, std::uint32_t);
dao::Detour<SetPartitionsFn> g_setPartitions;

void __cdecl SetPartitions(const Partition *partitions, std::uint32_t count)
{
    std::vector<Partition> ours(partitions, partitions + count);
    for (Partition &partition : ours)
    {
        spdlog::info("shadow map to {} m: {} a side (the game's {}), format {}", partition.distance, kMapSize,
                     partition.size, partition.format);
        partition.size = kMapSize;
    }
    g_setPartitions.Original()(ours.data(), count);
}

// The numbers the build's shaders read that are not the engine's to give:
// kLookRegisters float registers of the pixel shader from kLookRegister up,
// which none of the game's shaders reaches (their constants end under c70), so
// the engine never writes them and what is put there stays for every draw.
// build_shaders.py declares them as `daoLook` in each shader it rebuilds.
// Notes on the look's registers: the level's and the figures' shaders are
// handed what the engine has for them and nothing else. A number was first
// carried in the fourth of the sun's colour; an area with no sun (the Circle,
// the Dead Trenches) is given no sun's colour at all, so nothing arrived
// there (seen 2026-10-11: a build that paints the level green where the
// number is absent painted the Circle green). So they are set on the game's
// Direct3D device, taken as the game creates it, at the start of every
// scene, on the thread that draws.
constexpr UINT kLookRegister = 220;
constexpr std::size_t kLookRegisters = 4;
std::array<std::atomic<float>, kLookRegisters * 4> g_look{};
// The last of them is not a look's: whether the device's first target is the
// frame's own picture (look.hlsl's DAO_SHARE_ASKED), which is the drawing
// thread's to know and to say as the target changes (SetRenderTarget).
constexpr std::size_t kShareAsked = (kLookRegisters * 4) - 1;
bool g_drawsPicture = true;

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
    // And the glow's own small pictures (build_shaders.py's DaoGlowA to D): in
    // eight bits a faint glow is laid on in steps.
    const bool quarter = width == g_backWidth / 4 && height == g_backHeight / 4;
    const bool sixteenth = width == g_backWidth / 16 && height == g_backHeight / 16;
    return (usage & D3DUSAGE_RENDERTARGET) != 0 && format == D3DFMT_A8R8G8B8 && (whole || half || quarter || sixteenth);
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
bool g_tracing = false;
int g_draws = 0;

// The scene drawn into a render target of our own, 16-bit float. The game
// draws its depth pre-pass, its scene, its effects and its interface straight
// into the swap chain's back buffer, which is eight bits a channel and can be
// nothing else; a scene that holds more than white has to be drawn somewhere
// else. So wherever the game names the back buffer as a target, or as either
// end of a copy, it is given ours, of the same size and anti-aliasing, and as
// a frame is shown ours is resolved and drawn onto the real one.
// Notes on the scene target: seen in ten areas 2026-10-11, the Vanilla look
// in it matching the picture without it, and across two Resets of the device
// (the anti-aliasing changed in the options screen and changed back), ours
// made again each time. A file named kOwnSceneFile beside the DLL says
// otherwise: holding `off` the scene stays in the back buffer, holding
// `8bit` ours is made in the back buffer's own format.
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
    IDirect3DSurface9 *meter = nullptr;      // the picture made small, to be measured
    IDirect3DSurface9 *read = nullptr;       // and that brought to where it can be read
    UINT width = 0;
    UINT height = 0;
};
Scene g_scene;
// The picture's meter. How much of the picture as it was last shown is burnt
// out, at white or nearly, as a share of its middle (the corners are the
// game's own buttons and portraits): the drawing thread's word, for the game
// thread's keeping of the exposure (KeepSun). Measured every few frames on a
// copy 128 across, which costs a reading of 80 KB.
constexpr UINT kMeterWidth = 128;
constexpr UINT kMeterHeight = 80;
constexpr int kMeterEvery = 8;      // frames
constexpr float kMeterBurnt = 0.9F; // of white: a pixel this bright is burnt out
std::atomic<float> g_burnt{0};
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
    Released(g_scene.read);
    Released(g_scene.meter);
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
    // The meter reads halves; without the 16-bit target there is nothing over
    // white to make room for, and no meter.
    if (format != D3DFMT_A16B16G16R16F ||
        FAILED(device->CreateRenderTarget(kMeterWidth, kMeterHeight, format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                          &g_scene.meter, nullptr)) ||
        FAILED(device->CreateOffscreenPlainSurface(kMeterWidth, kMeterHeight, format, D3DPOOL_SYSTEMMEM, &g_scene.read,
                                                   nullptr)))
    {
        Released(g_scene.read);
        Released(g_scene.meter);
    }
}

// A half-precision float as a float.
float HalfAsFloat(std::uint16_t half)
{
    const int exponent = (half >> 10) & 0x1F;
    const float mantissa = static_cast<float>(half & 0x3FF) / 1024.0F;
    float value = 0;
    if (exponent == 0)
    {
        value = std::ldexp(mantissa, -14);
    }
    else if (exponent == 31)
    {
        value = 65504.0F;
    }
    else
    {
        value = std::ldexp(1.0F + mantissa, exponent - 15);
    }
    return (half & 0x8000) != 0 ? -value : value;
}

// Measures the picture just resolved: how much of its middle is burnt out.
void MeterScene(IDirect3DDevice9 *device)
{
    static int frames = 0;
    if (g_scene.meter == nullptr || ++frames % kMeterEvery != 0)
    {
        return;
    }
    D3DLOCKED_RECT pixels{};
    if (FAILED(g_stretchRect.Original()(device, g_scene.shownLevel, nullptr, g_scene.meter, nullptr, D3DTEXF_LINEAR)) ||
        FAILED(device->GetRenderTargetData(g_scene.meter, g_scene.read)) ||
        FAILED(g_scene.read->LockRect(&pixels, nullptr, D3DLOCK_READONLY)))
    {
        return;
    }
    std::size_t burnt = 0;
    std::size_t counted = 0;
    for (UINT y = kMeterHeight / 8; y < kMeterHeight - (kMeterHeight / 6); ++y)
    {
        const auto *row = reinterpret_cast<const std::uint16_t *>(static_cast<const std::byte *>(pixels.pBits) +
                                                                  (static_cast<std::size_t>(y) * pixels.Pitch));
        for (UINT x = kMeterWidth / 8; x < kMeterWidth - (kMeterWidth / 8); ++x)
        {
            const float most =
                std::max({HalfAsFloat(row[4 * x]), HalfAsFloat(row[(4 * x) + 1]), HalfAsFloat(row[(4 * x) + 2])});
            burnt += most >= kMeterBurnt ? 1 : 0;
            ++counted;
        }
    }
    g_scene.read->UnlockRect();
    g_burnt.store(static_cast<float>(burnt) / static_cast<float>(std::max<std::size_t>(counted, 1)),
                  std::memory_order_relaxed);
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
    MeterScene(device);
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

// The lights the frame's draws were handed, for the trace: the pixel
// shader's first twelve float registers and first four switches as each draw
// finds them, which in a figure's or a building's shader are g_vLightParams
// (a point light's place and reach in [0] and colour in [1], the sun's way
// and colour in [2] and [3], three more point lights in [5] to [11]) and the
// switches for the three and the sun. Each distinct set is counted, and the
// commonest are printed as the frame ends.
constexpr std::size_t kSetPixelFloatsSlot = 109; // IDirect3DDevice9::SetPixelShaderConstantF
constexpr std::size_t kSetPixelBoolsSlot = 113;  // IDirect3DDevice9::SetPixelShaderConstantB
using SetPixelFloatsFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, const float *, UINT);
using SetPixelBoolsFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, const BOOL *, UINT);
dao::Detour<SetPixelFloatsFn> g_setPixelFloats;
dao::Detour<SetPixelBoolsFn> g_setPixelBools;
struct LightsHanded
{
    std::array<float, 48> registers{};
    std::array<bool, 4> switches{};
    auto operator<=>(const LightsHanded &) const = default;
};
LightsHanded g_handed;                // as the device has them now
std::map<LightsHanded, int> g_lights; // each set a draw found, and how many draws

// The level's lamps, on figures. The engine lights a room's walls and floor
// by the lamps placed in it and a figure by three wide fill lights standing a
// long way off, and by no lamp: a figure a step from a brazier is as lit as
// one across the hall. That is its own choice of lights, and here a lamp is
// put where that choice finds it.
// Notes on the choice, read in the executable 2026-10-11 after the user's
// "did you investigate the code? if we need to, we can detour it":
// - Renderer::LightManagerDefault keeps six lists of lights, each with the
//   most an object takes from it (set at 0x00462210). As it takes a node's
//   lights in (0x00464E30), a point light goes by its ILight's kind (the
//   ILight's +0xC, the node's +0x18C) to one of two pairs of lists: kind 1
//   to +0x6C or +0x84, any other to +0x24 or +0xC, the first of each pair
//   for a light whose blueprint has a flag (0x0053C8D0), three for an
//   object, the second for one without, two.
// - A node is given its lights (0x004654E0) by one of two choosers by a bit
//   of its own (its +0x24, 0x4000): with the bit 0x00464560, from the first
//   pair; without it 0x00464310, from the second. Each has every light of a
//   list score itself against the object, sorts, and takes the best up to
//   the list's most (0x00463E40), fading a light in and out as it comes and
//   goes. A figure's shader has the flagged three in g_vLightParams [0], [5]
//   and [7] and one unflagged in [9].
// - A light's score (its virtual at +0x84, 0x0053D280) is its strength times
//   what is left of its reach at the object, and nothing for an object in a
//   room the light is not of (0x0053D210).
// - So a light lights either the things that move, the first pair, or the
//   level's own pieces, the second, by its kind, and never both. Seen as the
//   Circle loads: its lamps, 3 to 10 m, all go to +0x24, and the fills, 160
//   to 1000 m, to +0x6C; the unflagged lists get nothing. No figure is ever
//   offered a lamp, and the place its shader keeps in [9] is empty.
// Here a lamp, a point light of the level's lists with a lamp's reach, is put
// in both of the figures' lists as well, beside the fills in the flagged one
// (+0x6C) and alone in the unflagged (+0x84), by the list's own insert as the
// manager takes it in, and taken out as the manager lets it go. Left at that the fills would still take all three
// places: a score is strength times what is left of the reach, and a fill's reach is a thousand metres. So while the
// figures' chooser runs, a fill's score is made next to nothing (kFillScore), and a lamp's too where the figure is past
// the part of its reach that lights it (kLampNear). The engine then takes a
// figure's best three with the lamps that light it first and the fills for
// whatever places are left: beside three lamps no fill at all, away from
// every lamp the three fills as in vanilla. A lamp past that part of its
// reach still lights the figure a little, and the best of those takes the
// one unflagged place, which is scored for them alone (the user, 2026-10-11: "no fills but being affected by
// all the lights in the environment would be a more natural look"; without
// the fills a figure no lamp reaches is black, since the walls about it are
// lit by a baked map it has no part of). The engine fades each light in and
// out as it comes and goes. With the look's Lamps at 0 the fills' scores are
// left alone, and the choice is vanilla's.
// Seen first with the lamp in the unflagged list, one for a figure: in the
// Circle, a figure's draws with the wall lamp in [9] beside the three fills,
// the hero by it 153, 111, 87 for 83, 50, 47.
// Notes on what did not work, each measured as changing nothing on a figure:
// answering "no flag" for a lamp, or for every light (the figures then lost
// their fills and went dark); scoring the unflagged list without its test of
// a light's owner; putting a lamp in the flagged list of the wrong pair. And
// before those, the lamps were read from the lights the draws were handed
// and given to the figures' shaders in registers of our own, which guessed
// what a light was from what passed: it left ghosts of spells' lights, read
// values that were no lights, and knew only the lamps in view (the user,
// 2026-10-11: "are we hacking ourselves into a corner w/ the lamps?"). All
// of that is gone.
constexpr std::size_t kLevelsFlagged = 0x24; // the manager's lists
constexpr std::size_t kLevelsUnflagged = 0xC;
constexpr std::size_t kFiguresFlagged = 0x6C;
constexpr std::size_t kFiguresUnflagged = 0x84;
constexpr std::size_t kLightNodeScore = 0x184; // a float, what the light's Score leaves
// What a fill's score is multiplied by as a figure's lights are chosen: any
// lamp that reaches the figure then comes before any fill.
constexpr float kFillScore = 1.0e-6F;
// The part of its reach within which a lamp comes before the fills. Past it
// the lamp gives a figure a third of its light or less (the engine's fall-off
// is the square of a cosine), and a fill it took the place of would be
// missed: with every lamp that reached at all put first, a figure of the
// Arl's dungeon stood black, three far torches for its three fills (seen
// 2026-10-11). A judgement.
constexpr float kLampNear = 0.6F;
using NodePlaceFn = void(__fastcall *)(void *, int, float *);
constexpr std::size_t kNodePlaceSlot = 3;      // the node's virtual that says where it is in the world
constexpr std::size_t kLightNodeReach = 0x1B0; // a float, metres
constexpr float kLampReachMost = 100.0F;       // metres; a light that reaches further is a fill
// Metres; a light that reaches less is a glow on a thing, not a lamp in a
// room. A judgement (2026-10-11): Wynne's aura in Redcliffe is a violet
// light of 1.5 m at the middle of a body, made to tint the ground about it,
// and on figures it turned the faces beside her lavender for as long as the
// aura was up. The Circle's wall lamps reach 4 m.
constexpr float kLampReachLeast = 2.0F;
// Whatever these return is passed on as it came.
using ManagerLightsFn = std::uintptr_t(__fastcall *)(void *, int, void *);
using LightListFn = std::uintptr_t(__fastcall *)(void *, int, void *);
using ManagerChangedFn = void(__fastcall *)(void *, int);
dao::Detour<ManagerLightsFn> g_managerAdd;
dao::Detour<ManagerLightsFn> g_managerRemove;
dao::Detour<LightListFn> g_lightListInsert;
dao::Detour<LightListFn> g_lightListErase;
thread_local void *t_lightManager = nullptr; // the manager taking lights in or letting them go, on this thread
// The lamps put in the figures' lists, by their nodes: what tells them from
// the lights an area has for its figures already, some of a lamp's reach.
std::mutex g_ourLampsLock;
std::unordered_set<void *> g_ourLamps;
// And the lights the engine itself put in the figures' lists. A node can
// have a light of each kind, and be in the level's lists and the figures'
// both: Redcliffe's tavern lights its figures so, and with those taken for
// lamps of ours its figures stood dark in the Vanilla look (seen
// 2026-10-11). Such a light is the area's and is left alone.
std::unordered_set<void *> g_areasOwn;
thread_local bool t_lampsMoved = false;
// Every light in the level's lists, of any reach, and those of them found to
// be lamps of a level lit again (DimBakedLamps). In the order of their
// addresses, so that going through them is the same from one time to the
// next: a table by the pointers' hashes is gone through in an order that is
// nobody's (clang-tidy's bugprone-nondeterministic-pointer-iteration-order).
std::map<void *, void *> g_levelLights; // each with the manager whose list it is in
std::unordered_set<void *> g_bakedLamps;
std::atomic<std::size_t> g_bakedLampsCount{0};

void *ListOf(void *manager, std::size_t offset)
{
    return static_cast<std::byte *>(manager) + offset;
}

bool IsLevelsList(void *list)
{
    return t_lightManager != nullptr &&
           (list == ListOf(t_lightManager, kLevelsFlagged) || list == ListOf(t_lightManager, kLevelsUnflagged));
}

bool IsFiguresList(void *list)
{
    return t_lightManager != nullptr &&
           (list == ListOf(t_lightManager, kFiguresFlagged) || list == ListOf(t_lightManager, kFiguresUnflagged));
}

std::uintptr_t __fastcall LightListInsert(void *list, int unused, void *node)
{
    const std::uintptr_t result = g_lightListInsert.Original()(list, unused, node);
    if (node != nullptr && IsFiguresList(list))
    {
        const std::scoped_lock lock(g_ourLampsLock);
        g_areasOwn.insert(node);
    }
    if (node != nullptr && IsLevelsList(list))
    {
        {
            const std::scoped_lock lock(g_ourLampsLock);
            g_levelLights.insert_or_assign(node, t_lightManager);
        }
        float reach = 0;
        std::memcpy(&reach, static_cast<const std::byte *>(node) + kLightNodeReach, sizeof reach);
        if (reach >= kLampReachLeast && reach <= kLampReachMost)
        {
            g_lightListInsert.Original()(ListOf(t_lightManager, kFiguresFlagged), unused, node);
            g_lightListInsert.Original()(ListOf(t_lightManager, kFiguresUnflagged), unused, node);
            const std::scoped_lock lock(g_ourLampsLock);
            g_ourLamps.insert(node);
            t_lampsMoved = true;
        }
    }
    return result;
}

std::uintptr_t __fastcall LightListErase(void *list, int unused, void *node)
{
    const std::uintptr_t result = g_lightListErase.Original()(list, unused, node);
    if (node != nullptr && IsFiguresList(list))
    {
        const std::scoped_lock lock(g_ourLampsLock);
        g_areasOwn.erase(node);
    }
    if (node != nullptr && IsLevelsList(list))
    {
        // Whatever its reach is now: a light's reach can be moved, and one
        // left in the list after its node is gone would be read.
        g_lightListErase.Original()(ListOf(t_lightManager, kFiguresFlagged), unused, node);
        g_lightListErase.Original()(ListOf(t_lightManager, kFiguresUnflagged), unused, node);
        const std::scoped_lock lock(g_ourLampsLock);
        g_ourLamps.erase(node);
        g_levelLights.erase(node);
        g_bakedLamps.erase(node);
        g_bakedLampsCount.store(g_bakedLamps.size(), std::memory_order_relaxed);
        t_lampsMoved = true;
    }
    return result;
}

// The manager takes a node's lights in, or lets them go, with `call`.
std::uintptr_t ManagerLights(dao::Detour<ManagerLightsFn> &call, void *manager, int unused, void *object)
{
    t_lightManager = manager;
    t_lampsMoved = false;
    const std::uintptr_t result = call.Original()(manager, unused, object);
    t_lightManager = nullptr;
    if (t_lampsMoved)
    {
        // The figures choose again, as they do when the manager changes their lists itself.
        reinterpret_cast<ManagerChangedFn>(dao::Module::Address(dao::addr::kLightManager_MarkChanged))(manager, 0);
    }
    return result;
}

// The figures' chooser, the choice from one list within it, and a point
// light's score. Whatever they return is passed on as it came.
using ChooseForMovingFn = std::uintptr_t(__fastcall *)(void *, int, void *, void *, void *, std::uintptr_t);
using ChooseFromListFn = std::uintptr_t(__fastcall *)(void *, int, void *, float, void *, std::uintptr_t,
                                                      std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t,
                                                      void *);
using LightScoreFn = std::uintptr_t(__fastcall *)(void *, int, const float *, float, std::uintptr_t, std::uintptr_t);
dao::Detour<ChooseForMovingFn> g_chooseForMoving;
dao::Detour<ChooseFromListFn> g_chooseFromList;
dao::Detour<LightScoreFn> g_lightScore;
std::atomic<float> g_lampStrength{0}; // the game thread's word: the look's Lamps, 0 for no lamps
std::atomic<float> g_fillsLess{0};    // and its FillsLess, 0 for the fills as they are
// The scene the current area is drawn as, the game thread's word each frame,
// and where a light manager keeps the scene it is of: its base's constructor
// (0x0046C4F0, read 2026-10-11) stores it there.
std::atomic<void *> g_areasScene{nullptr};
constexpr std::size_t kManagerScene = 0x8;
// The least a fill's strength is written as: two of the game's face shaders
// test the sign of a light's fourth component, and none at all is not a sign.
constexpr float kFillLeast = 1e-3F;
// The fourth component of a light's colour, which the engine hands a shader
// with the rest and no shader of the game's reads as a number (two faces'
// test its sign): a lamp of ours carries the look's strength there.
constexpr std::size_t kLightNodeStrength = 0x1AC;
// This thread's: the manager choosing a figure's lights, and which of its
// two lists of them is being scored.
enum class FiguresList : std::uint8_t
{
    None,
    Flagged,
    Unflagged,
};
thread_local void *t_figuresManager = nullptr;
thread_local FiguresList t_figuresList = FiguresList::None;

// Light probes, blended, and more of them. A light probe is the light of a
// place in a level as its makers baked it: three matrices, one for each of
// red, green and blue, that give the light arriving on a surface by the way
// it faces. The engine chooses a figure one, the nearest of its room (the
// probe's score is one over its distance, nothing for a probe of another
// room), and a level has one to a room at most: 150 of the campaign's 232
// areas have one or none at all. So a figure's ambient light is its room's,
// the same in the room's darkest corner as under its lamps.
//
// With a look's ProbeBlend a figure is given a mix of the nearest few
// instead, each by the square of how far its score is above the next one's
// beyond them, so a probe has no part as it comes into the nearest few or
// leaves them and a figure's light never steps. That it steps without the
// mix was seen: a room given 43 probes of plain red, green and blue five
// metres apart had each figure one flat colour and another a step later
// (the user walked it, 2026-10-11). The mix is linear in the matrices, as
// light is.
//
// The probes mixed are the plugin's own where it has any: baked from the
// level's files on a thread of its own as the player comes into a level
// (ProbeBaker.h), a room the place and the light of the probe the level
// itself has there and the probes baked for it, and had here once the whole
// level is done. The engine's choice says which rooms: a probe of the
// level's that scores for a figure is of the figure's room or the next, and
// the baked room with that probe's place and light is the one; in a level
// made of terrain the level's one probe scores everywhere and its room is
// the whole level's. Where there is no such room the level's own probes are
// mixed. Nothing of the game's is replaced, and with ProbeBlend at 0 a
// figure's light is vanilla's.
//
// Where it goes, read 2026-10-11: a figure's lights are chosen into its
// light state (the groups at +0, +0x10, +0x20 for probes, +0x30), and the
// state's own function (0x0046B260, __thiscall with one argument, `ret 4`)
// fills from them what the shaders are handed when a material first asks in
// a frame: the point lights at +0x40 (g_vLightParams, 0xD0 bytes), the
// probe's cube map at +0x114, and its three matrices at +0x130, +0x170 and
// +0x1B0 (g_mLightParams), copied from the probe's blueprint (the node's
// +0x174, there at +0x160, +0x1A0 and +0x1E0) only when the probe chosen is
// not the one of last time, which the state keeps at +0x110 and nothing
// reads but to compare. So the mix is written over the three after that
// function has run, each time, and to give a state its probe's own light
// back the kept probe is made one no probe is, and the function copies
// again.
constexpr std::size_t kLightsProbes = 0x3C;     // the manager's list of light probes
constexpr std::size_t kStateProbes = 0x20;      // in a light state: the group of probes chosen
constexpr std::size_t kStateProbeKept = 0x110;  // the probe its matrices were last copied from
constexpr std::size_t kStateIrradiance = 0x130; // the three matrices
constexpr std::size_t kProbeBlueprint = 0x174;  // in a probe's node
constexpr std::size_t kBlueprintIrradiance = 0x160;
constexpr std::uintptr_t kNoProbe = 1;   // kept in a state to have its probe's light copied again
constexpr float kProbeKeyWithin = 0.05F; // metres: how near a level's probe is to a room's key to be it
// Where a light is, as its score asks: the second virtual of the light the
// node has at +0x180 (0x0053D423 in the probe's score). The node's own
// virtual that the lamps are placed by answers nothing for a probe: every
// probe of a level was at 0, 0, 0 by it (logged 2026-10-11).
constexpr std::size_t kNodeLight = 0x180;
constexpr std::size_t kLightPlaceSlot = 2;
using LightPlaceFn = void(__fastcall *)(void *, int, float *);
using dao::core::probes::Irradiance;
using StateFillFn = std::uintptr_t(__fastcall *)(void *, int, std::uintptr_t);
dao::Detour<LightScoreFn> g_probeScore;
dao::Detour<StateFillFn> g_stateFill;
std::atomic<float> g_probeBlend{0}; // the game thread's word: the look's ProbeBlend
using ProbeRoom = probebaker::Room;
using ProbeRooms = std::shared_ptr<const probebaker::Rooms>;
// How near a level's probe's light is to a room's for the room to be its:
// the level's files and the engine's copy of them are the same numbers.
constexpr float kProbeLightWithin = 1e-4F;
struct ScoredProbe
{
    void *node;
    float score;
};
struct Blend
{
    Irradiance light;
    bool wanted; // or the state is to have its own probe's light back
    bool whole;  // the mix is of probes that hold all the light of a place, and is not lifted
};
thread_local std::vector<ScoredProbe> *t_probesScored = nullptr;
std::mutex g_blendsLock;
std::unordered_map<void *, Blend> g_blends; // by light state
std::unordered_set<void *> g_statesWritten; // the light states whose matrices are ours now
// The look's Ambient, the game thread's word: how much more of its ambient
// light a figure takes where its area has no sun, 1 for twice. A level's own
// probe is the light that comes off the surfaces about it, as a camera
// there took it, and has none of the lamps' own: with the fills turned down
// it is lifted to stand for both, and so is a probe baked as the level's
// own was made, a terrain level's. A probe of a room's table is baked as all
// the light a surface there receives and is taken as it is. The lift was the
// shaders' until the table came (a multiplication where the ambient light
// joins a figure's lights), which could not tell the two apart.
std::atomic<float> g_ambient{0};
// The look's LightContrast, the game thread's word, and the brightness of
// baked light it turns about. The look may name that brightness (LightPivot);
// left at 0 it is the level's own middle, which the baker measured (a room's
// levelSeen) and the lights' chooser notes here when a figure of the area is
// given a baked room. A probe that holds all the light of its place is baked
// from the same lightmaps the walls are lit by, and is given the same
// contrast: its light by the power of how far its mean is from the pivot.
std::atomic<float> g_lightContrast{0};
std::atomic<float> g_lightPivot{0}; // the pivot in force: the look's, or the level's
std::atomic<float> g_levelSeen{0};  // the level's, or 0 while it is not known
// Whether the area's level has lightmaps of our own (Relighter.h), by the
// word of the baked room a figure of the area is given. Such lightmaps hold
// the level's lamps with their shadows and want no flat colour beside them:
// the level's ambient colour is taken away whole and its lamps are not added
// a second time by the shaders, whatever the look says of a level as the
// game baked it.
std::atomic<bool> g_levelRelit{false};

// The player's own tuning of the picture, a level at a time: four sliders of
// the options screen's, each a whole number of steps either side of nothing.
// The user asked for them so as to set a level to their liking and have what
// they set made that level's own (2026-10-11: "then i can tune it to my
// liking, save, you can look at the config, and make the settings the
// default for that map"). What the player has set is in graphics.chosen
// (`Tune.<level> = exposure, contrast, saturation, warmth`); what a level
// has when the player has set nothing is in graphics.tuned, the same lines,
// which is where a player's settings go to be every player's. The level is
// the one the area's figures were last matched to a room of.
enum TuneOf : std::size_t
{
    kTuneExposure,
    kTuneContrast,
    kTuneSaturation,
    kTuneWarmth,
    kTunes
};
using Tune = std::array<int, kTunes>;
constexpr int kTuneSteps = 4;                                                 // either side
constexpr std::array<float, kTunes> kTuneStep{0.25F, 0.1F, 0.125F, 0.125F}; // stops; and the chain's own units
std::mutex g_tuneLock;
std::map<std::string, Tune, std::less<>> g_tunedByPlayer;
std::map<std::string, Tune, std::less<>> g_tunedShipped;
std::string g_levelName; // under g_tuneLock: the lights' chooser says it, the game thread reads it

void NoteLevel(const std::string &level)
{
    const std::scoped_lock lock(g_tuneLock);
    if (g_levelName != level)
    {
        g_levelName = level;
    }
}

// The tuning in force: the player's for the level, else the level's own, else none.
Tune TuneNow()
{
    const std::scoped_lock lock(g_tuneLock);
    if (const auto found = g_tunedByPlayer.find(g_levelName); found != g_tunedByPlayer.end())
    {
        return found->second;
    }
    if (const auto found = g_tunedShipped.find(g_levelName); found != g_tunedShipped.end())
    {
        return found->second;
    }
    return {};
}

// Takes a line `Tune.<level> = a, b, c, d` into a table; false for any other line.
bool TakeTune(std::string_view key, std::string_view value, std::map<std::string, Tune, std::less<>> &into)
{
    constexpr std::string_view kPrefix = "Tune.";
    if (!key.starts_with(kPrefix))
    {
        return false;
    }
    Tune tune{};
    std::size_t at = 0;
    for (int &step : tune)
    {
        const std::size_t comma = value.find(',', at);
        const std::string number(value.substr(at, comma == std::string_view::npos ? comma : comma - at));
        step = std::clamp(static_cast<int>(std::strtol(number.c_str(), nullptr, 10)), -kTuneSteps, kTuneSteps);
        if (comma == std::string_view::npos)
        {
            break;
        }
        at = comma + 1;
    }
    into.insert_or_assign(std::string(key.substr(kPrefix.size())), tune);
    return true;
}

// The rooms lit again that the area's figures have been matched to since the
// area was come into, with the table that holds them: what a door's light is
// the mean of (RelitMean). A room is noted as its probes are chosen for a
// figure, when the engine has just handed over a probe that is alive.
std::mutex g_relitLock;
ProbeRooms g_relitTable;
std::vector<const ProbeRoom *> g_relitSeen;
bool g_relitMeanKnown = false;

// How many stops more a level lit again is exposed by for how dark it is,
// beside the look's InteriorExposure. A level's lights are what they are, and
// one level is ten times as dark as another: Orzammar's Commons, lit by fire
// and lava across a cave, has a quarter of the light where its people walk
// that the Circle's tower has by its candles and windows. One exposure for
// both left the Commons' doors and shop fronts black with their figures
// (toured 2026-10-11), and more light with no source is not the answer. So
// the picture is opened half way to what the eye would do: half the stops
// between the level's light and the tower's, the tower being what the look's
// InteriorExposure was set by. The level's light is the upper quartile of
// its probes', each the light on a surface whichever way it faces.
//
// Tried so, the Commons at 1.35 stops was "way too bright" (the user, the
// same day, of the agent's own window). So it is by the look's InteriorAdapt,
// a share of what is reckoned here, and that is 0: a dark level is dark.
std::atomic<float> g_relitAdapt{0};

float AdaptFor(std::span<const ProbeRoom *const> rooms)
{
    constexpr float kTowers = 0.12F; // the same quartile in the Circle's tower (lak303d, measured 2026-10-11)
    constexpr float kShare = 1.0F;   // the whole of it; the look's InteriorAdapt says how much is used
    std::vector<float> lumas;
    for (const ProbeRoom *room : rooms)
    {
        for (const dao::core::probes::Probe &probe : room->probes)
        {
            const auto whichever = [&](std::size_t channel) {
                const float *m = probe.light.data() + (channel * 16);
                return m[15] + ((m[0] + m[5] + m[10]) / 3);
            };
            lumas.push_back((0.2126F * whichever(0)) + (0.7152F * whichever(1)) + (0.0722F * whichever(2)));
        }
    }
    if (lumas.empty())
    {
        return 0;
    }
    const auto quartile = lumas.begin() + static_cast<std::ptrdiff_t>(lumas.size() * 3 / 4);
    std::ranges::nth_element(lumas, quartile);
    return std::clamp(kShare * std::log2(kTowers / std::max(*quartile, 1e-3F)), -2.0F, 3.0F);
}

void NoteRelitRoom(const ProbeRooms &rooms, const ProbeRoom *room)
{
    const std::scoped_lock lock(g_relitLock);
    if (g_relitTable != rooms)
    {
        g_relitTable = rooms;
        g_relitSeen.clear();
        g_relitMeanKnown = false;
    }
    if (std::ranges::find(g_relitSeen, room) == g_relitSeen.end())
    {
        g_relitSeen.push_back(room);
        g_relitMeanKnown = false;
        g_relitAdapt.store(AdaptFor(g_relitSeen), std::memory_order_relaxed);
    }
}

// Whether each area come into since the game started is lit again, by the
// area's id, as the last of its figures to be matched said. That a level is
// lit again is learnt when a figure's probes are next chosen, which is not
// at once: back in the Commons from a shop, the hall was drawn for a moment
// with its lightmaps read as the game's own (the user, 2026-10-11: "when
// transitioning area it takes a brief moment for lighting to change?"). An
// area that has been in before is known from its first frame. One come into
// for the first time by a door still has the moment: knowing it sooner
// wants the area's level by name, which the library does not yet say.
std::mutex g_areasLock;
std::unordered_map<std::uint32_t, bool> g_areasRelit;
std::atomic<std::uint32_t> g_areaNow{0};

void NoteAreaRelit(bool relit)
{
    g_levelRelit.store(relit, std::memory_order_relaxed);
    if (const std::uint32_t area = g_areaNow.load(std::memory_order_relaxed); area != 0)
    {
        const std::scoped_lock lock(g_areasLock);
        g_areasRelit[area] = relit;
    }
}

void ForgetRelitRooms()
{
    const std::scoped_lock lock(g_relitLock);
    g_relitTable.reset();
    g_relitSeen.clear();
    g_relitMeanKnown = false;
}
// How far the light of the shade is the sky's colour, under a sun: the look's
// CoolShade by the area's daylight, the game thread's word (KeepSun). The
// level's shaders tint their baked light by it (look.hlsl's kDaoSkyLight,
// which this is the same as), and a figure's ambient light is tinted here
// where it is not a room's.
std::atomic<float> g_coolShade{0};
constexpr std::array<float, 3> kSkyLight{0.70F, 0.95F, 1.40F};
// In a light state: the colour of the area's sun on this figure, the fourth
// register of the lights' block (+0x40), which the chooser writes before the
// state fills the rest (0x0046BB80).
constexpr std::size_t kStateSun = 0x70;

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

// The baked room of this probe of the level's, or null: the one whose key
// is where the probe is and whose own light is the probe's. The light tells
// two levels with a probe at one place apart, a village by day and by night.
const ProbeRoom *RoomOf(const ProbeRooms &rooms, void *node)
{
    const std::array<float, 4> at = PlaceOfLight(node);
    const float *light = LightOfProbe(node);
    for (const std::shared_ptr<const ProbeRoom> &room : *rooms)
    {
        if (Distance(room->key.data(), at.data()) > kProbeKeyWithin || light == nullptr)
        {
            continue;
        }
        const bool same = std::equal(room->own.begin(), room->own.end(), light, [](float ours, float theirs) {
            return std::abs(ours - theirs) <= kProbeLightWithin;
        });
        if (same)
        {
            return room.get();
        }
        // A few times a session: a room at the probe's place whose light is
        // not the probe's is another level's, or the files are not read as
        // the engine reads them, which the log is to say.
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 8)
        {
            spdlog::info("light probes: a room's key is the place of a probe whose light is not the room's ({:.4f} "
                         "against {:.4f}, {:.4f} against {:.4f})",
                         room->own[0], light[0], room->own[15], light[15]);
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

// The probes there are to mix for a figure at `place`, of the level's own
// that scored for it: the baked ones of each room there are any for, and
// then no others; else the level's own. `baked` says which, and `whole`
// whether the baked ones hold all the light of their place.
void LightsToMix(const ProbeRooms &rooms, const std::vector<ScoredProbe> &scored, const float *place,
                 std::vector<dao::core::probes::Near> &lights, bool &baked, bool &whole)
{
    lights.clear();
    baked = false;
    whole = true;
    for (const ScoredProbe &probe : scored)
    {
        if (const ProbeRoom *room = RoomOf(rooms, probe.node); room != nullptr && !room->probes.empty())
        {
            if (!baked)
            {
                lights.clear();
            }
            baked = true;
            whole = whole && room->whole;
            if (room->levelSeen > 0)
            {
                g_levelSeen.store(room->levelSeen, std::memory_order_relaxed);
            }
            NoteAreaRelit(room->relit);
            NoteLevel(room->level);
            if (room->relit)
            {
                NoteRelitRoom(rooms, room);
            }
            probebaker::Nearest(*room, place, lights);
        }
        else if (const float *light = baked ? nullptr : LightOfProbe(probe.node))
        {
            lights.push_back({probe.score, light});
        }
    }
    whole = whole && baked;
    if (!baked && !lights.empty())
    {
        NoteAreaRelit(false); // probes of the level's own and no room of ours: not a level lit again
    }
}

// Keeps the mix for the light state whose probes were just chosen into
// `group`, for a figure at `place`. With none scored and a probe chosen all
// the same, the choice was not made again and what was kept stands. One
// probe of the level's alone is no mix: the state has that probe's light
// from the engine.
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
    bool baked = false;
    bool whole = false;
    // The rooms as they are now, held while their probes' light is pointed at.
    const ProbeRooms rooms = probebaker::AllRooms();
    LightsToMix(rooms, scored, place, lights, baked, whole);
    const std::optional<Irradiance> mix = baked || lights.size() > 1 ? dao::core::probes::Mix(lights) : std::nullopt;
    void *state = static_cast<std::byte *>(group) - kStateProbes;
    const std::scoped_lock lock(g_blendsLock);
    if (mix)
    {
        g_blends[state] = {*mix, true, whole};
    }
    else if (const auto found = g_blends.find(state); found != g_blends.end())
    {
        found->second.wanted = false;
    }
}

// The engine making a source of a level's archive (ECResMan::ResMan::
// MakeSource): what tells the baker which level the player has come into. A level's rooms are in <level>\<level>.rim,
// the archive of a level's folder that has the folder's name. Read 2026-10-11: __thiscall, the archive's path as the
// engine's wide string, the source's kind, its priority and two more, `ret 0x14`; what it returns, the source made or
// nothing, is passed on. Whichever thread opens it; the baker only notes the path.
using OpenArchiveFn = std::uintptr_t(__fastcall *)(void *, int, const dao::core::WStringRep *, std::uintptr_t,
                                                   std::uintptr_t, std::uintptr_t, std::uintptr_t);
dao::Detour<OpenArchiveFn> g_openArchive;

std::uintptr_t __fastcall OpenArchive(void *self, int unused, const dao::core::WStringRep *path, std::uintptr_t a2,
                                      std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5)
{
    const std::uintptr_t result = g_openArchive.Original()(self, unused, path, a2, a3, a4, a5);
    // The engine names a level's archives by its own name for the levels'
    // folder: ENV:lak303d\lak303d.rim (logged 2026-10-11). Another name's
    // levels, an add-in's, are not baked yet.
    constexpr std::wstring_view kLevels = L"ENV:";
    if (path != nullptr && path->size > kLevels.size() && path->size < 1024 && path->View().starts_with(kLevels))
    {
        const std::filesystem::path file(path->View().substr(kLevels.size()));
        std::wstring extension = file.extension().wstring();
        std::ranges::transform(extension, extension.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (extension == L".rim" && _wcsicmp(file.stem().c_str(), file.parent_path().c_str()) == 0)
        {
            probebaker::Want(dao::Module::Path().parent_path().parent_path() / "packages" / "core" / "env" / file);
        }
    }
    return result;
}

// What a level's own probe's light is multiplied by for the figure of this
// light state: more by the look's Ambient where its area has no sun.
float AmbientLift(const void *state)
{
    const float ambient = g_ambient.load(std::memory_order_relaxed);
    if (ambient <= 0)
    {
        return 1.0F;
    }
    std::array<float, 3> sun{};
    std::memcpy(sun.data(), static_cast<const std::byte *>(state) + kStateSun, sizeof sun);
    const float indoors = 1.0F - std::clamp(20.0F * std::max({sun[0], sun[1], sun[2]}), 0.0F, 1.0F);
    return 1.0F + (ambient * indoors);
}

// The mix kept for a light state, if the look asks for mixes and it has one
// that is wanted; one no longer wanted is forgotten.
std::optional<Blend> BlendOf(void *state)
{
    const std::scoped_lock lock(g_blendsLock);
    const auto found = g_blends.find(state);
    if (found == g_blends.end())
    {
        return std::nullopt;
    }
    if (g_probeBlend.load(std::memory_order_relaxed) > 0 && found->second.wanted)
    {
        return found->second;
    }
    g_blends.erase(found);
    return std::nullopt;
}

// The light of the level's own probe a state was last filled from, or
// nothing if it has none.
std::optional<Irradiance> OwnProbesLight(const void *state)
{
    const std::byte *node = nullptr;
    std::memcpy(static_cast<void *>(&node), static_cast<const std::byte *>(state) + kStateProbeKept, sizeof node);
    if (reinterpret_cast<std::uintptr_t>(node) <= kNoProbe)
    {
        return std::nullopt;
    }
    const std::byte *blueprint = nullptr;
    std::memcpy(static_cast<void *>(&blueprint), node + kProbeBlueprint, sizeof blueprint);
    if (blueprint == nullptr)
    {
        return std::nullopt;
    }
    Irradiance light{};
    std::memcpy(light.data(), blueprint + kBlueprintIrradiance, sizeof light);
    return light;
}

// In a level lit again, the light for a state that has no mix of probes: the
// mean of the probes of the level's rooms that its figures have been matched
// to, with nothing lifted. Only what moves is given a mix as its probes are
// chosen; a door, a chest, a banner has the level's own probe's light from
// the engine, which is a photograph of the level as the game lit it.
// Orzammar's great doors stood red, and the Proving's pink, in corners where
// nothing else was lit (toured 2026-10-11). Where such a thing stands the
// state does not say, so it has the level's mean: dim in a dim level, and
// never the photograph.
//
// The probe a state keeps a pointer to is NOT asked which room it is of. The
// first form did, and the pointer outlives its probe: back in the Commons
// from a house, a state still held a probe of the level before, the probe's
// light was called through an empty table of functions, and the user's game
// was gone (2026-10-11 18:31, RoomOf under RelitMeanOf under StateFill).
// Nothing here follows a pointer the engine may have let go.
//
// No probe counts for more than a dim light in the mean: the few that stand
// by a brazier or over the lava are a hundred times the rest, and with them
// in whole the mean was a bright one, the Proving's doors pale pink between
// dark walls. At a fifth of white they were black doors in lit doorways, and
// at one and a half they read as the stone about them does; the look's
// StillMost says how much, so that it is set with the game running (of
// white, the light on a surface whichever way it faces).
std::atomic<float> g_stillMost{1.5F};

std::optional<Blend> RelitMean()
{
    if (!g_levelRelit.load(std::memory_order_relaxed))
    {
        return std::nullopt;
    }
    static Irradiance mean{};
    static float mostWas = 0;
    const float most = g_stillMost.load(std::memory_order_relaxed);
    const std::scoped_lock lock(g_relitLock);
    if (g_relitSeen.empty())
    {
        return std::nullopt;
    }
    if (!g_relitMeanKnown || most != mostWas)
    {
        std::array<double, 48> sum{};
        std::size_t count = 0;
        for (const ProbeRoom *room : g_relitSeen)
        {
            for (const dao::core::probes::Probe &probe : room->probes)
            {
                const auto whichever = [&](std::size_t channel) {
                    const float *m = probe.light.data() + (channel * 16);
                    return m[15] + ((m[0] + m[5] + m[10]) / 3);
                };
                const float luma = (0.2126F * whichever(0)) + (0.7152F * whichever(1)) + (0.0722F * whichever(2));
                const double by = luma > most ? double{most} / double{luma} : 1.0;
                for (std::size_t at = 0; at < sum.size(); ++at)
                {
                    sum.at(at) += by * double{probe.light.at(at)};
                }
                ++count;
            }
        }
        if (count == 0)
        {
            return std::nullopt;
        }
        for (std::size_t at = 0; at < sum.size(); ++at)
        {
            mean.at(at) = static_cast<float>(sum.at(at) / static_cast<double>(count));
        }
        g_relitMeanKnown = true;
        mostWas = most;
    }
    return Blend{mean, true, true};
}

std::uintptr_t __fastcall StateFill(void *state, int unused, std::uintptr_t argument)
{
    std::optional<Blend> mix = BlendOf(state);
    if (!mix)
    {
        mix = RelitMean();
    }
    const float lift = AmbientLift(state);
    const bool ours = mix.has_value() || lift != 1.0F;
    if (!ours)
    {
        // Matrices of ours left in the state are the engine's to write again.
        const std::scoped_lock lock(g_blendsLock);
        if (g_statesWritten.erase(state) != 0)
        {
            std::memcpy(static_cast<std::byte *>(state) + kStateProbeKept, &kNoProbe, sizeof kNoProbe);
        }
    }
    const std::uintptr_t result = g_stateFill.Original()(state, unused, argument);
    if (!ours)
    {
        return result;
    }
    std::optional<Irradiance> light = mix ? std::optional<Irradiance>(mix->light) : OwnProbesLight(state);
    if (!light)
    {
        return result;
    }
    if (!mix || !mix->whole)
    {
        const float cool = g_coolShade.load(std::memory_order_relaxed);
        for (std::size_t at = 0; at < light->size(); ++at)
        {
            light->at(at) *= lift * std::lerp(1.0F, kSkyLight.at(at / 16), cool);
        }
    }
    else if (const float pivot = g_lightPivot.load(std::memory_order_relaxed); pivot > 0)
    {
        // The light a surface gets there whichever way it faces: each
        // matrix's last term and a third of its first three down the diagonal.
        const auto mean = [&](std::size_t channel) {
            const float *m = light->data() + (channel * 16);
            return m[15] + ((m[0] + m[5] + m[10]) / 3);
        };
        const float luma = (0.2126F * mean(0)) + (0.7152F * mean(1)) + (0.0722F * mean(2));
        const float contrast = std::clamp(
            std::pow(std::max(luma, 1e-3F) / pivot, g_lightContrast.load(std::memory_order_relaxed)), 0.3F, 3.0F);
        for (float &value : *light)
        {
            value *= contrast;
        }
    }
    std::memcpy(static_cast<std::byte *>(state) + kStateIrradiance, light->data(), sizeof *light);
    const std::scoped_lock lock(g_blendsLock);
    g_statesWritten.insert(state);
    return result;
}

std::atomic<void *> g_probesList{nullptr};

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
    if (t_figuresManager != nullptr)
    {
        if (list == ListOf(t_figuresManager, kFiguresFlagged))
        {
            t_figuresList = FiguresList::Flagged;
        }
        else if (list == ListOf(t_figuresManager, kFiguresUnflagged))
        {
            t_figuresList = FiguresList::Unflagged;
        }
        else
        {
            t_figuresList = FiguresList::None;
        }
    }
    // The probes' list of the manager last seen, which is the area's: a thing
    // that does not move (a door, a shop's goods, a banner) has its probes
    // chosen from the same list by another caller, with no manager said. It
    // is given a mix by its own place as a figure is, where before it had
    // the level's mean, and a counter's goods glowed in a dark shop as if
    // the lava were beside them (the user, 2026-10-11: "more random glowing
    // from a shop front"). The pointer is only compared, never followed.
    if (t_figuresManager != nullptr)
    {
        g_probesList.store(ListOf(t_figuresManager, kLightsProbes), std::memory_order_relaxed);
    }
    const bool still = t_figuresManager == nullptr && list == g_probesList.load(std::memory_order_relaxed) &&
                       g_levelRelit.load(std::memory_order_relaxed);
    const bool probes = ((t_figuresManager != nullptr && list == ListOf(t_figuresManager, kLightsProbes)) || still) &&
                        g_probeBlend.load(std::memory_order_relaxed) > 0;
    thread_local std::vector<ScoredProbe> scored;
    if (probes)
    {
        scored.clear();
        t_probesScored = &scored;
    }
    const std::uintptr_t result =
        g_chooseFromList.Original()(list, unused, place, size, group, changed, owned, a7, a8, a9, only);
    t_figuresList = FiguresList::None;
    if (probes)
    {
        t_probesScored = nullptr;
        KeepBlend(group, scored, static_cast<const float *>(place));
        if (static std::atomic<int> said{0}; still && said.fetch_add(1) < 3)
        {
            const auto *at = static_cast<const float *>(place);
            spdlog::info(
                "light probes: a thing that does not move had its probes chosen at ({:.1f}, {:.1f}, {:.1f}), {} scored",
                at[0], at[1], at[2], scored.size());
        }
    }
    return result;
}

// What a light's score is multiplied by as a figure's lights are chosen from
// `list`. A light the area has for its figures is left as it is, but that
// with lamps asked for a fill of the flagged list goes behind them, and that
// a fill is given what the look's FillsLess leaves of it, for the shaders to
// read. A fill is a light of the current area's scene that reaches further
// than a lamp. Of the area's scene, and not any far light: a portrait and
// the inventory's figure are each a scene of their own, with a manager of
// its own and three lights for figures of some 200 m, a warm key of 1.25
// and two lesser, which are all the light that scene has (logged
// 2026-10-11, beside the Circle's three fills of 1000 m and a fifth of
// that). The shaders took every far light of a sunless scene for a fill
// until then, by reach alone, and the look's FillsLess of a half took 40%
// off every portrait and the inventory's figure; at 1 they were black (the
// user saw it, 2026-10-11). A lamp
// of ours, with none asked for, is not chosen at all: the choice is then
// vanilla's. Asked for, it is chosen from the flagged list, three for a
// figure, if it lights the figure, and from the unflagged, one, if it is too
// far to: so no lamp is taken twice.
float ScoreScale(FiguresList list, void *node, const float *place)
{
    const float strength = g_lampStrength.load(std::memory_order_relaxed);
    const bool lamps = strength > 0;
    float reach = 0;
    std::memcpy(&reach, static_cast<const std::byte *>(node) + kLightNodeReach, sizeof reach);
    bool ours = false;
    {
        const std::scoped_lock lock(g_ourLampsLock);
        ours = g_ourLamps.contains(node) && !g_areasOwn.contains(node);
    }
    if (!ours)
    {
        void *scene = nullptr;
        std::memcpy(static_cast<void *>(&scene), static_cast<const std::byte *>(t_figuresManager) + kManagerScene,
                    sizeof scene);
        const bool fill = reach > kLampReachMost && scene == g_areasScene.load(std::memory_order_relaxed);
        if (fill)
        {
            const float kept = std::max(1.0F - g_fillsLess.load(std::memory_order_relaxed), kFillLeast);
            std::memcpy(static_cast<std::byte *>(node) + kLightNodeStrength, &kept, sizeof kept);
        }
        return lamps && fill && list == FiguresList::Flagged ? kFillScore : 1.0F;
    }
    if (!lamps)
    {
        return 0.0F;
    }
    {
        // A lamp of a level lit again is out (DimBakedLamps), for figures too.
        const std::scoped_lock lock(g_ourLampsLock);
        if (g_bakedLamps.contains(node))
        {
            return 0.0F;
        }
    }
    std::memcpy(static_cast<std::byte *>(node) + kLightNodeStrength, &strength, sizeof strength);
    std::array<float, 4> at{};
    (*static_cast<NodePlaceFn **>(node))[kNodePlaceSlot](node, 0, at.data());
    const bool lights = std::hypot(at[0] - place[0], at[1] - place[1], at[2] - place[2]) <= kLampNear * reach;
    return lights == (list == FiguresList::Flagged) ? 1.0F : 0.0F;
}

std::uintptr_t __fastcall LightScore(void *node, int unused, const float *place, float size, std::uintptr_t room,
                                     std::uintptr_t other)
{
    const std::uintptr_t result = g_lightScore.Original()(node, unused, place, size, room, other);
    // A lamp of a level lit again scores nothing, for the level and for its
    // figures: it is then chosen for neither (DimBakedLamps).
    if (g_bakedLampsCount.load(std::memory_order_relaxed) != 0 && g_levelRelit.load(std::memory_order_relaxed))
    {
        const std::scoped_lock lock(g_ourLampsLock);
        if (g_bakedLamps.contains(node))
        {
            const float none = 0;
            std::memcpy(static_cast<std::byte *>(node) + kLightNodeScore, &none, sizeof none);
            return result;
        }
    }
    if (t_figuresList != FiguresList::None)
    {
        auto *at = static_cast<std::byte *>(node) + kLightNodeScore;
        float score = 0;
        std::memcpy(&score, at, sizeof score);
        score *= ScoreScale(t_figuresList, node, place);
        std::memcpy(at, &score, sizeof score);
    }
    return result;
}

std::uintptr_t __fastcall ManagerAdd(void *manager, int unused, void *object)
{
    return ManagerLights(g_managerAdd, manager, unused, object);
}

std::uintptr_t __fastcall ManagerRemove(void *manager, int unused, void *object)
{
    return ManagerLights(g_managerRemove, manager, unused, object);
}

HRESULT __stdcall SetPixelFloats(IDirect3DDevice9 *device, UINT first, const float *values, UINT count)
{
    for (UINT i = 0; i < count && first + i < 12; ++i)
    {
        std::copy_n(values + (4 * static_cast<std::size_t>(i)), 4,
                    g_handed.registers.begin() + static_cast<std::ptrdiff_t>(4 * static_cast<std::size_t>(first + i)));
    }
    return g_setPixelFloats.Original()(device, first, values, count);
}

HRESULT __stdcall SetPixelBools(IDirect3DDevice9 *device, UINT first, const BOOL *values, UINT count)
{
    for (UINT i = 0; i < count && first + i < 4; ++i)
    {
        g_handed.switches.at(first + i) = values[i] != FALSE;
    }
    return g_setPixelBools.Original()(device, first, values, count);
}

void LogLightsHanded()
{
    std::vector<std::pair<int, LightsHanded>> sets;
    sets.reserve(g_lights.size());
    for (const auto &[handed, draws] : g_lights)
    {
        sets.emplace_back(draws, handed);
    }
    std::ranges::sort(sets, [](const auto &a, const auto &b) { return a.first > b.first; });
    spdlog::info("  {} distinct sets of the first twelve registers; the commonest:", sets.size());
    for (std::size_t i = 0; i < sets.size() && i < 14; ++i)
    {
        const auto &r = sets[i].second.registers;
        const auto &s = sets[i].second.switches;
        const auto four = [&r](std::size_t at) {
            return fmt::format("({:.2f} {:.2f} {:.2f} {:.2f})", r.at(4 * at), r.at((4 * at) + 1), r.at((4 * at) + 2),
                               r.at((4 * at) + 3));
        };
        spdlog::info("    {:4} draws: switches {}{}{}{} [0]{} [1]{} [2]{} [3]{} [5]{} [6]{} [7]{} [8]{} [9]{} [11]{}",
                     sets[i].first, s[0] ? 1 : 0, s[1] ? 1 : 0, s[2] ? 1 : 0, s[3] ? 1 : 0, four(0), four(1), four(2),
                     four(3), four(5), four(6), four(7), four(8), four(9), four(11));
    }
    g_lights.clear();
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
        LogLightsHanded();
    }
    ++g_presented;
    g_tracing = g_presented % kTraceEvery == kTraceFirst;
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
// the picture is drawn into, and anywhere else the alpha blends as the
// colour does, which is the game's way.
DWORD g_separateAlphaAsked = FALSE;

HRESULT __stdcall SetRenderState(IDirect3DDevice9 *device, D3DRENDERSTATETYPE state, DWORD value)
{
    if (state == D3DRS_SEPARATEALPHABLENDENABLE)
    {
        g_separateAlphaAsked = value;
        value = g_drawsPicture ? value : FALSE;
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

// The look's last register as the drawing thread has it now: the look's
// three numbers and whether the frame's own picture is drawn into.
void HandShareAsked(IDirect3DDevice9 *device)
{
    std::array<float, 4> last{};
    for (std::size_t i = 0; i + 1 < last.size(); ++i)
    {
        last.at(i) = g_look.at(kShareAsked - 3 + i).load(std::memory_order_relaxed);
    }
    last.back() = g_drawsPicture ? 1.0F : 0.0F;
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
                                            picture ? g_separateAlphaAsked : FALSE);
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
    if (g_tracing)
    {
        ++g_lights[g_handed];
    }
    return g_draw.Original()(device, type, first, count);
}

HRESULT __stdcall DrawIndexed(IDirect3DDevice9 *device, D3DPRIMITIVETYPE type, INT base, UINT least, UINT vertices,
                              UINT first, UINT count)
{
    g_draws += g_tracing ? 1 : 0;
    if (g_tracing)
    {
        ++g_lights[g_handed];
    }
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
    if (!ownScene && !std::filesystem::exists(dao::Module::OwnFolder() / kLogFrameFile, error))
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
    if (std::filesystem::exists(dao::Module::OwnFolder() / kLogFrameFile, error))
    {
        g_setPixelFloats.AttachTo(Method<SetPixelFloatsFn>(device, kSetPixelFloatsSlot), &SetPixelFloats);
        g_setPixelBools.AttachTo(Method<SetPixelBoolsFn>(device, kSetPixelBoolsSlot), &SetPixelBools);
    }
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
// texture has mipmaps, it is given anisotropic filtering of the look's
// degree instead. A sampler with no mipmaps is a render target, a shadow map
// or a lookup, and is left as asked. The game's own asking is remembered for
// each sampler, so the degree can change, or go, while the game runs.
constexpr std::size_t kSamplers = 16;            // the pixel shader's
constexpr std::size_t kSetSamplerStateSlot = 69; // IDirect3DDevice9::SetSamplerState
using SetSamplerStateFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
dao::Detour<SetSamplerStateFn> g_setSamplerState;
std::atomic<int> g_anisotropy{0}; // the look's degree, 0 or 1 for the game's own filtering
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
    values.at(kShareAsked) = g_drawsPicture ? 1.0F : 0.0F;
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
                     placed ? "the look's registers are set every scene" : "no detour, the look's registers stay 0");
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
        spdlog::warn("Direct3D's CreateDevice: no detour, the look's registers stay 0");
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
        spdlog::warn("Direct3DCreate9: no detour, the look's registers stay 0");
    }
}

// The lights the scene holds, logged when their counts change: what an area
// has for the shaders to be given, which indoors is the question (a room's
// torches may be lights, or only light baked into its lightmap).
void LogLights()
{
    static std::array<std::int32_t, 4> last{-1, -1, -1, -1};
    const std::array<std::int32_t, 4> now{dao::Global<std::int32_t>(dao::addr::kLightManager_PointLights),
                                          dao::Global<std::int32_t>(dao::addr::kLightManager_PointLightsOther),
                                          dao::Global<std::int32_t>(dao::addr::kLightManager_LightsOfType2),
                                          dao::Global<std::int32_t>(dao::addr::kLightManager_LightsOfType3)};
    if (now != last)
    {
        last = now;
        spdlog::info("lights in the scene: {} point, {} point of the other sort, {} of type 2, {} of type 3", now[0],
                     now[1], now[2], now[3]);
    }
}

// The sunlight as the area has it now, logged when it changes: its two
// colours and the way it travels, to set beside the way the shadows fall.
void LogSunlight()
{
    static std::optional<dao::atmosphere::Sunlight> last;
    const std::optional<dao::atmosphere::Sunlight> now = dao::atmosphere::CurrentSunlight();
    if (now && now != last)
    {
        last = now;
        spdlog::info("sunlight: travels ({:.2f}, {:.2f}, {:.2f}), colour ({:.2f}, {:.2f}, {:.2f}), on creatures "
                     "({:.2f}, {:.2f}, {:.2f})",
                     now->direction[0], now->direction[1], now->direction[2], now->color[0], now->color[1],
                     now->color[2], now->characterColor[0], now->characterColor[1], now->characterColor[2]);
    }
}

// The sky as the area has it now, logged when it changes: what there is to
// work with for the sun's disc, the haze and the fog.
void LogSky()
{
    static std::optional<dao::atmosphere::Sky> last;
    const std::optional<dao::atmosphere::Sky> now = dao::atmosphere::CurrentSky();
    if (now && now != last)
    {
        last = now;
        std::string line;
        for (const dao::core::SkyField &field : dao::core::kSkyFields)
        {
            line += fmt::format("{}{} {:.3f}", line.empty() ? "" : ", ", field.name, (*now).*field.member);
        }
        spdlog::info("sky: {}", line);
    }
}

// One value of a look: its name in graphics.ini and in the game's
// colour-grade chain, and how much of it the look has.
struct LookValue
{
    std::string key;   // as the file has it
    std::wstring name; // as the chain has it: the same after "Dao"
    float value;
};

// A look: a section of graphics.ini, and a stop of the options screen's slider.
struct Look
{
    std::string name;
    std::vector<LookValue> values;
};

constexpr std::wstring_view kChain = L"da_hsvmatrix"; // the chain build_shaders.py puts our values in
constexpr int kLookEvery = 30;                        // frames between looks at the file and the chain
constexpr const char *kLooksFile = "graphics.ini";
constexpr const char *kChosenFile = "graphics.chosen"; // what is chosen, kept between sessions
constexpr const char *kTunedFile = "graphics.tuned";   // each level's own tuning, where the player has set none
// The look a player who has chosen none starts with: the lighting, with no
// sharpening and no change of saturation or contrast. Those are taste, and
// are to get controls of their own; the named looks that carry them are
// placeholders (the user, 2026-10-11).
constexpr std::string_view kFirstLook = "Lighting only";
constexpr std::string_view kVanillaLook = "Vanilla";
constexpr std::wstring_view kSmaa = L"DaoSmaa"; // the chain's switch for its anti-aliasing (smaa.py)

// The looks and the one chosen: the game thread's, as the options screen's
// callbacks and the frame's are.
std::vector<Look> g_looks;
std::size_t g_chosen = 0;
bool g_smaa = false; // SMAA beside the game's own anti-aliasing, or not

// The options screen's two sliders that are no level's but the player's
// machine's: how dark the ambient occlusion is, and the degree of the
// anisotropic filtering (the user, 2026-10-12: "implement gtao, and add an
// option / slider for it. also add one for anisotropic filtering"). Each is
// a stop of its list, kept in graphics.chosen; in a look but Vanilla they
// stand in place of the look's Occlusion and Anisotropy.
constexpr std::array<float, 4> kOcclusionStops{0.0F, 0.5F, 1.0F, 1.5F};          // Off, Low, Medium, High
constexpr std::array<const wchar_t *, 4> kOcclusionNames{L"Off", L"Low", L"Medium", L"High"};
constexpr std::array<int, 5> kAnisotropyStops{0, 2, 4, 8, 16};
constexpr std::array<const wchar_t *, 5> kAnisotropyNames{L"Off", L"2x", L"4x", L"8x", L"16x"};
std::atomic<int> g_occlusionStop{2};  // Medium
std::atomic<int> g_anisotropyStop{4}; // 16x

// A stop read from graphics.chosen, held to its list.
int StopOf(std::string_view value, std::size_t stops)
{
    const std::string number(value);
    return std::clamp(static_cast<int>(std::strtol(number.c_str(), nullptr, 10)), 0, static_cast<int>(stops) - 1);
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

std::wstring Wide(std::string_view text)
{
    std::wstring wide;
    for (const char letter : text)
    {
        wide.push_back(static_cast<wchar_t>(letter));
    }
    return wide;
}

// One line of a look, `Name = number`, or nothing if it is not one.
std::optional<LookValue> ReadValue(std::string_view text)
{
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos)
    {
        return std::nullopt;
    }
    const std::string_view key = Trimmed(text.substr(0, equals));
    const std::string_view number = Trimmed(text.substr(equals + 1));
    float value = 0;
    if (key.empty() || std::from_chars(number.data(), number.data() + number.size(), value).ec != std::errc{})
    {
        return std::nullopt;
    }
    return LookValue{std::string(key), L"Dao" + Wide(key), value};
}

// Reads graphics.ini: `[A look's name]` starts a look, a line under it is
// `Name = number`, and what follows a ';' is a comment.
std::vector<Look> ReadLooks(const std::filesystem::path &file)
{
    std::vector<Look> looks;
    std::ifstream in(file);
    for (std::string line; std::getline(in, line);)
    {
        const std::string_view text = Trimmed(std::string_view(line).substr(0, line.find(';')));
        if (text.size() > 2 && text.front() == '[' && text.back() == ']')
        {
            looks.push_back({std::string(Trimmed(text.substr(1, text.size() - 2))), {}});
        }
        else if (std::optional<LookValue> value = ReadValue(text); value && !looks.empty())
        {
            looks.back().values.push_back(std::move(*value));
        }
        else if (!text.empty())
        {
            spdlog::warn("{}: not a look's name or a value under one: {}", kLooksFile, line);
        }
    }
    return looks;
}

// The place of the look of a name among the looks, or nothing.
std::optional<std::size_t> LookNamed(std::string_view name)
{
    for (std::size_t index = 0; index < g_looks.size(); ++index)
    {
        if (g_looks[index].name == name)
        {
            return index;
        }
    }
    return std::nullopt;
}

// What was chosen last session: the look by its name (kFirstLook, if there
// was none or the file no longer has it), and the anti-aliasing.
void ReadChosen(const std::filesystem::path &folder)
{
    g_chosen = LookNamed(kFirstLook).value_or(0);
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
        if (key == "Look")
        {
            g_chosen = LookNamed(value).value_or(g_chosen);
        }
        const std::scoped_lock lock(g_tuneLock);
        TakeTune(key, value, g_tunedByPlayer);
    }
    // And what each level has of its own, beside it.
    std::ifstream shipped(folder / kTunedFile);
    for (std::string line; std::getline(shipped, line);)
    {
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos && !line.starts_with(';'))
        {
            const std::scoped_lock lock(g_tuneLock);
            TakeTune(Trimmed(std::string_view(line).substr(0, equals)), Trimmed(std::string_view(line).substr(equals + 1)),
                     g_tunedShipped);
        }
    }
}

// Keeps what is chosen for the next session.
void KeepChosen()
{
    std::ofstream out(dao::Module::OwnFolder() / kChosenFile, std::ios::trunc);
    out << "Look = " << g_looks[g_chosen].name << "\nAntiAliasing = " << (g_smaa ? "SMAA" : "Off") << '\n';
    out << "AmbientOcclusion = " << g_occlusionStop.load(std::memory_order_relaxed) << "\nAnisotropicFiltering = "
        << g_anisotropyStop.load(std::memory_order_relaxed) << '\n';
    {
        const std::scoped_lock lock(g_tuneLock);
        for (const auto &[level, tune] : g_tunedByPlayer)
        {
            out << "Tune." << level << " = " << tune[0] << ", " << tune[1] << ", " << tune[2] << ", " << tune[3] << '\n';
        }
    }
    if (!out)
    {
        spdlog::warn("{} was not written: the choice lasts this session", kChosenFile);
    }
}

std::int32_t ChooseLook(std::int32_t wanted)
{
    g_chosen = static_cast<std::size_t>(wanted);
    spdlog::info("the look: {}", g_looks[g_chosen].name);
    KeepChosen();
    return wanted;
}

std::int32_t ChooseAntiAliasing(std::int32_t wanted)
{
    g_smaa = wanted == 1;
    spdlog::info("extra anti-aliasing: {}", g_smaa ? "SMAA" : "off");
    KeepChosen();
    return wanted;
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

// The chosen look's value of a name, or `otherwise` if it has none: for the
// values the plugin acts on itself, which are no value of the chain.
float ChosenValue(std::string_view key, float otherwise = 0)
{
    for (const LookValue &value : g_looks[g_chosen].values)
    {
        if (value.key == key)
        {
            return value.value;
        }
    }
    return otherwise;
}

// What one of the player's sliders adds, in the units of what it is added to:
// nothing in the Vanilla look, which is the game as it is.
bool VanillaChosen()
{
    return g_chosen < g_looks.size() && g_looks[g_chosen].name == kVanillaLook;
}

float TunedBy(TuneOf which)
{
    if (VanillaChosen())
    {
        return 0.0F;
    }
    return static_cast<float>(TuneNow().at(which)) * kTuneStep.at(which);
}

// Keeps the sun on the world at the look's strength. An area's sunlight has
// a colour for its ground and buildings, which their shaders add to the
// baked light by the surface's normal map, its gloss and the shadow maps,
// and another for its creatures. The first is kept at the area's own times
// 1 + `sun`, and the second at its own times 1 + `onFigures`: with the first
// alone at three times, Redcliffe's ground was in bright sun and its people
// stood on it as dim as before (seen 2026-10-11). An area with no sun of its
// own (Orzammar's Commons has the level's colour black, seen the same day)
// is given none.
//
// The picture follows the sun too. The look's Exposure is for an area with a
// sun and counts for nothing in one without, which its makers lit for the
// picture as it is; and the brightness that comes out white (grade.hlsl's
// fDaoRange) is one and a half whites with no sun and longer by the sun's
// strength with one.
//
// A moon is a sun to the engine, and three times a moon is a day: the camp
// by night came out as bright as noon (seen 2026-10-11). What tells them
// apart in the areas looked at is the light's colour: a moon's has half as
// much red as blue (the camp's 0.25, 0.37, 0.48), a sun's three quarters or
// more, the coldest of them over snow (Haven's 0.42, 0.54, 0.57). Under a
// moon nothing is made stronger and the sky is left as it is.
//
// And how bright a day comes out depends on what the sun falls on, which
// nothing here knows: snow in a sun four times the area's own is all burnt
// out where a forest's floor in it is well lit (Soldier's Peak, the same
// day). The exposure brought down for the snow, as a camera's would be, left
// the shade beside it black and the people in it not to be seen: sun on
// snow is thirteen times its shade there, more than a picture holds. So it
// is the sun that is held back: where more than a few parts in a hundred of
// the picture's middle are at white (ShowScene's meter), the look's sun is
// taken down, to a fifth at the least, and it comes back slowly as the burn
// goes. The shade is left as it is.
// The share of the look's sun in force, by the meter: the game thread's own.
// The air's light (KeepAir) is the same sun's and is held back with it, or a
// sky burnt out towards the sun would hold the ground's sun down and stay
// burnt itself.
float g_sunHeld = 1;

float Daylight(const dao::atmosphere::Sunlight &own)
{
    const float warmth = own.color[0] / std::max(own.color[2], 1e-3F);
    const float t = std::clamp((warmth - 0.55F) / 0.15F, 0.0F, 1.0F);
    const float lit = std::clamp(20 * std::max({own.color[0], own.color[1], own.color[2]}), 0.0F, 1.0F);
    return lit * t * t * (3 - (2 * t));
}

void KeepSun(float sun, float onFigures, float exposure)
{
    constexpr float kRangeIndoors = 1.5F;
    constexpr float kRangeBySun = 1.2F;
    constexpr float kBurntMost = 0.04F;   // of the picture's middle: more, and the sun is held back
    constexpr float kBurntLeast = 0.015F; // less, and it comes back
    constexpr float kHeldLeast = 0.2F;    // of the look's sun
    float &held = g_sunHeld;
    static std::array<float, 4> heldFor{};
    static float logged = -1;
    const std::optional<dao::atmosphere::Sunlight> own = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sunlight> now = dao::atmosphere::CurrentSunlight();
    if (!own || !now)
    {
        return;
    }
    const float day = Daylight(*own);
    if (heldFor != own->color)
    {
        heldFor = own->color; // another area: the meter starts again
        held = 1;
    }
    const float burnt = g_burnt.load(std::memory_order_relaxed);
    if (sun * day <= 0)
    {
        held = 1;
    }
    else if (burnt > kBurntMost)
    {
        held = std::max(held - std::min(0.1F, 0.01F + (burnt - kBurntMost)), kHeldLeast);
    }
    else if (burnt < kBurntLeast)
    {
        held = std::min(held + 0.004F, 1.0F);
    }
    sun *= day * held;
    onFigures *= day * held;
    const float cool = std::clamp(ChosenValue("CoolShade"), 0.0F, 1.0F) * day;
    g_coolShade.store(cool, std::memory_order_relaxed);
    g_look.at(9).store(cool, std::memory_order_relaxed);
    dao::atmosphere::Sunlight wanted = *now;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        wanted.color.at(channel) = own->color.at(channel) * (1 + sun);
        wanted.characterColor.at(channel) = own->characterColor.at(channel) * (1 + onFigures);
    }
    wanted.color.at(3) = own->color.at(3);
    wanted.characterColor.at(3) = own->characterColor.at(3);
    // An area with no sun still has one for its figures alone, a warm light
    // to show them off by (the Circle's is 0.71, 0.50, 0.29 where its
    // ground's is black): in a hall lit by its candles they stood as bright
    // as in a studio. The look's KeyLess takes of it.
    if (std::max({own->color[0], own->color[1], own->color[2]}) <= 0)
    {
        // In a level lit again the room is as dark as its lights leave it, and
        // most of that light goes: at the look's own a party stood pale in the
        // dark between a hall's candles (seen in the Circle, 2026-10-11).
        // All of it, since the same day: what was left of it, a sixth, lit
        // Orzammar's great doors red in corners where nothing else was lit
        // (a door is lit as a figure is), and went dark with it gone.
        constexpr float kRelitKeyLess = 1.0F;
        const float less = g_levelRelit.load(std::memory_order_relaxed)
                               ? std::max(ChosenValue("KeyLess"), kRelitKeyLess)
                               : ChosenValue("KeyLess");
        const float left = 1 - std::clamp(less, 0.0F, 1.0F);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            wanted.characterColor.at(channel) = own->characterColor.at(channel) * left;
        }
    }
    // A level lit again is lit as light is: a lamp is many times brighter on
    // the pillar it hangs from than on the floor across the room, more than a
    // picture made for a room's even light can hold. So such a room is
    // exposed for its shade, a stop up, and the picture's range is long
    // enough for what is near a lamp to roll off and not burn (the user,
    // 2026-10-11, of a hall lit by its lanterns alone: "too dark now").
    // How many stops is the look's InteriorExposure, so that it is set with
    // the game running: one where a look has none.
    constexpr float kRelitExposure = 1.0F; // stops
    constexpr float kRelitRange = 8.0F;    // whites
    const bool relit = day <= 0 && g_levelRelit.load(std::memory_order_relaxed);
    Keep(L"DaoExposure",
         TunedBy(kTuneExposure) +
             (relit ? ChosenValue("InteriorExposure", kRelitExposure) +
                          (ChosenValue("InteriorAdapt", 0.0F) * g_relitAdapt.load(std::memory_order_relaxed))
                    : exposure * day),
         "Exposure");
    // Not through Keep: the meter moves it twice a second, which is no news.
    const float range = relit ? kRelitRange : kRangeIndoors + (kRangeBySun * sun);
    if (const std::optional<float> has = dao::framebuffer::Value(kChain, L"DaoRange");
        has && std::abs(*has - range) > 0.0001F)
    {
        dao::framebuffer::SetValue(kChain, L"DaoRange", range);
    }
    // Said when it has moved by a fifth: the meter moves it a little at a time.
    if (wanted != *now && dao::atmosphere::SetSunlight(wanted) && std::abs(sun - logged) > 0.2F)
    {
        logged = sun;
        spdlog::info("the world: its sun {:.2f} times the area's own", 1 + sun);
    }
}

// Hands the build's shaders the chosen look's numbers that are theirs
// (look.hlsl): how strongly a level's baked light is shaped by its normal
// maps, how far the figures' highlights are corrected, and how much of the
// sun's shadow falls on figures.
void KeepLookRegisters()
{
    // A look's value by its place in the registers (look.hlsl); those with no
    // name here are reckoned, below and in KeepSun.
    constexpr std::array<std::string_view, 14> kKeys{
        "Relief", "Materials", "ShadowOnFigures", "Lamps",        "FillsLess",   "Ambient",  "Fire", "",
        "",       "",          "Highlights",      "LampsOnLevel", "AmbientLess", "BakedLess"};
    for (std::size_t i = 0; i < kKeys.size(); ++i)
    {
        if (kKeys.at(i).empty())
        {
            continue;
        }
        // The materials and a surface left uncut want a scene that holds more
        // than white (materials.hlsl, look.hlsl's DaoCut).
        const bool idle = (kKeys.at(i) == "Materials" || kKeys.at(i) == "Highlights") && g_wideFormat == D3DFMT_UNKNOWN;
        g_look.at(i).store(idle ? 0.0F : ChosenValue(kKeys.at(i)), std::memory_order_relaxed);
    }
    g_anisotropy.store(VanillaChosen() ? static_cast<int>(ChosenValue("Anisotropy"))
                                       : kAnisotropyStops.at(static_cast<std::size_t>(
                                             g_anisotropyStop.load(std::memory_order_relaxed))),
                       std::memory_order_relaxed);
    g_lampStrength.store(ChosenValue("Lamps"), std::memory_order_relaxed);
    // The fills, the same: a level lit again has the light of its place in
    // its probes, and the fills are what it would be flat by.
    constexpr float kRelitFillsLess = 1.0F; // all of them: see kRelitKeyLess
    g_fillsLess.store(g_levelRelit.load(std::memory_order_relaxed) ? std::max(ChosenValue("FillsLess"), kRelitFillsLess)
                                                                   : ChosenValue("FillsLess"),
                      std::memory_order_relaxed);
    g_probeBlend.store(ChosenValue("ProbeBlend"), std::memory_order_relaxed);
    g_ambient.store(ChosenValue("Ambient"), std::memory_order_relaxed);
    g_stillMost.store(ChosenValue("StillMost", 1.5F), std::memory_order_relaxed);
    // The contrast's pivot: the look's own, or the level's middle; and with
    // neither known, no contrast.
    const float named = ChosenValue("LightPivot");
    const float pivot = named > 0 ? named : g_levelSeen.load(std::memory_order_relaxed);
    const float contrast = pivot > 0 ? ChosenValue("LightContrast") : 0.0F;
    g_look.at(7).store(contrast, std::memory_order_relaxed);
    g_look.at(8).store(pivot, std::memory_order_relaxed);
    g_lightContrast.store(contrast, std::memory_order_relaxed);
    g_lightPivot.store(contrast > 0 ? pivot : 0.0F, std::memory_order_relaxed);
    if (g_levelRelit.load(std::memory_order_relaxed))
    {
        // LampsOnLevel is left as the look has it: the level's own lamps are
        // put out one by one (DimBakedLamps), and every other light, a
        // spell's, lights the level as it does in vanilla. They were all put
        // out together once, by -1 here, and a spell lit nothing in the dark
        // of the Deep Roads (the user, 2026-10-11: "effects should cast
        // light").
        g_look.at(12).store(1.0F, std::memory_order_relaxed); // AmbientLess: all of it
    }
    // And the lightmaps are read as ours are written (look.hlsl's DAO_RELIT).
    g_look.at(14).store(g_levelRelit.load(std::memory_order_relaxed) ? 1.0F : 0.0F, std::memory_order_relaxed);
}

// Keeps the sky to the sun the look has. An area's fog and clouds have
// colours of their own, set for the sun the area was made with, and under a
// sun three times that they stay as dark as they were: Redcliffe's ground lay
// in bright sun under a slate sky, its mill thirty metres off grey with fog
// (seen 2026-10-11). Haze lit by a sun takes the sun's colour, and so do
// clouds. `haze` is how much of the sun's light, as the area has it for
// its ground, is added to the fog's, in the sky's colour; `clouds` how far the clouds' colour is
// moved to the sun's and how much thinner they are. An area with no sun has
// nothing added, and at 0 both are the area's own.
//
// And the area's fog hides most of what the engine could draw. Redcliffe's
// is capped at 0.85, which leaves 15 percent of a castle three hundred
// metres off, and its sky, the engine's own reckoning of air and haze under
// a low orange sun, is drawn at one part in a hundred (its "atmosphere
// alpha"). With the cap at 0.4 and the sky shown whole the village stands
// under a lit evening sky with its castle in the sun (seen 2026-10-11).
// `clear` is how much of the fog's cap and thickness is taken away, and
// `sky` how far the engine's sky is shown.
void KeepSky(float haze, float clouds, float clear, float sky)
{
    const std::optional<dao::atmosphere::Sky> own = dao::atmosphere::AreaOwnSky();
    const std::optional<dao::atmosphere::Sky> now = dao::atmosphere::CurrentSky();
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    if (!own || !now || !sun)
    {
        return;
    }
    constexpr float kCloudsThinner = 0.5F; // of their density, at the most
    constexpr float kCapLess = 0.6F;       // of the fog's cap, at the most
    constexpr float kFogThinner = 0.5F;    // of its thickness
    dao::atmosphere::Sky wanted = *own;
    // The haze of the distance is lit by the sun and sends on the sky's
    // colour, as the air does away from the sun; the sun's own colour in it
    // is towards the sun, which is SunHaze's (KeepAir). With the sun's own
    // colour all round, a village in it was one sheet of yellow from the
    // ground to the sky (seen in Redcliffe, 2026-10-11).
    const float sunBright = (0.2126F * sun->color[0]) + (0.7152F * sun->color[1]) + (0.0722F * sun->color[2]);
    wanted.fogRed += haze * sunBright * kSkyLight[0];
    wanted.fogGreen += haze * sunBright * kSkyLight[1];
    wanted.fogBlue += haze * sunBright * kSkyLight[2];
    const float day = Daylight(*sun);
    clear *= day;
    sky *= day;
    const float lit = std::clamp(clouds, 0.0F, 1.0F) * day;
    wanted.cloudRed = std::lerp(own->cloudRed, std::max(own->cloudRed, sun->color[0]), lit);
    wanted.cloudGreen = std::lerp(own->cloudGreen, std::max(own->cloudGreen, sun->color[1]), lit);
    wanted.cloudBlue = std::lerp(own->cloudBlue, std::max(own->cloudBlue, sun->color[2]), lit);
    wanted.cloudDensity = own->cloudDensity * (1 - (kCloudsThinner * lit));
    wanted.fogCap = own->fogCap * (1 - (kCapLess * std::clamp(clear, 0.0F, 1.0F)));
    wanted.fogIntensity = own->fogIntensity * (1 - (kFogThinner * std::clamp(clear, 0.0F, 1.0F)));
    wanted.atmosphereAlpha =
        std::lerp(own->atmosphereAlpha, std::max(own->atmosphereAlpha, 1.0F), std::clamp(sky, 0.0F, 1.0F));
    // A level lit again has no fog. An area's fog is a colour laid over what
    // is far off, the Circle's a warm yellow-brown: in a hall as dark as its
    // candles leave it that is a light with no source, and the far end of a
    // room was brighter than the floor underfoot (the user, 2026-10-11:
    // "areas are brighter when distant ... then darker when near").
    //
    // But with none at all a great cave has no depth, its far districts as
    // crisp as the street (the user, the same day: "might it be worth
    // bringing a form of it back for atmosphere?"). So a level lit again
    // keeps the look's RelitFog of its fog, as a haze that dims what is far
    // and does not light it: the fog's cap by that share, and its colour
    // held to a dark one, no brighter than kHazeMost.
    if (g_levelRelit.load(std::memory_order_relaxed))
    {
        constexpr float kHazeMost = 0.06F; // of white, the haze's brightest channel
        const float kept = std::clamp(ChosenValue("RelitFog", 0.0F), 0.0F, 1.0F);
        wanted.fogCap = own->fogCap * kept;
        wanted.fogIntensity = kept > 0 ? own->fogIntensity : 0.0F;
        const float brightest = std::max({own->fogRed, own->fogGreen, own->fogBlue, 1e-4F});
        const float dim = std::min(1.0F, kHazeMost / brightest);
        wanted.fogRed = own->fogRed * dim;
        wanted.fogGreen = own->fogGreen * dim;
        wanted.fogBlue = own->fogBlue * dim;
    }
    // A look may set any value of the sky outright, to find what a look
    // should do with it: "SkyFogCap = 0.5" for the sky's "fog cap".
    constexpr float kUnset = -12345.0F;
    for (const dao::core::SkyField &field : dao::core::kSkyFields)
    {
        std::string key = "Sky";
        bool start = true;
        for (const char letter : std::string_view(field.name))
        {
            if (letter != ' ')
            {
                key += start ? static_cast<char>(std::toupper(static_cast<unsigned char>(letter))) : letter;
            }
            start = letter == ' ';
        }
        if (const float set = ChosenValue(key, kUnset); set != kUnset)
        {
            wanted.*field.member = set;
        }
    }
    if (wanted != *now)
    {
        dao::atmosphere::SetSky(wanted);
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

// Tells the picture's passes its lens, each frame: the picture's width and
// height on a plane one unit before the eye, by the focal length two ways
// near the picture's middle give.
// @return the focal length in pixels, or nullopt in a frame with nothing to tell it by: the values stay
std::optional<float> KeepLens(const dao::world::ViewSize &size, const dao::SceneCamera &camera)
{
    const dao::Vector eye = camera.Position();
    const std::array<float, 2> middle{static_cast<float>(size.width) / 2, static_cast<float>(size.height) / 2};
    const auto [first, second] = NearestTheMiddle(eye, middle);
    if (!first || !second || std::hypot(first->at[0] - second->at[0], first->at[1] - second->at[1]) < 8)
    {
        return std::nullopt;
    }
    const float cosine =
        (first->way[0] * second->way[0]) + (first->way[1] * second->way[1]) + (first->way[2] * second->way[2]);
    const float focal = FocalLength(first->at, second->at, cosine);
    if (!(focal > 0))
    {
        return std::nullopt;
    }
    KeepQuietly(L"DaoLensX", static_cast<float>(size.width) / focal);
    KeepQuietly(L"DaoLensY", static_cast<float>(size.height) / focal);
    return focal;
}

// Tells the picture's last passes where the sun is in it, each frame, and
// what colour its light is, for the air it lights and its shafts (grade.hlsl's
// DaoAir, glow_shafts.hlsl): the look's SunHaze and Shafts. Nothing of the
// camera is read but where it is: a point far off along a way in the world
// is asked its place on the screen (dao::world::ToScreen), which is right
// whatever camera draws, a conversation's too. Two such ways near the
// picture's middle give its focal length; the way to the sun gives the sun's
// place, or the way from it the place straight against the sun when the sun
// is behind.
void KeepAir()
{
    const float strength = g_chosen < g_looks.size() ? std::max(ChosenValue("SunHaze"), ChosenValue("Shafts")) : 0.0F;
    const std::optional<dao::atmosphere::Sunlight> sun = dao::atmosphere::AreaOwnSunlight();
    const std::optional<dao::atmosphere::Sky> sky = dao::atmosphere::CurrentSky();
    const std::optional<dao::world::ViewSize> size = dao::world::ScreenSize();
    const dao::CClientExoApp *app = dao::CClientExoApp::Get();
    const dao::GameModeController *modes = app != nullptr ? app->GameModes() : nullptr;
    const dao::CameraController *controller = modes != nullptr ? modes->Camera() : nullptr;
    const dao::SceneCamera *camera = controller != nullptr ? controller->Scene() : nullptr;
    const float lit = sun && strength > 0 ? Daylight(*sun) : 0.0F;
    // The lens first, and whatever the area: the ambient occlusion reads it
    // under a roof as under the sun (occlusion.hlsl).
    const std::optional<float> lens =
        size && camera != nullptr && size->width > 0 && size->height > 0 ? KeepLens(*size, *camera) : std::nullopt;
    if (!sun || lit <= 0 || !sky || !size || camera == nullptr || size->width <= 0 || size->height <= 0)
    {
        KeepQuietly(L"DaoSunRed", 0);
        KeepQuietly(L"DaoSunGreen", 0);
        KeepQuietly(L"DaoSunBlue", 0);
        return;
    }
    if (!lens)
    {
        return; // nothing to tell the lens by this frame: the values stay
    }
    const dao::Vector eye = camera->Position();
    const std::array<float, 2> middle{static_cast<float>(size->width) / 2, static_cast<float>(size->height) / 2};
    const float focal = *lens;
    const std::array<float, 3> toSun{-sun->direction[0], -sun->direction[1], -sun->direction[2]};
    std::optional<SeenWay> where = SeeWay(eye, middle, toSun);
    float side = 1;
    if (!where)
    {
        where = SeeWay(eye, middle, {-toSun[0], -toSun[1], -toSun[2]});
        side = -1;
    }
    if (!where)
    {
        return;
    }
    KeepQuietly(L"DaoSunX", std::clamp(where->at[0] / focal, -64.0F, 64.0F));
    KeepQuietly(L"DaoSunY", std::clamp(where->at[1] / focal, -64.0F, 64.0F));
    KeepQuietly(L"DaoSunSide", side);
    KeepQuietly(L"DaoHazeThick", std::clamp(sky->fogIntensity, 0.0F, 1.0F));
    KeepQuietly(L"DaoSunRed", lit * g_sunHeld * sun->color[0]);
    KeepQuietly(L"DaoSunGreen", lit * g_sunHeld * sun->color[1]);
    KeepQuietly(L"DaoSunBlue", lit * g_sunHeld * sun->color[2]);
}

// Whether a light stands where a level's bake says one of the level's own
// lamps is.
// @param places the bake's lamps, three floats each (probebaker::BakedLamps)
bool IsBakedLamp(void *node, const std::vector<float> &places)
{
    // Where a light is, is asked of the light within the node (PlaceOfLight):
    // the node's own word for its place, which a figure's lamps are scored
    // by, is nothing for a light as it stands in the level's lists.
    constexpr float kSamePlace = 0.3F; // metres
    const std::array<float, 4> at = PlaceOfLight(node);
    for (std::size_t i = 0; i + 2 < places.size(); i += 3)
    {
        if (std::hypot(at[0] - places[i], at[1] - places[i + 1], at[2] - places[i + 2]) < kSamePlace)
        {
            return true;
        }
    }
    return false;
}

// Finds the bake's lamps among the level's lights not yet known to be, and
// has the managers whose lights they are choose again. g_ourLampsLock is the
// caller's to hold.
void FindBakedLamps(const std::vector<float> &places)
{
    std::set<void *> changed; // the managers whose lights are to be chosen again
    for (const auto &[node, manager] : g_levelLights)
    {
        if (!g_bakedLamps.contains(node) && IsBakedLamp(node, places))
        {
            g_bakedLamps.insert(node);
            changed.insert(manager);
        }
    }
    g_bakedLampsCount.store(g_bakedLamps.size(), std::memory_order_relaxed);
    for (void *manager : changed)
    {
        // As the engine does when it changes a manager's lists itself.
        reinterpret_cast<ManagerChangedFn>(dao::Module::Address(dao::addr::kLightManager_MarkChanged))(manager, 0);
    }
}

// Says how many of the level's lights are its bake's lamps, each time that
// is another number. g_ourLampsLock is the caller's to hold.
void SayBakedLamps(const std::vector<float> &places)
{
    static std::size_t said = ~std::size_t{0};
    static std::size_t saidOf = 0;
    if (g_bakedLamps.size() == said && g_levelLights.size() == saidOf)
    {
        return;
    }
    said = g_bakedLamps.size();
    saidOf = g_levelLights.size();
    // And how near the nearest light that is not one stands to a lamp
    // of the bake's, which says whether the places are read alike.
    float nearest = std::numeric_limits<float>::infinity();
    for (const auto &[node, manager] : g_levelLights)
    {
        if (g_bakedLamps.contains(node))
        {
            continue;
        }
        const std::array<float, 4> at = PlaceOfLight(node);
        for (std::size_t i = 0; i + 2 < places.size(); i += 3)
        {
            nearest = std::min(nearest, std::hypot(at[0] - places[i], at[1] - places[i + 1], at[2] - places[i + 2]));
        }
    }
    int shown = 0;
    for (const auto &[node, manager] : g_levelLights)
    {
        if (shown++ >= 6)
        {
            break;
        }
        const std::array<float, 4> at = PlaceOfLight(node);
        float reach = 0;
        std::memcpy(&reach, static_cast<const std::byte *>(node) + kLightNodeReach, sizeof reach);
        spdlog::debug("lamps: a light at ({:.1f}, {:.1f}, {:.1f}) reaching {:.1f} m", at[0], at[1], at[2], reach);
    }
    spdlog::info("lamps: {} of the level's {} lights are its own lamps, in its bake ({} said), and are put out; "
                 "of the rest the nearest to one is {:.2f} m off",
                 g_bakedLamps.size(), g_levelLights.size(), places.size() / 3, nearest);
}

// Puts out the lamps of a level lit again: the lights in the level's lists that
// stand where the bake says the level's own lamps are (<level>.lamps), by
// their strength, which is what the engine multiplies a light's colour by for
// whatever it lights. The bake is what says how such a level's fittings
// light it; a lamp its makers set to tint a fitting, lighting the wall
// again with no shadow, is the same light twice. A light that stands
// nowhere the bake knows, a spell's or an effect's, is left as it is.
void DimBakedLamps()
{
    constexpr int kLookAgain = 30; // frames between looks at the lights not yet known
    static int frames = 0;
    if (!g_levelRelit.load(std::memory_order_relaxed))
    {
        return;
    }
    const std::shared_ptr<const std::vector<float>> places = probebaker::BakedLamps();
    const bool look = (frames++ % kLookAgain) == 0 && !places->empty();
    const std::scoped_lock lock(g_ourLampsLock);
    if (look)
    {
        FindBakedLamps(*places);
        SayBakedLamps(*places);
    }
}

// The current area's scene for the lights' chooser, which may be on another thread.
void KeepAreasScene()
{
    const dao::CCArea *area = dao::world::CurrentArea();
    void *scene = area != nullptr ? area->Scene() : nullptr;
    if (scene != g_areasScene.load(std::memory_order_relaxed))
    {
        g_levelSeen.store(0, std::memory_order_relaxed); // another area: its level's middle is not known yet
        const std::uint32_t id = area != nullptr ? area->Id() : 0;
        g_areaNow.store(id, std::memory_order_relaxed);
        bool known = false;
        {
            const std::scoped_lock lock(g_areasLock);
            const auto found = g_areasRelit.find(id);
            known = found != g_areasRelit.end() && found->second;
        }
        g_levelRelit.store(known, std::memory_order_relaxed);
        ForgetRelitRooms();
    }
    g_areasScene.store(scene, std::memory_order_relaxed);
}

// Keeps the chain's values at the chosen look's, and its anti-aliasing on or
// off. The file is read again when
// it is written, so a look is tuned with the game running; the chain is asked
// each time, since a new game or a reload of the chain would put its values
// back to their own. The slider keeps the stops it was given at the start, so
// a look added or taken out shows there from the next session.
void ApplyLook()
{
    static std::filesystem::file_time_type read;
    static int frames = 0;
    if (++frames % kLookEvery != 0)
    {
        return;
    }
    const std::filesystem::path file = dao::Module::OwnFolder() / kLooksFile;
    std::error_code error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(file, error);
    if (!error && written != read)
    {
        const bool first = read == std::filesystem::file_time_type{};
        read = written;
        if (std::vector<Look> looks = ReadLooks(file); !first && looks.size() == g_looks.size())
        {
            g_looks = std::move(looks);
            spdlog::info("{}: read again", kLooksFile);
        }
    }
    if (g_chosen >= g_looks.size())
    {
        return;
    }
    for (const LookValue &value : g_looks[g_chosen].values)
    {
        // The highlights' roll-off is for a scene that holds more than white:
        // in the back buffer's eight bits it would only dim what is nearly
        // white.
        const bool idle = value.key == "Highlights" && g_wideFormat == D3DFMT_UNKNOWN;
        // The ambient occlusion's strength is the options screen's, but in
        // the Vanilla look: a value a relit look's section may still have for
        // it is not read.
        const bool sliders = value.key == "Occlusion" && !VanillaChosen();
        if (value.key != "Exposure" && !sliders) // Exposure is KeepSun's, by the area's sun
        {
            const float tuned = value.key == "Contrast" ? TunedBy(kTuneContrast) : 0.0F;
            Keep(value.name, idle ? 0.0F : value.value + tuned, value.key);
        }
    }
    // One place says how dark the occlusion is, the options screen's slider
    // (graphics.chosen), and the relit look's section has no line for it.
    // It had one that nothing read, and a test that set it compared a frame
    // with itself four times (2026-10-12; the user: "shouldn't the slider
    // write value to the ini, and vice versa? how do the two stay
    // consistent?").
    if (!VanillaChosen())
    {
        Keep(L"DaoOcclusion",
             kOcclusionStops.at(static_cast<std::size_t>(g_occlusionStop.load(std::memory_order_relaxed))),
             "Occlusion");
    }
    Keep(L"DaoSaturation", TunedBy(kTuneSaturation), "Saturation");
    Keep(L"DaoWarmth", TunedBy(kTuneWarmth), "Warmth");
    Keep(kSmaa, g_smaa ? 1.0F : 0.0F, "Smaa");
    KeepSun(ChosenValue("Sun"), ChosenValue("SunOnFigures"), ChosenValue("Exposure"));
    KeepSky(ChosenValue("Haze"), ChosenValue("Clouds"), ChosenValue("Clear"), ChosenValue("Sky"));
    KeepLookRegisters();
}

// The options screen's section, on the Video tab's right column, which holds
// a heading and twelve rows of 35 px (DAOLib #82), a slider 41 px (#84): two
// checkboxes and six sliders, with room for two sliders more. Vanilla or relit is a checkbox and no slider of looks (the user,
// 2026-10-11: "just vanilla & relit. instead of slider we can use
// checkbox"); the sliders are the player's own tuning of the level they are
// in (TuneNow).
constexpr const wchar_t *kOptionsTitle = L"Graphics";

// One of the tuning's sliders: nine stops, the middle one nothing.
dao::options::Choice TuneSlider(TuneOf which, const wchar_t *name, const wchar_t *label, const wchar_t *tooltip)
{
    dao::options::Choice slider;
    slider.name = name;
    slider.label = label;
    slider.tooltip = tooltip;
    for (int step = -kTuneSteps; step <= kTuneSteps; ++step)
    {
        slider.values.push_back(step > 0 ? L"+" + std::to_wstring(step) : std::to_wstring(step));
    }
    slider.get = [which] { return TuneNow().at(which) + kTuneSteps; };
    slider.set = [which](std::int32_t wanted) {
        {
            const std::scoped_lock lock(g_tuneLock);
            Tune tune{};
            if (const auto found = g_tunedByPlayer.find(g_levelName); found != g_tunedByPlayer.end())
            {
                tune = found->second;
            }
            else if (const auto own = g_tunedShipped.find(g_levelName); own != g_tunedShipped.end())
            {
                tune = own->second;
            }
            tune.at(which) = std::clamp(wanted - kTuneSteps, -kTuneSteps, kTuneSteps);
            g_tunedByPlayer.insert_or_assign(g_levelName, tune);
            spdlog::info("tuned in {}: exposure {}, contrast {}, saturation {}, warmth {}",
                         g_levelName.empty() ? "no level" : g_levelName, tune[0], tune[1], tune[2], tune[3]);
        }
        KeepChosen();
        return wanted;
    };
    return slider;
}

// A slider over a list of stops kept in graphics.chosen.
template <std::size_t N>
dao::options::Choice StopSlider(std::atomic<int> &stop, const std::array<const wchar_t *, N> &names, const wchar_t *name,
                                const wchar_t *label, const wchar_t *tooltip, const char *said)
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
        KeepChosen();
        return wanted;
    };
    return slider;
}

void AddToOptions()
{
    const std::optional<std::size_t> vanilla = LookNamed(kVanillaLook);
    const std::optional<std::size_t> relit = LookNamed(kFirstLook);
    if (!vanilla || !relit)
    {
        spdlog::warn("{} has not both the looks {} and {}; the options screen has no section", kLooksFile, kVanillaLook,
                     kFirstLook);
        return;
    }
    dao::options::Toggle lighting;
    lighting.name = L"GraphicsRelit";
    lighting.label = L"Relit lighting";
    lighting.tooltip = L"The plugin's lighting; off is the game as it is";
    lighting.get = [vanilla] { return g_chosen != *vanilla; };
    lighting.set = [vanilla, relit](bool wanted) {
        ChooseLook(static_cast<std::int32_t>(wanted ? *relit : *vanilla));
        return wanted;
    };
    dao::options::Toggle smaa;
    smaa.name = L"GraphicsAntiAliasing";
    smaa.label = L"Extra anti-aliasing (SMAA)";
    smaa.tooltip = L"Smooths the edges the game's own anti-aliasing misses; works with it or without";
    smaa.get = [] { return g_smaa; };
    smaa.set = [](bool wanted) { return ChooseAntiAliasing(wanted ? 1 : 0) == 1; };
    if (!dao::options::AddSection(
            kOptionsTitle, {lighting, smaa},
            {TuneSlider(kTuneExposure, L"GraphicsExposure", L"Exposure", L"How bright the picture is, in this area's level"),
             TuneSlider(kTuneContrast, L"GraphicsContrast", L"Contrast", L"How far apart light and dark are"),
             TuneSlider(kTuneSaturation, L"GraphicsSaturation", L"Saturation", L"How strong the colours are"),
             TuneSlider(kTuneWarmth, L"GraphicsWarmth", L"Warmth", L"Cooler or warmer"),
             StopSlider(g_occlusionStop, kOcclusionNames, L"GraphicsOcclusion", L"Ambient occlusion",
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
        spdlog::info("the options screen's Video tab shows the Graphics section");
        break;
    case dao::options::SectionState::NoRoom:
        spdlog::warn("the options screen has no room for the Look slider: other sections took the Video tab's rows");
        break;
    case dao::options::SectionState::Failed:
        spdlog::warn("the options screen was not as expected, and has no Look slider");
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
    if (!g_lightListInsert.Attach(dao::addr::kLightList_Insert, &LightListInsert) ||
        !g_lightListErase.Attach(dao::addr::kLightList_Erase, &LightListErase) ||
        !g_managerAdd.Attach(dao::addr::kLightManager_AddLights, &ManagerAdd) ||
        !g_managerRemove.Attach(dao::addr::kLightManager_RemoveLights, &ManagerRemove) ||
        !g_chooseForMoving.Attach(dao::addr::kLightManager_ChooseForMoving, &ChooseForMoving) ||
        !g_chooseFromList.Attach(dao::addr::kLightList_ChooseFor, &ChooseFromList) ||
        !g_lightScore.Attach(dao::addr::kObjectNodePointLight_Score, &LightScore))
    {
        spdlog::warn("the figures have no lamps: a detour is missing");
    }
    if (!g_probeScore.Attach(dao::addr::kObjectNodeLightProbe_Score, &ProbeScore) ||
        !g_stateFill.Attach(dao::addr::kLightState_Fill, &StateFill))
    {
        spdlog::warn("the light probes are not blended: a detour is missing");
    }
    if (!g_setPartitions.Attach(dao::addr::kShadowMaps_SetPartitions, &SetPartitions))
    {
        spdlog::warn("the shadow maps keep the game's size: no detour");
    }
    if (!g_openArchive.Attach(dao::addr::kResMan_MakeSource, &OpenArchive))
    {
        spdlog::warn("no light probes are baked: no detour");
    }
    probebaker::Start(dao::LogDirectory() / "elgarashaders" / "probes", dao::Module::Path().parent_path().parent_path(),
                      dao::Module::OwnFolder() / "override");
    WatchForDevice();
    g_looks = ReadLooks(dao::Module::OwnFolder() / kLooksFile);
    ReadChosen(dao::Module::OwnFolder());
    spdlog::info("{}: {} looks", kLooksFile, g_looks.size());
    AddToOptions();
    dao::OnFrame(&LogLights);
    dao::OnFrame(&LogSunlight);
    dao::OnFrame(&LogSky);
    dao::OnFrame(&LogOptionsSection);
    dao::OnFrame(&KeepAir);
    dao::OnFrame(&ApplyLook);
    dao::OnFrame(&KeepAreasScene);
    dao::OnFrame(&DimBakedLamps);
    dao::OnFrame(&resourcedump::Poll);
    return true;
}
