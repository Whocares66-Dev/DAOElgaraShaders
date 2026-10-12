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

#include "LevelFiles.h"

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
using namespace levelfiles;

constexpr float kSpacing = 5.0F;        // metres between a room's probes
constexpr float kTerrainSpacing = 8.0F; // and between a terrain tile's, where light changes over more ground
constexpr std::size_t kRays = 384;      // from each probe
constexpr std::uint32_t kSkySide = 32;  // a face of a probe's photograph is read this large
constexpr float kCell = 10.0F;          // metres a side: a cell of a room's grid of probes
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
constexpr float kSendsMost = 4.0F;    // the most a fitted share may be
constexpr std::size_t kFitLeast = 64; // surfaces seen in the photograph, under which nothing is fitted

constexpr std::uint32_t kTableMagic = 0x50474C45; // "ELGP"
constexpr std::uint32_t kTableVersion = 3;        // 2: a room's `seen`; 3: the stamp of the lightmaps baked from

// The light at a place as a room's probe has it: all the light a surface of
// the place receives, by the lightmaps its rays land on. Nothing for a place
// closed in.
// @param seen what the rays land on is added to, for a room's `seen`
std::optional<probes::Coefficients> BakeAt(Level &level, std::span<const Geometry *const> about,
                                           const core::Point &place, std::span<const core::Point> rays, SeenSum &seen)
{
    return probes::Bake(rays, [&](const core::Point &ray) -> std::optional<probes::Landed> {
        const std::optional<Lit> lit = Look(level, about, place, ray);
        if (lit)
        {
            seen.Add(*lit);
        }
        return lit ? std::optional<probes::Landed>(probes::Landed{lit->baked, lit->distance}) : std::nullopt;
    });
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
    SeenSum seen;
    for (const core::Point &place : probes::FloorPlaces(*about.front()->tree, kSpacing))
    {
        if (const std::optional<probes::Coefficients> light = BakeAt(level, about, place, rays, seen))
        {
            baked.probes.push_back({place, probes::Matrices(*light)});
        }
    }
    baked.seen = seen.Mean();
    spdlog::debug(
        "light probes: room {}: key {:.2f} {:.2f} {:.2f}, {} triangles of its own, {} rooms about it, {} probes",
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
        const std::size_t column = within % side;
        const std::size_t row = within / side;
        const float u = ((static_cast<float>(column) + 0.5F) / static_cast<float>(side) * 2) - 1; // to the right
        const float v = ((static_cast<float>(row) + 0.5F) / static_cast<float>(side) * 2) - 1;    // down
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
        return (((face * side) + cell(v)) * side) + cell(u);
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
    if (file.size() < kHeader + (6 * face))
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
        std::memcpy(one.data() + kHeader, file.data() + kHeader + (i * face) + before, bytesOf(wanted));
        const std::optional<core::dds::Image> image = core::dds::Decode(one);
        if (!image || image->width != photograph.side || image->height != photograph.side)
        {
            return std::nullopt;
        }
        for (std::size_t texel = 0; texel < std::size_t{photograph.side} * photograph.side; ++texel)
        {
            photograph.texels.push_back({static_cast<float>(image->rgba[texel * 4]) / 255.0F,
                                         static_cast<float>(image->rgba[(texel * 4) + 1]) / 255.0F,
                                         static_cast<float>(image->rgba[(texel * 4) + 2]) / 255.0F});
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

// The equations of those of the three shares that are chosen, a row for each:
// what multiplies each of them, and in the last column what the photograph
// shows.
struct Equations
{
    std::array<std::size_t, 3> which{}; // the shares chosen, of the baked light, the sun's reach and 1
    std::size_t count = 0;              // and how many they are
    std::array<std::array<double, 4>, 3> rows{};
};

// @param chosen a bit for each of the three shares
Equations EquationsOf(const FitSums &sums, unsigned chosen)
{
    Equations equations;
    for (std::size_t i = 0; i < 3; ++i)
    {
        if ((chosen >> i & 1U) != 0)
        {
            equations.which.at(equations.count++) = i;
        }
    }
    for (std::size_t i = 0; i < equations.count; ++i)
    {
        for (std::size_t j = 0; j < equations.count; ++j)
        {
            equations.rows.at(i).at(j) = sums.among.at(equations.which.at(i)).at(equations.which.at(j));
        }
        equations.rows.at(i).at(3) = sums.shown.at(equations.which.at(i));
    }
    return equations;
}

// Takes the share of one row out of every other row, the row with the most
// of it brought to its place first.
// @return false where no row has enough of it to divide by
bool Eliminate(Equations &equations, const FitSums &sums, std::size_t i)
{
    auto &rows = equations.rows;
    std::size_t pivot = i;
    for (std::size_t k = i + 1; k < equations.count; ++k)
    {
        pivot = std::abs(rows.at(k).at(i)) > std::abs(rows.at(pivot).at(i)) ? k : pivot;
    }
    std::swap(rows.at(i), rows.at(pivot));
    const std::size_t share = equations.which.at(i);
    const bool enough = std::abs(rows.at(i).at(i)) > 1e-9 * std::max(sums.among.at(share).at(share), 1e-30);
    if (!enough)
    {
        return false;
    }
    for (std::size_t k = 0; k < equations.count; ++k)
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
    return true;
}

// The shares that come nearest what the photograph shows with only those
// chosen let be more than nothing: their own equations, solved by
// elimination. Nothing where the equations do not say, or say a share less
// than nothing.
std::optional<std::array<double, 3>> SharesOf(const FitSums &sums, unsigned chosen)
{
    Equations equations = EquationsOf(sums, chosen);
    for (std::size_t i = 0; i < equations.count; ++i)
    {
        if (!Eliminate(equations, sums, i))
        {
            return std::nullopt;
        }
    }
    std::array<double, 3> shares{};
    for (std::size_t i = 0; i < equations.count; ++i)
    {
        const double share = equations.rows.at(i).at(3) / equations.rows.at(i).at(i);
        const bool some = share >= 0;
        if (!some)
        {
            return std::nullopt;
        }
        shares.at(equations.which.at(i)) = share;
    }
    return shares;
}

// What is left of the photograph's square with these shares: the less, the
// nearer they come to what it shows.
double LeftWith(const FitSums &sums, const std::array<double, 3> &shares)
{
    double left = sums.square;
    for (std::size_t i = 0; i < 3; ++i)
    {
        left -= 2 * shares.at(i) * sums.shown.at(i);
        for (std::size_t j = 0; j < 3; ++j)
        {
            left += shares.at(i) * shares.at(j) * sums.among.at(i).at(j);
        }
    }
    return left;
}

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
        const std::optional<std::array<double, 3>> shares = SharesOf(sums, chosen);
        if (!shares)
        {
            continue;
        }
        const double left = LeftWith(sums, *shares);
        if (left < least)
        {
            least = left;
            best = *shares;
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
            photograph.texels[texel] =
                count.at(band) != 0 ? mean.at(band)
                                    : probes::Colour{all[0] / static_cast<float>(sky), all[1] / static_cast<float>(sky),
                                                     all[2] / static_cast<float>(sky)};
        }
    }
}

// The light at a place as its photograph would have it: along each ray what
// the surface it lands on sends, or the sky. Nothing for a place closed in.
std::optional<probes::Coefficients> Reckon(Level &level, std::span<const Geometry *const> about,
                                           const core::Point &place, std::span<const core::Point> rays,
                                           const Sends &sends, const Photograph &sky, SeenSum *seen = nullptr)
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
            if (seen != nullptr)
            {
                seen->Add(*lit);
            }
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                colour.at(channel) = (sends.ofBaked.at(channel) * lit->baked.at(channel)) +
                                     (sends.ofSun.at(channel) * lit->sun) + sends.ofNothing.at(channel);
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

// How far up a triangle faces, by the order of its corners: 1 for one that
// lies flat with its face up, -1 for one with its face down.
float Upward(const core::Triangle &t)
{
    const core::Point e1{t.b[0] - t.a[0], t.b[1] - t.a[1], t.b[2] - t.a[2]};
    const core::Point e2{t.c[0] - t.a[0], t.c[1] - t.a[1], t.c[2] - t.a[2]};
    const core::Point normal{(e1[1] * e2[2]) - (e1[2] * e2[1]), (e1[2] * e2[0]) - (e1[0] * e2[2]),
                             (e1[0] * e2[1]) - (e1[1] * e2[0])};
    return normal[2] / std::max(std::hypot(normal[0], normal[1], normal[2]), 1e-30F);
}

// A tile's triangles for its floors to be found among: the ground's turned
// to face up, which the ground always does, and the rest left as they are
// (PlacesOn has why).
std::vector<core::Triangle> TurnedUp(const Geometry &tile)
{
    std::vector<core::Triangle> corners;
    corners.reserve(tile.lit.size());
    for (const level::LitTriangle &triangle : tile.lit)
    {
        corners.push_back(triangle.corners);
    }
    for (const auto &[from, to] : tile.ground)
    {
        for (std::size_t i = from; i < to; ++i)
        {
            if (Upward(corners[i]) < 0)
            {
                std::swap(corners[i].b, corners[i].c);
            }
        }
    }
    return corners;
}

// The box about some triangles.
struct Bounds
{
    core::Point low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity()};
    core::Point high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};
};

Bounds BoundsOf(std::span<const core::Triangle> triangles)
{
    Bounds bounds;
    for (const core::Triangle &triangle : triangles)
    {
        for (const core::Point *corner : {&triangle.a, &triangle.b, &triangle.c})
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                bounds.low.at(axis) = std::min(bounds.low.at(axis), corner->at(axis));
                bounds.high.at(axis) = std::max(bounds.high.at(axis), corner->at(axis));
            }
        }
    }
    return bounds;
}

// The lines of the level's grid along one way that lie from `low` up to
// `high`: the grid is the whole level's, its lines `spacing` apart, the
// first of them half a spacing from nothing. Counted in whole steps from the
// first line in reach, and not by adding the spacing to a float time after
// time (clang-tidy's bugprone-float-loop-counter); the two say the same for
// a spacing whose multiples a float holds whole, as the 4 and 8 metres asked
// for are.
std::vector<float> GridLines(float low, float high, float spacing)
{
    const float first = (std::floor(low / spacing) + 0.5F) * spacing;
    std::vector<float> lines;
    for (int step = 0;; ++step)
    {
        const float at = first + (static_cast<float>(step) * spacing);
        const bool within = at < high;
        if (!within)
        {
            break;
        }
        if (at >= low)
        {
            lines.push_back(at);
        }
    }
    return lines;
}

// The heights of the surfaces under a point of the grid that face up, from
// the lowest: every one of them a ray let down from over the tile goes
// through.
// @param top the height of the tile's highest corner
// @param stood the heights, in a list that is used again for the next point
void FloorsUnder(const core::TriangleTree &floors, float x, float y, float top, std::vector<float> &stood)
{
    const core::Point down{0, 0, -1};
    stood.clear();
    core::Point from{x, y, top + 1};
    for (std::size_t layer = 0; layer < kLayersMost; ++layer)
    {
        const std::optional<core::RayHit> hit = floors.Cast(from, down);
        if (!hit)
        {
            break;
        }
        from[2] -= hit->distance;
        if (Upward(floors.At(hit->triangle)) > kFacesUp)
        {
            stood.push_back(from[2]);
        }
    }
    std::ranges::sort(stood);
}

} // namespace

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
    const std::vector<core::Triangle> corners = TurnedUp(*about.front());
    std::vector<core::Point> places;
    if (corners.empty())
    {
        return places;
    }
    const Bounds bounds = BoundsOf(corners);
    const core::TriangleTree floors(corners);
    const core::Point up{0, 0, 1};
    const std::vector<float> ys = GridLines(bounds.low[1], bounds.high[1], spacing);
    std::vector<float> stood;
    for (const float x : GridLines(bounds.low[0], bounds.high[0], spacing))
    {
        for (const float y : ys)
        {
            // Every surface under this point that faces up, from the top down.
            FloorsUnder(floors, x, y, bounds.high[2], stood);
            float last = bounds.low[2] - (2 * kSameFloor);
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

namespace
{

// Where a probe of a level's own is, and one taken with its photograph.
const core::Point &PlaceOf(const OwnProbe &probe)
{
    return probe.at;
}
const core::Point &PlaceOf(const Taken &taken)
{
    return taken.probe.at;
}

// Which of a level's own probes is nearest a place: of two as near, the
// first. A place stood on is the room's whose own probe that is.
template <class Probes> std::size_t NearestOf(const Probes &owns, const core::Point &place)
{
    std::size_t nearest = 0;
    for (std::size_t i = 1; i < owns.size(); ++i)
    {
        if (Distance(PlaceOf(owns[i]), place) < Distance(PlaceOf(owns[nearest]), place))
        {
            nearest = i;
        }
    }
    return nearest;
}

// The probes of a level of terrain that is lit again (Relighter.h), a room
// for each of the level's own: baked as a room's are, each side of a figure
// lit as the surface behind it is, since a lightmap of ours holds all the
// light of a surface and the level's own probe, a photograph of the level as
// the game lit it, says nothing of it.
std::vector<Room> BakeTerrainRelit(Level &level, std::span<const core::Point> rays)
{
    const std::vector<OwnProbe> owns = level.ProbesOf(L"");
    std::vector<Room> rooms(owns.size());
    std::vector<SeenSum> seen(owns.size());
    for (std::size_t i = 0; i < owns.size(); ++i)
    {
        rooms[i].key = owns[i].at;
        rooms[i].own = owns[i].light;
        rooms[i].whole = true;
    }
    if (owns.empty())
    {
        return rooms;
    }
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
        for (const core::Point &place : PlacesOn(about, kTerrainSpacing))
        {
            const std::size_t own = NearestOf(owns, place);
            if (const std::optional<probes::Coefficients> light = BakeAt(level, about, place, rays, seen.at(own)))
            {
                rooms[own].probes.push_back({place, probes::Matrices(*light)});
            }
        }
    }
    for (std::size_t i = 0; i < rooms.size(); ++i)
    {
        rooms[i].seen = seen[i].Mean();
    }
    return rooms;
}

// A terrain level's own probes, each with its photograph made the sky alone
// (KeepSky); the surfaces the photographs show go to the fit's sums.
std::vector<Taken> TakePhotographs(Level &level, std::array<FitSums, 3> &sums, std::size_t &surfaces)
{
    std::vector<Taken> taken;
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
    return taken;
}

// What a lit surface of the level sends on: fitted to what the photographs
// show of the surfaces, where they show enough of them, and else what a
// surface is taken to send where nothing says.
Sends SendsOf(const std::array<FitSums, 3> &sums, std::size_t surfaces)
{
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
    return sends;
}

// The check: one of the level's own probes reckoned again at its own place,
// and said beside what the level's files have for it.
// @param own the probe's light as the level's files have it
void SayReckonedAgain(Level &level, std::span<const core::Point> rays, const Sends &sends, const Taken &taken,
                      const probes::Irradiance &own)
{
    const std::vector<const Geometry *> about = About(level, TileAt(level, taken.probe.at));
    const auto again = Reckon(level, about, taken.probe.at, rays, sends, taken.sky);
    if (!again)
    {
        return;
    }
    const probes::Irradiance ours = probes::Matrices(*again);
    double both = 0;
    double oursSquared = 0;
    double theirsSquared = 0;
    for (std::size_t at = 0; at < ours.size(); ++at)
    {
        both += double{ours.at(at)} * own.at(at);
        oursSquared += double{ours.at(at)} * ours.at(at);
        theirsSquared += double{own.at(at)} * own.at(at);
    }
    spdlog::info("light probes: {}: its own probe reckoned again is {:.3f} like it and {:.2f} as bright",
                 Text(level.Name()), both / std::sqrt(std::max(oursSquared * theirsSquared, 1e-30)),
                 std::sqrt(oursSquared / std::max(theirsSquared, 1e-30)));
}

// The probes of a level of terrain, a room for each of the level's own.
std::vector<Room> BakeTerrain(Level &level, std::span<const core::Point> rays)
{
    std::array<FitSums, 3> sums{};
    std::size_t surfaces = 0;
    const std::vector<Taken> taken = TakePhotographs(level, sums, surfaces);
    if (taken.empty())
    {
        spdlog::debug("light probes: {}: no probe of its own with a photograph", Text(level.Name()));
        return {};
    }
    const Sends sends = SendsOf(sums, surfaces);
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
        SayReckonedAgain(level, rays, sends, taken[i], rooms[i].own);
    }
    std::vector<SeenSum> seen(taken.size());
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
            const std::size_t own = NearestOf(taken, place);
            if (const auto light = Reckon(level, about, place, rays, sends, taken[own].sky, &seen.at(own)))
            {
                rooms[own].probes.push_back({place, probes::Matrices(*light)});
                ++open;
            }
        }
        spdlog::debug("light probes: tile {}: {} triangles, {} tiles about it, {} places, {} of them open", Text(id),
                      about.front()->lit.size(), about.size() - 1, places.size(), open);
    }
    for (std::size_t i = 0; i < rooms.size(); ++i)
    {
        rooms[i].seen = seen[i].Mean();
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
        return (std::size_t{y} * room.across) + x;
    };
    room.starts.assign((std::size_t{room.across} * room.down) + 1, 0);
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
// @param wanted the stamp the table is to have, or kAnyStamp for any but none
constexpr std::uint64_t kAnyStamp = ~0ULL;
std::optional<std::vector<Room>> Read(std::span<const std::byte> file, std::uint64_t wanted)
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t stamp = 0;
    std::uint32_t count = 0;
    if (!Take(file, magic) || !Take(file, version) || !Take(file, stamp) || !Take(file, count) ||
        magic != kTableMagic || version != kTableVersion || (wanted == kAnyStamp ? stamp == 0 : stamp != wanted) ||
        count > 4096)
    {
        return std::nullopt;
    }
    std::vector<Room> rooms(count);
    for (Room &room : rooms)
    {
        std::uint32_t whole = 0;
        std::uint32_t probes = 0;
        if (!Take(file, room.key) || !Take(file, room.own) || !Take(file, whole) || !Take(file, room.seen) ||
            !Take(file, probes) || file.size() / sizeof(probes::Probe) < probes)
        {
            return std::nullopt;
        }
        room.whole = whole != 0;
        room.relit = stamp != 0;
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
    std::shared_ptr<const std::vector<float>> lamps = std::make_shared<const std::vector<float>>(); // BakedLamps
    std::filesystem::path kept;
    std::filesystem::path data;
    std::filesystem::path relit; // where lightmaps of our own are looked for
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
    // The level's middle: its rooms' own, each by how many probes it has.
    double light = 0;
    double probes = 0;
    for (const Room &room : more)
    {
        light += double{room.seen} * static_cast<double>(room.probes.size());
        probes += static_cast<double>(room.probes.size());
    }
    auto next = std::make_shared<Rooms>(*old);
    for (Room &room : more)
    {
        room.levelSeen = probes > 0 ? static_cast<float>(light / probes) : 0.0F;
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
    const bool relit = !state.relit.empty() && RelitStamp(state.relit, levelRim.stem().wstring()) != 0;
    if (relit)
    {
        level.ReadRelitFrom(state.relit);
    }
    const std::vector<core::Point> rays = probes::Directions(kRays);
    std::vector<Room> baked;
    if (level.Terrain())
    {
        baked = relit ? BakeTerrainRelit(level, rays) : BakeTerrain(level, rays);
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
        room.relit = relit;
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

// Takes a level's own lamps' places from its bake's .lamps file into the
// state's, with those of the levels before it.
void ReadLamps(State &state, const std::filesystem::path &file)
{
    std::ifstream in(file, std::ios::binary);
    std::array<char, 4> magic{};
    std::uint32_t count = 0;
    in.read(magic.data(), magic.size());
    in.read(reinterpret_cast<char *>(&count), sizeof count);
    if (!in || magic != std::array<char, 4>{'E', 'L', 'G', 'L'} || count > 100000)
    {
        return;
    }
    std::vector<float> places(std::size_t{count} * 3);
    const std::span<std::byte> bytes = std::as_writable_bytes(std::span(places));
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in)
    {
        return;
    }
    const std::scoped_lock lock(state.lock);
    auto all = std::make_shared<std::vector<float>>(*state.lamps);
    all->insert(all->end(), places.begin(), places.end());
    state.lamps = std::move(all);
}

// A level's table: the one kept, or baked now and kept.
void BakeLevel(State &state, const std::filesystem::path &levelRim)
{
    const std::filesystem::path kept = state.kept / (levelRim.stem().wstring() + L".bin");
    const std::uint64_t stamp = state.relit.empty() ? 0 : RelitStamp(state.relit, levelRim.stem().wstring());
    // A level lit again comes with its probes, made of the lights themselves
    // (Relighter.cpp): they are taken as they are.
    if (!state.relit.empty())
    {
        std::ifstream in(state.relit / (levelRim.stem().wstring() + L".elgp"), std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::optional<std::vector<Room>> rooms =
                Read(std::as_bytes(std::span(bytes.data(), bytes.size())), kAnyStamp))
        {
            spdlog::info("light probes: {} lit again, {} rooms, {} probes of its own lights", levelRim.stem().string(),
                         rooms->size(), ProbesIn(*rooms));
            ReadLamps(state, state.relit / (levelRim.stem().wstring() + L".lamps"));
            Publish(state, std::move(*rooms));
            return;
        }
    }
    {
        std::ifstream in(kept, std::ios::binary);
        const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (std::optional<std::vector<Room>> rooms = Read(std::as_bytes(std::span(bytes.data(), bytes.size())), stamp))
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
    const std::vector<std::byte> file = Write(*baked, stamp);
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

void Start(const std::filesystem::path &kept, const std::filesystem::path &game, const std::filesystem::path &relit)
{
    State &state = TheState();
    state.kept = kept;
    state.relit = relit;
    state.data = game / "packages" / "core" / "data";
    std::thread(&Work).detach();
}

std::vector<Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game,
                          const std::filesystem::path &relit)
{
    State state;
    state.relit = relit;
    state.data = game / "packages" / "core" / "data";
    return BakeRooms(state, levelRim).value_or(std::vector<Room>{});
}

std::vector<std::byte> Write(const std::vector<Room> &rooms, std::uint64_t stamp)
{
    std::vector<std::byte> file;
    Put(file, kTableMagic);
    Put(file, kTableVersion);
    Put(file, stamp);
    Put(file, static_cast<std::uint32_t>(rooms.size()));
    for (const Room &room : rooms)
    {
        Put(file, room.key);
        Put(file, room.own);
        Put(file, std::uint32_t{room.whole ? 1U : 0U});
        Put(file, room.seen);
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

std::shared_ptr<const std::vector<float>> BakedLamps()
{
    State &state = TheState();
    const std::scoped_lock lock(state.lock);
    return state.lamps;
}

std::shared_ptr<const Rooms> AllRooms()
{
    State &state = TheState();
    const std::scoped_lock lock(state.lock);
    return state.rooms;
}

namespace
{

// A square of a room's grid, by the cells at its edges, which may lie
// outside the grid.
struct Square
{
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

// Adds to `out` the probes in a square's cells, each with one over its
// distance from a place for its score.
void AddCells(const Room &room, const float *place, const Square &square, std::vector<probes::Near> &out)
{
    for (int cy = std::max(square.top, 0); cy <= std::min(square.bottom, static_cast<int>(room.down) - 1); ++cy)
    {
        const std::size_t row = static_cast<std::size_t>(cy) * room.across;
        const std::uint32_t from = room.starts[row + static_cast<std::size_t>(std::max(square.left, 0))];
        const std::uint32_t to =
            room.starts[row + static_cast<std::size_t>(std::min(square.right, static_cast<int>(room.across) - 1)) + 1];
        for (std::uint32_t at = from; at < to; ++at)
        {
            const probes::Probe &probe = room.probes[room.order[at]];
            const float higher = std::max(std::abs(probe.at[2] - place[2]) - kSameStorey, 0.0F);
            const float distance = std::hypot(probe.at[0] - place[0], probe.at[1] - place[1], kHeightCounts * higher);
            out.push_back({1.0F / std::max(distance, 1e-3F), probe.light.data()});
        }
    }
}

// How far from a place anything outside a square of a room's grid is, at the
// least; infinity where the square is the whole grid.
float SureWithin(const Room &room, const float *place, const Square &square)
{
    float sure = std::numeric_limits<float>::infinity();
    if (square.left > 0)
    {
        sure = std::min(sure, place[0] - (room.lowX + (static_cast<float>(square.left) * kCell)));
    }
    if (square.right < static_cast<int>(room.across) - 1)
    {
        sure = std::min(sure, room.lowX + (static_cast<float>(square.right + 1) * kCell) - place[0]);
    }
    if (square.top > 0)
    {
        sure = std::min(sure, place[1] - (room.lowY + (static_cast<float>(square.top) * kCell)));
    }
    if (square.bottom < static_cast<int>(room.down) - 1)
    {
        sure = std::min(sure, room.lowY + (static_cast<float>(square.bottom + 1) * kCell) - place[1]);
    }
    return sure;
}

} // namespace

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
        const Square square{x - ring, x + ring, y - ring, y + ring};
        AddCells(room, place, square, out);
        // How far from the place anything outside the square is, at the least.
        const float sure = SureWithin(room, place, square);
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
            std::ranges::nth_element(scores, scores.begin() + static_cast<std::ptrdiff_t>(probes::kMixed),
                                     std::greater{});
            if (scores[probes::kMixed] >= 1.0F / sure)
            {
                return;
            }
        }
    }
}

} // namespace probebaker
