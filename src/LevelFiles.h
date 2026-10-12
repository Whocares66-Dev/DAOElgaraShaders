// A level's files as the baker and the relighter read them: the game's
// archives, a level's rooms or tiles, their models' triangles with their
// place in the lightmaps, and rays cast against them. What a level is on
// disk is ProbeBaker.cpp's head's to say; this is the reading, which was
// that file's own until the relighter (Relighter.cpp) wanted the same.
//
// Header only, and nothing of the engine: it reads files.
#pragma once

#include "dao/core/Dds.h"
#include "dao/core/Erf.h"
#include "dao/core/Gff4.h"
#include "dao/core/Level.h"
#include "dao/core/LightProbes.h"
#include "dao/core/RayCast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace levelfiles
{

namespace core = dao::core;
namespace level = dao::core::level;
namespace probes = dao::core::probes;

inline constexpr std::uint32_t kAtlasSide = 512;        // a lightmap is kept no larger a side
inline constexpr std::uint32_t kTerrainAtlasSide = 256; // nor a terrain level's, which has a hundred of them
inline constexpr int kTilesAbout = 2;                   // a ray from a tile may land on the tiles this many away
// GFF4's labels for what a layout says of a light probe (dev/gff4_fields.csv).
inline constexpr std::uint32_t kChildren = 6999;
inline constexpr std::uint32_t kTranslation = 6047;
inline constexpr std::uint32_t kCubeMap = 3104;       // EnvLightProbeEnvMap
inline constexpr std::uint32_t kIrradianceRed = 6336; // MMHLightProbeIrradianceRed; green and blue follow
inline constexpr std::size_t kNodesDeep = 64;
// The most light a lightmap of our own holds, in whites (Relighter.cpp writes
// it, look.hlsl's kDaoRelitMost reads it): what is stored is the square root
// of the light's share of this.
inline constexpr float kRelitMost = 8.0F;
// And for what it says of a lamp, and a model's tree of a mesh's material.
inline constexpr std::uint32_t kMaterial = 6001;    // MMHMaterialObject
inline constexpr std::uint32_t kMeshChunk = 6006;   // the chunk of the mesh file a node draws
inline constexpr std::uint32_t kLampColour = 6007;  // MMHNodePointLightColor
inline constexpr std::uint32_t kLampReach = 6008;   // MMHNodePointLightRadius

// A file's or a room's name for the log: they are plain letters and digits.
inline std::string Text(const std::wstring &name)
{
    return std::filesystem::path(name).string();
}

inline std::wstring Lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

// One of the game's archives: its directory, and any file of it by name.
// An archive read all through a bake is held open, the models' two and a
// level's rooms': opened for each reading, on a thread whose reading gives
// way to everything, a room of the Circle took seven seconds where it had
// taken half of one (2026-10-11). The rest are opened for each reading and
// not held: a terrain level has an archive or two to a tile, five hundred
// in Denerim's largest, and a process may hold 512 files open.
class Archive
{
  public:
    static std::optional<Archive> Open(const std::filesystem::path &path, bool held = false)
    {
        Archive archive;
        archive.path_ = path;
        archive.held_ = held;
        std::ifstream file(path, std::ios::binary);
        std::vector<std::byte> header(core::erf::kHeaderSize);
        if (!file.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size())))
        {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> count = core::erf::EntryCount(header);
        if (!count)
        {
            return std::nullopt;
        }
        header.resize(core::erf::kHeaderSize + (std::size_t{*count} * core::erf::kEntrySize));
        file.read(reinterpret_cast<char *>(header.data() + core::erf::kHeaderSize),
                  static_cast<std::streamsize>(header.size() - core::erf::kHeaderSize));
        for (core::erf::Entry &entry : core::erf::Entries(std::span(header).first(
                 core::erf::kHeaderSize + static_cast<std::size_t>(std::max<std::streamsize>(file.gcount(), 0)))))
        {
            archive.entries_.emplace(Lower(entry.name), entry);
        }
        return archive;
    }

    bool Has(const std::wstring &name) const
    {
        return entries_.contains(Lower(name));
    }

    std::optional<std::vector<std::byte>> Read(const std::wstring &name) const
    {
        const auto found = entries_.find(Lower(name));
        if (found == entries_.end())
        {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(found->second.size);
        std::ifstream opened;
        std::ifstream &file = held_ ? file_ : opened;
        if (!file.is_open())
        {
            file.open(path_, std::ios::binary);
        }
        file.clear();
        file.seekg(found->second.offset);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            return std::nullopt;
        }
        return bytes;
    }

    const std::map<std::wstring, core::erf::Entry> &Entries() const
    {
        return entries_;
    }

  private:
    std::filesystem::path path_;
    bool held_ = false;
    mutable std::ifstream file_; // open from the first reading on, if held
    std::map<std::wstring, core::erf::Entry> entries_;
};

// A lightmap as three or four bytes a pixel: its colour, and for a terrain
// level its alpha after, where the sun reaches.
struct Atlas
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 3;
    bool ours = false; // one of the relighter's: its light is kept as kRelitMost times the square of what is stored
    std::vector<std::uint8_t> pixels;
};

inline Atlas AtlasOf(const core::dds::Image &image, std::uint32_t channels, std::uint32_t sideMost)
{
    // Halved until it is small enough a side, each pixel the mean of four.
    Atlas atlas{image.width, image.height, channels, {}, {}};
    atlas.pixels.resize(std::size_t{image.width} * image.height * channels);
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i)
    {
        std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), channels,
                    atlas.pixels.begin() + static_cast<std::ptrdiff_t>(i * channels));
    }
    while ((atlas.width > sideMost || atlas.height > sideMost) && atlas.width % 2 == 0 && atlas.height % 2 == 0)
    {
        Atlas half{atlas.width / 2, atlas.height / 2, channels, {}, {}};
        half.pixels.resize(std::size_t{half.width} * half.height * channels);
        for (std::size_t y = 0; y < half.height; ++y)
        {
            for (std::size_t x = 0; x < half.width; ++x)
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    const auto at = [&](std::size_t px, std::size_t py) {
                        return unsigned{atlas.pixels[(((py * atlas.width) + px) * channels) + channel]};
                    };
                    half.pixels[(((y * half.width) + x) * channels) + channel] =
                        static_cast<std::uint8_t>((at(2 * x, 2 * y) + at((2 * x) + 1, 2 * y) + at(2 * x, (2 * y) + 1) +
                                                   at((2 * x) + 1, (2 * y) + 1) + 2) /
                                                  4);
                }
            }
        }
        atlas = std::move(half);
    }
    return atlas;
}

// A model as the rooms place it: its mesh nodes and its mesh file's chunks,
// and the material each chunk is drawn with, by the chunk's name, in lower
// case with no extension.
struct ModelParts
{
    level::Model model;
    std::vector<level::MeshChunk> chunks;
    std::map<std::wstring, std::wstring> materials;
};

// A lamp of a level's own, as a layout has it: a point light.
struct Lamp
{
    core::Point at{}; // in the level's space
    probes::Colour colour{};
    float reach = 0; // metres: its light is nothing from there on
    // Nothing for a lamp of the level's own, whose light falls off as the
    // engine's does. For a light of ours, a flame, the size in metres of
    // what burns, taken as a soft ball of light: the light falls off as the
    // square of the distance, as light does, but no faster near than that
    // size allows (Relighter.cpp).
    float core = 0;
    // How far from the light a shadow's ray stops, in metres, where that is
    // more than the little every lamp has: the size of the fitting the light
    // stands in, which is not in its own light's way.
    float clear = 0;
};

// A room's lightmapped triangles and the tree rays are cast against.
struct Geometry
{
    std::vector<level::LitTriangle> lit;
    std::optional<core::TriangleTree> tree; // over `lit`, in its order; none for a room not read
    std::set<std::uint32_t> atlases;        // the lightmaps its triangles are in
    std::vector<std::wstring> models;       // the models that are the level's own, a tile's ground
    std::vector<std::pair<std::size_t, std::size_t>> ground; // their triangles in `lit`: from, and to before
};

// A light probe of the level's own, as a layout has it.
struct OwnProbe
{
    core::Point at{}; // in the level's space
    probes::Irradiance light{};
    std::wstring cube; // its photograph's file
};

// What a ray landed on: the lightmap there, and how far it is.
struct Lit
{
    probes::Colour baked{};
    float sun = 0; // the lightmap's alpha: how much of the sun reaches the surface
    float distance = 0;
};

// A terrain level's tile by its name's end, "<x>_<y>_0".
struct Tile
{
    int x = 0;
    int y = 0;
};

inline std::optional<int> WholeNumber(std::wstring_view text)
{
    const bool minus = text.starts_with(L'-');
    const std::wstring_view digits = text.substr(minus ? 1 : 0);
    if (digits.empty() || digits.size() > 6 || !std::ranges::all_of(digits, [](wchar_t c) { return std::iswdigit(c) != 0; }))
    {
        return std::nullopt;
    }
    int value = 0;
    for (const wchar_t digit : digits)
    {
        value = (value * 10) + (digit - L'0');
    }
    return minus ? -value : value;
}

inline std::optional<Tile> TileOf(std::wstring_view id)
{
    const std::size_t first = id.find(L'_');
    const std::size_t second = first == std::wstring_view::npos ? first : id.find(L'_', first + 1);
    if (second == std::wstring_view::npos || id.substr(second + 1) != L"0")
    {
        return std::nullopt;
    }
    const std::optional<int> x = WholeNumber(id.substr(0, first));
    const std::optional<int> y = WholeNumber(id.substr(first + 1, second - first - 1));
    return x && y ? std::optional<Tile>(Tile{*x, *y}) : std::nullopt;
}

inline void ProbesUnder(const core::gff4::Struct &node, std::size_t deep, std::vector<OwnProbe> &out)
{
    if (deep > kNodesDeep)
    {
        return;
    }
    for (const core::gff4::Struct &child : node.Structs(kChildren))
    {
        if (child.Label() != "nlpr")
        {
            if (child.Shape().Find(kChildren) != nullptr)
            {
                ProbesUnder(child, deep + 1, out);
            }
            continue;
        }
        OwnProbe probe;
        bool lit = false;
        for (const core::gff4::Struct &part : child.Structs(kChildren))
        {
            if (part.Label() == "trsl")
            {
                if (const std::vector<float> at = part.Floats(kTranslation); at.size() >= 3)
                {
                    std::copy_n(at.begin(), 3, probe.at.begin());
                }
            }
            else if (part.Shape().Find(kCubeMap) != nullptr)
            {
                probe.cube = part.String(kCubeMap).value_or(L"");
            }
            else if (part.Shape().Find(kIrradianceRed) != nullptr)
            {
                lit = true;
                for (std::uint32_t channel = 0; channel < 3; ++channel)
                {
                    const std::vector<float> matrix = part.Floats(kIrradianceRed + channel);
                    lit = lit && matrix.size() == 16;
                    std::copy_n(matrix.begin(), std::min<std::size_t>(matrix.size(), 16),
                                probe.light.begin() + static_cast<std::ptrdiff_t>(channel * 16));
                }
            }
        }
        if (lit)
        {
            out.push_back(std::move(probe));
        }
    }
}

// What says which lightmaps of our own a level has in a folder (the plugin's
// override folder, where the relighter's are put): a number made of their
// names, sizes and times, 0 for none. A table of probes baked from them is
// good for as long as it is the same.
inline std::uint64_t RelitStamp(const std::filesystem::path &folder, const std::wstring &levelName)
{
    std::uint64_t stamp = 0;
    std::error_code error;
    const std::wstring stem = Lower(levelName) + L"_";
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
    {
        const std::wstring name = Lower(entry.path().filename().wstring());
        if (!name.starts_with(stem) || !name.ends_with(L".dds"))
        {
            continue;
        }
        std::uint64_t one = 1469598103934665603ULL;
        for (const wchar_t letter : name)
        {
            one = (one ^ static_cast<std::uint64_t>(letter)) * 1099511628211ULL;
        }
        one ^= static_cast<std::uint64_t>(entry.file_size(error)) * 0x9E3779B97F4A7C15ULL;
        one ^= static_cast<std::uint64_t>(entry.last_write_time(error).time_since_epoch().count());
        stamp += one | 1; // a sum, so the order they are met in does not count
    }
    return stamp;
}

// The materials of a model's mesh nodes, by the chunk each draws.
inline void MaterialsUnder(const core::gff4::Struct &node, std::size_t deep, std::map<std::wstring, std::wstring> &out);

// The lamps under a node of a layout, each at its own place in the layout.
inline void LampsUnder(const core::gff4::Struct &node, std::size_t deep, std::vector<Lamp> &out);

// A level being baked: its archives, and what has been read of it so far. A
// room is known by what its definition's name ends in: a room's number, a
// tile's "<x>_<y>_0", and nothing at all for the level's own definition.
class Level
{
  public:
    Level(std::filesystem::path folder, std::wstring name, Archive rooms, Archive *hierarchies, Archive *meshes)
        : folder_(std::move(folder)), name_(Lower(std::move(name))), rooms_(std::move(rooms)), hierarchies_(hierarchies),
          meshes_(meshes)
    {
        // Every archive of the level's folder but the rooms' definitions: a
        // room's layout and its lightmaps are in one or another of them.
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(folder_, error))
        {
            if (Lower(entry.path().extension().wstring()) == L".rim" && Lower(entry.path().stem().wstring()) != name_)
            {
                if (std::optional<Archive> archive = Archive::Open(entry.path()))
                {
                    for (const auto &[file, unused] : archive->Entries())
                    {
                        where_.emplace(file, others_.size());
                    }
                    others_.push_back(std::move(*archive));
                }
            }
        }
        const std::wstring stem = name_ + L"_";
        std::vector<std::pair<Tile, std::wstring>> tiles;
        for (const auto &[file, entry] : rooms_.Entries())
        {
            if (!file.starts_with(stem) || !file.ends_with(L".rml"))
            {
                continue;
            }
            std::wstring id = file.substr(stem.size(), file.size() - stem.size() - 4);
            if (WholeNumber(id).value_or(-1) >= 0)
            {
                numbered_.push_back(std::move(id));
            }
            else if (const std::optional<Tile> tile = TileOf(id))
            {
                tiles.emplace_back(*tile, std::move(id));
            }
        }
        // Column by column, so that what is two columns back is done with.
        std::ranges::sort(tiles, {}, [](const auto &tile) { return std::pair(tile.first.x, tile.first.y); });
        for (auto &[tile, id] : tiles)
        {
            tiles_.push_back(std::move(id));
        }
    }

    // Says where lightmaps of our own are to be looked for before the
    // level's own: a file there of an atlas's name is the atlas, as it is
    // for the game.
    void ReadRelitFrom(std::filesystem::path folder)
    {
        relit_ = std::move(folder);
    }

    // Whether the level is made of terrain, tiles and no rooms.
    bool Terrain() const
    {
        return numbered_.empty() && !tiles_.empty();
    }

    // The level's rooms, or its tiles in the order they are baked.
    const std::vector<std::wstring> &Rooms() const
    {
        return Terrain() ? tiles_ : numbered_;
    }

    const level::Room *RoomOf(const std::wstring &id)
    {
        if (const auto found = definitions_.find(id); found != definitions_.end())
        {
            return found->second ? &*found->second : nullptr;
        }
        std::optional<level::Room> room;
        if (const auto bytes = rooms_.Read(Stem(id) + L".rml"))
        {
            if (const auto file = core::gff4::File::Parse(*bytes))
            {
                room = level::RoomOf(*file);
            }
        }
        const auto kept = definitions_.emplace(id, std::move(room)).first;
        return kept->second ? &*kept->second : nullptr;
    }

    // The rooms whose triangles a ray from a room may land on beside its
    // own: those its definition says are seen from it, and for a tile those
    // within two of it.
    std::vector<std::wstring> SeenFrom(const std::wstring &id)
    {
        std::vector<std::wstring> seen;
        if (const std::optional<Tile> tile = Terrain() ? TileOf(id) : std::nullopt)
        {
            for (int x = tile->x - kTilesAbout; x <= tile->x + kTilesAbout; ++x)
            {
                for (int y = tile->y - kTilesAbout; y <= tile->y + kTilesAbout; ++y)
                {
                    std::wstring other = std::to_wstring(x) + L"_" + std::to_wstring(y) + L"_0";
                    if (other != id && rooms_.Has(Stem(other) + L".rml"))
                    {
                        seen.push_back(std::move(other));
                    }
                }
            }
        }
        else if (const level::Room *room = RoomOf(id))
        {
            for (const std::uint32_t number : room->sees)
            {
                if (std::wstring other = std::to_wstring(number); other != id)
                {
                    seen.push_back(std::move(other));
                }
            }
        }
        return seen;
    }

    // The level's own probes that a room's layout has, in the level's space.
    std::vector<OwnProbe> ProbesOf(const std::wstring &id)
    {
        std::vector<OwnProbe> found;
        const level::Room *room = RoomOf(id);
        const std::wstring layout = Stem(id) + L"_ll";
        const std::optional<std::vector<std::byte>> bytes = Find(layout + L".mmh");
        if (room == nullptr || !bytes)
        {
            return found;
        }
        const auto placed = std::ranges::find_if(room->models, [&](const level::Placed &model) {
            return Lower(model.file) == layout;
        });
        const auto file = core::gff4::File::Parse(*bytes);
        const std::optional<core::gff4::Struct> root = file ? file->Root() : std::nullopt;
        if (placed == room->models.end() || !root)
        {
            return found;
        }
        ProbesUnder(*root, 0, found);
        for (OwnProbe &probe : found)
        {
            probe.at = level::InLevel(*room, *placed, probe.at);
        }
        return found;
    }

    // The level's own lamps that a room's layout has, in the level's space.
    std::vector<Lamp> LampsOf(const std::wstring &id)
    {
        std::vector<Lamp> found;
        const level::Room *room = RoomOf(id);
        const std::wstring layout = Stem(id) + L"_ll";
        const std::optional<std::vector<std::byte>> bytes = Find(layout + L".mmh");
        if (room == nullptr || !bytes)
        {
            return found;
        }
        const auto placed = std::ranges::find_if(room->models, [&](const level::Placed &model) {
            return Lower(model.file) == layout;
        });
        const auto file = core::gff4::File::Parse(*bytes);
        const std::optional<core::gff4::Struct> root = file ? file->Root() : std::nullopt;
        if (placed == room->models.end() || !root)
        {
            return found;
        }
        LampsUnder(*root, 0, found);
        for (Lamp &lamp : found)
        {
            lamp.at = level::InLevel(*room, *placed, lamp.at);
        }
        return found;
    }

    // One of the models a room places, as triangles in the level's space,
    // lightmapped or not, each with its material's name.
    struct Drawn
    {
        core::Triangle corners;
        const std::wstring *material; // of the model's own, which the level keeps
    };
    std::vector<Drawn> TrianglesOf(const std::wstring &id, const level::Placed &placed)
    {
        std::vector<Drawn> out;
        const level::Room *room = RoomOf(id);
        const ModelParts *parts = ModelOf(placed.file);
        if (room == nullptr || parts == nullptr)
        {
            return out;
        }
        static const std::wstring kNone;
        for (const level::MeshNode &node : parts->model.nodes)
        {
            const auto chunk = std::ranges::find(parts->chunks, node.chunk, &level::MeshChunk::name);
            if (chunk == parts->chunks.end())
            {
                continue;
            }
            const auto material = parts->materials.find(Lower(node.chunk));
            const std::wstring *named = material != parts->materials.end() ? &material->second : &kNone;
            const auto corner = [&](std::uint32_t index) {
                const core::Point inModel = level::Turned(node.turn, chunk->positions[index]);
                return level::InLevel(*room, placed,
                                      {inModel[0] + node.at[0], inModel[1] + node.at[1], inModel[2] + node.at[2]});
            };
            for (std::size_t i = 0; i + 2 < chunk->indices.size(); i += 3)
            {
                out.push_back(
                    {{corner(chunk->indices[i]), corner(chunk->indices[i + 1]), corner(chunk->indices[i + 2])}, named});
            }
        }
        return out;
    }

    // A lightmap's file name by the number its triangles keep.
    const std::wstring &AtlasName(std::uint32_t number) const
    {
        static const std::wstring kNone;
        return number < atlasNames_.size() ? atlasNames_[number] : kNone;
    }
    std::size_t Atlases() const
    {
        return atlasNames_.size();
    }

    // The room's lightmapped triangles and a tree over them, made once.
    const Geometry &GeometryOf(const std::wstring &id)
    {
        if (const auto found = geometry_.find(id); found != geometry_.end())
        {
            return found->second;
        }
        Geometry &geometry = geometry_[id];
        std::vector<level::LitTriangle> &out = geometry.lit;
        const level::Room *room = RoomOf(id);
        if (room == nullptr)
        {
            return geometry;
        }
        for (const level::Placed &placed : room->models)
        {
            if (placed.parts.empty())
            {
                continue;
            }
            if (const ModelParts *parts = ModelOf(placed.file))
            {
                const bool own = Lower(placed.file).starts_with(name_ + L"_");
                const std::size_t from = out.size();
                level::AddTriangles(
                    *room, placed, parts->model, parts->chunks,
                    [&](const std::wstring &atlas) {
                        const std::uint32_t number = AtlasNumber(atlas);
                        geometry.atlases.insert(number);
                        return number;
                    },
                    out);
                if (own)
                {
                    geometry.models.push_back(Lower(placed.file));
                    geometry.ground.emplace_back(from, out.size());
                }
            }
        }
        std::vector<core::Triangle> corners;
        corners.reserve(out.size());
        for (const level::LitTriangle &triangle : out)
        {
            corners.push_back(triangle.corners);
        }
        geometry.tree.emplace(corners);
        return geometry;
    }

    // Lets go of the triangles, the lightmaps and the ground of the tiles
    // of the columns before `column`.
    void DropBefore(int column)
    {
        for (auto at = geometry_.begin(); at != geometry_.end();)
        {
            const std::optional<Tile> tile = TileOf(at->first);
            if (!tile || tile->x >= column)
            {
                ++at;
                continue;
            }
            for (const std::uint32_t atlas : at->second.atlases)
            {
                atlases_.erase(atlas);
            }
            for (const std::wstring &model : at->second.models)
            {
                models_.erase(model);
            }
            at = geometry_.erase(at);
        }
    }

    // The lightmap at a place of an atlas, each 0 to 1.
    Lit LitAt(std::uint32_t atlas, const level::Uv &uv, float distance)
    {
        const Atlas &image = AtlasOf(atlas);
        if (image.pixels.empty())
        {
            return {{}, 0, distance};
        }
        const auto x = static_cast<std::size_t>(std::clamp(uv[0] * static_cast<float>(image.width), 0.0F,
                                                           static_cast<float>(image.width - 1)));
        const auto y = static_cast<std::size_t>(std::clamp(uv[1] * static_cast<float>(image.height), 0.0F,
                                                           static_cast<float>(image.height - 1)));
        const std::uint8_t *pixel = &image.pixels[((y * image.width) + x) * image.channels];
        const auto light = [&](std::size_t channel) {
            const float stored = static_cast<float>(pixel[channel]) / 255.0F;
            return image.ours ? stored * stored * kRelitMost : stored;
        };
        return {{light(0), light(1), light(2)}, image.channels > 3 ? static_cast<float>(pixel[3]) / 255.0F : 0.0F,
                distance};
    }

    // A file of the level's own archives, a room's first.
    std::optional<std::vector<std::byte>> Find(const std::wstring &file) const
    {
        if (const auto found = where_.find(Lower(file)); found != where_.end())
        {
            return others_[found->second].Read(file);
        }
        return rooms_.Read(file);
    }

    const std::wstring &Name() const
    {
        return name_;
    }

  private:
    // A room's files' names before what says which file: <level>_<room>.
    std::wstring Stem(const std::wstring &id) const
    {
        return id.empty() ? name_ : name_ + L"_" + id;
    }

    const ModelParts *ModelOf(const std::wstring &name)
    {
        const std::wstring key = Lower(name);
        if (const auto found = models_.find(key); found != models_.end())
        {
            return found->second ? &*found->second : nullptr;
        }
        std::optional<ModelParts> parts;
        // The game's models, then the level's own: a tile's ground.
        std::optional<std::vector<std::byte>> tree = hierarchies_->Read(key + L".mmh");
        if (!tree)
        {
            tree = Find(key + L".mmh");
        }
        if (const auto file = tree ? core::gff4::File::Parse(*tree) : std::nullopt)
        {
            ModelParts read;
            read.model = level::ModelOf(*file);
            if (const std::optional<core::gff4::Struct> root = file->Root())
            {
                MaterialsUnder(*root, 0, read.materials);
            }
            std::wstring meshFile = read.model.meshFile.empty() ? key : read.model.meshFile;
            if (!meshFile.ends_with(L".msh"))
            {
                meshFile += L".msh";
            }
            std::optional<std::vector<std::byte>> mesh = meshes_->Read(meshFile);
            if (!mesh)
            {
                mesh = Find(meshFile);
            }
            if (const auto meshParsed = mesh ? core::gff4::File::Parse(*mesh) : std::nullopt)
            {
                read.chunks = level::Meshes(*meshParsed);
                parts = std::move(read);
            }
        }
        const auto kept = models_.emplace(key, std::move(parts)).first;
        return kept->second ? &*kept->second : nullptr;
    }

    std::uint32_t AtlasNumber(const std::wstring &file)
    {
        const std::wstring key = Lower(file);
        if (const auto found = atlasNumbers_.find(key); found != atlasNumbers_.end())
        {
            return found->second;
        }
        const auto number = static_cast<std::uint32_t>(atlasNames_.size());
        atlasNames_.push_back(key);
        atlasNumbers_.emplace(key, number);
        return number;
    }

    const Atlas &AtlasOf(std::uint32_t number)
    {
        if (const auto found = atlases_.find(number); found != atlases_.end())
        {
            return found->second;
        }
        Atlas atlas;
        if (number < atlasNames_.size())
        {
            std::optional<std::vector<std::byte>> bytes;
            bool made = false; // by the relighter
            if (!relit_.empty())
            {
                std::ifstream ours(relit_ / atlasNames_[number], std::ios::binary);
                if (ours)
                {
                    const std::vector<char> read((std::istreambuf_iterator<char>(ours)), std::istreambuf_iterator<char>());
                    bytes.emplace(read.size());
                    std::memcpy(bytes->data(), read.data(), read.size());
                    made = true;
                }
            }
            if (!bytes)
            {
                bytes = Find(atlasNames_[number]);
            }
            if (bytes)
            {
                if (const std::optional<core::dds::Image> image = core::dds::Decode(*bytes))
                {
                    atlas = Terrain() ? levelfiles::AtlasOf(*image, 4, kTerrainAtlasSide)
                                      : levelfiles::AtlasOf(*image, 3, kAtlasSide);
                    atlas.ours = made;
                }
            }
        }
        return atlases_.emplace(number, std::move(atlas)).first->second;
    }

    std::filesystem::path folder_;
    std::filesystem::path relit_; // where lightmaps of our own are, or nothing
    std::wstring name_;
    Archive rooms_;
    Archive *hierarchies_;
    Archive *meshes_;
    std::vector<Archive> others_;
    std::unordered_map<std::wstring, std::size_t> where_; // a file's name to the one of others_ it is in
    std::vector<std::wstring> numbered_;
    std::vector<std::wstring> tiles_;
    std::map<std::wstring, std::optional<level::Room>> definitions_;
    std::map<std::wstring, Geometry> geometry_;
    std::unordered_map<std::wstring, std::optional<ModelParts>> models_;
    std::unordered_map<std::wstring, std::uint32_t> atlasNumbers_;
    std::vector<std::wstring> atlasNames_;
    std::map<std::uint32_t, Atlas> atlases_;
};

inline void MaterialsUnder(const core::gff4::Struct &node, std::size_t deep, std::map<std::wstring, std::wstring> &out)
{
    if (deep > kNodesDeep)
    {
        return;
    }
    const std::optional<std::wstring> chunk = node.String(kMeshChunk);
    const std::optional<std::wstring> material = node.String(kMaterial);
    if (chunk && material && !chunk->empty())
    {
        std::wstring name = Lower(*material);
        if (const std::size_t dot = name.rfind(L'.'); dot != std::wstring::npos)
        {
            name.erase(dot);
        }
        out.emplace(Lower(*chunk), std::move(name));
    }
    if (node.Shape().Find(kChildren) != nullptr)
    {
        for (const core::gff4::Struct &child : node.Structs(kChildren))
        {
            MaterialsUnder(child, deep + 1, out);
        }
    }
}

inline void LampsUnder(const core::gff4::Struct &node, std::size_t deep, std::vector<Lamp> &out)
{
    if (deep > kNodesDeep)
    {
        return;
    }
    for (const core::gff4::Struct &child : node.Structs(kChildren))
    {
        if (child.Label() != "nplt")
        {
            if (child.Shape().Find(kChildren) != nullptr)
            {
                LampsUnder(child, deep + 1, out);
            }
            continue;
        }
        Lamp lamp;
        const std::vector<float> colour = child.Floats(kLampColour);
        if (colour.size() >= 3)
        {
            std::copy_n(colour.begin(), 3, lamp.colour.begin());
        }
        lamp.reach = child.Get<float>(kLampReach).value_or(0.0F);
        for (const core::gff4::Struct &part : child.Structs(kChildren))
        {
            if (part.Label() == "trsl")
            {
                if (const std::vector<float> at = part.Floats(kTranslation); at.size() >= 3)
                {
                    std::copy_n(at.begin(), 3, lamp.at.begin());
                }
            }
        }
        out.push_back(lamp);
    }
}

// A room and the rooms seen from it that have triangles, the room first,
// each with its own tree: a ray is cast against each and lands on the
// nearest.
inline std::vector<const Geometry *> About(Level &level, const std::wstring &id)
{
    std::vector<const Geometry *> about{&level.GeometryOf(id)};
    for (const std::wstring &seen : level.SeenFrom(id))
    {
        if (const Geometry &other = level.GeometryOf(seen); !other.lit.empty())
        {
            about.push_back(&other);
        }
    }
    return about;
}

// The baked light rays have landed on, summed, for a room's `seen`.
struct SeenSum
{
    double light = 0;
    std::size_t rays = 0;

    void Add(const Lit &lit)
    {
        light += (0.2126 * lit.baked[0]) + (0.7152 * lit.baked[1]) + (0.0722 * lit.baked[2]);
        ++rays;
    }
    float Mean() const
    {
        return rays != 0 ? static_cast<float>(light / static_cast<double>(rays)) : 0.0F;
    }
};

// What a ray from a place lands on, or nothing.
inline std::optional<Lit> Look(Level &level, std::span<const Geometry *const> about, const core::Point &place,
                        const core::Point &ray)
{
    const Geometry *nearest = nullptr;
    core::RayHit hit;
    float most = std::numeric_limits<float>::infinity();
    for (const Geometry *geometry : about)
    {
        if (!geometry->tree)
        {
            continue;
        }
        if (const std::optional<core::RayHit> found = geometry->tree->Cast(place, ray, 1e-3F, most))
        {
            nearest = geometry;
            hit = *found;
            most = found->distance;
        }
    }
    if (nearest == nullptr)
    {
        return std::nullopt;
    }
    const level::LitTriangle &triangle = nearest->lit[hit.triangle];
    const float w = 1 - hit.u - hit.v;
    return level.LitAt(triangle.atlas,
                       {(w * triangle.uv[0][0]) + (hit.u * triangle.uv[1][0]) + (hit.v * triangle.uv[2][0]),
                        (w * triangle.uv[0][1]) + (hit.u * triangle.uv[1][1]) + (hit.v * triangle.uv[2][1])},
                       hit.distance);
}

} // namespace levelfiles
