// The baker's thread and its reading of the game's archives. What a probe is
// and how one is baked is dao/core/LightProbes.h's; how a level's geometry is
// read is dao/core/Level.h's. Here: which files, in what order, and how much
// is held at a time, which matters in a game of 32 bits.
//
// A level is its folder under packages\core\env: <level>.rim has every
// room's definition (<level>_<room>.rml), and each room's own archives
// (<level>_<room>.rim and others of that stem) its layout
// (<level>_<room>_ll.mmh, where its light probe is) and its lightmap
// atlases. The models' trees and meshes are the whole game's, in
// packages\core\data's modelhierarchies.erf and modelmeshdata.erf.
//
// A room's probes are baked against its own triangles and those of the
// rooms its definition says are seen from it. Triangles and lightmaps are
// kept for the rooms of the level as they are first wanted and dropped when
// the level is done, and the level's probes are handed over together then; a lightmap is kept at half its size a side, which is
// plenty for the light of a place and a quarter of the memory.
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
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
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

constexpr float kSpacing = 5.0F;       // metres between a room's probes
constexpr std::size_t kRays = 384;     // from each probe
constexpr std::uint32_t kAtlasSide = 512; // a lightmap is kept no larger a side

std::wstring Lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t letter) { return static_cast<wchar_t>(std::towlower(letter)); });
    return text;
}

// One of the game's archives, open: its directory, and any file of it by name.
class Archive
{
  public:
    static std::optional<Archive> Open(const std::filesystem::path &path)
    {
        Archive archive;
        archive.file_.open(path, std::ios::binary);
        std::vector<std::byte> header(core::erf::kHeaderSize);
        if (!archive.file_.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size())))
        {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> count = core::erf::EntryCount(header);
        if (!count)
        {
            return std::nullopt;
        }
        header.resize(core::erf::kHeaderSize + std::size_t{*count} * core::erf::kEntrySize);
        archive.file_.read(reinterpret_cast<char *>(header.data() + core::erf::kHeaderSize),
                           static_cast<std::streamsize>(header.size() - core::erf::kHeaderSize));
        for (core::erf::Entry &entry : core::erf::Entries(std::span(header).first(
                 core::erf::kHeaderSize + static_cast<std::size_t>(std::max<std::streamsize>(archive.file_.gcount(), 0)))))
        {
            archive.entries_.emplace(Lower(entry.name), entry);
        }
        archive.file_.clear();
        return archive;
    }

    bool Has(const std::wstring &name) const
    {
        return entries_.contains(Lower(name));
    }

    std::optional<std::vector<std::byte>> Read(const std::wstring &name)
    {
        const auto found = entries_.find(Lower(name));
        if (found == entries_.end())
        {
            return std::nullopt;
        }
        std::vector<std::byte> bytes(found->second.size);
        file_.clear();
        file_.seekg(found->second.offset);
        if (!file_.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
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
    std::ifstream file_;
    std::map<std::wstring, core::erf::Entry> entries_;
};

// A lightmap as three bytes a pixel.
struct Atlas
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

Atlas AtlasOf(const core::dds::Image &image)
{
    // Halved until it is small enough a side, each pixel the mean of four.
    Atlas atlas{image.width, image.height, {}};
    atlas.rgb.resize(std::size_t{image.width} * image.height * 3);
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i)
    {
        std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(i * 4), 3,
                    atlas.rgb.begin() + static_cast<std::ptrdiff_t>(i * 3));
    }
    while ((atlas.width > kAtlasSide || atlas.height > kAtlasSide) && atlas.width % 2 == 0 && atlas.height % 2 == 0)
    {
        Atlas half{atlas.width / 2, atlas.height / 2, {}};
        half.rgb.resize(std::size_t{half.width} * half.height * 3);
        for (std::size_t y = 0; y < half.height; ++y)
        {
            for (std::size_t x = 0; x < half.width; ++x)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const auto at = [&](std::size_t px, std::size_t py) {
                        return unsigned{atlas.rgb[(py * atlas.width + px) * 3 + channel]};
                    };
                    half.rgb[(y * half.width + x) * 3 + channel] = static_cast<std::uint8_t>(
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
};

// A level being baked: its archives, and what has been read of it so far.
class Level
{
  public:
    Level(std::filesystem::path folder, std::wstring name, Archive rooms, Archive *hierarchies, Archive *meshes)
        : folder_(std::move(folder)), name_(std::move(name)), rooms_(std::move(rooms)), hierarchies_(hierarchies),
          meshes_(meshes)
    {
        // Every archive of the level's folder but the rooms' definitions: a
        // room's layout and its lightmaps are in one or another of them.
        std::error_code error;
        for (const auto &entry : std::filesystem::directory_iterator(folder_, error))
        {
            if (Lower(entry.path().extension().wstring()) == L".rim" &&
                Lower(entry.path().stem().wstring()) != Lower(name_))
            {
                if (std::optional<Archive> archive = Archive::Open(entry.path()))
                {
                    others_.push_back(std::move(*archive));
                }
            }
        }
    }

    // The numbers of the level's rooms.
    std::vector<std::uint32_t> RoomNumbers() const
    {
        std::vector<std::uint32_t> numbers;
        const std::wstring stem = Lower(name_) + L"_";
        for (const auto &[file, entry] : rooms_.Entries())
        {
            if (file.starts_with(stem) && file.ends_with(L".rml"))
            {
                const std::wstring number = file.substr(stem.size(), file.size() - stem.size() - 4);
                if (!number.empty() && std::ranges::all_of(number, [](wchar_t c) { return std::iswdigit(c) != 0; }))
                {
                    numbers.push_back(static_cast<std::uint32_t>(std::stoul(number)));
                }
            }
        }
        return numbers;
    }

    const level::Room *RoomOf(std::uint32_t number)
    {
        if (const auto found = definitions_.find(number); found != definitions_.end())
        {
            return found->second ? &*found->second : nullptr;
        }
        std::optional<level::Room> room;
        if (const auto bytes = rooms_.Read(name_ + L"_" + std::to_wstring(number) + L".rml"))
        {
            if (const auto file = core::gff4::File::Parse(*bytes))
            {
                room = level::RoomOf(*file);
            }
        }
        const auto kept = definitions_.emplace(number, std::move(room)).first;
        return kept->second ? &*kept->second : nullptr;
    }

    // Where the level's own probe of a room is, in the level's space.
    std::optional<core::Point> ProbeOf(std::uint32_t number)
    {
        const level::Room *room = RoomOf(number);
        const std::wstring layout = name_ + L"_" + std::to_wstring(number) + L"_ll";
        const std::optional<std::vector<std::byte>> bytes = FromOthers(layout + L".mmh");
        if (room == nullptr || !bytes)
        {
            return std::nullopt;
        }
        const auto placed = std::ranges::find_if(room->models, [&](const level::Placed &model) {
            return Lower(model.file) == Lower(layout);
        });
        const auto file = core::gff4::File::Parse(*bytes);
        const std::optional<core::Point> at = file ? level::ProbeOf(*file) : std::nullopt;
        if (placed == room->models.end() || !at)
        {
            return std::nullopt;
        }
        return level::InLevel(*room, *placed, *at);
    }

    // The room's lightmapped triangles and a tree over them, made once.
    const Geometry &GeometryOf(std::uint32_t number)
    {
        if (const auto found = geometry_.find(number); found != geometry_.end())
        {
            return found->second;
        }
        Geometry &geometry = geometry_[number];
        std::vector<level::LitTriangle> &out = geometry.lit;
        const level::Room *room = RoomOf(number);
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
                level::AddTriangles(*room, placed, parts->model, parts->chunks,
                                    [&](const std::wstring &atlas) { return AtlasNumber(atlas); }, out);
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

    // The lightmap's colour at a place of an atlas, each 0 to 1.
    probes::Colour Colour(std::uint32_t atlas, const level::Uv &uv)
    {
        const Atlas &image = AtlasOf(atlas);
        if (image.rgb.empty())
        {
            return {};
        }
        const auto x = static_cast<std::size_t>(std::clamp(uv[0] * static_cast<float>(image.width), 0.0F,
                                                           static_cast<float>(image.width - 1)));
        const auto y = static_cast<std::size_t>(std::clamp(uv[1] * static_cast<float>(image.height), 0.0F,
                                                           static_cast<float>(image.height - 1)));
        const std::uint8_t *pixel = &image.rgb[(y * image.width + x) * 3];
        return {static_cast<float>(pixel[0]) / 255.0F, static_cast<float>(pixel[1]) / 255.0F,
                static_cast<float>(pixel[2]) / 255.0F};
    }

  private:
    std::optional<std::vector<std::byte>> FromOthers(const std::wstring &file)
    {
        for (Archive &archive : others_)
        {
            if (archive.Has(file))
            {
                return archive.Read(file);
            }
        }
        return std::nullopt;
    }

    const ModelParts *ModelOf(const std::wstring &name)
    {
        const std::wstring key = Lower(name);
        if (const auto found = models_.find(key); found != models_.end())
        {
            return found->second ? &*found->second : nullptr;
        }
        std::optional<ModelParts> parts;
        if (const auto tree = hierarchies_->Read(key + L".mmh"))
        {
            if (const auto file = core::gff4::File::Parse(*tree))
            {
                ModelParts read;
                read.model = level::ModelOf(*file);
                const std::wstring meshFile = read.model.meshFile.empty() ? key + L".msh" : read.model.meshFile;
                if (const auto mesh = meshes_->Read(meshFile.ends_with(L".msh") ? meshFile : meshFile + L".msh"))
                {
                    if (const auto meshParsed = core::gff4::File::Parse(*mesh))
                    {
                        read.chunks = level::Meshes(*meshParsed);
                        parts = std::move(read);
                    }
                }
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
            if (const auto bytes = FromOthers(atlasNames_[number]))
            {
                if (const std::optional<core::dds::Image> image = core::dds::Decode(*bytes))
                {
                    atlas = probebaker::AtlasOf(*image);
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
    std::map<std::uint32_t, std::optional<level::Room>> definitions_;
    std::map<std::uint32_t, Geometry> geometry_;
    std::unordered_map<std::wstring, std::optional<ModelParts>> models_;
    std::unordered_map<std::wstring, std::uint32_t> atlasNumbers_;
    std::vector<std::wstring> atlasNames_;
    std::map<std::uint32_t, Atlas> atlases_;
};

// The probes of one room: baked against its own triangles and those of the
// rooms seen from it.
std::optional<probes::Room> BakeRoom(Level &level, std::uint32_t number, std::span<const core::Point> rays)
{
    const std::optional<core::Point> key = level.ProbeOf(number);
    const level::Room *room = level.RoomOf(number);
    if (!key || room == nullptr)
    {
        spdlog::debug("light probes: room {}: {}", number, room == nullptr ? "no definition read" : "no probe of its own");
        return std::nullopt;
    }
    const Geometry &own = level.GeometryOf(number);
    if (own.lit.empty())
    {
        spdlog::debug("light probes: room {}: no lightmapped triangles of {} models", number, room->models.size());
        return std::nullopt;
    }
    // The room and the rooms seen from it, each with its own tree: a ray is
    // cast against each and lands on the nearest.
    std::vector<const Geometry *> about{&own};
    std::size_t triangles = own.lit.size();
    for (const std::uint32_t seen : room->sees)
    {
        if (const Geometry &other = level.GeometryOf(seen); seen != number && !other.lit.empty())
        {
            about.push_back(&other);
            triangles += other.lit.size();
        }
    }
    probes::Room baked;
    baked.key = *key;
    for (const core::Point &place : probes::FloorPlaces(*own.tree, kSpacing))
    {
        const std::optional<probes::Coefficients> light =
            probes::Bake(rays, [&](const core::Point &ray) -> std::optional<probes::Landed> {
                const Geometry *nearest = nullptr;
                core::RayHit hit;
                float most = std::numeric_limits<float>::infinity();
                for (const Geometry *geometry : about)
                {
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
                return probes::Landed{
                    level.Colour(triangle.atlas,
                                 {w * triangle.uv[0][0] + hit.u * triangle.uv[1][0] + hit.v * triangle.uv[2][0],
                                  w * triangle.uv[0][1] + hit.u * triangle.uv[1][1] + hit.v * triangle.uv[2][1]}),
                    hit.distance};
            });
        if (light)
        {
            baked.probes.push_back({place, probes::Matrices(*light)});
        }
    }
    spdlog::debug("light probes: room {}: key {:.2f} {:.2f} {:.2f}, {} triangles of its own and {} about it, {} probes",
                  number, baked.key[0], baked.key[1], baked.key[2], own.lit.size(), triangles, baked.probes.size());
    return baked;
}

struct State
{
    std::filesystem::path kept;
    std::filesystem::path data;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::filesystem::path> wanted;
    std::set<std::wstring> had; // the levels there is a table for, or one being made
    std::shared_ptr<const std::vector<probes::Room>> rooms = std::make_shared<std::vector<probes::Room>>();
    std::optional<Archive> hierarchies;
    std::optional<Archive> meshes;
};

State &TheState()
{
    static State state;
    return state;
}

// Hands over more rooms: a new list made of the old and these, put in the
// old one's place. The list is made with nothing held, so whoever asks for
// the rooms meanwhile waits for no copying; only this thread and Start hand
// rooms over, and never both at once.
void Publish(State &state, const std::vector<probes::Room> &more)
{
    std::shared_ptr<const std::vector<probes::Room>> old;
    {
        const std::scoped_lock lock(state.lock);
        old = state.rooms;
    }
    auto next = std::make_shared<std::vector<probes::Room>>(*old);
    next->insert(next->end(), more.begin(), more.end());
    const std::scoped_lock lock(state.lock);
    state.rooms = std::move(next);
}

// The rooms of a level, baked, or nothing if its archives cannot be read.
std::optional<std::vector<probes::Room>> BakeRooms(State &state, const std::filesystem::path &levelRim)
{
    std::optional<Archive> rooms = Archive::Open(levelRim);
    if (!rooms)
    {
        spdlog::warn("light probes: {} could not be read", levelRim.string());
        return std::nullopt;
    }
    if (!state.hierarchies)
    {
        state.hierarchies = Archive::Open(state.data / "modelhierarchies.erf");
        state.meshes = Archive::Open(state.data / "modelmeshdata.erf");
    }
    if (!state.hierarchies || !state.meshes)
    {
        spdlog::warn("light probes: the models' archives could not be read in {}", state.data.string());
        return std::nullopt;
    }
    Level level(levelRim.parent_path(), levelRim.stem().wstring(), std::move(*rooms), &*state.hierarchies,
                &*state.meshes);
    const std::vector<core::Point> rays = probes::Directions(kRays);
    std::vector<probes::Room> baked;
    for (const std::uint32_t number : level.RoomNumbers())
    {
        if (std::optional<probes::Room> room = BakeRoom(level, number, rays); room && !room->probes.empty())
        {
            baked.push_back(std::move(*room));
        }
    }
    return baked;
}

void BakeLevel(State &state, const std::filesystem::path &levelRim)
{
    const auto began = std::chrono::steady_clock::now();
    const std::optional<std::vector<probes::Room>> baked = BakeRooms(state, levelRim);
    if (!baked)
    {
        return;
    }
    // The level whole, and not a room at a time: a figure's light is of one
    // kind across a level from the frame the level's probes come.
    Publish(state, *baked);
    std::error_code error;
    std::filesystem::create_directories(state.kept, error);
    const std::vector<std::byte> file = probes::Write(*baked);
    std::ofstream out(state.kept / (levelRim.stem().wstring() + L".bin"), std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    std::size_t count = 0;
    for (const probes::Room &room : *baked)
    {
        count += room.probes.size();
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    spdlog::info("light probes: {} baked, {} rooms, {} probes, {} ms", levelRim.stem().string(), baked->size(), count,
                 took.count());
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
    std::vector<probes::Room> read;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(kept, error))
    {
        if (entry.path().extension() != ".bin")
        {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::optional<std::vector<probes::Room>> rooms =
                probes::Read(std::as_bytes(std::span(bytes.data(), bytes.size()))))
        {
            state.had.insert(Lower(entry.path().stem().wstring()));
            read.insert(read.end(), rooms->begin(), rooms->end());
        }
    }
    spdlog::info("light probes: {} levels' kept, {} rooms", state.had.size(), read.size());
    Publish(state, read);
    std::thread(&Work).detach();
}

std::vector<probes::Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game)
{
    State state;
    state.data = game / "packages" / "core" / "data";
    return BakeRooms(state, levelRim).value_or(std::vector<probes::Room>{});
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
    spdlog::info("light probes: {} to bake", levelRim.stem().string());
    state.wake.notify_one();
}

std::shared_ptr<const std::vector<probes::Room>> Rooms()
{
    State &state = TheState();
    const std::scoped_lock lock(state.lock);
    return state.rooms;
}

} // namespace probebaker
