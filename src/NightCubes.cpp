#include "NightCubes.h"

#include "DayNight.h"
#include "GameFolder.h"

#include "dao/Resources.h"
#include "dao/core/Dds.h"
#include "dao/core/Erf.h"

#include <d3d9.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace nightcubes
{

namespace
{

namespace dds = dao::core::dds;

// A level's probe cube is <level>_probe_<id>.dds in its .gpu.rim, and the
// night level has the same probe under its own name: Redcliffe's day and
// night each have one, lak100d_probe_8uz.dds and lak100n_probe_8uz.dds,
// DXT1, 256 a face and nine pictures (ghilana, 2026-10-22). The day's holds
// the low sun's orange glare, which the lake mirrored at night.
constexpr std::wstring_view kProbe = L"_probe_";
// How far the night's share moves before the cube is made again: fifty
// steps from day to night, each a blend and an encoding of six faces, off
// the drawing thread.
constexpr float kStep = 0.02F;
constexpr std::size_t kDdsHeader = 128; // "DDS " and the header
constexpr std::size_t kDxt1Block = 8;

struct Cubes
{
    std::array<dds::Image, 6> day;
    std::array<dds::Image, 6> night;
};

// What a blend made: the two cubes it read (none where the night has none),
// and each face as a DDS file of DXT1.
struct Made
{
    std::shared_ptr<const Cubes> cubes;
    std::array<std::vector<std::byte>, 6> faces;
};

struct Kept
{
    IDirect3DCubeTexture9 *texture = nullptr;
    std::wstring name;  // the day's, lak100d_probe_8uz.dds
    std::wstring level; // the day level's, lak100d
    std::shared_ptr<const Cubes> cubes;
    bool none = false; // the night has no such cube, or it could not be read
    float written = 0; // the night's share the texture holds
    float making = 0;  // the share the blend under way is for
    std::future<Made> made;
};

std::mutex g_lock;
std::vector<std::unique_ptr<Kept>> g_kept;

std::wstring Lower(std::wstring_view text)
{
    std::wstring lower(text);
    std::ranges::transform(lower, lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return lower;
}

// A level's probe cube of a name; with no name, its first.
std::optional<std::array<dds::Image, 6>> ReadCube(const std::wstring &level, const std::wstring &name)
{
    const std::filesystem::path rim = GameFolder() / "packages" / "core" / "env" / level / (level + L".gpu.rim");
    const std::optional<dao::core::erf::Archive> archive = dao::core::erf::Archive::Open(rim);
    if (!archive || !archive->Readable())
    {
        return std::nullopt;
    }
    const auto first = std::ranges::find_if(archive->Entries(), [&](const dao::core::erf::Entry &entry) {
        return entry.name.starts_with(level + std::wstring(kProbe)) && entry.name.ends_with(L".dds");
    });
    const dao::core::erf::Entry *entry = first != archive->Entries().end() ? &*first : nullptr;
    if (!name.empty())
    {
        entry = archive->Find(name);
    }
    const std::optional<std::vector<std::byte>> bytes = entry != nullptr ? archive->Read(*entry) : std::nullopt;
    const std::optional<dds::Cube> cube = bytes ? dds::DecodeCube(*bytes) : std::nullopt;
    if (!cube)
    {
        return std::nullopt;
    }
    std::array<dds::Image, 6> faces;
    for (std::size_t f = 0; f < faces.size(); ++f)
    {
        if (cube->faces.at(f).empty())
        {
            return std::nullopt;
        }
        faces.at(f) = cube->faces.at(f).front();
    }
    return faces;
}

// A borrowed night's cube: the day's own place, as the night lights it. The
// level's bake goes by night to the night sky's even light (night_sky.hlsl),
// so what the cube sees is that light on what it sees: each channel of the
// day's cube taken to the night sky's light over the cube's mean. Another
// level's night cube is another place: Redcliffe's village's in the castle's
// puddles, and in Denerim's or Lothering's (issue 37).
std::array<dds::Image, 6> AtNight(std::array<dds::Image, 6> faces, const std::array<float, 3> &nightSky)
{
    std::array<double, 3> sum{};
    std::size_t texels = 0;
    for (const dds::Image &face : faces)
    {
        for (std::size_t i = 0; i + 3 < face.rgba.size(); i += 4)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                sum.at(c) += face.rgba.at(i + c);
            }
        }
        texels += face.rgba.size() / 4;
    }
    std::array<float, 3> by{};
    for (std::size_t c = 0; c < 3; ++c)
    {
        const double mean = texels > 0 ? sum.at(c) / static_cast<double>(texels) / 255 : 0;
        by.at(c) = mean > 0 ? static_cast<float>(nightSky.at(c) / mean) : 0;
    }
    for (dds::Image &face : faces)
    {
        for (std::size_t i = 0; i + 3 < face.rgba.size(); i += 4)
        {
            for (std::size_t c = 0; c < 3; ++c)
            {
                face.rgba.at(i + c) = static_cast<std::uint8_t>(
                    std::clamp(std::lround(static_cast<float>(face.rgba.at(i + c)) * by.at(c)), 0L, 255L));
            }
        }
    }
    return faces;
}

// The two cubes, read where they have not been; then the night's `share` of
// the night's, each face encoded as the engine's is. `nightSky` is the night
// sky's light (daynight::NightSky), for a borrowed night's cube.
Made Make(std::shared_ptr<const Cubes> cubes, const std::wstring &level, const std::wstring &name, float share,
          std::array<float, 3> nightSky)
{
    Made made;
    if (!cubes)
    {
        // The night level's cube of the same probe where it is the day's own
        // night (lak100d's and lak100n's), else the day's own at night.
        const std::wstring night = daynight::NightLevel(level);
        const bool own = night == level.substr(0, level.size() - 1) + L"n";
        std::optional<std::array<dds::Image, 6>> day = ReadCube(level, name);
        std::optional<std::array<dds::Image, 6>> nightCube;
        if (own)
        {
            nightCube = ReadCube(night, night + name.substr(level.size()));
        }
        else if (!night.empty() && day)
        {
            nightCube = AtNight(*day, nightSky);
        }
        if (!day || !nightCube)
        {
            return made;
        }
        cubes = std::make_shared<const Cubes>(Cubes{std::move(*day), std::move(*nightCube)});
    }
    made.cubes = cubes;
    for (std::size_t f = 0; f < made.faces.size(); ++f)
    {
        const dds::Image &day = cubes->day.at(f);
        const dds::Image &night = cubes->night.at(f);
        if (day.width != night.width || day.height != night.height)
        {
            made.cubes = nullptr;
            return made;
        }
        dds::Image blend{day.width, day.height, day.rgba};
        for (std::size_t i = 0; i < blend.rgba.size(); ++i)
        {
            blend.rgba.at(i) = static_cast<std::uint8_t>(std::lround(
                std::lerp(static_cast<float>(day.rgba.at(i)), static_cast<float>(night.rgba.at(i)), share)));
        }
        made.faces.at(f) = dds::Encode(blend, dds::Format::Dxt1);
    }
    return made;
}

// Each face's pictures copied into the texture's own, block row by block row.
bool Upload(IDirect3DCubeTexture9 *texture, const Made &made)
{
    const DWORD levels = texture->GetLevelCount();
    for (std::size_t f = 0; f < made.faces.size(); ++f)
    {
        const std::vector<std::byte> &file = made.faces.at(f);
        std::size_t at = kDdsHeader;
        for (DWORD level = 0; level < levels; ++level)
        {
            D3DSURFACE_DESC desc{D3DFMT_UNKNOWN, D3DRTYPE_SURFACE, 0, D3DPOOL_DEFAULT, D3DMULTISAMPLE_NONE, 0, 0, 0};
            if (FAILED(texture->GetLevelDesc(level, &desc)) || desc.Format != D3DFMT_DXT1)
            {
                return false;
            }
            const std::size_t across = std::max<std::size_t>(1, (desc.Width + 3) / 4);
            const std::size_t down = std::max<std::size_t>(1, (desc.Height + 3) / 4);
            const std::size_t row = across * kDxt1Block;
            if (at + (row * down) > file.size())
            {
                return false;
            }
            D3DLOCKED_RECT locked{};
            if (FAILED(texture->LockRect(static_cast<D3DCUBEMAP_FACES>(f), level, &locked, nullptr, 0)))
            {
                return false;
            }
            for (std::size_t y = 0; y < down; ++y)
            {
                std::memcpy(static_cast<std::byte *>(locked.pBits) + (y * static_cast<std::size_t>(locked.Pitch)),
                            file.data() + at + (y * row), row);
            }
            texture->UnlockRect(static_cast<D3DCUBEMAP_FACES>(f), level);
            at += row * down;
        }
    }
    return true;
}

} // namespace

void Install()
{
    dao::resources::OnTexture(
        [](const dao::resources::Texture &texture) {
            const std::wstring name = Lower(texture.name);
            const std::size_t probe = name.find(kProbe);
            // A day level's: its name's last letter before _probe_ a d.
            if (probe == std::wstring::npos || probe == 0 || name[probe - 1] != L'd')
            {
                return;
            }
            IDirect3DCubeTexture9 *cube = nullptr;
            if (texture.texture == nullptr || texture.texture->GetType() != D3DRTYPE_CUBETEXTURE)
            {
                return;
            }
            cube = static_cast<IDirect3DCubeTexture9 *>(texture.texture);
            auto kept = std::make_unique<Kept>();
            kept->texture = cube;
            kept->name = name;
            kept->level = name.substr(0, probe);
            const std::scoped_lock lock(g_lock);
            g_kept.push_back(std::move(kept));
        },
        [](const dao::resources::Texture &texture) {
            const std::scoped_lock lock(g_lock);
            std::erase_if(g_kept, [&](const std::unique_ptr<Kept> &kept) {
                return static_cast<IDirect3DBaseTexture9 *>(kept->texture) == texture.texture;
            });
        });
}

void Presented()
{
    const float share = daynight::NightShare();
    const std::scoped_lock lock(g_lock);
    for (const std::unique_ptr<Kept> &kept : g_kept)
    {
        if (kept->none)
        {
            continue;
        }
        if (kept->made.valid())
        {
            if (kept->made.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                continue;
            }
            const Made made = kept->made.get();
            kept->cubes = made.cubes;
            if (!made.cubes || !Upload(kept->texture, made))
            {
                kept->none = true;
                spdlog::info(
                    "night cubes: {} is left the day's: its level has no night cube, or it could not be written",
                    std::filesystem::path(kept->name).string());
                continue;
            }
            kept->written = kept->making;
        }
        // Again where the share has moved a step, and always to the ends:
        // the day's own cube by day, the night's by night.
        const bool end = (share == 0 || share == 1) && share != kept->written;
        if (std::abs(share - kept->written) < kStep && !end)
        {
            continue;
        }
        kept->making = share;
        kept->made =
            std::async(std::launch::async, Make, kept->cubes, kept->level, kept->name, share, daynight::NightSky());
    }
}

} // namespace nightcubes
