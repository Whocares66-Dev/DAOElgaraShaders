// The colours of a level's surfaces, out of the files that say them: which
// texture a material paints a surface with, where on that texture a triangle
// is laid, and the texture's colour there. It is what the relighter's
// pictures of a level, its reflection cubes (Relighter.cpp), show a surface
// by, where the bake itself takes every surface for one grey.
//
// Read 2026-10-12, in lak303d's and orz100d's files and across the game's:
//
//   <material>.mao, in materialobjects.erf, or for a tile's ground in the
//     level's own archives: a small XML file. `<Texture Name="mml_tDiffuse"
//     ResName="tti_master_d.dds">` names the texture a surface is painted
//     with. A tile's ground (terrain.mat) is painted from a palette by masks
//     and has no such line; it names a picture of the whole tile as it is
//     drawn from far off, `LowLodMap`, which is taken in its place.
//   the texture: a DDS in the level's own archives, in the texture pack
//     (packages\core\textures\high\texturepack.erf) or in textures.erf. Of
//     the 3603 found that the game's material objects and those two levels'
//     name so, 2145 are DXT1 and 1325 DXT5; 98 are 24 bits a pixel and not
//     packed, which DAOLib's Dds.h does not read and this does; 33 are 32
//     bits; two are test pieces of 16 bits and of 8, which nothing here
//     reads.
//   <model>.msh: a chunk's vertices have the texture's coordinates as their
//     first set. DAOLib's Level.h keeps the second, the lightmap's, alone, so
//     the first is read here.
//
// A material's texture is not one colour, nor is all of it on a surface. A
// tileset is one material and one texture, the mage tower's tti_master 4096
// by 1024 with its walls' pale blocks, its darker flagstones, a mosaic and
// slate side by side. And a texture may have a part nothing is laid on: the
// top quarter of woodtrim_d.dds, which the library's shelves are drawn with,
// is plain white, and by the material's mean the shelves of dark wood were a
// pale grey (tried and seen, 2026-10-12). So a triangle's colour is the mean
// of the part of the texture it is laid over.
//
// Header only, and nothing of the engine: it reads bytes.
#pragma once

#include "LevelFiles.h"

#include <string_view>

namespace surfacecolours
{

namespace core = dao::core;
namespace level = dao::core::level;
using Colour = dao::core::probes::Colour;

// GFF4's labels for a mesh file's chunks (dev/gff4_fields.csv), as DAOLib's Level.cpp has them.
inline constexpr std::uint32_t kName = 2;
inline constexpr std::uint32_t kChunkVertexSize = 8000;
inline constexpr std::uint32_t kChunkVertexCount = 8001;
inline constexpr std::uint32_t kChunkVertexOffset = 8006;
inline constexpr std::uint32_t kMeshChunks = 8021;
inline constexpr std::uint32_t kMeshVertices = 8022;
inline constexpr std::uint32_t kChunkDeclarators = 8025;
inline constexpr std::uint32_t kDeclaratorStream = 8026;
inline constexpr std::uint32_t kDeclaratorOffset = 8027;
inline constexpr std::uint32_t kDeclaratorType = 8028;
inline constexpr std::uint32_t kDeclaratorUsage = 8029;
inline constexpr std::uint32_t kDeclaratorUsageIndex = 8030;
// Direct3D's D3DDECLUSAGE_TEXCOORD, and the D3DDECLTYPEs a texture's coordinates are kept as.
inline constexpr std::uint32_t kUsageTexcoord = 5;
inline constexpr std::uint32_t kTypeFloat2 = 1;
inline constexpr std::uint32_t kTypeHalf2 = 15;

// A texture is brought to no more than this a side before its colours are
// asked for. A triangle's colour is a mean over a part of it: at this size a
// wall's blocks and a floor's flags in the tower's tileset are tens of
// texels across, and its narrowest band of carving one or two.
inline constexpr std::uint32_t kPaintSide = 128;

// A half float as a float (Direct3D's D3DDECLTYPE_FLOAT16_2).
inline float Half(std::uint16_t half)
{
    const std::uint32_t exponent = (half >> 10) & 31U;
    const std::uint32_t fraction = half & 1023U;
    float value = 0;
    if (exponent == 0)
    {
        value = std::ldexp(static_cast<float>(fraction), -24);
    }
    else if (exponent == 31)
    {
        value = std::numeric_limits<float>::infinity();
    }
    else
    {
        value = std::ldexp(static_cast<float>(fraction | 1024U), static_cast<int>(exponent) - 25);
    }
    return (half >> 15) != 0 ? -value : value;
}

// The first set of texture coordinates of one chunk of a mesh file: where
// each vertex is on the texture its material paints it with.
struct ChunkUv
{
    std::wstring name;         // the chunk's
    std::vector<level::Uv> uv; // one a vertex, in the vertices' order
};

// Where in a chunk's vertices their first set of texture coordinates is, and
// whether as half floats; nothing for a chunk that has none, or has them as
// neither two floats nor two halves.
inline std::optional<std::pair<std::size_t, bool>> PaintedAt(const core::gff4::Struct &chunk)
{
    for (const core::gff4::Struct &declarator : chunk.Structs(kChunkDeclarators))
    {
        if (declarator.Get<std::int32_t>(kDeclaratorStream).value_or(-1) != 0 ||
            declarator.Get<std::uint32_t>(kDeclaratorUsage).value_or(~0U) != kUsageTexcoord ||
            declarator.Get<std::uint32_t>(kDeclaratorUsageIndex).value_or(0) != 0)
        {
            continue;
        }
        const std::uint32_t type = declarator.Get<std::uint32_t>(kDeclaratorType).value_or(~0U);
        if (type != kTypeFloat2 && type != kTypeHalf2)
        {
            return std::nullopt;
        }
        return std::pair(static_cast<std::size_t>(declarator.Get<std::int32_t>(kDeclaratorOffset).value_or(0)),
                         type == kTypeHalf2);
    }
    return std::nullopt;
}

// Returns the first set of texture coordinates of every chunk of a mesh file
// (.msh) that has one. A chunk whose vertices run out of the file is left out.
inline std::vector<ChunkUv> PaintedUvs(const core::gff4::File &mesh)
{
    std::vector<ChunkUv> chunks;
    const std::optional<core::gff4::Struct> root = mesh.Root();
    if (!root)
    {
        return chunks;
    }
    const std::span<const std::byte> vertices = root->ListBytes(kMeshVertices, core::gff4::Type::U8);
    for (const core::gff4::Struct &chunk : root->Structs(kMeshChunks))
    {
        const std::size_t size = chunk.Get<std::uint32_t>(kChunkVertexSize).value_or(0);
        const std::size_t count = chunk.Get<std::uint32_t>(kChunkVertexCount).value_or(0);
        const std::size_t start = chunk.Get<std::uint32_t>(kChunkVertexOffset).value_or(0);
        const std::optional<std::pair<std::size_t, bool>> at = PaintedAt(chunk);
        if (!at || size == 0 || start > vertices.size() || count > (vertices.size() - start) / size)
        {
            continue;
        }
        ChunkUv read{chunk.String(kName).value_or(L""), std::vector<level::Uv>(count)};
        bool whole = true;
        for (std::size_t i = 0; i < count && whole; ++i)
        {
            const std::size_t from = start + (i * size) + at->first;
            for (std::size_t k = 0; k < 2; ++k)
            {
                if (at->second)
                {
                    const auto value = core::ReadAt<std::uint16_t>(vertices, from + (k * 2));
                    whole = whole && value.has_value();
                    read.uv[i].at(k) = Half(value.value_or(0));
                }
                else
                {
                    const auto value = core::ReadAt<float>(vertices, from + (k * 4));
                    whole = whole && value.has_value();
                    read.uv[i].at(k) = value.value_or(0.0F);
                }
            }
        }
        if (whole)
        {
            chunks.push_back(std::move(read));
        }
    }
    return chunks;
}

// The file a material object names for one of its textures, in lower case;
// nothing where it names none for that slot.
// @param mao the material object's text
// @param slot the texture's name there, such as mml_tDiffuse
inline std::wstring TextureNamed(std::string_view mao, std::string_view slot)
{
    constexpr std::string_view kFile = "ResName=\"";
    const std::size_t at = mao.find("Name=\"" + std::string(slot) + "\"");
    const std::size_t ends = at == std::string_view::npos ? at : mao.find('>', at);
    const std::size_t from = at == std::string_view::npos ? at : mao.find(kFile, at);
    if (from == std::string_view::npos || from > ends)
    {
        return {};
    }
    const std::size_t to = mao.find('"', from + kFile.size());
    if (to == std::string_view::npos)
    {
        return {};
    }
    // Plain letters, digits and underscores, as every file's name in the game's archives is.
    const std::string_view name = mao.substr(from + kFile.size(), to - from - kFile.size());
    std::wstring file(name.begin(), name.end());
    if (!file.empty() && file.find(L'.') == std::wstring::npos)
    {
        file += L".dds";
    }
    return levelfiles::Lower(std::move(file));
}

// The texture a material object paints a surface with: its diffuse texture,
// or for a tile's ground the picture of the tile from far off.
inline std::wstring PaintNamed(std::string_view mao)
{
    std::wstring file = TextureNamed(mao, "mml_tDiffuse");
    return file.empty() ? TextureNamed(mao, "LowLodMap") : file;
}

// One of a DDS file's pictures: the largest that is no more than `sideMost`
// a side, or the smallest the file has. DXT1, DXT3, DXT5 and 32 bits by
// DAOLib's reader, which reads a file's first picture: the picture is handed
// to it as a file of its own, under this file's header with the picture's
// size. And 24 bits not packed, blue first, here.
inline std::optional<core::dds::Image> PictureOf(std::span<const std::byte> file, std::uint32_t sideMost)
{
    constexpr std::size_t kHeader = 128;
    constexpr std::uint32_t kDxt1 = 0x31545844; // "DXT1"
    constexpr std::uint32_t kDxt3 = 0x33545844;
    constexpr std::uint32_t kDxt5 = 0x35545844;
    constexpr std::uint32_t kLargest = 16384; // more is no texture's
    std::uint32_t height = core::ReadAt<std::uint32_t>(file, 12).value_or(0);
    std::uint32_t width = core::ReadAt<std::uint32_t>(file, 16).value_or(0);
    const std::uint32_t pictures = core::ReadAt<std::uint32_t>(file, 28).value_or(1);
    const std::uint32_t format = core::ReadAt<std::uint32_t>(file, 84).value_or(0);
    const std::uint32_t bits = core::ReadAt<std::uint32_t>(file, 88).value_or(0);
    const bool packed = format == kDxt1 || format == kDxt3 || format == kDxt5;
    if (file.size() < kHeader || width == 0 || height == 0 || width > kLargest || height > kLargest ||
        (!packed && (format != 0 || (bits != 24 && bits != 32))))
    {
        return std::nullopt;
    }
    const auto bytesOf = [&](std::uint32_t w, std::uint32_t h) {
        return packed ? std::size_t{(w + 3) / 4} * ((h + 3) / 4) * (format == kDxt1 ? 8U : 16U)
                      : std::size_t{w} * h * (bits / 8);
    };
    std::size_t at = kHeader;
    for (std::uint32_t picture = 1; picture < pictures && std::max(width, height) > sideMost; ++picture)
    {
        at += bytesOf(width, height);
        width = std::max(width / 2, 1U);
        height = std::max(height / 2, 1U);
    }
    const std::size_t size = bytesOf(width, height);
    if (at > file.size() || file.size() - at < size)
    {
        return std::nullopt;
    }
    const std::span<const std::byte> data = file.subspan(at, size);
    if (!packed && bits == 24)
    {
        core::dds::Image image{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4)};
        for (std::size_t pixel = 0; pixel < std::size_t{width} * height; ++pixel)
        {
            image.rgba[pixel * 4] = std::to_integer<std::uint8_t>(data[(pixel * 3) + 2]);
            image.rgba[(pixel * 4) + 1] = std::to_integer<std::uint8_t>(data[(pixel * 3) + 1]);
            image.rgba[(pixel * 4) + 2] = std::to_integer<std::uint8_t>(data[pixel * 3]);
            image.rgba[(pixel * 4) + 3] = 255;
        }
        return image;
    }
    std::vector<std::byte> one(file.begin(), file.begin() + kHeader);
    std::memcpy(one.data() + 12, &height, sizeof height);
    std::memcpy(one.data() + 16, &width, sizeof width);
    one.insert(one.end(), data.begin(), data.end());
    return core::dds::Decode(one);
}

// Whether a texture's colours are taken as light (LightOf) or as they are
// stored. As they are stored: as light was what was asked for at first, the
// relighter's numbers being light, and it was wrong. The game's shaders multiply a
// texture's value as it is stored by the light on it and show the product,
// with no curve between, and a cube's light is added to that. Measured
// 2026-10-12 on the floor's face of the library's cube (lak303d_probe_3kn),
// the carpet's red over its green beside the flagstones' about it: 2.8 times
// theirs in vanilla's cube, the game's own picture of the room; 6.1 times
// with the colours as light; 3.0 with them as stored. And as stored the
// Commons' colours are 0.38 in the mean of what its cube sees, its recipe's
// `surfaces` of 0.4 all but exactly, where as light they are 0.14.
inline constexpr bool kPaintAsLight = false;

// What a texture's stored value is as light, by the sRGB curve. A texture is
// painted for the screen, and the mean of what it stores is not the mean of
// the light it stands for: black and white squares send half of white's
// light, and their stored mean, a half, is a grey that sends a fifth. The
// curve itself and not a power of 2.2, which is off it in the dark, where
// most of this game's textures are.
inline float LightOf(std::uint8_t stored)
{
    if (!kPaintAsLight)
    {
        return static_cast<float>(stored) / 255.0F;
    }
    static const std::array<float, 256> kLight = [] {
        std::array<float, 256> light{};
        for (std::size_t value = 0; value < light.size(); ++value)
        {
            const float share = static_cast<float>(value) / 255.0F;
            light.at(value) = share <= 0.04045F ? share / 12.92F : std::pow((share + 0.055F) / 1.055F, 2.4F);
        }
        return light;
    }();
    return kLight.at(stored);
}

// The texels a stretch of a texture's coordinate covers, the texture
// repeating: one run of them, from and to before, or two where the stretch
// goes over the texture's edge; all of them for a stretch as long as the
// texture or longer.
inline std::array<std::pair<std::uint32_t, std::uint32_t>, 2> RunsOf(float from, float to, std::uint32_t side)
{
    if (!std::isfinite(from) || !std::isfinite(to) || !(to - from < 1))
    {
        return {{{0, side}, {0, 0}}};
    }
    const float start = from - std::floor(from);
    const float end = start + (to - from);
    const std::uint32_t low = std::min(static_cast<std::uint32_t>(start * static_cast<float>(side)), side - 1);
    const std::uint32_t high = std::max(static_cast<std::uint32_t>(std::ceil(end * static_cast<float>(side))), low + 1);
    if (high <= side)
    {
        return {{{low, high}, {0, 0}}};
    }
    return {{{low, side}, {0, std::min(high - side, low)}}};
}

// A texture as what it paints: its colours as light, brought small, kept as
// sums so that the mean over any upright part of it is four readings.
class Paint
{
  public:
    // @return nothing for a file that is no DDS this reads
    static std::optional<Paint> Of(std::span<const std::byte> dds)
    {
        const std::optional<core::dds::Image> image = PictureOf(dds, kPaintSide);
        if (!image)
        {
            return std::nullopt;
        }
        // A file with the one picture is brought down here, by whole texels.
        std::uint32_t by = 1;
        while (std::max(image->width, image->height) / by > kPaintSide)
        {
            by *= 2;
        }
        Paint paint;
        paint.width_ = std::max(image->width / by, 1U);
        paint.height_ = std::max(image->height / by, 1U);
        // A texel that is cut away, a carpet's fringe or a grille's gaps
        // (alpha 0), is no part of the surface and of no colour; a texture
        // all of whose alpha is 0 keeps something else there, and is whole.
        bool someShown = false;
        for (std::size_t pixel = 0; pixel < image->rgba.size() / 4 && !someShown; ++pixel)
        {
            someShown = image->rgba[(pixel * 4) + 3] != 0;
        }
        std::vector<std::array<double, 4>> texels(std::size_t{paint.width_} * paint.height_);
        for (std::uint32_t y = 0; y < image->height; ++y)
        {
            for (std::uint32_t x = 0; x < image->width; ++x)
            {
                const std::uint8_t *pixel = &image->rgba[((std::size_t{y} * image->width) + x) * 4];
                if (someShown && pixel[3] == 0)
                {
                    continue;
                }
                std::array<double, 4> &texel =
                    texels[(std::size_t{std::min(y / by, paint.height_ - 1)} * paint.width_) +
                           std::min(x / by, paint.width_ - 1)];
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    texel.at(channel) += double{LightOf(pixel[channel])};
                }
                texel[3] += 1;
            }
        }
        const std::size_t across = std::size_t{paint.width_} + 1;
        paint.sums_.assign(across * (std::size_t{paint.height_} + 1), {});
        for (std::uint32_t y = 0; y < paint.height_; ++y)
        {
            for (std::uint32_t x = 0; x < paint.width_; ++x)
            {
                for (std::size_t channel = 0; channel < 4; ++channel)
                {
                    paint.sums_[((std::size_t{y} + 1) * across) + x + 1].at(channel) =
                        texels[(std::size_t{y} * paint.width_) + x].at(channel) +
                        paint.sums_[(std::size_t{y} * across) + x + 1].at(channel) +
                        paint.sums_[((std::size_t{y} + 1) * across) + x].at(channel) -
                        paint.sums_[(std::size_t{y} * across) + x].at(channel);
                }
            }
        }
        paint.mean_ = paint.Within({{{0, paint.width_}, {0, 0}}}, {{{0, paint.height_}, {0, 0}}}).value_or(Colour{});
        return paint;
    }

    // The whole texture's mean colour, as light.
    const Colour &Mean() const
    {
        return mean_;
    }

    // The mean colour, as light, of the part of the texture a triangle is
    // laid over: of the upright box about its three corners, the texture
    // repeating. A triangle with nothing of the texture's under it but what
    // is cut away has the whole texture's.
    Colour Over(const std::array<level::Uv, 3> &uv) const
    {
        const auto [left, right] = std::minmax({uv[0][0], uv[1][0], uv[2][0]});
        const auto [top, bottom] = std::minmax({uv[0][1], uv[1][1], uv[2][1]});
        return Within(RunsOf(left, right, width_), RunsOf(top, bottom, height_)).value_or(mean_);
    }

  private:
    using Runs = std::array<std::pair<std::uint32_t, std::uint32_t>, 2>;

    // The mean over the texels of some columns and rows; nothing where none of them counts.
    std::optional<Colour> Within(const Runs &columns, const Runs &rows) const
    {
        const std::size_t across = std::size_t{width_} + 1;
        std::array<double, 4> sum{};
        for (const auto &[x0, x1] : columns)
        {
            for (const auto &[y0, y1] : rows)
            {
                for (std::size_t channel = 0; channel < 4; ++channel)
                {
                    sum.at(channel) += sums_[(y1 * across) + x1].at(channel) - sums_[(y0 * across) + x1].at(channel) -
                                       sums_[(y1 * across) + x0].at(channel) + sums_[(y0 * across) + x0].at(channel);
                }
            }
        }
        if (sum[3] < 0.5)
        {
            return std::nullopt;
        }
        return Colour{static_cast<float>(sum[0] / sum[3]), static_cast<float>(sum[1] / sum[3]),
                      static_cast<float>(sum[2] / sum[3])};
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::array<double, 4>> sums_; // red, green and blue as light and how many texels, of all above and left
    Colour mean_{};
};

} // namespace surfacecolours
