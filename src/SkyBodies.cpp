#include "SkyBodies.h"

#include "DayNight.h"
#include "ShaderCode.h"

#include "dao/Resources.h"
#include "dao/core/Dds.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace skybodies
{

namespace
{

namespace dds = dao::core::dds;

// The two pictures, the sun's and the moon's: the files sb_day_base.mao and
// sky.mat.xml name (SkyBodies.h). The vanilla pair are 256 a side, DXT5.
constexpr std::array<const wchar_t *, 2> kFiles{L"defaultsun.dds", L"defaultmoon.dds"};
// The most a side is read at: a mod's picture is kept to it.
constexpr std::uint32_t kSideMost = 1024;

struct Body
{
    std::optional<dds::Image> image; // as read, until it is made on the device
    bool asked = false;
    bool failed = false;
    IDirect3DTexture9 *texture = nullptr;
};

std::mutex g_lock; // over g_bodies' images and flags, which the game's thread writes
std::array<Body, 2> g_bodies;
bool g_polled = false; // the game's thread's

// The drawing thread's: the sky's shaders by the sampler each reads its disc
// from, the device the textures were made on, and the picture a draw took
// away to give back.
std::unordered_map<IDirect3DPixelShader9 *, UINT> g_skyShaders;
IDirect3DDevice9 *g_madeOn = nullptr;
struct Taken
{
    UINT sampler = 0;
    IDirect3DBaseTexture9 *had = nullptr;
};
std::optional<Taken> g_taken;

// The next mip of a picture: each pixel the mean of four.
dds::Image Halved(const dds::Image &image)
{
    dds::Image half;
    half.width = std::max(image.width / 2, 1U);
    half.height = std::max(image.height / 2, 1U);
    half.rgba.resize(static_cast<std::size_t>(half.width) * half.height * 4);
    for (std::uint32_t y = 0; y < half.height; ++y)
    {
        for (std::uint32_t x = 0; x < half.width; ++x)
        {
            for (std::size_t c = 0; c < 4; ++c)
            {
                unsigned sum = 0;
                for (std::uint32_t dy = 0; dy < 2; ++dy)
                {
                    for (std::uint32_t dx = 0; dx < 2; ++dx)
                    {
                        const std::uint32_t sx = std::min((x * 2) + dx, image.width - 1);
                        const std::uint32_t sy = std::min((y * 2) + dy, image.height - 1);
                        sum += image.rgba.at((((static_cast<std::size_t>(sy) * image.width) + sx) * 4) + c);
                    }
                }
                half.rgba.at((((static_cast<std::size_t>(y) * half.width) + x) * 4) + c) =
                    static_cast<std::uint8_t>((sum + 2) / 4);
            }
        }
    }
    return half;
}

// A managed texture of a picture with its mips, A8R8G8B8.
IDirect3DTexture9 *Made(IDirect3DDevice9 *device, const dds::Image &image)
{
    IDirect3DTexture9 *texture = nullptr;
    if (FAILED(device->CreateTexture(image.width, image.height, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture,
                                     nullptr)) ||
        texture == nullptr)
    {
        return nullptr;
    }
    dds::Image level = image;
    for (DWORD at = 0; at < texture->GetLevelCount(); ++at)
    {
        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(at, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }
        for (std::uint32_t y = 0; y < level.height; ++y)
        {
            auto *row = static_cast<std::uint8_t *>(locked.pBits) + (static_cast<std::size_t>(y) * locked.Pitch);
            for (std::uint32_t x = 0; x < level.width; ++x)
            {
                const std::uint8_t *from = &level.rgba.at(((static_cast<std::size_t>(y) * level.width) + x) * 4);
                std::uint8_t *to = row + (static_cast<std::size_t>(x) * 4); // B, G, R, A in memory
                to[0] = from[2];
                to[1] = from[1];
                to[2] = from[0];
                to[3] = from[3];
            }
        }
        texture->UnlockRect(at);
        level = Halved(level);
    }
    return texture;
}

// The body's texture on this device, made from its picture the first time;
// none until the picture is read.
IDirect3DTexture9 *TextureOf(IDirect3DDevice9 *device, std::size_t which)
{
    const std::scoped_lock lock(g_lock);
    if (g_madeOn != device)
    {
        for (Body &body : g_bodies)
        {
            if (body.texture != nullptr)
            {
                body.texture->Release();
                body.texture = nullptr;
            }
        }
        g_madeOn = device;
    }
    Body &body = g_bodies.at(which);
    if (body.texture == nullptr && body.image)
    {
        body.texture = Made(device, *body.image);
        if (body.texture == nullptr)
        {
            spdlog::warn("sky's bodies: {} could not be made on the device",
                         std::filesystem::path(kFiles.at(which)).string());
            body.failed = true;
        }
        body.image.reset();
    }
    return body.texture;
}

} // namespace

void Poll()
{
    if (g_polled)
    {
        return;
    }
    g_polled = true;
    for (std::size_t which = 0; which < kFiles.size(); ++which)
    {
        dao::resources::ReadAsync(kFiles.at(which), [which](std::optional<std::vector<std::byte>> bytes) {
            std::optional<dds::Image> image = bytes ? dds::Decode(*bytes, kSideMost) : std::nullopt;
            const std::string name = std::filesystem::path(kFiles.at(which)).string();
            if (image)
            {
                spdlog::info("sky's bodies: {}, {} by {}", name, image->width, image->height);
            }
            else
            {
                spdlog::warn("sky's bodies: {} could not be read; the sky keeps its own", name);
            }
            const std::scoped_lock lock(g_lock);
            g_bodies.at(which).image = std::move(image);
            g_bodies.at(which).failed = !g_bodies.at(which).image;
        });
    }
}

void CreatedPixel(const std::vector<std::byte> &code, IDirect3DPixelShader9 *shader)
{
    // The last shader made at an address is the one there.
    if (const std::optional<UINT> sampler = shadercode::RegisterOf(code, "smpCelestialBody"))
    {
        g_skyShaders[shader] = *sampler;
    }
    else
    {
        g_skyShaders.erase(shader);
    }
}

void Drawing(IDirect3DDevice9 *device)
{
    if (g_skyShaders.empty() || !daynight::Running())
    {
        return;
    }
    IDirect3DPixelShader9 *pixel = nullptr;
    device->GetPixelShader(&pixel);
    const auto found = g_skyShaders.find(pixel);
    if (pixel != nullptr)
    {
        pixel->Release();
    }
    if (found == g_skyShaders.end())
    {
        return;
    }
    IDirect3DTexture9 *ours = TextureOf(device, daynight::DiscIsMoon() ? 1 : 0);
    if (ours == nullptr)
    {
        return;
    }
    Taken taken{found->second, nullptr};
    device->GetTexture(taken.sampler, &taken.had);
    device->SetTexture(taken.sampler, ours);
    g_taken = taken;
}

void Drawn(IDirect3DDevice9 *device)
{
    if (!g_taken)
    {
        return;
    }
    device->SetTexture(g_taken->sampler, g_taken->had);
    if (g_taken->had != nullptr)
    {
        g_taken->had->Release();
    }
    g_taken.reset();
}

} // namespace skybodies
