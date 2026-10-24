#include "Glowing.h"

#include "LevelFiles.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <numbers>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

namespace glowing
{

namespace
{

namespace level = levelfiles::level;
namespace dds = dao::core::dds;

std::map<std::string, Layers> g_palettes; // by texture name, lower case; read once, at Read

// The side of the squares of ground each glowing patch's light stands for,
// metres, and how far that light reaches: Haven's mountain top's cracks of
// lava are a metre or two across and tens of metres long (lak403d's masks,
// 2026-10-23), and at its reach a light lights the patches beside its own.
// At 6 m the level's lava was 304 lights, and those near the plateau took
// all of the shaders' sixteen from the temple gate's torches.
constexpr float kPatch = 10.0F;
constexpr float kReach = 10.0F;
// How high over its ground a patch's light stands, metres: over the ground
// about it, which a light on the ground would light at a grazing angle.
constexpr float kAbove = 1.0F;
// A patch's light at its place: glowing ground of area A, its colour L,
// lights what is d off as L A / (pi d^2), here at 1.5 m. The lights fall
// off as the level's do (shaders/effect_lights.hlsl), not as the square.
constexpr float kNear = 1.5F;
// Ground that glows less than this share of its own colour lights nothing:
// the soft edges of a mask.
constexpr float kLeast = 0.1F;
// A patch whose light is dimmer than this at its place gives none: a sixth
// of a torch's (effects.ini's fxe_fire_small_p, 0.63 0.32 0.12), as lava a
// few patches of mask make, 0.1 m2 and less.
constexpr float kDimmest = 0.1F;

std::string Lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// A texture of the level's, decoded, or none.
std::optional<dds::Image> TextureOf(const levelfiles::Level &level, const std::string &name)
{
    if (name.empty())
    {
        return std::nullopt;
    }
    const std::optional<std::vector<std::byte>> bytes = level.Find(std::wstring(name.begin(), name.end()));
    return bytes ? dds::Decode(*bytes) : std::nullopt;
}

// A texel's four channels, 0 to 1, at a place 0 to 1 across, wrapped as the
// shader's sampler does.
std::array<float, 4> At(const dds::Image &image, float u, float v)
{
    const auto wrap = [](float t, std::uint32_t side) {
        const float f = t - std::floor(t);
        return std::min(static_cast<std::uint32_t>(f * static_cast<float>(side)), side - 1);
    };
    const std::size_t at = ((static_cast<std::size_t>(wrap(v, image.height)) * image.width) + wrap(u, image.width)) * 4;
    constexpr float kByte = 255.0F;
    std::array<float, 4> texel{};
    for (std::size_t c = 0; c < texel.size(); ++c)
    {
        texel.at(c) = static_cast<float>(image.rgba[at + c]) / kByte;
    }
    return texel;
}

// A palette layer's mean colour. Layers 0 to 3 go down its left column, 4 to
// 7 down its right (terrain1.psh's palette coordinates, read 2026-10-23: a
// layer's column is its number over four; lak403d's layer 5, the right
// column's second, is its yellow ground, as its specular colour is).
std::array<float, 3> LayerColour(const dds::Image &palette, std::size_t layer)
{
    const std::uint32_t width = palette.width / 2;
    const std::uint32_t height = palette.height / 4;
    const std::uint32_t left = static_cast<std::uint32_t>(layer / 4) * width;
    const std::uint32_t top = static_cast<std::uint32_t>(layer % 4) * height;
    std::array<double, 3> sum{};
    for (std::uint32_t y = top; y < top + height; ++y)
    {
        for (std::uint32_t x = left; x < left + width; ++x)
        {
            const std::size_t at = ((static_cast<std::size_t>(y) * palette.width) + x) * 4;
            for (std::size_t c = 0; c < 3; ++c)
            {
                sum.at(c) += palette.rgba[at + c] / 255.0;
            }
        }
    }
    const double count = static_cast<double>(width) * height;
    return {static_cast<float>(sum[0] / count), static_cast<float>(sum[1] / count), static_cast<float>(sum[2] / count)};
}

// A material object's textures, by their parameters' names, lower case.
std::map<std::string, std::string> TexturesOf(std::string_view text)
{
    static const std::regex kTexture(R"re(<Texture\s+Name\s*=\s*"([^"]+)"\s+ResName\s*=\s*"([^"]+)")re");
    std::map<std::string, std::string> textures;
    using Found = std::regex_iterator<std::string_view::const_iterator>;
    for (auto it = Found(text.begin(), text.end(), kTexture); it != Found(); ++it)
    {
        textures[Lower((*it)[1].str())] = Lower((*it)[2].str());
    }
    return textures;
}

// What glows of one material: each palette layer's colour times its glow,
// and the masks that weigh the layers.
struct GlowingMaterial
{
    std::array<std::array<float, 3>, 8> glow{};
    dds::Image maskA;
    dds::Image maskA2;
};

// Each palette's layers' colours, by its name: a level's rooms share one.
using PaletteColours = std::map<std::string, std::array<std::array<float, 3>, 8>>;

// One material of the level's, where some layer of its palette glows.
std::optional<GlowingMaterial> GlowingOf(const levelfiles::Level &level, const std::wstring &material,
                                         PaletteColours &palettes)
{
    const std::optional<std::vector<std::byte>> bytes = level.Find(material + L".mao");
    if (!bytes)
    {
        return std::nullopt;
    }
    const std::map<std::string, std::string> textures =
        TexturesOf(std::string_view(reinterpret_cast<const char *>(bytes->data()), bytes->size()));
    const auto named = [&](const std::string &name) {
        const auto found = textures.find(name);
        return found != textures.end() ? found->second : std::string();
    };
    const std::optional<Layers> layers = Of(named("palette"));
    if (!layers)
    {
        return std::nullopt;
    }
    auto [colours, unread] = palettes.try_emplace(named("palette"));
    if (unread)
    {
        const std::optional<dds::Image> palette = TextureOf(level, named("palette"));
        for (std::size_t layer = 0; palette && layer < layers->size(); ++layer)
        {
            colours->second.at(layer) = LayerColour(*palette, layer);
        }
    }
    std::optional<dds::Image> maskA = TextureOf(level, named("maska"));
    std::optional<dds::Image> maskA2 = TextureOf(level, named("maska2"));
    if (!maskA || !maskA2)
    {
        return std::nullopt;
    }
    GlowingMaterial glowing{{}, std::move(*maskA), std::move(*maskA2)};
    for (std::size_t layer = 0; layer < layers->size(); ++layer)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            glowing.glow.at(layer).at(c) = colours->second.at(layer).at(c) * layers->at(layer);
        }
    }
    return glowing;
}

// A square of glowing ground: its glowing area, and its places and colours
// summed, each times its area.
struct Patch
{
    double area = 0;
    std::array<double, 3> at{};
    std::array<double, 3> colour{};
};

using Patches = std::map<std::pair<int, int>, Patch>;

// A triangle's glow, the masks' weights at its middle: the share of it that
// glows, and its colour where it glows.
std::pair<float, std::array<float, 3>> GlowAt(const GlowingMaterial &glowing, float u, float v)
{
    const std::array<float, 4> a = At(glowing.maskA, u, v);
    const std::array<float, 4> a2 = At(glowing.maskA2, u, v);
    const float all = a[0] + a[1] + a[2] + a[3] + a2[0] + a2[1] + a2[2] + a2[3];
    std::array<float, 3> colour{};
    float share = 0;
    for (std::size_t layer = 0; layer < 8 && all > 0; ++layer)
    {
        const float weight = (layer < 4 ? a.at(layer) : a2.at(layer - 4)) / all;
        const std::array<float, 3> &glow = glowing.glow.at(layer);
        if (glow[0] + glow[1] + glow[2] > 0)
        {
            share += weight;
            for (std::size_t c = 0; c < 3; ++c)
            {
                colour.at(c) += weight * glow.at(c);
            }
        }
    }
    if (share > 0)
    {
        for (float &c : colour)
        {
            c /= share;
        }
    }
    return {share, colour};
}

// The glowing ground of one mesh node of a model a room places, added to
// the patches of the squares it lies in.
void AddGround(const level::Room &room, const level::Placed &placed, const level::MeshNode &node,
               const level::MeshChunk &chunk, const GlowingMaterial &glowing, Patches &patches)
{
    if (chunk.paintUv.size() != chunk.positions.size())
    {
        return;
    }
    const auto world = [&](std::uint32_t index) {
        const levelfiles::core::Point inModel = level::Turned(node.turn, chunk.positions[index]);
        return level::InLevel(room, placed,
                              {inModel[0] + node.at[0], inModel[1] + node.at[1], inModel[2] + node.at[2]});
    };
    for (std::size_t i = 0; i + 2 < chunk.indices.size(); i += 3)
    {
        const std::array<std::uint32_t, 3> corner{chunk.indices[i], chunk.indices[i + 1], chunk.indices[i + 2]};
        float u = 0;
        float v = 0;
        for (const std::uint32_t c : corner)
        {
            u += chunk.paintUv[c][0] / 3;
            v += chunk.paintUv[c][1] / 3;
        }
        const auto [share, colour] = GlowAt(glowing, u, v);
        if (share < kLeast)
        {
            continue;
        }
        const levelfiles::core::Point p0 = world(corner[0]);
        const levelfiles::core::Point p1 = world(corner[1]);
        const levelfiles::core::Point p2 = world(corner[2]);
        const std::array<double, 3> e1{p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const std::array<double, 3> e2{p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const std::array<double, 3> across{(e1[1] * e2[2]) - (e1[2] * e2[1]), (e1[2] * e2[0]) - (e1[0] * e2[2]),
                                           (e1[0] * e2[1]) - (e1[1] * e2[0])};
        const double area =
            0.5 * std::sqrt((across[0] * across[0]) + (across[1] * across[1]) + (across[2] * across[2])) * share;
        const std::array<double, 3> middle{(p0[0] + p1[0] + p2[0]) / 3.0, (p0[1] + p1[1] + p2[1]) / 3.0,
                                           (p0[2] + p1[2] + p2[2]) / 3.0};
        Patch &patch = patches[{static_cast<int>(std::floor(middle[0] / kPatch)),
                                static_cast<int>(std::floor(middle[1] / kPatch))}];
        patch.area += area;
        for (std::size_t c = 0; c < 3; ++c)
        {
            patch.at.at(c) += middle.at(c) * area;
            patch.colour.at(c) += colour.at(c) * area;
        }
    }
}

// The glowing ground of one model a room places, the level's own.
void AddModel(levelfiles::Level &level, const level::Room &room, const level::Placed &placed,
              std::map<std::wstring, std::optional<GlowingMaterial>> &materials, PaletteColours &palettes,
              Patches &patches)
{
    const std::wstring file = levelfiles::Lower(placed.file);
    const std::optional<std::vector<std::byte>> tree = level.Find(file + L".mmh");
    const auto parsed = tree ? levelfiles::core::gff4::File::Parse(*tree) : std::nullopt;
    const std::optional<levelfiles::core::gff4::Struct> root = parsed ? parsed->Root() : std::nullopt;
    if (!root)
    {
        return;
    }
    std::map<std::wstring, std::wstring> named;
    levelfiles::MaterialsUnder(*root, 0, named);
    const level::Model model = level::ModelOf(*parsed);
    std::vector<level::MeshChunk> chunks;
    bool meshRead = false;
    for (const level::MeshNode &node : model.nodes)
    {
        const auto material = named.find(levelfiles::Lower(node.chunk));
        if (material == named.end())
        {
            continue;
        }
        auto [glowing, unread] = materials.try_emplace(material->second);
        if (unread)
        {
            glowing->second = GlowingOf(level, material->second, palettes);
        }
        if (!glowing->second)
        {
            continue;
        }
        if (!meshRead)
        {
            meshRead = true;
            // The one its tree names, with its extension or without, or one
            // of its own name (LevelFiles.h's MeshFile).
            std::wstring meshFile = model.meshFile.empty() ? file : levelfiles::Lower(model.meshFile);
            if (!meshFile.ends_with(L".msh"))
            {
                meshFile += L".msh";
            }
            const std::optional<std::vector<std::byte>> mesh = level.Find(meshFile);
            if (const auto meshParsed = mesh ? levelfiles::core::gff4::File::Parse(*mesh) : std::nullopt)
            {
                chunks = level::Meshes(*meshParsed);
            }
        }
        const auto chunk = std::ranges::find_if(chunks, [&](const level::MeshChunk &each) {
            return levelfiles::Lower(each.name) == levelfiles::Lower(node.chunk);
        });
        if (chunk != chunks.end())
        {
            AddGround(room, placed, node, *chunk, *glowing->second, patches);
        }
    }
}

} // namespace

void Read(const std::filesystem::path &folder)
{
    std::ifstream in(folder / "glowing.ini");
    std::string line;
    while (std::getline(in, line))
    {
        const std::size_t equals = line.find('=');
        if (line.empty() || line[0] == ';' || equals == std::string::npos)
        {
            continue;
        }
        std::string name = line.substr(0, equals);
        std::erase_if(name, [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
        std::istringstream said(line.substr(equals + 1, line.find(';', equals) - equals - 1));
        Layers layers{};
        std::size_t layer = 0;
        float strength = 0;
        while (said >> layer >> strength)
        {
            if (layer < layers.size())
            {
                layers.at(layer) = strength;
            }
        }
        g_palettes[Lower(name)] = layers;
    }
    spdlog::info("glowing: {} palettes with layers that glow, by glowing.ini", g_palettes.size());
}

std::optional<Layers> Of(std::string_view palette)
{
    const auto found = g_palettes.find(Lower(palette));
    return found != g_palettes.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<std::vector<std::byte>> Patched(std::span<const std::byte> bytes)
{
    if (g_palettes.empty())
    {
        return std::nullopt;
    }
    const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::map<std::string, std::string> textures = TexturesOf(text);
    const auto palette = textures.find("palette");
    const std::optional<Layers> layers = palette != textures.end() ? Of(palette->second) : std::nullopt;
    constexpr std::string_view kEnd = "</MaterialObject>";
    const std::size_t at = text.rfind(kEnd);
    if (!layers || at == std::string_view::npos)
    {
        return std::nullopt;
    }
    const Layers &l = *layers;
    const std::string added = std::format("\t<Vector4fArray Name=\"mml_vDaoGlow\" value=\"{} {} {} {} {} {} {} {}\" "
                                          "Size=\"2\"></Vector4fArray>\r\n",
                                          l[0], l[1], l[2], l[3], l[4], l[5], l[6], l[7]);
    std::vector<std::byte> out(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    const auto *start = reinterpret_cast<const std::byte *>(added.data());
    out.insert(out.end(), start, start + added.size());
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return out;
}

std::vector<Light> LightsOf(levelfiles::Level &level)
{
    std::vector<Light> lights;
    if (g_palettes.empty())
    {
        return lights;
    }
    Patches patches;
    std::map<std::wstring, std::optional<GlowingMaterial>> materials;
    PaletteColours palettes;
    const std::wstring own = level.Name() + L"_";
    for (const std::wstring &id : level.Rooms())
    {
        if (const level::Room *room = level.RoomOf(id))
        {
            for (const level::Placed &placed : room->models)
            {
                // The level's own ground: a terrain's material is its own.
                if (levelfiles::Lower(placed.file).starts_with(own))
                {
                    AddModel(level, *room, placed, materials, palettes, patches);
                }
            }
        }
    }
    const double near = std::numbers::pi * kNear * kNear;
    for (const auto &[square, patch] : patches)
    {
        if (patch.area <= 0)
        {
            continue;
        }
        Light light;
        for (std::size_t c = 0; c < 3; ++c)
        {
            light.at.at(c) = static_cast<float>(patch.at.at(c) / patch.area);
            light.colour.at(c) = static_cast<float>(patch.colour.at(c) / near);
        }
        light.at[2] += kAbove;
        light.reach = kReach;
        if (std::ranges::max(light.colour) < kDimmest)
        {
            continue;
        }
        lights.push_back(light);
    }
    return lights;
}

} // namespace glowing
