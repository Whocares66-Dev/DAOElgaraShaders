#include "NightLightmaps.h"

#include "DayNight.h"
#include "GameFolder.h"
#include "ShaderCode.h"

#include "dao/Module.h"
#include "dao/Resources.h"
#include "dao/core/Dds.h"

#include <d3d9.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace nightlightmaps
{

namespace
{

namespace dds = dao::core::dds;

// relit_night.hlsl's daoRelitNight, under water's light (c159): the game's
// pixel shaders reach c55 at the most.
constexpr UINT kRelitNightRegister = 157;
// A level's lightmap is <level>_room<n>.dds (lak503d_room500.dds), each an
// atlas of a room's surfaces, its texels where the room's lightmapped
// triangles' second coordinates put them; the relighter's of a room is laid
// out alike, at its own size.
constexpr std::wstring_view kRoom = L"_room";

std::wstring Lower(std::wstring_view text)
{
    std::wstring lower(text);
    for (wchar_t &c : lower)
    {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return lower;
}

std::filesystem::path NightFolder()
{
    return dao::Module::OwnFolder() / "night";
}

// The night lightmaps the plugin has, by name, read once as it loads.
std::set<std::wstring> g_nights;
std::mutex g_lock;
// The engine's lightmaps that have a night, by the texture it binds (under
// g_lock: made and freed on the game's reading threads).
std::map<IDirect3DBaseTexture9 *, std::wstring> g_lightmaps;
// The levels whose nights are not of the copy opened (LevelOpened), under g_lock.
std::set<std::wstring> g_off;

// The level a night lightmap is of: its name before _room.
std::wstring LevelOf(const std::wstring &name)
{
    const std::size_t room = name.find(kRoom);
    return room == std::wstring::npos ? std::wstring{} : name.substr(0, room);
}
// The night lightmaps made, by name (the drawing thread's): none where the
// file could not be read.
std::map<std::wstring, IDirect3DTexture9 *> g_made;
std::set<std::wstring> g_failed;
// The level shaders that read a lightmap and have a sampler for its night:
// the lightmap's sampler and the night's (the drawing thread's).
struct Samplers
{
    UINT lightmap = 0;
    UINT night = 0;
};
std::map<IDirect3DPixelShader9 *, Samplers> g_shaders;

// The sampler a draw was lent the night lightmap on, and what it held, given
// back after the draw (Drawn), as LevelShadows.cpp lends its atlas. The
// engine keeps what it last bound to each sampler and does not bind it again:
// left on a sampler a weapon's shader reads its own texture on, the night
// lightmap was that texture, the party's sheathed weapons a glowing gold
// patchwork as the camera turned (the user, 2026-10-30: "there's weird glowing
// on the weapons at certain angles").
constexpr std::array<D3DSAMPLERSTATETYPE, 6> kLentStates{D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
                                                         D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_SRGBTEXTURE};
struct Lent
{
    bool lent = false;
    UINT sampler = 0;
    IDirect3DBaseTexture9 *texture = nullptr; // held, released as it is given back
    std::array<DWORD, kLentStates.size()> states{};
};
Lent g_lent; // the drawing thread's

// A packed night lightmap (DXT1 or DXT5, DdsPackTool.cpp) as a texture of
// its own format, its largest picture copied as the file has it: unpacked, a
// map of the makers' 1024 a side was 4 MB of video memory.
IDirect3DTexture9 *MakePacked(IDirect3DDevice9 *device, std::span<const std::byte> file, const dds::Description &about)
{
    constexpr std::size_t kHeader = 128; // "DDS " and the header
    const bool dxt1 = about.format == "DXT1";
    const std::size_t block = dxt1 ? 8 : 16;
    const std::size_t across = std::max<std::size_t>(1, (about.width + 3) / 4);
    const std::size_t down = std::max<std::size_t>(1, (about.height + 3) / 4);
    if (file.size() < kHeader + (across * down * block))
    {
        return nullptr;
    }
    IDirect3DTexture9 *texture = nullptr;
    if (FAILED(device->CreateTexture(about.width, about.height, 1, 0, dxt1 ? D3DFMT_DXT1 : D3DFMT_DXT5, D3DPOOL_MANAGED,
                                     &texture, nullptr)))
    {
        return nullptr;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
    {
        texture->Release();
        return nullptr;
    }
    for (std::size_t row = 0; row < down; ++row)
    {
        std::memcpy(static_cast<std::byte *>(locked.pBits) + (row * static_cast<std::size_t>(locked.Pitch)),
                    file.data() + kHeader + (row * across * block), across * block);
    }
    texture->UnlockRect(0);
    return texture;
}

// A night lightmap as a texture of 8 bits a channel, its picture as the file
// has it (the relighter writes the root of the light's share of eight
// whites, relit_night.hlsl); a packed one as it is (MakePacked).
IDirect3DTexture9 *Make(IDirect3DDevice9 *device, const std::wstring &name)
{
    std::ifstream in(NightFolder() / name, std::ios::binary);
    std::vector<char> raw{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte *>(raw.data()), raw.size());
    if (const std::optional<dds::Description> about = raw.empty() ? std::nullopt : dds::Describe(bytes);
        about && (about->format == "DXT1" || about->format == "DXT5"))
    {
        return MakePacked(device, bytes, *about);
    }
    const std::optional<dds::Image> image =
        raw.empty()
            ? std::nullopt
            : dds::Decode(std::span<const std::byte>(reinterpret_cast<const std::byte *>(raw.data()), raw.size()));
    if (!image || image->width == 0 || image->height == 0)
    {
        spdlog::warn("night lightmaps: {} could not be read", std::filesystem::path(name).string());
        return nullptr;
    }
    IDirect3DTexture9 *texture = nullptr;
    if (FAILED(device->CreateTexture(image->width, image->height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture,
                                     nullptr)))
    {
        return nullptr;
    }
    D3DLOCKED_RECT locked{};
    if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
    {
        texture->Release();
        return nullptr;
    }
    for (std::uint32_t y = 0; y < image->height; ++y)
    {
        auto *row = static_cast<std::uint8_t *>(locked.pBits) + (static_cast<std::size_t>(y) * locked.Pitch);
        for (std::uint32_t x = 0; x < image->width; ++x)
        {
            const std::uint8_t *from = &image->rgba[((static_cast<std::size_t>(y) * image->width) + x) * 4];
            // A8R8G8B8 is blue, green, red, alpha in memory.
            row[(x * 4) + 0] = from[2];
            row[(x * 4) + 1] = from[1];
            row[(x * 4) + 2] = from[0];
            row[(x * 4) + 3] = from[3];
        }
    }
    texture->UnlockRect(0);
    return texture;
}

IDirect3DTexture9 *NightOf(IDirect3DDevice9 *device, IDirect3DBaseTexture9 *lightmap)
{
    std::wstring name;
    {
        const std::scoped_lock lock(g_lock);
        const auto found = g_lightmaps.find(lightmap);
        if (found == g_lightmaps.end())
        {
            return nullptr;
        }
        name = found->second;
    }
    if (const auto made = g_made.find(name); made != g_made.end())
    {
        return made->second;
    }
    if (g_failed.contains(name))
    {
        return nullptr;
    }
    IDirect3DTexture9 *texture = Make(device, name);
    if (texture == nullptr)
    {
        g_failed.insert(name);
        return nullptr;
    }
    spdlog::info("night lightmaps: {} made", std::filesystem::path(name).string());
    g_made[name] = texture;
    return texture;
}

} // namespace

void Install()
{
    std::error_code error;
    for (const std::filesystem::directory_entry &file : std::filesystem::directory_iterator(NightFolder(), error))
    {
        const std::wstring name = Lower(file.path().filename().wstring());
        if (name.find(kRoom) != std::wstring::npos && file.path().extension() == L".dds")
        {
            g_nights.insert(name);
        }
    }
    spdlog::info("night lightmaps: {} of the plugin's", g_nights.size());
    if (g_nights.empty())
    {
        return;
    }
    dao::resources::OnTexture(
        [](const dao::resources::Texture &texture) {
            const std::wstring name = Lower(texture.name);
            if (texture.texture == nullptr || !g_nights.contains(name))
            {
                return;
            }
            const std::scoped_lock lock(g_lock);
            if (!g_off.contains(LevelOf(name)))
            {
                g_lightmaps[texture.texture] = name;
            }
        },
        [](const dao::resources::Texture &texture) {
            const std::scoped_lock lock(g_lock);
            g_lightmaps.erase(texture.texture);
        });
}

void LevelOpened(const std::filesystem::path &levelRim)
{
    const std::wstring level = Lower(levelRim.stem().wstring());
    if (std::ranges::none_of(g_nights, [&](const std::wstring &name) { return LevelOf(name) == level; }))
    {
        return;
    }
    // The game's own copy is in its levels' folder; a mod's (Dragon Age
    // Evolved's lak200d, #66) or an add-in's is elsewhere, its rooms perhaps
    // other than the ones the night lightmaps were baked for.
    std::error_code error;
    const bool games = std::filesystem::equivalent(levelRim.parent_path().parent_path(),
                                                   GameFolder() / "packages" / "core" / "env", error);
    const std::scoped_lock lock(g_lock);
    if (games)
    {
        g_off.erase(level);
        return;
    }
    if (g_off.insert(level).second)
    {
        spdlog::info("night lightmaps: {} opened from {}, not the game's own; its own night kept",
                     std::filesystem::path(level).string(), levelRim.parent_path().string());
    }
}

bool LevelOn(std::wstring_view level)
{
    const std::scoped_lock lock(g_lock);
    return !g_off.contains(Lower(level));
}

bool Has(std::wstring_view level)
{
    const std::wstring lower = Lower(level);
    return std::ranges::any_of(g_nights, [&](const std::wstring &name) { return LevelOf(name) == lower; }) &&
           LevelOn(lower);
}

void CreatedPixel(const std::vector<std::byte> &code, IDirect3DPixelShader9 *shader)
{
    // The last shader made at an address is the one there.
    const std::optional<UINT> lightmap = shadercode::RegisterOf(code, "smpLightmap");
    const std::optional<UINT> night = shadercode::RegisterOf(code, "smpDaoNightLightmap");
    if (lightmap && night)
    {
        g_shaders[shader] = {*lightmap, *night};
    }
    else
    {
        g_shaders.erase(shader);
    }
}

void Drawing(IDirect3DDevice9 *device, bool picture)
{
    if (g_shaders.empty())
    {
        return;
    }
    IDirect3DPixelShader9 *pixel = nullptr;
    device->GetPixelShader(&pixel);
    const auto found = g_shaders.find(pixel);
    if (pixel != nullptr)
    {
        pixel->Release();
    }
    if (found == g_shaders.end())
    {
        return;
    }
    // Nothing of the night unless one is bound for this draw: the sampler
    // holds whatever was last put there.
    std::array<float, 4> relit{0, 1, 0, 0};
    const std::array<float, 4> hour = daynight::RelitNight();
    if (picture && hour[0] > 0)
    {
        IDirect3DBaseTexture9 *lightmap = nullptr;
        device->GetTexture(found->second.lightmap, &lightmap);
        IDirect3DTexture9 *night = lightmap != nullptr ? NightOf(device, lightmap) : nullptr;
        if (lightmap != nullptr)
        {
            lightmap->Release();
        }
        if (night != nullptr)
        {
            const UINT sampler = found->second.night;
            g_lent.lent = true;
            g_lent.sampler = sampler;
            device->GetTexture(sampler, &g_lent.texture);
            for (std::size_t at = 0; at < kLentStates.size(); ++at)
            {
                device->GetSamplerState(sampler, kLentStates.at(at), &g_lent.states.at(at));
            }
            device->SetTexture(sampler, night);
            device->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            device->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            device->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            // Read as the makers' lightmap is, so that the relighter's numbers
            // mean what they meant when its levels took the makers' place.
            DWORD srgb = FALSE;
            device->GetSamplerState(found->second.lightmap, D3DSAMP_SRGBTEXTURE, &srgb);
            device->SetSamplerState(sampler, D3DSAMP_SRGBTEXTURE, srgb);
            relit = hour;
        }
    }
    device->SetPixelShaderConstantF(kRelitNightRegister, relit.data(), 1);
}

void Drawn(IDirect3DDevice9 *device)
{
    if (!g_lent.lent)
    {
        return;
    }
    g_lent.lent = false;
    device->SetTexture(g_lent.sampler, g_lent.texture);
    for (std::size_t at = 0; at < kLentStates.size(); ++at)
    {
        device->SetSamplerState(g_lent.sampler, kLentStates.at(at), g_lent.states.at(at));
    }
    if (g_lent.texture != nullptr)
    {
        g_lent.texture->Release();
        g_lent.texture = nullptr;
    }
}

} // namespace nightlightmaps
