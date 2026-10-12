// The baker's thread and its reading of the game's archives. What a probe is
// and how one is baked is dao/core/LightProbes.h's; how a level's geometry is
// read is dao/core/Level.h's. Here: which files, in what order, and how much
// is held at a time, which matters in a game of 32 bits.
//
// A level is its folder under packages\core\env, and is one of two kinds
// (all 158 looked at, 2026-10-11: 106 of rooms, 52 of terrain).
//
// A level of rooms. <level>.rim has every room's definition
// (<level>_<room>.rml), and each room's own archives (<level>_<room>.rim and
// others of that stem) its layout (<level>_<room>_ll.mmh, where its light
// probe is) and its lightmap atlases. A room's probes are baked against its
// own triangles and those of the rooms its definition says are seen from it,
// each as all the light a surface of its place receives (LightProbes.h).
//
// A level of terrain: everything out of doors, and Orzammar's halls. Its
// rooms are tiles on a grid, <level>_<x>_<y>_<detail>.rml, the detail 0 the
// one seen from near. The ground is in patches two tiles by two, each a
// model of its own that one of its four tiles places (<level>_<n>_0.mmh and
// .msh, in that tile's archive <level>_<x>_<y>_0.rim; Redcliffe's are 132 m
// a side on tiles of 66), and a tile's lightmaps are in
// <level>_<x>_<y>_0.gpu.rim. So a ray from a tile is cast against the tiles
// two away on every side, which is every patch that touches its own. The level has one
// light probe for all of it, seldom two or three, in the layout the level's
// own definition places (<level>.rml, <level>_ll.mmh), and the engine gives
// a figure anywhere in the level that probe (seen in Redcliffe with a table
// of one red probe: figures 190 m from it were red, 2026-10-11). So the
// tiles' probes are kept as that probe's room.
//
// A lightmap out of doors says little by its colour, which is one dim grey
// over a whole village; what the sun does is in its alpha, where the sun
// reaches, and the rest of a figure's light there is the sky. So a terrain
// level's probes are baked as its own probe was made, and not as a room's
// are. That probe is a photograph: a cube map taken at its place (its faces
// are Direct3D's in the level's own axes, and its nine coefficients are the
// .mtx file's to 0.003, reckoned 2026-10-11), and the probe's light is what
// that picture sends to its middle. The baker takes from the photograph
// what it cannot work out, the sky wherever a ray from the probe's place
// lands on nothing, and what a lit surface of this level sends on: how much
// of its baked light, how much of the sun where the lightmap says the sun
// reaches, and how much with neither (the area's ambient light, which no
// lightmap has), fitted to what the photograph shows of the surfaces rays
// from its place land on. A probe anywhere else in the level is then the
// photograph taken again by reckoning: the sky where a ray lands on
// nothing, the surface's own sending where it lands. Such a probe holds
// what the level's own does and no more, and is lit up like one where the
// area has no sun (Graphics.cpp's Ambient).
//
// Triangles and lightmaps are kept for the rooms of a level as they are
// first wanted and dropped when the level is done, a terrain level's when
// the tiles three columns back are done; the level's probes are handed over
// together. A lightmap is kept at half its size a side, a terrain level's
// at a quarter, which is plenty for the light of a place.
//
// Not done: a room with no probe of the level's own is left out (the engine
// has nothing to choose there for the table's room to be found by), and a
// file a mod overrides loose is not seen, only the archives.
#include "ProbeBaker.h"

#include "dao/core/Dds.h"
#include "dao/core/Erf.h"
#include "dao/core/Gff4.h"
#include "dao/core/Level.h"
#include "dao/core/RayCast.h"

#include <spdlog/spdlog.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

namespace probebaker
{

namespace
{

namespace core = dao::core;
namespace level = dao::core::level;
namespace probes = dao::core::probes;

constexpr float kSpacing = 5.0F;                 // metres between a room's probes
constexpr float kTerrainSpacing = 8.0F;          // and between a terrain tile's, where light changes over more ground
constexpr std::size_t kRays = 384;               // from each probe
constexpr std::uint32_t kAtlasSide = 512;        // a lightmap is kept no larger a side
constexpr std::uint32_t kTerrainAtlasSide = 256; // nor a terrain level's, which has a hundred of them
constexpr std::uint32_t kSkySide = 32;           // a face of a probe's photograph is read this large
constexpr int kTilesAbout = 2;                   // a ray from a tile may land on the tiles this many away
constexpr float kCell = 10.0F;                   // metres a side: a cell of a room's grid of probes
// How many metres along the ground a metre of height counts for, in how near
// a probe is: light changes more from one floor to the next than along one,
// and a probe on a roof is not near the figure under it. Up to kSameStorey
// of height counts for nothing: a probe is a metre over its floor and a
// figure's place is wherever the engine has it, its feet or its middle.
constexpr float kHeightCounts = 3.0F;
constexpr float kSameStorey = 1.5F;
// Where a figure can stand, as LightProbes.h's FloorPlaces has it: a metre
// above a surface that faces up, with this much free air over it.
constexpr float kStandsAt = 1.0F;
constexpr float kHeadroom = 2.2F;
constexpr float kFacesUp = 0.7F;        // of a normal's length upward: a floor
constexpr float kSameFloor = 0.2F;      // metres: surfaces nearer than this in height are one floor
constexpr std::size_t kLayersMost = 64; // surfaces looked through under one point of the grid
// LightProbes.h's own, for a probe baked here: a place is closed in, inside
// a wall or a rock, when three rays in ten land within half a metre.
constexpr float kNearWall = 0.5F;
constexpr float kNearWallShare = 0.3F;
// What a lit surface sends on of its baked light where the photograph does
// not say: the share the Circle's own probes were of a bake with every
// surface white (0.6, DAOLib #65).
constexpr float kSendsOn = 0.6F;
constexpr float kSendsMost = 4.0F;   // the most a fitted share may be
constexpr std::size_t kFitLeast = 64; // surfaces seen in the photograph, under which nothing is fitted

// GFF4's labels for what a layout says of a light probe (dev/gff4_fields.csv).
constexpr std::uint32_t kChildren = 6999;
constexpr std::uint32_t kTranslation = 6047;
constexpr std::uint32_t kCubeMap = 3104;       // EnvLightProbeEnvMap
constexpr std::uint32_t kIrradianceRed = 6336; // MMHLightProbeIrradianceRed; green and blue follow
constexpr std::size_t kNodesDeep = 64;

constexpr std::uint32_t kTableMagic = 0x50474C45; // "ELGP"
constexpr std::uint32_t kTableVersion = 1;

// A file's or a room's name for the log: they are plain letters and digits.
std::string Text(const std::wstring &name)
{
    return std::filesystem::path(name).string();
}

std::wstring Lower(std::wstring text)
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
        header.resize(core::erf::kHeaderSize + std::size_t{*count} * core::erf::kEntrySize);
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
    std::vector<std::uint8_t> pixels;
};

Atlas AtlasOf(const core::dds::Image &image, std::uint32_t channels, std::uint32_t sideMost)
{
    // Halved until it is small enough a side, each pixel the mean of four.
    Atlas atlas{image.width, image.height, channels, {}};
    atlas.pixels.resize(std::size_t{image.width} * image.height * channels);
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i)
    {
        std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), channels,
                    atlas.pixels.begin() + static_cast<std::ptrdiff_t>(i * channels));
    }
    while ((atlas.width > sideMost || atlas.height > sideMost) && atlas.width % 2 == 0 && atlas.height % 2 == 0)
    {
        Atlas half{atlas.width / 2, atlas.height / 2, channels, {}};
        half.pixels.resize(std::size_t{half.width} * half.height * channels);
        for (std::size_t y = 0; y < half.height; ++y)
        {
            for (std::size_t x = 0; x < half.width; ++x)
            {
                for (std::size_t channel = 0; channel < channels; ++channel)
                {
                    const auto at = [&](std::size_t px, std::size_t py) {
                        return unsigned{atlas.pixels[(py * atlas.width + px) * channels + channel]};
                    };
                    half.pixels[(y * half.width + x) * channels + channel] = static_cast<std::uint8_t>(
                        (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1) + 2) / 4);
                }
            }
        }
        atlas = std::move(half);
    }
    return atlas;
}

// A model as the rooms place it: its mesh nodes and its mesh file's chunks.
struct ModelParts
{
    level::Model model;
    std::vector<level::MeshChunk> chunks;
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

std::optional<int> WholeNumber(std::wstring_view text)
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
        value = value * 10 + (digit - L'0');
    }
    return minus ? -value : value;
}

std::optional<Tile> TileOf(std::wstring_view id)
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

void ProbesUnder(const core::gff4::Struct &node, std::size_t deep, std::vector<OwnProbe> &out)
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
        const std::uint8_t *pixel = &image.pixels[(y * image.width + x) * image.channels];
        return {{static_cast<float>(pixel[0]) / 255.0F, static_cast<float>(pixel[1]) / 255.0F,
                 static_cast<float>(pixel[2]) / 255.0F},
                image.channels > 3 ? static_cast<float>(pixel[3]) / 255.0F : 0.0F, distance};
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
            if (const auto bytes = Find(atlasNames_[number]))
            {
                if (const std::optional<core::dds::Image> image = core::dds::Decode(*bytes))
                {
                    atlas = Terrain() ? probebaker::AtlasOf(*image, 4, kTerrainAtlasSide)
                                      : probebaker::AtlasOf(*image, 3, kAtlasSide);
                }
            }
        }
        return atlases_.emplace(number, std::move(atlas)).first->second;
    }

    std::filesystem::path folder_;
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

// A room and the rooms seen from it that have triangles, the room first,
// each with its own tree: a ray is cast against each and lands on the
// nearest.
std::vector<const Geometry *> About(Level &level, const std::wstring &id)
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

// What a ray from a place lands on, or nothing.
std::optional<Lit> Look(Level &level, std::span<const Geometry *const> about, const core::Point &place,
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
                       {w * triangle.uv[0][0] + hit.u * triangle.uv[1][0] + hit.v * triangle.uv[2][0],
                        w * triangle.uv[0][1] + hit.u * triangle.uv[1][1] + hit.v * triangle.uv[2][1]},
                       hit.distance);
}

// The probes of one room of a level of rooms: baked against its own
// triangles and those of the rooms seen from it.
std::optional<Room> BakeRoom(Level &level, const std::wstring &id, std::span<const core::Point> rays)
{
    const std::vector<OwnProbe> own = level.ProbesOf(id);
    if (own.empty())
    {
        spdlog::debug("light probes: room {}: no probe of its own", Text(id));
        return std::nullopt;
    }
    const std::vector<const Geometry *> about = About(level, id);
    if (about.front()->lit.empty())
    {
        spdlog::debug("light probes: room {}: no lightmapped triangles", Text(id));
        return std::nullopt;
    }
    Room baked;
    baked.key = own.front().at;
    baked.own = own.front().light;
    baked.whole = true;
    for (const core::Point &place : probes::FloorPlaces(*about.front()->tree, kSpacing))
    {
        const std::optional<probes::Coefficients> light =
            probes::Bake(rays, [&](const core::Point &ray) -> std::optional<probes::Landed> {
                const std::optional<Lit> lit = Look(level, about, place, ray);
                return lit ? std::optional<probes::Landed>(probes::Landed{lit->baked, lit->distance}) : std::nullopt;
            });
        if (light)
        {
            baked.probes.push_back({place, probes::Matrices(*light)});
        }
    }
    spdlog::debug("light probes: room {}: key {:.2f} {:.2f} {:.2f}, {} triangles of its own, {} rooms about it, {} probes",
                  Text(id), baked.key[0], baked.key[1], baked.key[2], about.front()->lit.size(), about.size() - 1,
                  baked.probes.size());
    return baked;
}

// A probe's photograph: the six faces of its cube map, each kSkySide a side
// or as near as the file has, Direct3D's faces (+x, -x, +y, -y, +z, -z) in
// the level's own axes, z up (fitted to the .mtx files' coefficients,
// 2026-10-11).
struct Photograph
{
    std::uint32_t side = 0;
    std::vector<probes::Colour> texels; // face after face, each row by row from its top

    // The way a texel looks, of length 1.
    core::Point Direction(std::size_t index) const
    {
        const std::size_t face = index / (std::size_t{side} * side);
        const std::size_t within = index % (std::size_t{side} * side);
        const float u = (static_cast<float>(within % side) + 0.5F) / static_cast<float>(side) * 2 - 1; // to the right
        const float v = (static_cast<float>(within / side) + 0.5F) / static_cast<float>(side) * 2 - 1; // down
        const std::array<core::Point, 6> ways{
            {{1, -v, -u}, {-1, -v, u}, {u, 1, v}, {u, -1, -v}, {u, -v, 1}, {-u, -v, -1}}};
        const core::Point &way = ways.at(face);
        const float length = std::hypot(way[0], way[1], way[2]);
        return {way[0] / length, way[1] / length, way[2] / length};
    }

    // The texel that looks a way.
    std::size_t Toward(const core::Point &way) const
    {
        const float ax = std::abs(way[0]);
        const float ay = std::abs(way[1]);
        const float az = std::abs(way[2]);
        std::size_t face = 0;
        float u = 0;
        float v = 0;
        if (ax >= ay && ax >= az)
        {
            face = way[0] > 0 ? 0 : 1;
            u = (way[0] > 0 ? -way[2] : way[2]) / ax;
            v = -way[1] / ax;
        }
        else if (ay >= az)
        {
            face = way[1] > 0 ? 2 : 3;
            u = way[0] / ay;
            v = (way[1] > 0 ? way[2] : -way[2]) / ay;
        }
        else
        {
            face = way[2] > 0 ? 4 : 5;
            u = (way[2] > 0 ? way[0] : -way[0]) / az;
            v = -way[1] / az;
        }
        const auto cell = [&](float at) {
            return std::min<std::size_t>(
                static_cast<std::size_t>(std::clamp((at + 1) / 2, 0.0F, 1.0F) * static_cast<float>(side)), side - 1);
        };
        return (face * side + cell(v)) * side + cell(u);
    }
};

// A probe's photograph out of its file: a DDS cube map, each face with its
// smaller pictures after it. The face's picture of the size wanted is read
// by giving the decoder the file's header with that picture after it.
std::optional<Photograph> PhotographOf(std::span<const std::byte> file)
{
    constexpr std::size_t kHeader = 128;
    const auto word = [&](std::size_t at) {
        std::uint32_t value = 0;
        std::memcpy(&value, file.data() + at, sizeof value);
        return value;
    };
    if (file.size() < kHeader)
    {
        return std::nullopt;
    }
    const std::uint32_t side = word(16);
    const std::uint32_t pictures = std::max<std::uint32_t>(word(28), 1);
    const std::uint32_t format = word(84);
    constexpr std::uint32_t kDxt1 = 0x31545844;
    constexpr std::uint32_t kDxt3 = 0x33545844;
    constexpr std::uint32_t kDxt5 = 0x35545844;
    if (side == 0 || side != word(12) || side > 4096 || pictures > 16 ||
        (format != kDxt1 && format != kDxt3 && format != kDxt5))
    {
        return std::nullopt;
    }
    const std::size_t block = format == kDxt1 ? 8 : 16;
    const auto bytesOf = [&](std::uint32_t picture) {
        const std::size_t across = (std::max<std::uint32_t>(side >> picture, 1) + 3) / 4;
        return across * across * block;
    };
    std::uint32_t wanted = 0;
    while (wanted + 1 < pictures && (side >> wanted) > kSkySide)
    {
        ++wanted;
    }
    std::size_t before = 0;
    std::size_t face = 0;
    for (std::uint32_t picture = 0; picture < pictures; ++picture)
    {
        before += picture < wanted ? bytesOf(picture) : 0;
        face += bytesOf(picture);
    }
    if (file.size() < kHeader + 6 * face)
    {
        return std::nullopt;
    }
    Photograph photograph;
    photograph.side = std::max<std::uint32_t>(side >> wanted, 1);
    std::vector<std::byte> one(kHeader + bytesOf(wanted));
    std::memcpy(one.data(), file.data(), kHeader);
    std::memcpy(one.data() + 12, &photograph.side, sizeof photograph.side);
    std::memcpy(one.data() + 16, &photograph.side, sizeof photograph.side);
    for (std::size_t i = 0; i < 6; ++i)
    {
        std::memcpy(one.data() + kHeader, file.data() + kHeader + i * face + before, bytesOf(wanted));
        const std::optional<core::dds::Image> image = core::dds::Decode(one);
        if (!image || image->width != photograph.side || image->height != photograph.side)
        {
            return std::nullopt;
        }
        for (std::size_t texel = 0; texel < std::size_t{photograph.side} * photograph.side; ++texel)
        {
            photograph.texels.push_back({static_cast<float>(image->rgba[texel * 4]) / 255.0F,
                                         static_cast<float>(image->rgba[texel * 4 + 1]) / 255.0F,
                                         static_cast<float>(image->rgba[texel * 4 + 2]) / 255.0F});
        }
    }
    return photograph;
}

// What a lit surface of a level sends on, a colour for each: of its baked
// light, of the sun where the lightmap says the sun reaches it, and with
// neither, which is the area's ambient light on it. The surface's own colour
// is in all three, and the sun's in the second.
struct Sends
{
    probes::Colour ofBaked{kSendsOn, kSendsOn, kSendsOn};
    probes::Colour ofSun{};
    probes::Colour ofNothing{};
};

// The sums a fit of Sends is made from, for one of red, green and blue, over
// the surfaces seen: the products, two at a time, of the baked light, the
// sun's reach and 1 (`among`), each of them with what the photograph shows
// (`shown`), and the photograph's own square.
struct FitSums
{
    std::array<std::array<double, 3>, 3> among{};
    std::array<double, 3> shown{};
    double square = 0;

    void Add(double baked, double sun, double photograph)
    {
        const std::array<double, 3> values{baked, sun, 1.0};
        for (std::size_t i = 0; i < 3; ++i)
        {
            for (std::size_t j = 0; j < 3; ++j)
            {
                among.at(i).at(j) += values.at(i) * values.at(j);
            }
            shown.at(i) += values.at(i) * photograph;
        }
        square += photograph * photograph;
    }
};

// The shares that come nearest what the photograph shows, none less than
// nothing: least squares over the baked light, the sun's reach and 1, and
// where that makes a share negative, over the two or the one of them that
// do best with none negative.
std::array<double, 3> Fit(const FitSums &sums)
{
    std::array<double, 3> best{sums.among[0][0] > 0 ? sums.shown[0] / sums.among[0][0] : double{kSendsOn}, 0, 0};
    double least = std::numeric_limits<double>::infinity();
    for (unsigned chosen = 1; chosen < 8; ++chosen)
    {
        // The chosen shares' own equations, solved by elimination.
        std::array<std::size_t, 3> which{};
        std::size_t count = 0;
        for (std::size_t i = 0; i < 3; ++i)
        {
            if ((chosen >> i & 1U) != 0)
            {
                which.at(count++) = i;
            }
        }
        std::array<std::array<double, 4>, 3> rows{};
        for (std::size_t i = 0; i < count; ++i)
        {
            for (std::size_t j = 0; j < count; ++j)
            {
                rows.at(i).at(j) = sums.among.at(which.at(i)).at(which.at(j));
            }
            rows.at(i).at(3) = sums.shown.at(which.at(i));
        }
        bool solved = true;
        for (std::size_t i = 0; i < count && solved; ++i)
        {
            std::size_t pivot = i;
            for (std::size_t k = i + 1; k < count; ++k)
            {
                pivot = std::abs(rows.at(k).at(i)) > std::abs(rows.at(pivot).at(i)) ? k : pivot;
            }
            std::swap(rows.at(i), rows.at(pivot));
            solved = std::abs(rows.at(i).at(i)) > 1e-9 * std::max(sums.among.at(which.at(i)).at(which.at(i)), 1e-30);
            for (std::size_t k = 0; k < count && solved; ++k)
            {
                if (k != i)
                {
                    const double by = rows.at(k).at(i) / rows.at(i).at(i);
                    for (std::size_t j = 0; j < 4; ++j)
                    {
                        rows.at(k).at(j) -= by * rows.at(i).at(j);
                    }
                }
            }
        }
        std::array<double, 3> shares{};
        for (std::size_t i = 0; i < count && solved; ++i)
        {
            shares.at(which.at(i)) = rows.at(i).at(3) / rows.at(i).at(i);
            solved = shares.at(which.at(i)) >= 0;
        }
        if (!solved)
        {
            continue;
        }
        // What is left of the photograph's square with these shares.
        double left = sums.square;
        for (std::size_t i = 0; i < 3; ++i)
        {
            left -= 2 * shares.at(i) * sums.shown.at(i);
            for (std::size_t j = 0; j < 3; ++j)
            {
                left += shares.at(i) * shares.at(j) * sums.among.at(i).at(j);
            }
        }
        if (left < least)
        {
            least = left;
            best = shares;
        }
    }
    return best;
}

// One of a terrain level's own probes with what its photograph gives: the
// sky about the level, and the surfaces it shows for the fit.
struct Taken
{
    OwnProbe probe;
    Photograph sky; // the photograph with what is not sky in it taken out
};

// Makes a photograph the sky alone: a texel whose ray from the probe's
// place lands on the level shows the level there, which is somewhere else
// from another place, and is given the mean of the sky's texels as high up
// as it; the surfaces so found go to the fit's sums.
void KeepSky(Level &level, std::span<const Geometry *const> about, const OwnProbe &probe, Photograph &photograph,
             std::array<FitSums, 3> &sums, std::size_t &surfaces)
{
    constexpr std::size_t kBands = 8;
    std::array<probes::Colour, kBands> mean{};
    std::array<std::size_t, kBands> count{};
    std::vector<bool> landed(photograph.texels.size());
    const auto bandOf = [&](const core::Point &way) {
        return std::min<std::size_t>(static_cast<std::size_t>((way[2] + 1) / 2 * kBands), kBands - 1);
    };
    for (std::size_t texel = 0; texel < photograph.texels.size(); ++texel)
    {
        const core::Point way = photograph.Direction(texel);
        const probes::Colour &shown = photograph.texels[texel];
        if (const std::optional<Lit> lit = Look(level, about, probe.at, way))
        {
            landed[texel] = true;
            ++surfaces;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                sums.at(channel).Add(lit->baked.at(channel), lit->sun, shown.at(channel));
            }
            continue;
        }
        const std::size_t band = bandOf(way);
        ++count.at(band);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            mean.at(band).at(channel) += shown.at(channel);
        }
    }
    probes::Colour all{};
    std::size_t sky = 0;
    for (std::size_t band = 0; band < kBands; ++band)
    {
        sky += count.at(band);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            all.at(channel) += mean.at(band).at(channel);
            mean.at(band).at(channel) /= static_cast<float>(std::max<std::size_t>(count.at(band), 1));
        }
    }
    if (sky == 0)
    {
        return; // closed in on every side: the photograph stays what it is
    }
    for (std::size_t texel = 0; texel < photograph.texels.size(); ++texel)
    {
        if (landed[texel])
        {
            const std::size_t band = bandOf(photograph.Direction(texel));
            photograph.texels[texel] = count.at(band) != 0
                                           ? mean.at(band)
                                           : probes::Colour{all[0] / static_cast<float>(sky), all[1] / static_cast<float>(sky),
                                                            all[2] / static_cast<float>(sky)};
        }
    }
}

// The light at a place as its photograph would have it: along each ray what
// the surface it lands on sends, or the sky. Nothing for a place closed in.
std::optional<probes::Coefficients> Reckon(Level &level, std::span<const Geometry *const> about, const core::Point &place,
                                           std::span<const core::Point> rays, const Sends &sends, const Photograph &sky)
{
    probes::Coefficients light{};
    const float each = 4 * std::numbers::pi_v<float> / static_cast<float>(rays.size());
    std::size_t close = 0; // rays that land within kNearWall
    for (const core::Point &ray : rays)
    {
        probes::Colour colour{};
        if (const std::optional<Lit> lit = Look(level, about, place, ray))
        {
            close += lit->distance < kNearWall ? 1 : 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                colour.at(channel) = sends.ofBaked.at(channel) * lit->baked.at(channel) +
                                     sends.ofSun.at(channel) * lit->sun + sends.ofNothing.at(channel);
            }
        }
        else
        {
            colour = sky.texels[sky.Toward(ray)];
        }
        const std::array<float, 9> harmonics = probes::Harmonics(ray);
        for (std::size_t term = 0; term < light.size(); ++term)
        {
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                light.at(term).at(channel) += harmonics.at(term) * colour.at(channel) * each;
            }
        }
    }
    if (static_cast<float>(close) > kNearWallShare * static_cast<float>(rays.size()))
    {
        return std::nullopt;
    }
    return light;
}

float Distance(const core::Point &a, const core::Point &b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

// The tile a place of the level is in: the one whose middle is nearest,
// seen from above.
std::wstring TileAt(Level &level, const core::Point &place)
{
    std::wstring nearest;
    float least = std::numeric_limits<float>::infinity();
    for (const std::wstring &id : level.Rooms())
    {
        if (const level::Room *room = level.RoomOf(id))
        {
            const float distance = std::hypot(room->at[0] - place[0], room->at[1] - place[1]);
            if (distance < least)
            {
                least = distance;
                nearest = id;
            }
        }
    }
    return nearest;
}

// The places to stand on a tile: over a grid that is the whole level's, a
// metre above every surface of the tile that faces up and has free air over
// it. A room has one place to a point of its grid, on its lowest floor
// (LightProbes.h's FloorPlaces); a level of terrain has walks over walks,
// Orzammar's streets over its lava, and with the lowest alone a street had
// no probe at all (68 of the Commons' probes within 50 m were at the height
// its people walk at, 2026-10-11).
//
// A floor is a surface that faces up, by the order of its corners, and a
// patch of ground has its corners in the other order from a room's floor:
// Redcliffe's sixteen patches gave no place at all, and its 175 were on
// floors and roofs (the same day). So the ground's triangles are turned to
// face up, which the ground always does, and the rest are left as they are:
// a ceiling is no floor.
std::vector<core::Point> PlacesOn(std::span<const Geometry *const> about, float spacing)
{
    const Geometry &tile = *about.front();
    std::vector<core::Triangle> corners;
    corners.reserve(tile.lit.size());
    core::Point low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity()};
    core::Point high{-low[0], -low[1], -low[2]};
    for (const level::LitTriangle &triangle : tile.lit)
    {
        corners.push_back(triangle.corners);
        for (const core::Point *corner : {&triangle.corners.a, &triangle.corners.b, &triangle.corners.c})
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                low.at(axis) = std::min(low.at(axis), corner->at(axis));
                high.at(axis) = std::max(high.at(axis), corner->at(axis));
            }
        }
    }
    const auto upward = [](const core::Triangle &t) {
        const core::Point e1{t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]};
        const core::Point e2{t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
        const core::Point normal{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                 e1[0] * e2[1] - e1[1] * e2[0]};
        return normal[2] / std::max(std::hypot(normal[0], normal[1], normal[2]), 1e-30F);
    };
    for (const auto &[from, to] : tile.ground)
    {
        for (std::size_t i = from; i < to; ++i)
        {
            if (upward(corners[i]) < 0)
            {
                std::swap(corners[i].b, corners[i].c);
            }
        }
    }
    std::vector<core::Point> places;
    if (corners.empty())
    {
        return places;
    }
    const core::TriangleTree floors(corners);
    const core::Point down{0, 0, -1};
    const core::Point up{0, 0, 1};
    std::vector<float> stood;
    for (float x = (std::floor(low[0] / spacing) + 0.5F) * spacing; x < high[0]; x += spacing)
    {
        for (float y = (std::floor(low[1] / spacing) + 0.5F) * spacing; y < high[1]; y += spacing)
        {
            if (x < low[0] || y < low[1])
            {
                continue;
            }
            // Every surface under this point that faces up, from the top down.
            stood.clear();
            core::Point from{x, y, high[2] + 1};
            for (std::size_t layer = 0; layer < kLayersMost; ++layer)
            {
                const std::optional<core::RayHit> hit = floors.Cast(from, down);
                if (!hit)
                {
                    break;
                }
                from[2] -= hit->distance;
                if (upward(floors.At(hit->triangle)) > kFacesUp)
                {
                    stood.push_back(from[2]);
                }
            }
            std::ranges::sort(stood);
            float last = low[2] - 2 * kSameFloor;
            for (const float floor : stood)
            {
                if (floor - last < kSameFloor)
                {
                    continue;
                }
                last = floor;
                const core::Point feet{x, y, floor + 0.1F};
                const bool roofed = std::ranges::any_of(about, [&](const Geometry *geometry) {
                    return geometry->tree && geometry->tree->Cast(feet, up, 1e-3F, kHeadroom).has_value();
                });
                if (!roofed)
                {
                    places.push_back({x, y, floor + kStandsAt});
                }
            }
        }
    }
    return places;
}

// The probes of a level of terrain, a room for each of the level's own.
std::vector<Room> BakeTerrain(Level &level, std::span<const core::Point> rays)
{
    std::vector<Taken> taken;
    std::array<FitSums, 3> sums{};
    std::size_t surfaces = 0;
    for (OwnProbe &probe : level.ProbesOf(L""))
    {
        const std::optional<std::vector<std::byte>> file = level.Find(probe.cube);
        std::optional<Photograph> photograph = file ? PhotographOf(*file) : std::nullopt;
        if (!photograph)
        {
            spdlog::debug("light probes: {}: a probe with no photograph read ({})", Text(level.Name()),
                          Text(probe.cube));
            continue;
        }
        const std::vector<const Geometry *> about = About(level, TileAt(level, probe.at));
        KeepSky(level, about, probe, *photograph, sums, surfaces);
        taken.push_back({std::move(probe), std::move(*photograph)});
    }
    if (taken.empty())
    {
        spdlog::debug("light probes: {}: no probe of its own with a photograph", Text(level.Name()));
        return {};
    }
    Sends sends;
    if (surfaces >= kFitLeast)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            const std::array<double, 3> shares = Fit(sums.at(channel));
            const auto bounded = [](double share) { return static_cast<float>(std::min(share, double{kSendsMost})); };
            sends.ofBaked.at(channel) = bounded(shares[0]);
            sends.ofSun.at(channel) = bounded(shares[1]);
            sends.ofNothing.at(channel) = bounded(shares[2]);
        }
    }
    spdlog::info("light probes: {}: {} of its own, {} surfaces in their photographs; a surface sends on "
                 "({:.2f}, {:.2f}, {:.2f}) of its baked light, ({:.2f}, {:.2f}, {:.2f}) of the sun and "
                 "({:.3f}, {:.3f}, {:.3f}) with neither",
                 Text(level.Name()), taken.size(), surfaces, sends.ofBaked[0], sends.ofBaked[1], sends.ofBaked[2],
                 sends.ofSun[0], sends.ofSun[1], sends.ofSun[2], sends.ofNothing[0], sends.ofNothing[1],
                 sends.ofNothing[2]);
    std::vector<Room> rooms(taken.size());
    for (std::size_t i = 0; i < taken.size(); ++i)
    {
        rooms[i].key = taken[i].probe.at;
        rooms[i].own = taken[i].probe.light;
        // The check: the level's own probe reckoned again at its own place.
        const std::vector<const Geometry *> about = About(level, TileAt(level, taken[i].probe.at));
        if (const auto again = Reckon(level, about, taken[i].probe.at, rays, sends, taken[i].sky))
        {
            const probes::Irradiance ours = probes::Matrices(*again);
            double both = 0;
            double oursSquared = 0;
            double theirsSquared = 0;
            for (std::size_t at = 0; at < ours.size(); ++at)
            {
                both += double{ours.at(at)} * rooms[i].own.at(at);
                oursSquared += double{ours.at(at)} * ours.at(at);
                theirsSquared += double{rooms[i].own.at(at)} * rooms[i].own.at(at);
            }
            spdlog::info("light probes: {}: its own probe reckoned again is {:.3f} like it and {:.2f} as bright",
                         Text(level.Name()), both / std::sqrt(std::max(oursSquared * theirsSquared, 1e-30)),
                         std::sqrt(oursSquared / std::max(theirsSquared, 1e-30)));
        }
    }
    const auto nearestOwn = [&](const core::Point &place) {
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < taken.size(); ++i)
        {
            if (Distance(taken[i].probe.at, place) < Distance(taken[nearest].probe.at, place))
            {
                nearest = i;
            }
        }
        return nearest;
    };
    for (const std::wstring &id : level.Rooms())
    {
        if (const std::optional<Tile> tile = TileOf(id))
        {
            level.DropBefore(tile->x - kTilesAbout);
        }
        const std::vector<const Geometry *> about = About(level, id);
        if (about.front()->lit.empty())
        {
            continue;
        }
        const std::vector<core::Point> places = PlacesOn(about, kTerrainSpacing);
        std::size_t open = 0;
        for (const core::Point &place : places)
        {
            const std::size_t own = nearestOwn(place);
            if (const auto light = Reckon(level, about, place, rays, sends, taken[own].sky))
            {
                rooms[own].probes.push_back({place, probes::Matrices(*light)});
                ++open;
            }
        }
        spdlog::debug("light probes: tile {}: {} triangles, {} tiles about it, {} places, {} of them open",
                      Text(id), about.front()->lit.size(), about.size() - 1, places.size(), open);
    }
    return rooms;
}

// Lays a room's grid over its probes.
void Index(Room &room)
{
    room.starts.clear();
    room.order.clear();
    room.across = room.down = 0;
    if (room.probes.empty())
    {
        return;
    }
    float highX = room.lowX = room.probes.front().at[0];
    float highY = room.lowY = room.probes.front().at[1];
    for (const probes::Probe &probe : room.probes)
    {
        room.lowX = std::min(room.lowX, probe.at[0]);
        room.lowY = std::min(room.lowY, probe.at[1]);
        highX = std::max(highX, probe.at[0]);
        highY = std::max(highY, probe.at[1]);
    }
    room.across = static_cast<std::uint32_t>(std::min((highX - room.lowX) / kCell, 4000.0F)) + 1;
    room.down = static_cast<std::uint32_t>(std::min((highY - room.lowY) / kCell, 4000.0F)) + 1;
    const auto cellOf = [&](const probes::Probe &probe) {
        const auto x = std::min(static_cast<std::uint32_t>((probe.at[0] - room.lowX) / kCell), room.across - 1);
        const auto y = std::min(static_cast<std::uint32_t>((probe.at[1] - room.lowY) / kCell), room.down - 1);
        return std::size_t{y} * room.across + x;
    };
    room.starts.assign(std::size_t{room.across} * room.down + 1, 0);
    for (const probes::Probe &probe : room.probes)
    {
        ++room.starts[cellOf(probe) + 1];
    }
    for (std::size_t cell = 1; cell < room.starts.size(); ++cell)
    {
        room.starts[cell] += room.starts[cell - 1];
    }
    std::vector<std::uint32_t> next(room.starts.begin(), room.starts.end() - 1);
    room.order.resize(room.probes.size());
    for (std::uint32_t i = 0; i < room.probes.size(); ++i)
    {
        room.order[next[cellOf(room.probes[i])]++] = i;
    }
}

template <class T> void Put(std::vector<std::byte> &file, const T &value)
{
    const auto *bytes = reinterpret_cast<const std::byte *>(&value);
    file.insert(file.end(), bytes, bytes + sizeof value);
}

template <class T> bool Take(std::span<const std::byte> &file, T &value)
{
    if (file.size() < sizeof value)
    {
        return false;
    }
    std::memcpy(&value, file.data(), sizeof value);
    file = file.subspan(sizeof value);
    return true;
}

// The rooms of a table's file, or nothing for bytes that are not one of this
// version or stop short.
std::optional<std::vector<Room>> Read(std::span<const std::byte> file)
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t count = 0;
    if (!Take(file, magic) || !Take(file, version) || !Take(file, count) || magic != kTableMagic ||
        version != kTableVersion || count > 4096)
    {
        return std::nullopt;
    }
    std::vector<Room> rooms(count);
    for (Room &room : rooms)
    {
        std::uint32_t whole = 0;
        std::uint32_t probes = 0;
        if (!Take(file, room.key) || !Take(file, room.own) || !Take(file, whole) || !Take(file, probes) ||
            file.size() / sizeof(probes::Probe) < probes)
        {
            return std::nullopt;
        }
        room.whole = whole != 0;
        room.probes.resize(probes);
        for (probes::Probe &probe : room.probes)
        {
            Take(file, probe.at);
            Take(file, probe.light);
        }
        Index(room);
    }
    return rooms;
}

struct State
{
    std::filesystem::path kept;
    std::filesystem::path data;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::filesystem::path> wanted;
    std::set<std::wstring> had; // the levels whose table is had, or is being read or made
    std::shared_ptr<const Rooms> rooms = std::make_shared<Rooms>();
    std::optional<Archive> hierarchies;
    std::optional<Archive> meshes;
};

State &TheState()
{
    static State state;
    return state;
}

// Hands over more rooms: a new list made of the old and these, put in the
// old one's place. Only the baker's thread hands rooms over.
void Publish(State &state, std::vector<Room> more)
{
    std::shared_ptr<const Rooms> old;
    {
        const std::scoped_lock lock(state.lock);
        old = state.rooms;
    }
    auto next = std::make_shared<Rooms>(*old);
    for (Room &room : more)
    {
        next->push_back(std::make_shared<const Room>(std::move(room)));
    }
    const std::scoped_lock lock(state.lock);
    state.rooms = std::move(next);
}

// The rooms of a level, baked, or nothing if its archives cannot be read.
std::optional<std::vector<Room>> BakeRooms(State &state, const std::filesystem::path &levelRim)
{
    std::optional<Archive> rooms = Archive::Open(levelRim, true);
    if (!rooms)
    {
        spdlog::warn("light probes: {} could not be read", levelRim.string());
        return std::nullopt;
    }
    if (!state.hierarchies)
    {
        state.hierarchies = Archive::Open(state.data / "modelhierarchies.erf", true);
        state.meshes = Archive::Open(state.data / "modelmeshdata.erf", true);
    }
    if (!state.hierarchies || !state.meshes)
    {
        spdlog::warn("light probes: the models' archives could not be read in {}", state.data.string());
        return std::nullopt;
    }
    Level level(levelRim.parent_path(), levelRim.stem().wstring(), std::move(*rooms), &*state.hierarchies,
                &*state.meshes);
    const std::vector<core::Point> rays = probes::Directions(kRays);
    std::vector<Room> baked;
    if (level.Terrain())
    {
        baked = BakeTerrain(level, rays);
    }
    else
    {
        for (const std::wstring &id : level.Rooms())
        {
            if (std::optional<Room> room = BakeRoom(level, id, rays))
            {
                baked.push_back(std::move(*room));
            }
        }
    }
    std::erase_if(baked, [](const Room &room) { return room.probes.empty(); });
    for (Room &room : baked)
    {
        Index(room);
    }
    return baked;
}

std::size_t ProbesIn(const std::vector<Room> &rooms)
{
    std::size_t count = 0;
    for (const Room &room : rooms)
    {
        count += room.probes.size();
    }
    return count;
}

// A level's table: the one kept, or baked now and kept.
void BakeLevel(State &state, const std::filesystem::path &levelRim)
{
    const std::filesystem::path kept = state.kept / (levelRim.stem().wstring() + L".bin");
    {
        std::ifstream in(kept, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::optional<std::vector<Room>> rooms = Read(std::as_bytes(std::span(bytes.data(), bytes.size()))))
        {
            spdlog::info("light probes: {} kept, {} rooms, {} probes", levelRim.stem().string(), rooms->size(),
                         ProbesIn(*rooms));
            Publish(state, std::move(*rooms));
            return;
        }
    }
    const auto began = std::chrono::steady_clock::now();
    std::optional<std::vector<Room>> baked = BakeRooms(state, levelRim);
    if (!baked)
    {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(state.kept, error);
    const std::vector<std::byte> file = Write(*baked);
    std::ofstream out(kept, std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    spdlog::info("light probes: {} baked, {} rooms, {} probes, {} ms", levelRim.stem().string(), baked->size(),
                 ProbesIn(*baked), took.count());
    // The level whole, and not a room at a time: a figure's light is of one
    // kind across a level from the frame the level's probes come.
    Publish(state, std::move(*baked));
}

void Work()
{
    State &state = TheState();
    // Windows' background mode: the thread's processor time, its reading of
    // the disk and its memory all give way to everything else, the game's
    // own loading first of all.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    for (;;)
    {
        std::filesystem::path levelRim;
        {
            std::unique_lock lock(state.lock);
            state.wake.wait(lock, [&] { return !state.wanted.empty(); });
            levelRim = std::move(state.wanted.front());
            state.wanted.pop_front();
        }
        try
        {
            BakeLevel(state, levelRim);
        }
        catch (const std::exception &error)
        {
            spdlog::warn("light probes: {} not baked: {}", levelRim.stem().string(), error.what());
        }
    }
}

} // namespace

void Start(const std::filesystem::path &kept, const std::filesystem::path &game)
{
    State &state = TheState();
    state.kept = kept;
    state.data = game / "packages" / "core" / "data";
    std::thread(&Work).detach();
}

std::vector<Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game)
{
    State state;
    state.data = game / "packages" / "core" / "data";
    return BakeRooms(state, levelRim).value_or(std::vector<Room>{});
}

std::vector<std::byte> Write(const std::vector<Room> &rooms)
{
    std::vector<std::byte> file;
    Put(file, kTableMagic);
    Put(file, kTableVersion);
    Put(file, static_cast<std::uint32_t>(rooms.size()));
    for (const Room &room : rooms)
    {
        Put(file, room.key);
        Put(file, room.own);
        Put(file, std::uint32_t{room.whole ? 1U : 0U});
        Put(file, static_cast<std::uint32_t>(room.probes.size()));
        for (const probes::Probe &probe : room.probes)
        {
            Put(file, probe.at);
            Put(file, probe.light);
        }
    }
    return file;
}

void Want(const std::filesystem::path &levelRim)
{
    State &state = TheState();
    {
        const std::scoped_lock lock(state.lock);
        if (!state.had.insert(Lower(levelRim.stem().wstring())).second)
        {
            return;
        }
        state.wanted.push_back(levelRim);
    }
    spdlog::info("light probes: {} wanted", levelRim.stem().string());
    state.wake.notify_one();
}

std::shared_ptr<const Rooms> AllRooms()
{
    State &state = TheState();
    const std::scoped_lock lock(state.lock);
    return state.rooms;
}

void Nearest(const Room &room, const float *place, std::vector<probes::Near> &out)
{
    if (room.across == 0 || room.down == 0)
    {
        return;
    }
    const std::size_t first = out.size();
    const auto cellOf = [](float from, float low, std::uint32_t cells) {
        return static_cast<int>(std::clamp((from - low) / kCell, 0.0F, static_cast<float>(cells - 1)));
    };
    const int x = cellOf(place[0], room.lowX, room.across);
    const int y = cellOf(place[1], room.lowY, room.down);
    thread_local std::vector<float> scores;
    // A square of cells about the place's own, a ring wider each time, until
    // the nearest few's furthest is nearer than anything outside the square.
    for (int ring = 1;; ++ring)
    {
        out.resize(first);
        const int left = x - ring;
        const int right = x + ring;
        const int top = y - ring;
        const int bottom = y + ring;
        for (int cy = std::max(top, 0); cy <= std::min(bottom, static_cast<int>(room.down) - 1); ++cy)
        {
            const std::size_t row = static_cast<std::size_t>(cy) * room.across;
            const std::uint32_t from = room.starts[row + static_cast<std::size_t>(std::max(left, 0))];
            const std::uint32_t to =
                room.starts[row + static_cast<std::size_t>(std::min(right, static_cast<int>(room.across) - 1)) + 1];
            for (std::uint32_t at = from; at < to; ++at)
            {
                const probes::Probe &probe = room.probes[room.order[at]];
                const float higher = std::max(std::abs(probe.at[2] - place[2]) - kSameStorey, 0.0F);
                const float distance =
                    std::hypot(probe.at[0] - place[0], probe.at[1] - place[1], kHeightCounts * higher);
                out.push_back({1.0F / std::max(distance, 1e-3F), probe.light.data()});
            }
        }
        // How far from the place anything outside the square is, at the least.
        float sure = std::numeric_limits<float>::infinity();
        if (left > 0)
        {
            sure = std::min(sure, place[0] - (room.lowX + static_cast<float>(left) * kCell));
        }
        if (right < static_cast<int>(room.across) - 1)
        {
            sure = std::min(sure, room.lowX + static_cast<float>(right + 1) * kCell - place[0]);
        }
        if (top > 0)
        {
            sure = std::min(sure, place[1] - (room.lowY + static_cast<float>(top) * kCell));
        }
        if (bottom < static_cast<int>(room.down) - 1)
        {
            sure = std::min(sure, room.lowY + static_cast<float>(bottom + 1) * kCell - place[1]);
        }
        if (std::isinf(sure))
        {
            return; // the square is the whole grid
        }
        if (out.size() - first > probes::kMixed && sure > 0)
        {
            scores.clear();
            for (std::size_t at = first; at < out.size(); ++at)
            {
                scores.push_back(out[at].score);
            }
            std::ranges::nth_element(scores, scores.begin() + static_cast<std::ptrdiff_t>(probes::kMixed), std::greater{});
            if (scores[probes::kMixed] >= 1.0F / sure)
            {
                return;
            }
        }
    }
}

} // namespace probebaker
