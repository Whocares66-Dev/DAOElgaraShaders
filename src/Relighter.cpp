// The relighter: Relighter.h says what it is for. Here, how.
//
// A lightmap is a picture laid over a level's surfaces: every lightmapped
// triangle has a place in one (an atlas), which the baker's reading of a
// level already gives (LevelFiles.h). So each texel of an atlas that a
// triangle covers is a place in the level with a way it faces, and its light
// is reckoned there:
//
// - from each of the level's lamps that reaches it, the lamp's colour by how
//   it faces the lamp and by the engine's own fall-off with distance (the
//   square of the cosine of a quarter turn at the lamp's reach, read in
//   static1.psh), with a ray cast to the lamp for its shadow;
// - from what glows, a window or lava: points spread over the glowing
//   surface, a few of them tried from each texel, each as a small patch that
//   sends light by its size and by how the two face each other;
// - and from the surfaces about, what they send on of the light they got:
//   rays cast out over the half of the world the texel faces, the light read
//   where each lands, once for the light that came straight and again for
//   what came off a surface before.
//
// A triangle's facing is taken from the order of its corners, which holds
// for a level of rooms (ProbeBaker's floors are found by it); the ground of
// a terrain level has the other order and is faced up.
//
// What is written is eight bits a channel, as the game's own, but not the
// light itself: the square root of its share of eight whites, which the
// level's shaders undo for a level that is relit (look.hlsl's DAO_RELIT). A
// pool of candlelight is brighter than a white wall, and cut at white it
// was a flat patch in a room "way too dark" (the user, 2026-10-11). The
// lightmap's alpha, where the sun reaches, is the game's own, kept.
#include "Relighter.h"

#include "LevelFiles.h"
#include "ProbeBaker.h"
#include "SurfaceColours.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <numbers>
#include <set>
#include <thread>
#include <utility>

#ifdef ELGARA_EMBREE
#include <embree4/rtcore.h>
#endif

// Windows' own, which a logging header brings: both are good names here.
#undef far
#undef near

namespace relighter
{

namespace
{

using namespace levelfiles;

constexpr float kOffSurface = 0.03F;   // metres a ray starts off its surface
constexpr float kShortOfLamp = 0.35F;  // metres a shadow's ray stops short of a lamp, which stands in its own fitting
constexpr float kGlowReach = 45.0F;    // metres from which a glowing thing is not counted
constexpr std::size_t kGlowTried = 16; // points of each glowing thing tried from a texel
constexpr float kGlowNearest = 1.0F;   // metres: a point nearer than this lights as if it were this far
constexpr float kGlowPointsAMetre = 6.0F; // points spread over a square metre of a glowing thing
constexpr float kOwnPointsAMetre = 70.0F; // and of one that is lit by its own glow: some 0.12 m apart
constexpr int kFilled = 4;                // texels the light is spread out past the triangles' edges

using Colour = probes::Colour;

float Dot(const core::Point &a, const core::Point &b)
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}
core::Point Sub(const core::Point &a, const core::Point &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
core::Point Cross(const core::Point &a, const core::Point &b)
{
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]), (a[0] * b[1]) - (a[1] * b[0])};
}
core::Point Along(const core::Point &from, const core::Point &way, float far)
{
    return {from[0] + (way[0] * far), from[1] + (way[1] * far), from[2] + (way[2] * far)};
}
core::Point Unit(const core::Point &v)
{
    const float length = std::sqrt(Dot(v, v));
    return length > 1e-20F ? core::Point{v[0] / length, v[1] / length, v[2] / length} : core::Point{0, 0, 1};
}

// A number from 0 to 1 for a place and a count, the same every time: where a
// texel's rays go and which points of a glowing thing it tries.
float Chance(std::uint32_t a, std::uint32_t b, std::uint32_t c)
{
    std::uint32_t x = (a * 0x9E3779B1U) ^ ((b + 0x7F4A7C15U) * 0x85EBCA77U) ^ ((c + 0x165667B1U) * 0xC2B2AE3DU);
    x ^= x >> 15;
    x *= 0x2C1B3C6DU;
    x ^= x >> 12;
    x *= 0x297A2D39U;
    x ^= x >> 15;
    return static_cast<float>(x >> 8) / 16777216.0F;
}

// A box about a room's triangles, to know without a cast that a ray misses them all.
struct Box
{
    core::Point low{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                    std::numeric_limits<float>::infinity()};
    core::Point high{-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity()};

    void Add(const core::Point &point)
    {
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            low.at(axis) = std::min(low.at(axis), point.at(axis));
            high.at(axis) = std::max(high.at(axis), point.at(axis));
        }
    }
    // Whether the stretch from `from` for `far` along `way`, of length 1, comes into the box.
    bool Meets(const core::Point &from, const core::Point &way, float far) const
    {
        float enters = 0;
        float leaves = far;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            if (std::abs(way.at(axis)) < 1e-9F)
            {
                if (from.at(axis) < low.at(axis) || from.at(axis) > high.at(axis))
                {
                    return false;
                }
                continue;
            }
            float a = (low.at(axis) - from.at(axis)) / way.at(axis);
            float b = (high.at(axis) - from.at(axis)) / way.at(axis);
            if (a > b)
            {
                std::swap(a, b);
            }
            enters = std::max(enters, a);
            leaves = std::min(leaves, b);
        }
        return enters <= leaves;
    }
};

// A room as the relighter has it: its triangles' tree and box, and the rooms
// a ray from it may land in, itself the first.
struct RoomLit
{
    const Geometry *geometry = nullptr;
    Box box;
    std::vector<std::size_t> about;
};

// One texel of a lightmap that a triangle covers.
struct Texel
{
    core::Point at{};
    core::Point facing{};
    std::uint32_t room = 0;
    bool covered = false;
    bool loose = false; // covered by a triangle that only comes near its middle
};

// One lightmap: its texels, the game's own picture brought to its size, and
// the light reckoned.
struct Map
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<Texel> texels;
    std::vector<Colour> own;       // the game's baked light
    std::vector<std::uint8_t> sun; // its alpha
    std::vector<Colour> straight;  // the light that came straight from a lamp or a glow
    std::vector<Colour> glowed;    // the glows' part of it, before it is smoothed and added
    std::vector<Colour> came;      // the light of the bounce before, to be read by the next
    std::vector<Colour> whole;     // all of it
};

// Something that glows, as points over its surface.
struct Glowing
{
    std::vector<core::Point> points;
    std::vector<core::Point> facings;
    core::Point middle{};
    float reach = 0; // how far its furthest point is from its middle
    float patch = 0; // the square metres a point stands for
    Colour colour{};
    bool sky = false; // an opening the sky is seen through: it sends light the way it faces, and downward
    float own = 0;    // of its colour, what its own surface is lit by
};

// A light from far off and the openings it comes in by.
struct Beaming
{
    core::Point toSource{}; // the way back along the light, of length 1
    Colour colour{};
    std::optional<core::TriangleTree> openings;
    std::optional<core::TriangleTree> bars; // what stands in an opening: a window's tracery
    std::vector<core::Point> outward;       // each opening's way out of the level, two triangles an opening
};

// What glows, as the surfaces it is. The light is reckoned from points spread
// over a glowing thing; a picture of the level (the reflection cubes) sees
// the thing itself, lava as a river and a window as a pane.
struct Shining
{
    std::vector<core::Triangle> triangles;
    std::vector<std::uint32_t> glow;        // each triangle's glowing thing, by its number among the scene's
    std::optional<core::TriangleTree> tree; // over the triangles, made when a picture is wanted
};

struct Scene
{
    std::vector<RoomLit> rooms;
    std::vector<Lamp> lamps;
    std::vector<Glowing> glows;
    Shining shining;
    std::vector<Beaming> beams;
    std::map<std::uint32_t, Map> maps; // by the atlas's number
    // What each room's lightmapped triangles send on of their light as a
    // picture of the level sees them, their own colours (PaintSurfaces):
    // none until a picture is wanted. The bake's own light is reckoned with
    // every surface the one grey, `surfaces`.
    std::vector<std::vector<Colour>> sends;
#ifdef ELGARA_EMBREE
    RTCScene embree = nullptr; // every room of the world, a geometry each by the room's number; or none
#endif
};

#ifdef ELGARA_EMBREE
// Embree's device and the scene made on it, let go with this.
struct Embree
{
    RTCDevice device = nullptr;
    RTCScene scene = nullptr;
    Embree() = default;
    Embree(const Embree &) = delete;
    Embree &operator=(const Embree &) = delete;
    ~Embree()
    {
        if (scene != nullptr)
        {
            rtcReleaseScene(scene);
        }
        if (device != nullptr)
        {
            rtcReleaseDevice(device);
        }
    }
};

// Makes Embree's scene of a world: each room's lit triangles a geometry
// whose number is the room's, so that what a ray lands on says which room
// and which of its triangles.
RTCScene MakeEmbree(Embree &held, const Scene &scene)
{
    held.device = rtcNewDevice(nullptr);
    if (held.device == nullptr)
    {
        return nullptr;
    }
    held.scene = rtcNewScene(held.device);
    rtcSetSceneBuildQuality(held.scene, RTC_BUILD_QUALITY_HIGH);
    for (std::size_t room = 0; room < scene.rooms.size(); ++room)
    {
        const Geometry *geometry = scene.rooms[room].geometry;
        if (geometry == nullptr || geometry->lit.empty())
        {
            continue;
        }
        const std::size_t count = geometry->lit.size();
        RTCGeometry mesh = rtcNewGeometry(held.device, RTC_GEOMETRY_TYPE_TRIANGLE);
        auto *corners = static_cast<float *>(
            rtcSetNewGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), 3 * count));
        auto *indices = static_cast<unsigned *>(
            rtcSetNewGeometryBuffer(mesh, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned), count));
        if (corners == nullptr || indices == nullptr)
        {
            rtcReleaseGeometry(mesh);
            continue;
        }
        for (std::size_t i = 0; i < count; ++i)
        {
            const core::Triangle &triangle = geometry->lit[i].corners;
            const std::array<const core::Point *, 3> three{&triangle.a, &triangle.b, &triangle.c};
            for (std::size_t corner = 0; corner < 3; ++corner)
            {
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    corners[(9 * i) + (3 * corner) + axis] = three.at(corner)->at(axis);
                }
                indices[(3 * i) + corner] = static_cast<unsigned>((3 * i) + corner);
            }
        }
        rtcCommitGeometry(mesh);
        rtcAttachGeometryByID(held.scene, mesh, static_cast<unsigned>(room));
        rtcReleaseGeometry(mesh);
    }
    rtcCommitScene(held.scene);
    return held.scene;
}
#endif

// Takes a lamp into the scene, unless it gives no light or is one already
// there: a level's layouts name some twice, once for each detail of a tile.
void AddLamp(Scene &scene, const Lamp &lamp, const Options &options)
{
    // A light of a hundred metres is no lamp: it is how the level's makers
    // gave a whole hall its colour, one flat orange from end to end, which is
    // the light with no source this is here to be rid of (Orzammar's Commons
    // lit by four of them was one sheet of yellow, 2026-10-11).
    // And with `lamps` at nothing none of the level's own is taken: a level
    // is then lit by its fittings alone, each by what kind of thing it is.
    if (options.lamps <= 0 || lamp.reach <= 0 || lamp.reach > options.furthest ||
        std::max({lamp.colour[0], lamp.colour[1], lamp.colour[2]}) <= 0)
    {
        return;
    }
    for (const Lamp &has : scene.lamps)
    {
        const core::Point apart = Sub(has.at, lamp.at);
        if (Dot(apart, apart) < 0.01F)
        {
            return;
        }
    }
    scene.lamps.push_back(lamp);
}

// Where a fitting's light is. A model's own place is where it is fixed, a
// lantern's the foot of its bracket in the pillar it hangs from: a light put
// there was inside the stone, and the pillar under four lanterns stood dark
// (seen 2026-10-11). The level's makers put a light of their own at the
// lantern's flame, half a metre out, to tint the fitting: where there is one
// within a metre, that is the place. Else it is the middle of what the model
// is made of.
core::Point WhereItBurns(Level &level, const std::wstring &id, const level::Room &room, const level::Placed &placed,
                         std::span<const core::Point> hints)
{
    constexpr float kHintWithin = 1.0F;
    core::Point at = level::InLevel(room, placed, {0, 0, 0});
    const std::vector<Level::Drawn> drawn = level.TrianglesOf(id, placed);
    if (!drawn.empty())
    {
        core::Point sum{};
        for (const Level::Drawn &triangle : drawn)
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                sum.at(axis) +=
                    (triangle.corners.a.at(axis) + triangle.corners.b.at(axis) + triangle.corners.c.at(axis)) / 3;
            }
        }
        const auto count = static_cast<float>(drawn.size());
        at = {sum[0] / count, sum[1] / count, sum[2] / count};
    }
    const core::Point fixed = level::InLevel(room, placed, {0, 0, 0});
    float nearest = kHintWithin;
    for (const core::Point &hint : hints)
    {
        const core::Point apart = Sub(hint, fixed);
        const float far = std::sqrt(Dot(apart, apart));
        if (far < nearest)
        {
            nearest = far;
            at = hint;
        }
    }
    return at;
}

// Takes a level's flames into the scene as lamps. Flames that hang or stand
// together, a chandelier's twelve, a candlestick's three, are one lamp where
// their middle is, of their light together; flames about a lamp that is
// there already, the level's own or an earlier rule's, are that lamp's
// flames, and are lit by it already.
//
// A flame's light falls off as the square of the distance, softened near
// (Straight has how). The first form gave each little group of flames the
// engine's fall-off over four and a half metres: a chandelier half a metre
// from a wall burnt the wall white and left the floor four metres under it
// dark (the user, 2026-10-11: "why's the light so strong against the wall?
// the light should be isotropic").
void AddFlames(Scene &scene, std::span<const core::Point> flames, const Flame &flame, std::size_t levelsOwn)
{
    constexpr float kTogether = 1.6F;    // metres from a group's middle
    constexpr float kLampsOwn = 1.6F;    // metres from a lamp of the level's own
    constexpr float kFlamesMost = 10.0F; // a group is no brighter than this many flames
    constexpr float kCore = 1.5F;        // metres: Lamp::core
    struct Group
    {
        core::Point sum{};
        float count = 0;
        core::Point Middle() const
        {
            return {sum[0] / count, sum[1] / count, sum[2] / count};
        }
    };
    std::vector<Group> groups;
    for (const core::Point &at : flames)
    {
        Group *joined = nullptr;
        for (Group &group : groups)
        {
            const core::Point apart = Sub(group.Middle(), at);
            if (Dot(apart, apart) < kTogether * kTogether)
            {
                joined = &group;
                break;
            }
        }
        if (joined == nullptr)
        {
            joined = &groups.emplace_back();
        }
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            joined->sum.at(axis) += at.at(axis);
        }
        joined->count += 1;
    }
    for (const Group &group : groups)
    {
        const core::Point middle = group.Middle();
        const bool lit = std::ranges::any_of(
            scene.lamps.begin(), scene.lamps.begin() + static_cast<std::ptrdiff_t>(levelsOwn), [&](const Lamp &has) {
                const core::Point apart = Sub(has.at, middle);
                return Dot(apart, apart) < kLampsOwn * kLampsOwn;
            });
        if (lit)
        {
            continue;
        }
        const float many = std::min({group.count, kFlamesMost, std::max(flame.most, 1.0F)});
        scene.lamps.push_back({{middle[0], middle[1], middle[2] + flame.lift},
                               {flame.colour[0] * many, flame.colour[1] * many, flame.colour[2] * many},
                               flame.reach,
                               L"",
                               kCore,
                               flame.clear});
    }
}

// What a ray lands on first among a room's own and the rooms about it.
struct Landing
{
    const Geometry *geometry = nullptr;
    core::RayHit hit;
    std::uint32_t room = 0; // the room landed in, by its number among the scene's
};
std::optional<Landing> Cast(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point &way,
                            float far)
{
#ifdef ELGARA_EMBREE
    if (scene.embree != nullptr)
    {
        RTCRayHit cast{};
        cast.ray.org_x = from[0];
        cast.ray.org_y = from[1];
        cast.ray.org_z = from[2];
        cast.ray.dir_x = way[0];
        cast.ray.dir_y = way[1];
        cast.ray.dir_z = way[2];
        cast.ray.tnear = 1e-3F;
        cast.ray.tfar = far;
        cast.ray.mask = 0xFFFFFFFFU;
        cast.hit.geomID = RTC_INVALID_GEOMETRY_ID;
        cast.hit.instID[0] = RTC_INVALID_GEOMETRY_ID;
        rtcIntersect1(scene.embree, &cast);
        if (cast.hit.geomID == RTC_INVALID_GEOMETRY_ID)
        {
            return std::nullopt;
        }
        // Embree's u and v are the second and third corners' weights, as the tree's are.
        return Landing{scene.rooms[cast.hit.geomID].geometry,
                       core::RayHit{cast.hit.primID, cast.ray.tfar, cast.hit.u, cast.hit.v}, cast.hit.geomID};
    }
#endif
    std::optional<Landing> nearest;
    float most = far;
    for (const std::size_t other : scene.rooms[room].about)
    {
        const RoomLit &lit = scene.rooms[other];
        if (lit.geometry == nullptr || !lit.geometry->tree || !lit.box.Meets(from, way, most))
        {
            continue;
        }
        if (const std::optional<core::RayHit> found = lit.geometry->tree->Cast(from, way, 1e-3F, most))
        {
            nearest = Landing{lit.geometry, *found, static_cast<std::uint32_t>(other)};
            most = found->distance;
        }
    }
    return nearest;
}

// Whether anything of the world is in a ray's way within `far`: what a shadow
// asks, which needs no word of what it was.
bool Blocked(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point &way, float far)
{
#ifdef ELGARA_EMBREE
    if (scene.embree != nullptr)
    {
        RTCRay ray{};
        ray.org_x = from[0];
        ray.org_y = from[1];
        ray.org_z = from[2];
        ray.dir_x = way[0];
        ray.dir_y = way[1];
        ray.dir_z = way[2];
        ray.tnear = 1e-3F;
        ray.tfar = far;
        ray.mask = 0xFFFFFFFFU;
        rtcOccluded1(scene.embree, &ray);
        return ray.tfar < 0; // Embree's word for "something was"
    }
#endif
    return Cast(scene, room, from, way, far).has_value();
}

// Runs `work` for every number below `count`, on as many threads as the
// machine has to spare.
template <class Work> void ForEach(std::size_t count, const Work &work)
{
    const unsigned threads =
        std::max(1U, std::thread::hardware_concurrency() > 3 ? std::thread::hardware_concurrency() - 2 : 1U);
    std::atomic<std::size_t> next{0};
    constexpr std::size_t kBatch = 256;
    const auto run = [&] {
        for (;;)
        {
            const std::size_t from = next.fetch_add(kBatch);
            if (from >= count)
            {
                return;
            }
            for (std::size_t i = from; i < std::min(count, from + kBatch); ++i)
            {
                work(i);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i)
    {
        pool.emplace_back(run);
    }
    run();
    for (std::thread &thread : pool)
    {
        thread.join();
    }
}

// A triangle's three corners in a lightmap, in texels.
using InMap = std::array<std::array<float, 2>, 3>;

// A texel's place on a triangle, as shares of its three corners.
struct OnTriangle
{
    std::array<float, 3> shares{};
    bool loose = false; // the triangle only comes near the texel's middle
};

// The place on a triangle's edges nearest a texel's middle, where an edge
// comes within the texel's own square; nothing where none does.
std::optional<std::array<float, 3>> NearestOnEdges(const InMap &uv, float px, float py)
{
    std::optional<std::array<float, 3>> shares;
    float nearest = 0.55F; // texels, squared: within the texel's own square, about
    for (std::size_t edge = 0; edge < 3; ++edge)
    {
        const std::array<float, 2> &from = uv.at(edge);
        const std::array<float, 2> &to = uv.at((edge + 1) % 3);
        const float ex = to[0] - from[0];
        const float ey = to[1] - from[1];
        const float along = std::clamp(
            (((px - from[0]) * ex) + ((py - from[1]) * ey)) / std::max((ex * ex) + (ey * ey), 1e-12F), 0.0F, 1.0F);
        const float dx = from[0] + (along * ex) - px;
        const float dy = from[1] + (along * ey) - py;
        if ((dx * dx) + (dy * dy) < nearest)
        {
            nearest = (dx * dx) + (dy * dy);
            shares.emplace();
            shares->at(edge) = 1 - along;
            shares->at((edge + 1) % 3) = along;
        }
    }
    return shares;
}

// Where a texel's middle is on a triangle: nothing for a texel the triangle
// neither covers nor comes near, or only comes near and another has already.
// @param area twice the triangle's own in the lightmap, signed
// @param covered whether a triangle has the texel already
std::optional<OnTriangle> PlaceOn(const InMap &uv, float area, float px, float py, bool covered)
{
    // The texel's middle as shares of the three corners; a little
    // outside still counts, so a triangle's edge is not left bare.
    const float b = (((px - uv[0][0]) * (uv[2][1] - uv[0][1])) - ((uv[2][0] - uv[0][0]) * (py - uv[0][1]))) / area;
    const float c = (((uv[1][0] - uv[0][0]) * (py - uv[0][1])) - ((px - uv[0][0]) * (uv[1][1] - uv[0][1]))) / area;
    const float a = 1 - b - c;
    constexpr float kOutside = -0.02F;
    const bool outside = a < kOutside || b < kOutside || c < kOutside;
    if (!outside)
    {
        return OnTriangle{{a, b, c}, false};
    }
    // A triangle that covers no texel's middle, the riser of a
    // step a hand high, still has texels drawn from: left
    // bare, they were filled from whatever lay beside them in
    // the lightmap, and a platform's edge shone as lava does
    // (the user, 2026-10-11: "why's the platform edge glowing?
    // that's not lava"). So a texel no triangle covers takes
    // the nearest place on one that comes within its square.
    if (covered)
    {
        return std::nullopt;
    }
    const std::optional<std::array<float, 3>> near = NearestOnEdges(uv, px, py);
    return near ? std::optional<OnTriangle>(OnTriangle{*near, true}) : std::nullopt;
}

// Lays one triangle into its lightmap, for Lay.
void LayTriangle(Map &map, std::uint32_t room, const level::LitTriangle &triangle, const core::Point &facing)
{
    const auto w = static_cast<float>(map.width);
    const auto h = static_cast<float>(map.height);
    const InMap uv{{{triangle.uv[0][0] * w, triangle.uv[0][1] * h},
                    {triangle.uv[1][0] * w, triangle.uv[1][1] * h},
                    {triangle.uv[2][0] * w, triangle.uv[2][1] * h}}};
    const float area =
        ((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1])) - ((uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]));
    if (std::abs(area) < 1e-9F)
    {
        return;
    }
    const int left = std::max(0, static_cast<int>(std::floor(std::min({uv[0][0], uv[1][0], uv[2][0]}) - 0.5F)));
    const int right = std::min(static_cast<int>(map.width) - 1,
                               static_cast<int>(std::ceil(std::max({uv[0][0], uv[1][0], uv[2][0]}) + 0.5F)));
    const int top = std::max(0, static_cast<int>(std::floor(std::min({uv[0][1], uv[1][1], uv[2][1]}) - 0.5F)));
    const int bottom = std::min(static_cast<int>(map.height) - 1,
                                static_cast<int>(std::ceil(std::max({uv[0][1], uv[1][1], uv[2][1]}) + 0.5F)));
    for (int y = top; y <= bottom; ++y)
    {
        for (int x = left; x <= right; ++x)
        {
            Texel &texel = map.texels[(static_cast<std::size_t>(y) * map.width) + static_cast<std::size_t>(x)];
            const std::optional<OnTriangle> on =
                PlaceOn(uv, area, static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F, texel.covered);
            if (!on)
            {
                continue;
            }
            texel.loose = on->loose;
            texel.covered = true;
            texel.room = room;
            texel.facing = facing;
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                texel.at.at(axis) = (on->shares[0] * triangle.corners.a.at(axis)) +
                                    (on->shares[1] * triangle.corners.b.at(axis)) +
                                    (on->shares[2] * triangle.corners.c.at(axis));
            }
        }
    }
}

// Lays a room's triangles into the lightmaps: each texel whose middle a
// triangle covers is given its place on the triangle and the triangle's
// facing.
void Lay(Scene &scene, std::uint32_t room, const Geometry &geometry)
{
    std::vector<bool> ground(geometry.lit.size());
    for (const auto &[from, to] : geometry.ground)
    {
        std::fill(ground.begin() + static_cast<std::ptrdiff_t>(from), ground.begin() + static_cast<std::ptrdiff_t>(to),
                  true);
    }
    for (std::size_t index = 0; index < geometry.lit.size(); ++index)
    {
        const level::LitTriangle &triangle = geometry.lit[index];
        const auto found = scene.maps.find(triangle.atlas);
        if (found == scene.maps.end())
        {
            continue;
        }
        core::Point facing =
            Unit(Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a)));
        if (ground[index] && facing[2] < 0)
        {
            facing = {-facing[0], -facing[1], -facing[2]};
        }
        LayTriangle(found->second, room, triangle, facing);
    }
}

// A light as it reaches a place: the way to it, and its light on a surface
// that faces it.
struct Reaching
{
    core::Point way{};
    Colour strength{};
};

// A lamp's light at a place, with a ray cast for its shadow; nothing where
// the lamp does not reach, is faced away from, or is shadowed.
std::optional<Reaching> LampAt(const Scene &scene, std::uint32_t room, const core::Point &from,
                               const core::Point *facing, const Options &options, const Lamp &lamp)
{
    const core::Point to = Sub(lamp.at, from);
    const float far = std::sqrt(Dot(to, to));
    // Every light falls off by the square of the distance, as light does
    // (the user, 2026-10-11: "The light should behave in a physically
    // accurate way"), from a ball of light `core` across and not from a
    // point, so that a wall half a metre from a lamp is not thirty times
    // as bright as the floor under it; and is brought to nothing at its
    // reach with no edge.
    //
    // A lamp of the level's own says how strong it is by its colour and
    // nothing by its reach: the mage tower's lanterns reach one metre,
    // enough to colour their own fittings, and the light they were meant
    // to give was in the game's bake. Fitted to that reach they "barely
    // give off any light" (the user, 2026-10-11). So a lamp is its
    // colour, by the look's `lamps`, a metre from a core a metre across,
    // carrying twenty metres by `reach`.
    constexpr float kLampCore = 1.0F;
    constexpr float kLampCarries = 20.0F;
    const bool ours = lamp.core > 0;
    const float core = ours ? lamp.core : kLampCore;
    const float reach = ours ? lamp.reach : kLampCarries * options.reach;
    if (far >= reach || far < 1e-3F)
    {
        return std::nullopt;
    }
    const core::Point way{to[0] / far, to[1] / far, to[2] / far};
    if (facing != nullptr && Dot(*facing, way) <= 0)
    {
        return std::nullopt;
    }
    const float shortOf = std::max(kShortOfLamp, lamp.clear);
    if (far > shortOf && Blocked(scene, room, from, way, far - shortOf))
    {
        return std::nullopt;
    }
    const float within = 1 - ((far / reach) * (far / reach));
    const float share = (ours ? 1.0F : options.lamps) * within * within / ((far * far) + (core * core));
    return Reaching{way, Colour{share * lamp.colour[0], share * lamp.colour[1], share * lamp.colour[2]}};
}

// Whether a light from far off reaches a place: the way back along it goes
// out by one of its openings before it meets anything of the level.
bool BeamReaches(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point *facing,
                 const Beaming &beam)
{
    constexpr float kBeamFar = 120.0F; // metres an opening may be from what it lights
    if (!beam.openings || (facing != nullptr && Dot(*facing, beam.toSource) <= 0))
    {
        return false;
    }
    const std::optional<core::RayHit> out = beam.openings->Cast(from, beam.toSource, 1e-3F, kBeamFar);
    if (!out || (out->distance > 0.1F && Blocked(scene, room, from, beam.toSource, out->distance - 0.05F)))
    {
        return false;
    }
    // The light comes in by an opening, from outside: a window in the wall
    // the light is travelling towards lets none in, and with no word on
    // which side is out, the wall about such a window was lit through it
    // from the room (the user, 2026-10-11: "windows seem to have light
    // shining on them instead of through them").
    //
    // (The tracery's own shadow is not cast: a window's model has its glass
    // in it with its bars, and a pane that stops the light stops all of it.)
    const bool fromInside = Dot(beam.toSource, beam.outward[out->triangle / 2]) <= 0;
    return !fromInside;
}

// The lights that reach a place straight, each handed to `seen` as the way
// to it and its light on a surface that faces it: the lamps', with a ray
// cast for the shadow, and what comes in from far off. With `facing`, a
// light the place faces away from is passed over before any ray is cast.
template <class Seen>
void Direct(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point *facing,
            const Options &options, const Seen &seen)
{
    for (const Lamp &lamp : scene.lamps)
    {
        if (const std::optional<Reaching> light = LampAt(scene, room, from, facing, options, lamp))
        {
            seen(light->way, light->strength);
        }
    }
    for (const Beaming &beam : scene.beams)
    {
        if (BeamReaches(scene, room, from, facing, beam))
        {
            seen(beam.toSource, beam.colour);
        }
    }
}

// What glows and reaches a place straight, each point of it tried handed to
// `seen` as the way to it and its light on a surface that faces it: a
// surface that glows is a light as wide as it is, lava a street's width, and
// is taken as points spread over it, each standing for its share.
template <class Seen>
void Glowed(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point *facing,
            std::uint32_t seed, const Seen &seen)
{
    for (std::size_t g = 0; g < scene.glows.size(); ++g)
    {
        const Glowing &glow = scene.glows[g];
        const core::Point apart = Sub(glow.middle, from);
        if (std::sqrt(Dot(apart, apart)) > kGlowReach + glow.reach)
        {
            continue;
        }
        // The points tried are spread evenly through the thing's own, each a
        // chance step within its share of them: with three picked anywhere,
        // a wall by a window was specks of light and dark (2026-10-11).
        const std::size_t trying = std::min(kGlowTried, glow.points.size());
        // Each point tried stands for its share of the whole surface.
        const float stands = glow.patch * static_cast<float>(glow.points.size()) / static_cast<float>(trying);
        for (std::size_t tried = 0; tried < trying; ++tried)
        {
            const float within = (static_cast<float>(tried) +
                                  Chance(seed, static_cast<std::uint32_t>(g), static_cast<std::uint32_t>(tried))) /
                                 static_cast<float>(trying);
            const auto point = std::min(glow.points.size() - 1,
                                        static_cast<std::size_t>(within * static_cast<float>(glow.points.size())));
            const core::Point to = Sub(glow.points[point], from);
            const float far = std::sqrt(Dot(to, to));
            if (far < 1e-3F || far > kGlowReach)
            {
                continue;
            }
            const core::Point way{to[0] / far, to[1] / far, to[2] / far};
            if (facing != nullptr && Dot(*facing, way) <= 0)
            {
                continue;
            }
            // What glows sends to both its sides; the sky through an opening
            // sends into the room alone, and downward: `way` is from the
            // surface up to the opening.
            const float sends = glow.sky ? std::max(0.0F, -Dot(glow.facings[point], way)) * std::max(0.0F, way[2])
                                         : std::abs(Dot(glow.facings[point], way));
            if (sends <= 0 || (far > 0.1F && Blocked(scene, room, from, way, far - 0.05F)))
            {
                continue;
            }
            const float share =
                stands * sends / (std::numbers::pi_v<float> * std::max(far * far, kGlowNearest * kGlowNearest));
            seen(way, Colour{share * glow.colour[0], share * glow.colour[1], share * glow.colour[2]});
        }
    }
}

// The light that comes straight to a texel: the lamps', what comes in from
// far off, and what glows.
// @param soft what glows adds its light to: a wide light tried at a few
//  points of itself, which is smoothed before it is added (Smooth)
Colour Straight(const Scene &scene, const Texel &texel, std::uint32_t seed, const Options &options, Colour &soft)
{
    Colour light{};
    const core::Point from = Along(texel.at, texel.facing, kOffSurface);
    Direct(scene, texel.room, from, &texel.facing, options, [&](const core::Point &way, const Colour &strength) {
        const float faces = Dot(texel.facing, way);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            light.at(channel) += faces * strength.at(channel);
        }
    });
    Glowed(scene, texel.room, from, &texel.facing, seed, [&](const core::Point &way, const Colour &strength) {
        const float faces = Dot(texel.facing, way);
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            soft.at(channel) += faces * strength.at(channel);
        }
    });
    // A texel of a glowing thing itself: one that lies on it, by a point of
    // the thing's being right by it and facing as it does. At 0.35 m, with
    // the points 0.4 m apart, the side of the table a crystal stood on was
    // "on it" too, and one texel of a lightmap is a hand's breadth or two:
    // the table's whole front was a flat bright blue (the user, 2026-10-11:
    // "lyrium light looks off").
    for (const Glowing &glow : scene.glows)
    {
        constexpr float kOnIt = 0.1F; // metres
        const core::Point apart = Sub(glow.middle, texel.at);
        if (glow.own <= 0 || Dot(apart, apart) > (glow.reach + kOnIt) * (glow.reach + kOnIt))
        {
            continue;
        }
        for (std::size_t point = 0; point < glow.points.size(); ++point)
        {
            const core::Point off = Sub(glow.points[point], texel.at);
            if (Dot(off, off) < kOnIt * kOnIt && Dot(glow.facings[point], texel.facing) > 0.8F)
            {
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    light.at(channel) += glow.own * glow.colour.at(channel);
                }
                break;
            }
        }
    }
    return light;
}

// The light a map has at a place of it, 0 to 1 each way, by the nearest texel.
const Colour &At(const Map &map, const std::vector<Colour> &light, float u, float v)
{
    const auto x = static_cast<std::size_t>(
        std::clamp(u * static_cast<float>(map.width), 0.0F, static_cast<float>(map.width - 1)));
    const auto y = static_cast<std::size_t>(
        std::clamp(v * static_cast<float>(map.height), 0.0F, static_cast<float>(map.height - 1)));
    return light[(y * map.width) + x];
}

// What comes back to a texel off the surfaces about: the mean, over rays
// cast out the way it faces and more of them the nearer that way, of the
// light where each lands, by what a surface sends on.
Colour Back(const Scene &scene, const Texel &texel, std::uint32_t seed, const Options &options)
{
    const core::Point from = Along(texel.at, texel.facing, kOffSurface);
    const core::Point aside =
        Unit(std::abs(texel.facing[2]) < 0.9F ? Cross(texel.facing, {0, 0, 1}) : Cross(texel.facing, {1, 0, 0}));
    const core::Point other = Cross(texel.facing, aside);
    Colour sum{};
    for (std::size_t ray = 0; ray < options.rays; ++ray)
    {
        const float turn = 2 * std::numbers::pi_v<float> * Chance(seed, static_cast<std::uint32_t>(ray), 7);
        const float out = std::sqrt(Chance(seed, static_cast<std::uint32_t>(ray), 13));
        const float up = std::sqrt(std::max(0.0F, 1 - (out * out)));
        const core::Point way{
            (aside[0] * out * std::cos(turn)) + (other[0] * out * std::sin(turn)) + (texel.facing[0] * up),
            (aside[1] * out * std::cos(turn)) + (other[1] * out * std::sin(turn)) + (texel.facing[1] * up),
            (aside[2] * out * std::cos(turn)) + (other[2] * out * std::sin(turn)) + (texel.facing[2] * up)};
        const std::optional<Landing> landing =
            Cast(scene, texel.room, from, way, std::numeric_limits<float>::infinity());
        if (!landing)
        {
            continue;
        }
        const level::LitTriangle &triangle = landing->geometry->lit[landing->hit.triangle];
        const auto map = scene.maps.find(triangle.atlas);
        if (map == scene.maps.end())
        {
            continue;
        }
        const float w = 1 - landing->hit.u - landing->hit.v;
        const Colour &there =
            At(map->second, map->second.came,
               (w * triangle.uv[0][0]) + (landing->hit.u * triangle.uv[1][0]) + (landing->hit.v * triangle.uv[2][0]),
               (w * triangle.uv[0][1]) + (landing->hit.u * triangle.uv[1][1]) + (landing->hit.v * triangle.uv[2][1]));
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            sum.at(channel) += there.at(channel);
        }
    }
    const float share = options.surfaces / static_cast<float>(std::max<std::size_t>(options.rays, 1));
    return {sum[0] * share, sum[1] * share, sum[2] * share};
}

// Smooths one kind of a bake's light over each texel's neighbours on the same
// surface: those within `about` texels of it that face as it does and are
// near it in the level. What a wide glow sends and what comes back off the
// surfaces about are reckoned from a few chance rays a texel, and each texel's
// chances are its own: unsmoothed, a cave's roof lit by lava was mottled with
// bright specks (the user, 2026-10-11: "what is this mottled appearance?";
// the rock's own texture is plain grey). Both are soft lights, the same over
// a hand's breadth, so the mean of some thirty texels is the light with a
// fifth of the specks. A lamp's own light, which has edges, is not smoothed.
void Smooth(std::span<const std::pair<Map *, std::uint32_t>> order, std::vector<Colour> Map::*which, int about)
{
    constexpr float kSameFacing = 0.9F;
    constexpr float kNearby = 1.5F; // metres
    std::vector<Colour> smooth(order.size());
    ForEach(order.size(), [&](std::size_t i) {
        const Map &map = *order[i].first;
        const std::vector<Colour> &light = map.*which;
        const Texel &texel = map.texels[order[i].second];
        const int x = static_cast<int>(order[i].second % map.width);
        const int y = static_cast<int>(order[i].second / map.width);
        Colour sum{};
        float count = 0;
        for (int ny = std::max(0, y - about); ny <= std::min(static_cast<int>(map.height) - 1, y + about); ++ny)
        {
            for (int nx = std::max(0, x - about); nx <= std::min(static_cast<int>(map.width) - 1, x + about); ++nx)
            {
                const std::size_t at = (static_cast<std::size_t>(ny) * map.width) + static_cast<std::size_t>(nx);
                const Texel &other = map.texels[at];
                const core::Point apart = Sub(other.at, texel.at);
                if (!other.covered || Dot(other.facing, texel.facing) < kSameFacing ||
                    Dot(apart, apart) > kNearby * kNearby)
                {
                    continue;
                }
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    sum.at(channel) += light[at].at(channel);
                }
                count += 1;
            }
        }
        smooth[i] = count > 0 ? Colour{sum[0] / count, sum[1] / count, sum[2] / count} : light[order[i].second];
    });
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        (order[i].first->*which)[order[i].second] = smooth[i];
    }
}

// The mean light of the texels about one that have any, of the eight beside
// it; nothing where none of them has.
// @param has which texels of the map have light
std::optional<Colour> MeanAbout(const Map &map, const std::vector<bool> &has, const std::vector<Colour> &light, int x,
                                int y)
{
    Colour sum{};
    int count = 0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int nx = x + dx;
            const int ny = y + dy;
            if (nx < 0 || ny < 0 || std::cmp_greater_equal(nx, map.width) || std::cmp_greater_equal(ny, map.height))
            {
                continue;
            }
            const std::size_t near = (static_cast<std::size_t>(ny) * map.width) + static_cast<std::size_t>(nx);
            if (has[near])
            {
                ++count;
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    sum.at(channel) += light[near].at(channel);
                }
            }
        }
    }
    if (count == 0)
    {
        return std::nullopt;
    }
    return Colour{sum[0] / static_cast<float>(count), sum[1] / static_cast<float>(count),
                  sum[2] / static_cast<float>(count)};
}

// Gives the texels no triangle covers the mean of their covered neighbours',
// a ring at a time: a lightmap is read between its texels, and a bare texel
// beside a triangle's edge would darken the edge.
void Fill(const Map &map, std::vector<Colour> &light)
{
    std::vector<bool> has(map.texels.size());
    for (std::size_t i = 0; i < map.texels.size(); ++i)
    {
        has[i] = map.texels[i].covered;
    }
    for (int ring = 0; ring < kFilled; ++ring)
    {
        std::vector<bool> next = has;
        std::vector<Colour> filled = light;
        for (std::uint32_t y = 0; y < map.height; ++y)
        {
            for (std::uint32_t x = 0; x < map.width; ++x)
            {
                const std::size_t at = (std::size_t{y} * map.width) + x;
                if (has[at])
                {
                    continue;
                }
                if (const std::optional<Colour> mean =
                        MeanAbout(map, has, light, static_cast<int>(x), static_cast<int>(y)))
                {
                    filled[at] = *mean;
                    next[at] = true;
                }
            }
        }
        has = std::move(next);
        light = std::move(filled);
    }
}

// The whole number nearest a number that is not less than nothing, a half
// going up, as a byte. By the sum and not by std::lround, which is the plain
// way to say it: the float just under a half and a half make one, where
// std::lround makes nothing of it, and a lightmap is to be what it was to
// the byte.
std::uint8_t NearestByte(float value)
{
    return static_cast<std::uint8_t>(std::floor(value + 0.5F));
}

// A picture, four numbers a pixel, row by row.
using Picture = std::vector<std::array<float, 4>>;

// A picture at half its size each way, a pixel the mean of the four it
// covers: the next smaller of a DDS file's.
Picture Halved(const Picture &picture, std::uint32_t width, std::uint32_t height)
{
    const std::uint32_t nextWidth = std::max(width / 2, 1U);
    const std::uint32_t nextHeight = std::max(height / 2, 1U);
    Picture half(std::size_t{nextWidth} * nextHeight);
    for (std::uint32_t y = 0; y < nextHeight; ++y)
    {
        for (std::uint32_t x = 0; x < nextWidth; ++x)
        {
            std::array<float, 4> sum{};
            for (std::uint32_t dy = 0; dy < 2; ++dy)
            {
                for (std::uint32_t dx = 0; dx < 2; ++dx)
                {
                    const std::array<float, 4> &from =
                        picture[(std::size_t{std::min((2 * y) + dy, height - 1)} * width) +
                                std::min((2 * x) + dx, width - 1)];
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        sum.at(channel) += from.at(channel) / 4;
                    }
                }
            }
            half[(std::size_t{y} * nextWidth) + x] = sum;
        }
    }
    return half;
}

// A lightmap as a DDS file, not packed, with its smaller pictures after it.
std::vector<std::byte> DdsOf(const Map &map, const Options &options)
{
    std::uint32_t width = map.width;
    std::uint32_t height = map.height;
    Picture picture(std::size_t{width} * height);
    for (std::size_t i = 0; i < picture.size(); ++i)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            // Kept as the square root of the light's share of the most a
            // lightmap of ours holds (LevelFiles.h's kRelitMost).
            const float light = map.whole[i].at(channel) + (options.kept * map.own[i].at(channel));
            picture[i].at(channel) = std::sqrt(std::clamp(light / kRelitMost, 0.0F, 1.0F));
        }
        picture[i][3] = static_cast<float>(map.sun[i]) / 255.0F;
    }
    std::uint32_t pictures = 1;
    for (std::uint32_t w = width, h = height; w > 1 || h > 1; w = std::max(w / 2, 1U), h = std::max(h / 2, 1U))
    {
        ++pictures;
    }
    std::vector<std::byte> file(128);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(file.data() + at, &value, sizeof value); };
    put(0, 0x20534444);                               // "DDS "
    put(4, 124);                                      // the header's size
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x8 | 0x20000); // caps, height, width, pixel format, pitch, mip count
    put(12, height);
    put(16, width);
    put(20, width * 4);
    put(28, pictures);
    put(76, 32);         // the pixel format's size
    put(80, 0x41);       // RGB with alpha
    put(88, 32);         // bits a pixel
    put(92, 0x00FF0000); // red
    put(96, 0x0000FF00);
    put(100, 0x000000FF);
    put(104, 0xFF000000);
    put(108, 0x1000 | 0x8 | 0x400000); // a texture, complex, with mip maps
    for (std::uint32_t level = 0; level < pictures; ++level)
    {
        for (const std::array<float, 4> &pixel : picture)
        {
            for (const std::size_t channel : {std::size_t{2}, std::size_t{1}, std::size_t{0}, std::size_t{3}})
            {
                file.push_back(static_cast<std::byte>(NearestByte(pixel.at(channel) * 255.0F)));
            }
        }
        picture = Halved(picture, width, height);
        width = std::max(width / 2, 1U);
        height = std::max(height / 2, 1U);
    }
    return file;
}

// The game's own picture of a lightmap brought to a map's size: each texel
// the mean of what it covers.
void Shrink(const core::dds::Image &image, Map &map)
{
    map.own.assign(map.texels.size(), {});
    map.sun.assign(map.texels.size(), 0);
    const std::uint32_t across = std::max(image.width / map.width, 1U);
    const std::uint32_t down = std::max(image.height / map.height, 1U);
    for (std::uint32_t y = 0; y < map.height; ++y)
    {
        for (std::uint32_t x = 0; x < map.width; ++x)
        {
            std::array<float, 4> sum{};
            for (std::uint32_t dy = 0; dy < down; ++dy)
            {
                for (std::uint32_t dx = 0; dx < across; ++dx)
                {
                    const std::size_t at = ((std::size_t{std::min((y * down) + dy, image.height - 1)} * image.width) +
                                            std::min((x * across) + dx, image.width - 1)) *
                                           4;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        sum.at(channel) += static_cast<float>(image.rgba[at + channel]);
                    }
                }
            }
            const float count = static_cast<float>(across * down) * 255.0F;
            const std::size_t to = (std::size_t{y} * map.width) + x;
            map.own[to] = {sum[0] / count, sum[1] / count, sum[2] / count};
            map.sun[to] = NearestByte(sum[3] / static_cast<float>(across * down));
        }
    }
}

// The triangles of a glowing thing, each with the area of those up to and
// with it, in square metres: a place along that sum is a triangle, a larger
// one the likelier.
using GlowingParts = std::vector<std::pair<float, const Level::Drawn *>>;

// Those of a model's triangles that a rule says glow.
GlowingParts PartsThatGlow(const std::vector<Level::Drawn> &drawn, const level::Placed &placed, const Glow &rule)
{
    GlowingParts parts;
    float area = 0;
    // By its material's name, or by the model's own: a house's lit
    // window in Orzammar (dne_window*) is of the material its walls
    // are of, and no material's name tells it from them.
    const bool whole = Lower(placed.file).find(Lower(rule.named)) != std::wstring::npos;
    for (const Level::Drawn &triangle : drawn)
    {
        if (!whole && triangle.material->find(Lower(rule.named)) == std::wstring::npos)
        {
            continue;
        }
        const core::Point across =
            Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a));
        area += 0.5F * std::sqrt(Dot(across, across));
        parts.emplace_back(area, &triangle);
    }
    return parts;
}

// What a rule makes of a model: the part of it that glows, as points spread
// evenly over its surface; nothing for a model with none of it.
// @param parts those of the model's triangles that the rule says glow
// @param seed what the points' chances are drawn by, each thing's its own
std::optional<Glowing> GlowOf(const GlowingParts &parts, const Glow &rule, std::uint32_t seed)
{
    const float area = parts.empty() ? 0.0F : parts.back().first;
    if (parts.empty() || area < 1e-4F)
    {
        return std::nullopt;
    }
    Glowing glow;
    glow.colour = rule.colour;
    glow.own = rule.own;
    // A thing lit by its own glow is found again by its points
    // (Straight), and wants them close together.
    const float density = rule.own > 0 ? kOwnPointsAMetre : kGlowPointsAMetre;
    const auto count = static_cast<std::size_t>(std::clamp(area * density, 4.0F, 256.0F));
    glow.patch = area / static_cast<float>(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const float along = (static_cast<float>(i) + 0.5F) / static_cast<float>(count) * area;
        const auto part = std::ranges::lower_bound(parts, along, {}, [](const auto &entry) { return entry.first; });
        const Level::Drawn &triangle = *(part != parts.end() ? part : parts.end() - 1)->second;
        float b = Chance(seed, static_cast<std::uint32_t>(i), 1);
        float c = Chance(seed, static_cast<std::uint32_t>(i), 2);
        if (b + c > 1)
        {
            b = 1 - b;
            c = 1 - c;
        }
        core::Point point{};
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            point.at(axis) = ((1 - b - c) * triangle.corners.a.at(axis)) + (b * triangle.corners.b.at(axis)) +
                             (c * triangle.corners.c.at(axis));
            glow.middle.at(axis) += point.at(axis) / static_cast<float>(count);
        }
        glow.points.push_back(point);
        glow.facings.push_back(
            Unit(Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a))));
    }
    for (const core::Point &point : glow.points)
    {
        const core::Point apart = Sub(point, glow.middle);
        glow.reach = std::max(glow.reach, std::sqrt(Dot(apart, apart)));
    }
    return glow;
}

// What glows among the models a room places, each model one glowing thing:
// into the scene as the points it lights by, and as the triangles it is.
void FindGlows(Level &level, const std::wstring &id, const Options &options, Scene &scene)
{
    const level::Room *room = level.RoomOf(id);
    if (room == nullptr || options.glows.empty())
    {
        return;
    }
    for (const level::Placed &placed : room->models)
    {
        const std::vector<Level::Drawn> drawn = level.TrianglesOf(id, placed);
        for (const Glow &rule : options.glows)
        {
            const GlowingParts parts = PartsThatGlow(drawn, placed, rule);
            const auto number = static_cast<std::uint32_t>(scene.glows.size());
            if (std::optional<Glowing> glow = GlowOf(parts, rule, number))
            {
                for (const auto &[area, triangle] : parts)
                {
                    scene.shining.triangles.push_back(triangle->corners);
                    scene.shining.glow.push_back(number);
                }
                scene.glows.push_back(std::move(*glow));
            }
        }
    }
}

// The light at a place for a figure standing there, as a probe: the lights
// that reach the place straight, each from its own side, and what the
// surfaces about send on of the light the lightmaps have for them. A probe
// baked from the lightmaps alone takes each side of a figure to be lit as
// the surface behind it is, which is nothing of a lamp's own light or the
// moon's falling on the figure itself: the hero stood in a patch of
// moonlight as dark as the room about it (the user, 2026-10-11: "char is
// dark despite standing in light").
//
// The numbers are a probe's as LightProbes.h has them: a light that gives a
// surface facing it `strength` is pi times that, by the harmonics of the way
// to it; what a surface sends on comes from where the ray lands.
std::optional<probes::Coefficients> ProbeAt(const Scene &scene, std::uint32_t room, const core::Point &place,
                                            std::span<const core::Point> rays, const Options &options)
{
    probes::Coefficients light{};
    const auto add = [&](const core::Point &way, const Colour &colour, float by) {
        const std::array<float, 9> harmonics = probes::Harmonics(way);
        for (std::size_t term = 0; term < light.size(); ++term)
        {
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                light.at(term).at(channel) += harmonics.at(term) * colour.at(channel) * by;
            }
        }
    };
    Direct(scene, room, place, nullptr, options,
           [&](const core::Point &way, const Colour &strength) { add(way, strength, std::numbers::pi_v<float>); });
    // And what glows: a figure by Orzammar's lava is lit by the lava, and one
    // under a window by the sky through it, which no lightmap of a surface
    // behind the figure says.
    Glowed(scene, room, place, nullptr, 0,
           [&](const core::Point &way, const Colour &strength) { add(way, strength, std::numbers::pi_v<float>); });
    const float each = 4 * std::numbers::pi_v<float> / static_cast<float>(rays.size());
    std::size_t close = 0;
    for (const core::Point &ray : rays)
    {
        const std::optional<Landing> landing = Cast(scene, room, place, ray, std::numeric_limits<float>::infinity());
        if (!landing)
        {
            continue;
        }
        close += landing->hit.distance < 0.5F ? 1 : 0;
        const level::LitTriangle &triangle = landing->geometry->lit[landing->hit.triangle];
        const auto map = scene.maps.find(triangle.atlas);
        if (map == scene.maps.end())
        {
            continue;
        }
        const float w = 1 - landing->hit.u - landing->hit.v;
        const Colour &there =
            At(map->second, map->second.whole,
               (w * triangle.uv[0][0]) + (landing->hit.u * triangle.uv[1][0]) + (landing->hit.v * triangle.uv[2][0]),
               (w * triangle.uv[0][1]) + (landing->hit.u * triangle.uv[1][1]) + (landing->hit.v * triangle.uv[2][1]));
        add(ray, there, options.surfaces * each);
    }
    if (static_cast<float>(close) > 0.3F * static_cast<float>(rays.size()))
    {
        return std::nullopt; // closed in: inside a pillar or a wall
    }
    return light;
}

// The probes of a level of rooms, a room's worth for each room that has a
// probe of the level's own (which is how the plugin knows a figure's room).
std::vector<probebaker::Room> Probes(Level &level, const Scene &scene, std::span<const std::wstring> ids,
                                     const Options &options)
{
    constexpr float kSpacing = 2.5F; // metres: a pool of candlelight is a few across
    const std::vector<core::Point> rays = probes::Directions(128);
    std::vector<probebaker::Room> rooms;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const std::vector<OwnProbe> own = level.ProbesOf(ids[i]);
        const Geometry *geometry = scene.rooms[i].geometry;
        if (own.empty() || geometry == nullptr || !geometry->tree || geometry->lit.empty())
        {
            continue;
        }
        probebaker::Room room;
        room.key = own.front().at;
        room.own = own.front().light;
        room.whole = true;
        room.relit = true;
        const std::vector<core::Point> places = probes::FloorPlaces(*geometry->tree, kSpacing);
        std::vector<std::optional<probes::Coefficients>> lights(places.size());
        ForEach(places.size(), [&](std::size_t at) {
            lights[at] = ProbeAt(scene, static_cast<std::uint32_t>(i), places[at], rays, options);
        });
        for (std::size_t at = 0; at < places.size(); ++at)
        {
            if (lights[at])
            {
                room.probes.push_back({places[at], probes::Matrices(*lights[at])});
            }
        }
        rooms.push_back(std::move(room));
    }
    return rooms;
}

// The probes of a level of terrain, a room's worth for each probe of the
// level's own, as the plugin's own bake of such a level has them
// (ProbeBaker.cpp): each place stood on is the room's whose own probe is
// nearest. They stand four metres apart, half the plugin's own spacing: a
// torch's pool is a few metres across.
std::vector<probebaker::Room> ProbesOfTerrain(Level &level, const Scene &scene, std::span<const std::wstring> ids,
                                              const Options &options)
{
    constexpr float kSpacing = 4.0F;
    const std::vector<core::Point> rays = probes::Directions(128);
    const std::vector<OwnProbe> owns = level.ProbesOf(L"");
    std::vector<probebaker::Room> rooms(owns.size());
    for (std::size_t i = 0; i < owns.size(); ++i)
    {
        rooms[i].key = owns[i].at;
        rooms[i].own = owns[i].light;
        rooms[i].whole = true;
        rooms[i].relit = true;
    }
    if (owns.empty())
    {
        return rooms;
    }
    for (std::size_t i = 0; i < scene.rooms.size(); ++i)
    {
        const Geometry *geometry = scene.rooms[i].geometry;
        if (ids[i].empty() || geometry == nullptr || !geometry->tree || geometry->lit.empty())
        {
            continue; // nobody stands on the backdrop
        }
        std::vector<const Geometry *> about{geometry};
        for (const std::size_t other : scene.rooms[i].about)
        {
            if (other != i && scene.rooms[other].geometry != nullptr)
            {
                about.push_back(scene.rooms[other].geometry);
            }
        }
        const std::vector<core::Point> places = probebaker::PlacesOn(about, kSpacing);
        std::vector<std::optional<probes::Coefficients>> lights(places.size());
        ForEach(places.size(), [&](std::size_t at) {
            lights[at] = ProbeAt(scene, static_cast<std::uint32_t>(i), places[at], rays, options);
        });
        for (std::size_t at = 0; at < places.size(); ++at)
        {
            if (!lights[at])
            {
                continue;
            }
            std::size_t own = 0;
            float nearest = std::numeric_limits<float>::infinity();
            for (std::size_t o = 0; o < owns.size(); ++o)
            {
                const core::Point apart = Sub(owns[o].at, places[at]);
                const float far = Dot(apart, apart);
                if (far < nearest)
                {
                    nearest = far;
                    own = o;
                }
            }
            rooms[own].probes.push_back({places[at], probes::Matrices(*lights[at])});
        }
    }
    std::erase_if(rooms, [](const probebaker::Room &room) { return room.probes.empty(); });
    return rooms;
}

// A tile's place among the tiles by its name, <x>_<y>_<detail>, whichever
// its detail (LevelFiles.h's TileOf knows the first alone).
std::optional<Tile> TileOfAnyDetail(std::wstring_view id)
{
    const std::size_t last = id.rfind(L'_');
    if (last == std::wstring_view::npos || last + 1 >= id.size())
    {
        return std::nullopt;
    }
    return TileOf(std::wstring(id.substr(0, last)) + L"_0");
}

// The rooms a ray from a room may land in, the room itself the first.
// @param numbers each of the world's rooms' place in `ids`, by its name
// @param room the room's own place in `ids`
std::vector<std::size_t> RoomsAbout(Level &level, const std::vector<std::wstring> &ids,
                                    const std::map<std::wstring, std::size_t> &numbers, std::size_t room)
{
    std::vector<std::size_t> about{room};
    for (const std::wstring &seen : level.SeenFrom(ids[room]))
    {
        if (const auto found = numbers.find(seen); found != numbers.end())
        {
            about.push_back(found->second);
        }
    }
    // A tile's far detail has the tiles about it by its name; and the
    // level's own room, its backdrop and the shell of its cave, is about
    // every room as every room is about it.
    if (const std::optional<Tile> tile =
            level.Terrain() && !TileOf(ids[room]) ? TileOfAnyDetail(ids[room]) : std::nullopt)
    {
        for (std::size_t other = 0; other < ids.size(); ++other)
        {
            const std::optional<Tile> there = other != room ? TileOfAnyDetail(ids[other]) : std::nullopt;
            if (there && std::abs(there->x - tile->x) <= kTilesAbout && std::abs(there->y - tile->y) <= kTilesAbout)
            {
                about.push_back(other);
            }
        }
    }
    for (std::size_t other = 0; other < ids.size(); ++other)
    {
        if (other != room && (ids[room].empty() || ids[other].empty()))
        {
            about.push_back(other);
        }
    }
    return about;
}

// The lamps a room's layouts have. A tile's lamps are in the layout its
// definition places, and in a terrain level that is often the layout of the
// tile's other detail alone (Orzammar's Commons: <x>_<y>_1_ll in the level's
// own archive, 60 lamps in all, of which the tiles seen from near name 27).
std::vector<Lamp> LampsOfRoom(Level &level, const std::wstring &id)
{
    std::vector<Lamp> lamps = level.LampsOf(id);
    if (level.Terrain() && id.ends_with(L"_0"))
    {
        const std::vector<Lamp> others = level.LampsOf(id.substr(0, id.size() - 1) + L"1");
        lamps.insert(lamps.end(), others.begin(), others.end());
    }
    return lamps;
}

// The first step of lighting a world: its rooms into the scene, each with
// its triangles, the box about them and the rooms about it, and with them
// the lamps of the level's own and what glows.
void GatherRooms(Level &level, Scene &scene, const Options &options, const std::vector<std::wstring> &ids)
{
    scene.rooms.resize(ids.size());
    std::map<std::wstring, std::size_t> numbers;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        numbers.emplace(ids[i], i);
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        RoomLit &lit = scene.rooms[i];
        lit.geometry = &level.GeometryOf(ids[i]);
        for (const level::LitTriangle &triangle : lit.geometry->lit)
        {
            lit.box.Add(triangle.corners.a);
            lit.box.Add(triangle.corners.b);
            lit.box.Add(triangle.corners.c);
        }
        lit.about = RoomsAbout(level, ids, numbers, i);
        for (const Lamp &lamp : LampsOfRoom(level, ids[i]))
        {
            AddLamp(scene, lamp, options);
        }
        FindGlows(level, ids[i], options, scene);
    }
    for (const Lamp &lamp : level.LampsOf(L"")) // the level's own layout, where it has one
    {
        AddLamp(scene, lamp, options);
    }
}

#ifdef ELGARA_EMBREE
// Makes Embree's scene of the world for the rays to be cast by, and says how
// long that took.
// @param name the level's, for the log
// @return whether there is one: the rays are DAOLib's tree's where not
bool CastByEmbree(Embree &held, Scene &scene, const std::string &name)
{
    const auto before = std::chrono::steady_clock::now();
    scene.embree = MakeEmbree(held, scene);
    if (scene.embree == nullptr)
    {
        return false;
    }
    spdlog::info(
        "relight: {}: Embree's scene made in {} ms", name,
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count());
    return true;
}
#endif

// The level's middle: the middle of its rooms' boxes together.
core::Point MiddleOf(const Scene &scene)
{
    Box all;
    for (const RoomLit &lit : scene.rooms)
    {
        if (lit.geometry != nullptr && !lit.geometry->lit.empty())
        {
            all.Add(lit.box.low);
            all.Add(lit.box.high);
        }
    }
    return {(all.low[0] + all.high[0]) / 2, (all.low[1] + all.high[1]) / 2, (all.low[2] + all.high[2]) / 2};
}

constexpr float kInFromTheEdge = 0.92F; // of a window's width and height, what its opening is

// A window as the flat it is: its model's own three ways in the level, and
// how far the opening's triangles go along each.
struct Flat
{
    core::Point origin{};              // the model's own place in the level
    std::array<core::Point, 3> ways{}; // its three ways, each of length 1
    std::array<float, 3> low{};        // the least and the most its triangles go along each, from the origin
    std::array<float, 3> high{};
    std::size_t thin = 0; // the way it goes least along: its thickness, through the wall
    std::size_t u = 0;    // and the two ways it is wide and high along
    std::size_t v = 0;

    float Wide() const
    {
        return high.at(u) - low.at(u);
    }
    float High() const
    {
        return high.at(v) - low.at(v);
    }
    float Thick() const
    {
        return high.at(thin) - low.at(thin);
    }
    // A place of the flat: `su` and `sv` from -1 to 1 across it and up it,
    // a little in from its edges, and `off` metres out of it along its
    // thickness.
    core::Point Corner(float su, float sv, float off) const
    {
        const float alongU = ((low.at(u) + high.at(u)) / 2) + (su * kInFromTheEdge * (high.at(u) - low.at(u)) / 2);
        const float alongV = ((low.at(v) + high.at(v)) / 2) + (sv * kInFromTheEdge * (high.at(v) - low.at(v)) / 2);
        const float alongThin = ((low.at(thin) + high.at(thin)) / 2) + off;
        core::Point at = origin;
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            at.at(axis) +=
                (ways.at(u).at(axis) * alongU) + (ways.at(v).at(axis) * alongV) + (ways.at(thin).at(axis) * alongThin);
        }
        return at;
    }
};

// The flat of a window's model, by those of its triangles that are the
// opening's.
Flat FlatOf(const level::Room &room, const level::Placed &placed, std::span<const core::Triangle> own)
{
    // The model's own three ways in the level, and how far it
    // goes along each: the least is its thickness.
    Flat flat;
    flat.origin = level::InLevel(room, placed, {0, 0, 0});
    flat.ways = {Unit(Sub(level::InLevel(room, placed, {1, 0, 0}), flat.origin)),
                 Unit(Sub(level::InLevel(room, placed, {0, 1, 0}), flat.origin)),
                 Unit(Sub(level::InLevel(room, placed, {0, 0, 1}), flat.origin))};
    flat.low.fill(std::numeric_limits<float>::infinity());
    flat.high.fill(-std::numeric_limits<float>::infinity());
    for (const core::Triangle &triangle : own)
    {
        for (const core::Point *corner : {&triangle.a, &triangle.b, &triangle.c})
        {
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                const float along = Dot(Sub(*corner, flat.origin), flat.ways.at(axis));
                flat.low.at(axis) = std::min(flat.low.at(axis), along);
                flat.high.at(axis) = std::max(flat.high.at(axis), along);
            }
        }
    }
    for (std::size_t axis = 1; axis < 3; ++axis)
    {
        flat.thin = flat.high.at(axis) - flat.low.at(axis) < flat.high.at(flat.thin) - flat.low.at(flat.thin)
                        ? axis
                        : flat.thin;
    }
    flat.u = (flat.thin + 1) % 3;
    flat.v = (flat.thin + 2) % 3;
    return flat;
}

// Which side of a window the room is on.
struct Sides
{
    int ahead = 0;          // rays from the window's middle that land on something the way of its thickness
    int behind = 0;         // and the other way
    bool roomAhead = false; // the room is the way of its thickness
};

// Which side is the room: the one that is closed in. Outside a
// level there is nothing at all, so rays cast from the window's
// middle land on something on the room's side and on nothing on
// the other. (Taken the other way about, by where rays went
// furthest, every window faced out and no light came in:
// 2026-10-11.)
// @param room the number of the room that places the window
// @param across the way of the window's thickness
Sides SidesOf(const Scene &scene, std::uint32_t room, const core::Point &middle, const core::Point &across,
              const core::Point &levelMiddle)
{
    constexpr float kLooked = 60.0F;
    Sides sides;
    for (const core::Point &ray : probes::Directions(128))
    {
        if (Cast(scene, room, middle, ray, kLooked))
        {
            ++(Dot(ray, across) > 0 ? sides.ahead : sides.behind);
        }
    }
    // And where rays land on both sides, as they do from a window
    // high in a tower's outer wall, with the level's other rooms
    // behind it across the round: a window is in a level's outer
    // wall, and the room is towards the level's middle.
    sides.roomAhead = std::abs(sides.ahead - sides.behind) > 48 ? sides.ahead > sides.behind
                                                                : Dot(Sub(levelMiddle, middle), across) > 0;
    return sides;
}

// The sky's own light through an opening, as a glowing thing: points over
// the opening, `shift` out of the window's flat, that send into the room.
Glowing SkyThrough(const Flat &flat, const Colour &sky, float shift, const core::Point &inward)
{
    constexpr int kAcross = 6;
    constexpr int kUp = 8;
    Glowing glow;
    glow.sky = true;
    glow.colour = sky;
    glow.middle = flat.Corner(0, 0, shift);
    glow.patch = kInFromTheEdge * kInFromTheEdge * flat.Wide() * flat.High() / static_cast<float>(kAcross * kUp);
    for (int i = 0; i < kAcross; ++i)
    {
        for (int j = 0; j < kUp; ++j)
        {
            const core::Point at = flat.Corner(((static_cast<float>(i) + 0.5F) / kAcross * 2) - 1,
                                               ((static_cast<float>(j) + 0.5F) / kUp * 2) - 1, shift);
            glow.points.push_back(at);
            glow.facings.push_back(inward);
            const core::Point apart = Sub(at, glow.middle);
            glow.reach = std::max(glow.reach, std::sqrt(Dot(apart, apart)));
        }
    }
    return glow;
}

// The openings of a light from far off, as they are found window by window.
struct Openings
{
    std::vector<core::Triangle> flats; // two triangles an opening
    std::vector<core::Triangle> bars;  // what stands in them: the windows' own triangles
    std::vector<core::Point> outward;  // each opening's way out of the level
    std::size_t faced = 0;             // openings with a face of the wall before them
};

// Takes a window as an opening of a light from far off, and where the light
// has the sky's with it, as a glowing thing of the scene's.
// @param room the number of the room that places the window
void AddOpening(Scene &scene, const Beam &beam, std::uint32_t room, const Flat &flat, const core::Point &levelMiddle,
                Openings &found)
{
    // A window stands in the thickness of its wall, in
    // this level a little behind the wall's own face: where there is
    // a face close before it on the room's side, the opening is put
    // just clear of that; else just clear of the model.
    const core::Point across = flat.ways.at(flat.thin);
    const core::Point back{-across[0], -across[1], -across[2]};
    const core::Point middle = flat.Corner(0, 0, 0);
    const Sides sides = SidesOf(scene, room, middle, across, levelMiddle);
    const core::Point inward = sides.roomAhead ? across : back;
    constexpr float kFaceWithin = 1.5F; // metres: a face of the wall before the window
    constexpr float kClear = 0.15F;     // metres the opening stands before it
    const std::optional<Landing> face = Cast(scene, room, middle, inward, kFaceWithin);
    const float proud = face ? face->hit.distance + kClear : (flat.Thick() / 2) + kClear;
    found.faced += face ? 1 : 0;
    const float shift = sides.roomAhead ? proud : -proud;
    found.flats.push_back({flat.Corner(-1, -1, shift), flat.Corner(1, -1, shift), flat.Corner(1, 1, shift)});
    found.flats.push_back({flat.Corner(-1, -1, shift), flat.Corner(1, 1, shift), flat.Corner(-1, 1, shift)});
    found.outward.push_back({-inward[0], -inward[1], -inward[2]});
    if (std::max({beam.sky[0], beam.sky[1], beam.sky[2]}) > 0)
    {
        // And as the pane a picture of the room sees it by: the opening's own two triangles.
        scene.shining.triangles.insert(scene.shining.triangles.end(), found.flats.end() - 2, found.flats.end());
        scene.shining.glow.insert(scene.shining.glow.end(), 2, static_cast<std::uint32_t>(scene.glows.size()));
        scene.glows.push_back(SkyThrough(flat, beam.sky, shift, inward));
    }
    spdlog::debug("relight: an opening at {:.1f} {:.1f} {:.1f}, {:.1f} by {:.1f} m, rays landing {} to {}, "
                  "a face {:.2f} m before it",
                  middle[0], middle[1], middle[2], flat.Wide(), flat.High(), std::max(sides.ahead, sides.behind),
                  std::min(sides.ahead, sides.behind), face ? face->hit.distance : -1.0F);
}

// The openings of a light from far off, the whole world's together. A
// window as the level has it is a model of its tracery alone, gilded bars
// before a dark pane that is the wall's own: with the bars themselves
// taken for the opening, the moon lit slivers of sill and arch beside
// them and nothing else (the user, 2026-10-11: "where's this random blue
// from?"). So a window's opening is the flat of its model, as wide and as
// high as the model is, a little in from its edges; and the bars are
// what stands in it, and shade what is behind them.
Openings OpeningsOf(Level &level, Scene &scene, const Beam &beam, const std::vector<std::wstring> &ids,
                    const core::Point &levelMiddle)
{
    Openings found;
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        const std::wstring &id = ids[index];
        const level::Room *room = level.RoomOf(id);
        for (const level::Placed &placed : room != nullptr ? room->models : std::vector<level::Placed>{})
        {
            std::vector<core::Triangle> own;
            for (const Level::Drawn &drawn : level.TrianglesOf(id, placed))
            {
                if (drawn.material->find(Lower(beam.named)) != std::wstring::npos)
                {
                    own.push_back(drawn.corners);
                }
            }
            if (own.empty())
            {
                continue;
            }
            AddOpening(scene, beam, static_cast<std::uint32_t>(index), FlatOf(*room, placed, own), levelMiddle, found);
            found.bars.insert(found.bars.end(), own.begin(), own.end());
        }
    }
    return found;
}

// The lights from far off into the scene, each with its openings.
// @param name the level's, for the log
void AddBeams(Level &level, Scene &scene, const Options &options, const std::vector<std::wstring> &ids,
              const std::string &name)
{
    const core::Point levelMiddle = MiddleOf(scene);
    for (const Beam &beam : options.beams)
    {
        Openings found = OpeningsOf(level, scene, beam, ids, levelMiddle);
        Beaming beaming;
        beaming.toSource = Unit({-beam.travels[0], -beam.travels[1], -beam.travels[2]});
        beaming.colour = beam.colour;
        spdlog::info(
            "relight: {}: a light from far off through {} openings of {}, {} of them behind a face of the wall", name,
            found.flats.size() / 2, Text(beam.named), found.faced);
        if (!found.flats.empty())
        {
            beaming.openings.emplace(found.flats);
            beaming.bars.emplace(found.bars);
            beaming.outward = std::move(found.outward);
            scene.beams.push_back(std::move(beaming));
        }
    }
}

// Where the level's makers put a light, whether or not it is taken as one:
// it says where a fitting's flame is.
std::vector<core::Point> FlameHints(Level &level, const std::vector<std::wstring> &sources)
{
    std::vector<core::Point> hints;
    for (const std::wstring &id : sources)
    {
        for (const std::wstring &layout :
             {id, level.Terrain() && id.ends_with(L"_0") ? id.substr(0, id.size() - 1) + L"1" : id})
        {
            for (const Lamp &lamp : level.LampsOf(layout))
            {
                hints.push_back(lamp.at);
            }
        }
    }
    return hints;
}

// Where a light that lands is put: at the height of the first surface
// straight under its place, or at its place where there is none.
// @param room the number of the room the look down is from
core::Point LandedUnder(const Scene &scene, std::uint32_t room, core::Point at)
{
    constexpr float kFrom = 0.5F;   // metres over the place the look down starts from
    constexpr float kDeep = 200.0F; // and how far down it looks
    const core::Point from{at[0], at[1], at[2] + kFrom};
    if (const std::optional<Landing> floor = Cast(scene, room, from, {0, 0, -1}, kDeep))
    {
        at[2] = from[2] - floor->hit.distance;
    }
    return at;
}

// Where the flames of one kind are: every model the rooms `sources` place
// whose name the rule has, each where it burns.
std::vector<core::Point> FlamesOf(Level &level, const Scene &scene, const Flame &flame,
                                  const std::vector<std::wstring> &ids, const std::vector<std::wstring> &sources,
                                  std::span<const core::Point> hints)
{
    std::vector<core::Point> flames;
    for (const std::wstring &id : sources)
    {
        const level::Room *room = level.RoomOf(id);
        for (const level::Placed &placed : room != nullptr ? room->models : std::vector<level::Placed>{})
        {
            if (Lower(placed.file).find(Lower(flame.named)) == std::wstring::npos)
            {
                continue;
            }
            core::Point at = WhereItBurns(level, id, *room, placed, hints);
            if (flame.lands > 0)
            {
                const auto index = static_cast<std::uint32_t>(
                    std::ranges::find(ids, id) != ids.end() ? std::ranges::find(ids, id) - ids.begin() : 0);
                at = LandedUnder(scene, index, at);
            }
            flames.push_back(at);
        }
    }
    return flames;
}

// The level's flames into the scene as lamps, a kind at a time.
void PlaceFlames(Level &level, Scene &scene, const Options &options, const std::vector<std::wstring> &ids,
                 const std::vector<std::wstring> &sources)
{
    const std::vector<core::Point> hints = FlameHints(level, sources);
    for (const Flame &flame : options.flames)
    {
        const std::vector<core::Point> flames = FlamesOf(level, scene, flame, ids, sources, hints);
        if (!flames.empty())
        {
            spdlog::info("relight: {} flames of {} found, before those an earlier kind lit", flames.size(),
                         std::filesystem::path(flame.named).string());
            for (const core::Point &at : flames)
            {
                spdlog::debug("relight:   {} at {:.1f} {:.1f} {:.1f}", std::filesystem::path(flame.named).string(),
                              at[0], at[1], at[2]);
            }
        }
        // What an earlier rule lit is this rule's "already lit": a lantern's
        // rule before the candles' keeps the candle inside it from counting twice.
        AddFlames(scene, flames, flame, scene.lamps.size());
    }
}

// The lightmaps, each at its own shape, no larger a side than asked, with
// the game's own picture brought to that size.
void MakeMaps(Level &level, Scene &scene, const Options &options)
{
    for (std::uint32_t atlas = 0; atlas < level.Atlases(); ++atlas)
    {
        const std::optional<std::vector<std::byte>> file = level.Find(level.AtlasName(atlas));
        const std::optional<core::dds::Image> image = file ? core::dds::Decode(*file) : std::nullopt;
        if (!image)
        {
            continue;
        }
        Map map;
        map.width = image->width;
        map.height = image->height;
        while ((map.width > options.side || map.height > options.side) && map.width % 2 == 0 && map.height % 2 == 0)
        {
            map.width /= 2;
            map.height /= 2;
        }
        map.texels.resize(std::size_t{map.width} * map.height);
        Shrink(*image, map);
        scene.maps.emplace(atlas, std::move(map));
    }
}

// Every covered texel of the scene's lightmaps, for the threads to share out.
using Order = std::vector<std::pair<Map *, std::uint32_t>>;

// Lays every room into the lightmaps and makes the tables the light is
// reckoned into.
// @return the texels to light
Order LayRooms(Scene &scene)
{
    for (std::size_t i = 0; i < scene.rooms.size(); ++i)
    {
        Lay(scene, static_cast<std::uint32_t>(i), *scene.rooms[i].geometry);
    }
    // A lightmap none of this world's rooms is drawn with is let go before
    // the light's own tables are made: the far world of a level of terrain
    // has the near world's lightmaps by name and lays nothing into them, and
    // at 512 a side the tables for all of them were more than a 32-bit
    // process has (the bake ended with 0xC0000409, 2026-10-11).
    std::erase_if(scene.maps, [](const auto &entry) {
        return std::ranges::none_of(entry.second.texels, [](const Texel &texel) { return texel.covered; });
    });
    Order order;
    for (auto &[atlas, map] : scene.maps)
    {
        map.straight.assign(map.texels.size(), {});
        map.glowed.assign(map.texels.size(), {});
        for (std::uint32_t i = 0; i < map.texels.size(); ++i)
        {
            if (map.texels[i].covered)
            {
                order.emplace_back(&map, i);
            }
        }
    }
    return order;
}

// The light that comes straight to every texel, a lamp's and a glow's, the
// glows' smoothed first; and spread out past the triangles' edges, it is the
// whole of the light so far and what the first bounce reads.
void LightStraight(Scene &scene, const Order &order, const Options &options)
{
    ForEach(order.size(), [&](std::size_t i) {
        Map &map = *order[i].first;
        map.straight[order[i].second] = Straight(scene, map.texels[order[i].second], static_cast<std::uint32_t>(i),
                                                 options, map.glowed[order[i].second]);
    });
    Smooth(order, &Map::glowed, 2);
    for (const auto &[map, at] : order)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            map->straight[at].at(channel) += map->glowed[at].at(channel);
        }
    }
    for (auto &[atlas, map] : scene.maps)
    {
        Fill(map, map.straight);
        map.whole = map.straight;
        map.came = map.straight;
    }
}

// Follows the light off the surfaces once: what comes back to every texel
// of the light that came the time before, smoothed, spread out past the
// triangles' edges and added to the whole.
// @param bounce which time this is, from 0: each has chances of its own
void Bounce(Scene &scene, const Order &order, const Options &options, int bounce)
{
    std::vector<Colour> back(order.size());
    ForEach(order.size(), [&](std::size_t i) {
        back[i] = Back(scene, order[i].first->texels[order[i].second],
                       static_cast<std::uint32_t>(i) + (static_cast<std::uint32_t>(bounce) * 0x51ED27U), options);
    });
    for (auto &[atlas, map] : scene.maps)
    {
        map.came.assign(map.texels.size(), {});
    }
    for (std::size_t i = 0; i < order.size(); ++i)
    {
        order[i].first->came[order[i].second] = back[i];
    }
    Smooth(order, &Map::came, 3);
    for (auto &[atlas, map] : scene.maps)
    {
        Fill(map, map.came);
        for (std::size_t i = 0; i < map.whole.size(); ++i)
        {
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                map.whole[i].at(channel) += map.came[i].at(channel);
            }
        }
    }
}

// Writes the scene's lightmaps into a folder, each a DDS of its atlas's own
// name.
// @param done the lightmaps written so far, by name: none is written twice
// @return how many were written
std::size_t WriteMaps(const Level &level, const Scene &scene, const std::filesystem::path &out, const Options &options,
                      std::set<std::wstring> &done)
{
    std::error_code error;
    std::filesystem::create_directories(out, error);
    std::size_t written = 0;
    for (const auto &[atlas, map] : scene.maps)
    {
        if (std::ranges::none_of(map.texels, [](const Texel &texel) { return texel.covered; }) ||
            !done.insert(Lower(level.AtlasName(atlas))).second)
        {
            continue;
        }
        const std::vector<std::byte> file = DdsOf(map, options);
        std::ofstream stream(out / level.AtlasName(atlas), std::ios::binary);
        stream.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
        ++written;
    }
    return written;
}

// Writes the figures' probes, of the same lights, beside the lightmaps
// (<level>.elgp): the plugin takes them as they are in place of baking its
// own.
void WriteProbes(Level &level, const Scene &scene, const std::filesystem::path &levelRim,
                 const std::filesystem::path &out, const Options &options, const std::vector<std::wstring> &ids)
{
    const std::vector<probebaker::Room> lit =
        level.Terrain() ? ProbesOfTerrain(level, scene, ids, options) : Probes(level, scene, ids, options);
    std::size_t count = 0;
    for (const probebaker::Room &room : lit)
    {
        count += room.probes.size();
    }
    const std::vector<std::byte> file = probebaker::Write(lit, 1);
    std::ofstream stream(out / (levelRim.stem().wstring() + L".elgp"), std::ios::binary);
    stream.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    spdlog::info("relight: {}: {} probes in {} rooms", levelRim.stem().string(), count, lit.size());
}

// A world's light against vanilla's bake of it, over every texel lit: each
// channel's sum of both, and every texel's brightness in both.
struct Against
{
    std::array<double, 3> ours{};
    std::array<double, 3> vanillas{};
    std::vector<float> ourLuma;
    std::vector<float> vanillasLuma;
};

constexpr std::array<double, 3> kLuma{0.2126, 0.7152, 0.0722};

Against Measured(const Scene &scene)
{
    Against found;
    for (const auto &[atlas, map] : scene.maps)
    {
        for (std::size_t i = 0; i < map.texels.size(); ++i)
        {
            if (!map.texels[i].covered)
            {
                continue;
            }
            double mine = 0;
            double theirs = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                found.ours.at(channel) += double{map.whole[i].at(channel)};
                found.vanillas.at(channel) += double{map.own[i].at(channel)};
                mine += kLuma.at(channel) * double{map.whole[i].at(channel)};
                theirs += kLuma.at(channel) * double{map.own[i].at(channel)};
            }
            found.ourLuma.push_back(static_cast<float>(mine));
            found.vanillasLuma.push_back(static_cast<float>(theirs));
        }
    }
    return found;
}

// What a world's light is multiplied by to be brought to vanilla's bake: its
// colour by the means, and its brightness by the bright end, so that what is
// brighter than nineteen texels in twenty is as bright as vanilla's is. By
// the means alone the tower came out a third brighter where its light falls,
// since a level lit from its sources is darker than vanilla's bake where no
// light reaches and must then be brighter where it does (the user,
// 2026-10-11, of a pillar by a window: "the moonlight is too bright now").
// Nothing lit is to be brighter than vanilla shows the level's bright
// places; what is dark is left dark.
Colour GainTo(Against &measured, const Options &options)
{
    constexpr double kBrightEnd = 0.95;
    Colour found{1, 1, 1};
    if (measured.ourLuma.empty())
    {
        return found;
    }
    const auto at = static_cast<std::ptrdiff_t>(static_cast<double>(measured.ourLuma.size() - 1) * kBrightEnd);
    std::ranges::nth_element(measured.ourLuma, measured.ourLuma.begin() + at);
    std::ranges::nth_element(measured.vanillasLuma, measured.vanillasLuma.begin() + at);
    const auto oursBright = double{measured.ourLuma[static_cast<std::size_t>(at)]};
    const auto vanillasBright = double{measured.vanillasLuma[static_cast<std::size_t>(at)]};
    const double bright = oursBright > 0 ? vanillasBright / oursBright : 1.0;
    // The colour's part: each channel's mean against vanilla's, with the brightness of that taken out.
    double oursMean = 0;
    double vanillasMean = 0;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        oursMean += kLuma.at(channel) * measured.ours.at(channel);
        vanillasMean += kLuma.at(channel) * measured.vanillas.at(channel);
    }
    if (oursMean <= 0 || vanillasMean <= 0)
    {
        return found;
    }
    // What is kept of vanilla's bake is under ours (DdsOf): ours has the rest of the bright end.
    const double share = std::max(0.0, 1.0 - double{options.kept});
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        if (measured.ours.at(channel) > 0 && measured.vanillas.at(channel) > 0)
        {
            const double hue = (measured.vanillas.at(channel) / measured.ours.at(channel)) / (vanillasMean / oursMean);
            const float toHue = std::lerp(1.0F, static_cast<float>(hue), std::min(options.match, 1.0F));
            const float toBright =
                std::lerp(1.0F, static_cast<float>(share * bright), std::clamp(options.bright, 0.0F, 1.0F));
            found.at(channel) = toHue * toBright;
        }
    }
    return found;
}

// Brings a world's light to vanilla's bake (Options::match, GainTo): the
// lightmaps' light, and the lights themselves, which the probes are yet to
// be made of. Light adds up, so every source and what has come of it
// multiplied alike is the level lit by sources that much brighter or dimmer.
// @param gain what a level's first world was multiplied by, for its second
//  to be multiplied by the same; found here where it has no value yet
void MatchVanilla(Scene &scene, const Options &options, std::optional<Colour> &gain, const std::string &name)
{
    if (options.match <= 0)
    {
        return;
    }
    if (!gain)
    {
        Against measured = Measured(scene);
        const Colour found = GainTo(measured, options);
        gain = found;
        spdlog::info("relight: {}: brought to vanilla's bake: its light by {:.2f}, {:.2f}, {:.2f}", name, found[0],
                     found[1], found[2]);
    }
    const Colour by = *gain;
    const auto scaled = [&by](Colour &colour) {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            colour.at(channel) *= by.at(channel);
        }
    };
    for (auto &[atlas, map] : scene.maps)
    {
        std::ranges::for_each(map.whole, scaled);
    }
    for (Lamp &lamp : scene.lamps)
    {
        scaled(lamp.colour);
    }
    for (Glowing &glow : scene.glows)
    {
        scaled(glow.colour);
    }
    for (Beaming &beam : scene.beams)
    {
        scaled(beam.colour);
    }
}

// The level's reflection cubes (issue 9, 2026-10-12). What a level's metal
// mirrors is a cube map, a picture of a room all round from where the room's
// light-probe object stands (materials.hlsl's DaoReflected). Vanilla's is of
// the room as vanilla lit it, by lamps that did not ship, and in eight bits,
// a flame in it no brighter than a white wall: armour in a level lit again
// mirrored a room that is no longer there, and had no lamp to shine by. So
// each is made again here, of the level as the lightmaps above have it: a
// ray cast for every texel of every face, and what it sees.
//
// Which cube is which room's is the layouts' to say (read 2026-10-12 in
// lak303d's and orz100d's): a room's layout (<level>_<room>_ll.mmh; a level
// of terrain's own, <level>_ll.mmh) has the probe as a node "nlpr" whose
// parts are its place and the cube's file's name (EnvLightProbeEnvMap),
// which LevelFiles.h's ProbesOf already reads for the figures' probes. In
// every one of the game's 158 levels the cube maps in the level's archives
// are the ones its probes name, no more and no fewer.
//
// The numbers are the lightmaps': light, 1 the white of a white surface, kept
// as the square root of its share of kRelitMost.

constexpr float kLampDisc = 0.12F; // metres: the radius a lamp is drawn with, a flame's size about

// The way out of a cube's middle to a place on one of its faces, `across`
// and `down` from -1 to 1 over the face as its picture is stored, of length 1.
// The faces are Direct3D's, in its order (+x, -x, +y, -y, +z, -z), and the
// axes are the level's own, z up: read 2026-10-12 off vanilla's cubes, where
// the fifth face is a ceiling and the sixth a floor, the fourth stands
// upright, the third on its head, and the first two lie on their sides with
// the floor to the right and to the left.
core::Point WayOfCube(std::size_t face, float across, float down)
{
    switch (face)
    {
    case 0:
        return Unit({1, -down, -across});
    case 1:
        return Unit({-1, -down, across});
    case 2:
        return Unit({across, 1, down});
    case 3:
        return Unit({across, -1, -down});
    case 4:
        return Unit({across, -down, 1});
    default:
        return Unit({-across, -down, -1});
    }
}

// A lamp as a cube's picture has it. A lamp of the bake's is a point with no
// body, and a picture needs one: a disc that faces the eye, kLampDisc from
// its middle to its edge.
struct LampSeen
{
    core::Point way{}; // to it, of length 1
    float wide = 0;    // the angle from its middle to its edge
    // What the disc shows: the lamp's colour over the square of the disc's
    // radius. A lamp of colour c lights a surface that faces it from d metres
    // by c / d^2; a disc of radius r that shows v, as a white wall lit by v
    // does, lights it by v r^2 / d^2. So v = c / r^2 is the one brightness
    // at which what the picture sends is the light the bake's lamp gives: a
    // candle's 0.8 is 55 whites, a brazier's 16 over a thousand. That is past
    // what the file holds (kRelitMost) at the picture's full size; the
    // smaller pictures are means of the light and not of the file's bytes,
    // so a flame far off, a part of one texel, is there by its share.
    Colour shows{};
};

// The lamps in sight of a place: each with nothing between, by the ray its
// own light is shadowed by (LampAt), which stops short of the fitting the
// lamp stands in.
std::vector<LampSeen> LampsSeenFrom(const Scene &scene, std::uint32_t room, const core::Point &from,
                                    const Options &options)
{
    std::vector<LampSeen> seen;
    for (const Lamp &lamp : scene.lamps)
    {
        const core::Point to = Sub(lamp.at, from);
        const float far = std::sqrt(Dot(to, to));
        if (far <= kLampDisc)
        {
            continue;
        }
        const core::Point way{to[0] / far, to[1] / far, to[2] / far};
        const float shortOf = std::max(kShortOfLamp, lamp.clear);
        if (far > shortOf && Blocked(scene, room, from, way, far - shortOf))
        {
            continue;
        }
        // A lamp of the level's own is its colour by the look's `lamps` (LampAt).
        const float by = (lamp.core > 0 ? 1.0F : options.lamps) / (kLampDisc * kLampDisc);
        seen.push_back(
            {way, std::asin(kLampDisc / far), {by * lamp.colour[0], by * lamp.colour[1], by * lamp.colour[2]}});
    }
    return seen;
}

// The game's archives a surface's colour is read from (SurfaceColours.h's
// head has what is in which). Any may be missing: the surfaces whose colours
// it had are then the bake's grey.
struct PaintFiles
{
    std::optional<Archive> materials; // materialobjects.erf
    std::optional<Archive> pack;      // the texture pack
    std::optional<Archive> textures;  // textures.erf

    // A material object by the material's name: the game's, or the level's own.
    std::optional<std::vector<std::byte>> Material(const Level &level, const std::wstring &name) const
    {
        std::optional<std::vector<std::byte>> file = materials ? materials->Read(name + L".mao") : std::nullopt;
        return file ? file : level.Find(name + L".mao");
    }

    // A texture by its file's name: the game's, or the level's own.
    std::optional<std::vector<std::byte>> Texture(const Level &level, const std::wstring &file) const
    {
        for (const std::optional<Archive> *archive : {&pack, &textures})
        {
            if (std::optional<std::vector<std::byte>> read = *archive ? (*archive)->Read(file) : std::nullopt)
            {
                return read;
            }
        }
        return level.Find(file);
    }
};

// What paints a level's surfaces, each read once: a material's texture,
// nothing for one whose texture could not be found or read.
struct Paints
{
    std::map<std::wstring, std::optional<surfacecolours::Paint>> materials;
};

// The texture a material paints with, or null.
const surfacecolours::Paint *PaintOf(const Level &level, const PaintFiles &files, Paints &kept,
                                     const std::wstring &material)
{
    auto found = kept.materials.find(material);
    if (found == kept.materials.end())
    {
        std::optional<surfacecolours::Paint> paint;
        if (const std::optional<std::vector<std::byte>> mao = files.Material(level, material))
        {
            const std::wstring texture =
                surfacecolours::PaintNamed({reinterpret_cast<const char *>(mao->data()), mao->size()});
            const std::optional<std::vector<std::byte>> dds =
                texture.empty() ? std::nullopt : files.Texture(level, texture);
            paint = dds ? surfacecolours::Paint::Of(*dds) : std::nullopt;
            const Colour mean = paint ? paint->Mean() : Colour{};
            spdlog::debug("relight: the material {} is painted with {}, {}: {:.3f} {:.3f} {:.3f} in the mean",
                          Text(material), texture.empty() ? "nothing" : Text(texture), paint ? "read" : "not read",
                          mean[0], mean[1], mean[2]);
        }
        found = kept.materials.emplace(material, std::move(paint)).first;
    }
    return found->second ? &*found->second : nullptr;
}

// What of a world's lightmapped surface is painted, for PaintSurfaces.
struct Painted
{
    std::vector<std::vector<bool>> triangles; // each room's triangles: whether it has a colour of its own
    double area = 0;                          // square metres painted from a texture
    double whole = 0;                         // of them, by the whole texture's mean: where on it is not known
    double grey = 0;                          // square metres left the bake's grey
    std::size_t materials = 0;                // how many materials the surface is drawn with
    std::size_t read = 0;                     // and of how many the texture was read
    std::string unread;                       // the others' names, with commas between
};

// Gives the triangles of one part of a room (LevelFiles.h's Part) their
// colours: each the mean of its material's texture over where the triangle
// is laid on it, as light. A part whose material has no texture that could
// be read is left as it is.
// @param painted the room's triangles: which have been given a colour
void PaintPart(Level &level, const PaintFiles &files, Paints &kept, const Geometry &geometry, std::size_t index,
               std::vector<Colour> &sends, std::vector<bool> &painted, Painted &tally)
{
    const Part &part = geometry.parts[index];
    const std::size_t to = index + 1 < geometry.parts.size() ? geometry.parts[index + 1].from : geometry.lit.size();
    const ModelParts *model = level.PartsOf(part.placed->file);
    if (model == nullptr || part.chunk >= model->chunks.size())
    {
        return;
    }
    const level::MeshChunk &chunk = model->chunks[part.chunk];
    const std::wstring *material = Found(model->materials, Lower(chunk.name));
    const surfacecolours::Paint *paint = material != nullptr ? PaintOf(level, files, kept, *material) : nullptr;
    // Where each vertex of the chunk is on its material's texture, one a vertex,
    // or none for a chunk whose mesh file says nothing of it (DAOLib's Level.h).
    const std::vector<level::Uv> &uv = chunk.paintUv;
    for (std::size_t at = part.from; at < to; ++at)
    {
        const core::Triangle &corners = geometry.lit[at].corners;
        const core::Point across = Cross(Sub(corners.b, corners.a), Sub(corners.c, corners.a));
        const double area = 0.5 * double{std::sqrt(Dot(across, across))};
        if (paint == nullptr)
        {
            tally.grey += area;
            continue;
        }
        // The part's triangles are the chunk's, three of its indices each, in their order.
        const std::size_t first = 3 * (at - part.from);
        const bool placed = !uv.empty() && first + 2 < chunk.indices.size();
        sends[at] =
            placed ? paint->Over({uv[chunk.indices[first]], uv[chunk.indices[first + 1]], uv[chunk.indices[first + 2]]})
                   : paint->Mean();
        painted[at] = true;
        tally.area += area;
        tally.whole += placed ? 0.0 : area;
    }
}

// Gives every lightmapped triangle of the world its own colour, as light
// (2026-10-12): what a picture of the level sees it send on of the light on
// it, once BringToSurfaces has said how bright the colours are together. With
// every surface the bake's one grey, what armour mirrored indoors was a grey
// room, the red carpet of the tower's library grey stone.
//
// A surface whose material's texture could not be found or read is left the
// bake's grey, `surfaces`.
Painted PaintSurfaces(Level &level, Scene &scene, const PaintFiles &files, const Options &options)
{
    Paints kept;
    Painted tally;
    tally.triangles.resize(scene.rooms.size());
    scene.sends.assign(scene.rooms.size(), {});
    for (std::size_t room = 0; room < scene.rooms.size(); ++room)
    {
        const Geometry *geometry = scene.rooms[room].geometry;
        if (geometry == nullptr)
        {
            continue;
        }
        scene.sends[room].assign(geometry->lit.size(), {options.surfaces, options.surfaces, options.surfaces});
        tally.triangles[room].assign(geometry->lit.size(), false);
        for (std::size_t part = 0; part < geometry->parts.size(); ++part)
        {
            PaintPart(level, files, kept, *geometry, part, scene.sends[room], tally.triangles[room], tally);
        }
    }
    tally.materials = kept.materials.size();
    for (const auto &[material, paint] : kept.materials)
    {
        tally.read += paint ? 1 : 0;
        if (!paint)
        {
            tally.unread += (tally.unread.empty() ? "" : ", ") + Text(material);
        }
    }
    return tally;
}

// A texel of a cube's picture, by its number among the picture's, face by
// face and row by row.
struct CubeTexel
{
    core::Point way{}; // out of the cube's middle through the texel's, of length 1
    // 1 and the squares of how far across and down its face the texel is,
    // each from -1 to 1: a texel is a square 2 / side across on a face one
    // away from the middle, and from the middle it is smaller the further
    // out on its face, by this to the power of three halves.
    float out = 1;
};

CubeTexel TexelOf(std::size_t number, std::uint32_t side)
{
    const std::size_t aFace = std::size_t{side} * side;
    const std::size_t column = (number % aFace) % side;
    const std::size_t row = (number % aFace) / side;
    const float across = (((static_cast<float>(column) + 0.5F) / static_cast<float>(side)) * 2) - 1;
    const float down = (((static_cast<float>(row) + 0.5F) / static_cast<float>(side)) * 2) - 1;
    return {WayOfCube(number / aFace, across, down), 1 + (across * across) + (down * down)};
}

// A cube the level has a file for: where its light-probe object stands, in
// which room of the scene's, and the file's name.
struct CubeAt
{
    std::uint32_t room = 0;
    core::Point at{};
    std::wstring file;
};

// The level's cubes, each once: only a cube the level has a file for, whose
// stand-in ours is.
std::vector<CubeAt> CubesOf(Level &level, const std::vector<std::wstring> &ids)
{
    std::vector<CubeAt> cubes;
    for (std::size_t room = 0; room < ids.size(); ++room)
    {
        for (const OwnProbe &probe : level.ProbesOf(ids[room]))
        {
            std::wstring file = Lower(probe.cube);
            if (!file.empty() && level.Find(file) && std::ranges::find(cubes, file, &CubeAt::file) == cubes.end())
            {
                cubes.push_back({static_cast<std::uint32_t>(room), probe.at, std::move(file)});
            }
        }
    }
    return cubes;
}

// What the ray of one texel of one of a level's cubes lands on, for
// BringToSurfaces: the share of all the ways round that the texel is (but
// for a number the same for all), and the surface's colour by that share;
// nothing of either where it lands on nothing painted.
struct Landed
{
    float share = 0;
    Colour colour{};
};

// What every texel of every cube of the level lands on, a ray a texel as
// the cubes' pictures are made.
// @param side a cube's face's, in texels
std::vector<Landed> LandedFrom(const Scene &scene, const Painted &painted, std::span<const CubeAt> cubes,
                               std::uint32_t side)
{
    const std::size_t texels = std::size_t{side} * side * 6;
    std::vector<Landed> landed(cubes.size() * texels);
    ForEach(landed.size(), [&](std::size_t i) {
        const CubeAt &cube = cubes[i / texels];
        const CubeTexel texel = TexelOf(i % texels, side);
        const std::optional<Landing> landing =
            Cast(scene, cube.room, cube.at, texel.way, std::numeric_limits<float>::infinity());
        if (landing && painted.triangles[landing->room][landing->hit.triangle])
        {
            const float share = 1 / (texel.out * std::sqrt(texel.out));
            const Colour &colour = scene.sends[landing->room][landing->hit.triangle];
            landed[i] = {share, {share * colour[0], share * colour[1], share * colour[2]}};
        }
    });
    return landed;
}

// Says how bright the surfaces' colours are together: all of them multiplied
// by the one number that brings their mean brightness, over what the level's
// cubes see, to the bake's `surfaces`.
//
// A texture's colours taken as light (SurfaceColours.h's LightOf) are dark:
// what the tower's cubes see is 0.13 of white in the mean and the Commons'
// 0.14, where their recipes' `surfaces` are 0.75 and 0.4. The cubes were
// looked at in the game with every surface that grey, and how bright a
// level's are is to stay what it was. So the mean is brought to it (the
// tower's colours by 5.9, the Commons' by 2.9), and a colour is a share of
// the level's mean and not a surface's true whiteness: the palest come out
// brighter than white, and the library's carpet sends on more red than
// falls on it.
//
// The mean is over what the cubes see, a ray a texel as their pictures are
// made, each by the share of all the ways round that its texel is; and not
// over the level's surface by its area, which was tried first. Orzammar's
// one cube stands in the market among pale buildings on a pale floor, and
// most of the level's surface is the dark rock of its cave far off (0.08 in
// the mean): by area the cube was two fifths brighter than with the grey
// (0.155 to 0.219 in the mean), and by what it sees it is 0.161.
// @param side a cube's face's, in texels
void BringToSurfaces(Scene &scene, const Painted &painted, std::span<const CubeAt> cubes, std::uint32_t side,
                     const Options &options, const std::string &name)
{
    const std::vector<Landed> landed = LandedFrom(scene, painted, cubes, side);
    double share = 0;
    std::array<double, 3> colour{};
    for (const Landed &one : landed)
    {
        share += double{one.share};
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            colour.at(channel) += double{one.colour.at(channel)};
        }
    }
    const double light = (kLuma[0] * colour[0]) + (kLuma[1] * colour[1]) + (kLuma[2] * colour[2]);
    const float by = light > 0 ? static_cast<float>(double{options.surfaces} * share / light) : 1.0F;
    for (std::size_t room = 0; room < scene.sends.size(); ++room)
    {
        for (std::size_t at = 0; at < scene.sends[room].size(); ++at)
        {
            if (painted.triangles[room][at])
            {
                std::ranges::for_each(scene.sends[room][at], [by](float &channel) { channel *= by; });
            }
        }
    }
    // And how much of what the cubes see comes out brighter than white in one of its colours.
    double over = 0;
    for (const Landed &one : landed)
    {
        over += std::ranges::max(one.colour) * by > one.share ? double{one.share} : 0.0;
    }
    const double seen = std::max(share, 1e-9);
    const double all = std::max(painted.area + painted.grey, 1e-9);
    spdlog::info("relight: {}: {} of {} materials painted from their textures, {:.1f}% of the lightmapped surface "
                 "({:.1f}% by a whole texture's mean); their colour {:.3f} {:.3f} {:.3f} in the mean of what the "
                 "cubes see, by {:.2f} to the bake's {:.2f}, {:.1f}% of it over white; left grey: {}",
                 name, painted.read, painted.materials, 100 * painted.area / all, 100 * painted.whole / all,
                 colour[0] / seen, colour[1] / seen, colour[2] / seen, by, options.surfaces, 100 * over / seen,
                 painted.unread.empty() ? "none" : painted.unread);
}

// What a ray sees of the level's surfaces: the light the lightmap has where
// it lands, as WriteMaps writes it, by what the surface there sends on, which
// is its own colour (PaintSurfaces); or what glows there, by its own colour;
// or nothing.
//
// Only what is lightmapped is in the world the rays are cast in, which is
// nearly all that a level's rooms place, its statues, shelves and beds with
// its walls (counted 2026-10-12: 809 thousand triangles of the tower's 820,
// 722 of the Commons' 737). What the rooms place with no lightmap is mostly
// not for a picture: the black that closes a level off, lava and window
// panes, which are here as what glows, a lantern's glass. Left out are a few
// open books and stools and the Commons' vistas, its far city as flat
// pictures; and what an area places in a level, which the level's rooms do
// not have: no door's leaf and no chest is among the models the tower's
// rooms place.
struct Sight
{
    Colour seen{};
    Colour falls{};      // the light that falls where the ray lands, as the lightmap has it
    bool landed = false; // whether it landed on a lightmap at all
};

Sight SeenAlong(const Scene &scene, std::uint32_t room, const core::Point &from, const core::Point &way,
                const Options &options)
{
    Colour seen{};
    Colour falls{};
    bool landed = false;
    const std::optional<Landing> landing = Cast(scene, room, from, way, std::numeric_limits<float>::infinity());
    if (landing)
    {
        const level::LitTriangle &triangle = landing->geometry->lit[landing->hit.triangle];
        if (const auto map = scene.maps.find(triangle.atlas); map != scene.maps.end())
        {
            const float w = 1 - landing->hit.u - landing->hit.v;
            const float u =
                (w * triangle.uv[0][0]) + (landing->hit.u * triangle.uv[1][0]) + (landing->hit.v * triangle.uv[2][0]);
            const float v =
                (w * triangle.uv[0][1]) + (landing->hit.u * triangle.uv[1][1]) + (landing->hit.v * triangle.uv[2][1]);
            const Colour &lit = At(map->second, map->second.whole, u, v);
            const Colour &own = At(map->second, map->second.own, u, v);
            const bool painted =
                landing->room < scene.sends.size() && landing->hit.triangle < scene.sends[landing->room].size();
            const Colour sends = painted ? scene.sends[landing->room][landing->hit.triangle]
                                         : Colour{options.surfaces, options.surfaces, options.surfaces};
            landed = true;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                falls.at(channel) = lit.at(channel) + (options.kept * own.at(channel));
                seen.at(channel) = sends.at(channel) * falls.at(channel);
            }
        }
    }
    if (!scene.shining.tree)
    {
        return {seen, falls, landed};
    }
    // What glows, where it is the first thing in the ray's way: a glowing
    // thing that is lightmapped too, a crystal, is at the landing itself.
    constexpr float kAtTheLanding = 0.01F; // metres
    const float within = landing ? landing->hit.distance + kAtTheLanding : std::numeric_limits<float>::infinity();
    const std::optional<core::RayHit> shines = scene.shining.tree->Cast(from, way, 1e-3F, within);
    if (!shines)
    {
        return {seen, falls, landed};
    }
    const Glowing &glow = scene.glows[scene.shining.glow[shines->triangle]];
    if (!glow.sky)
    {
        return {glow.colour, falls, landed};
    }
    // The sky through a window is seen from the room alone, and by how far
    // the way to it is upward, as it lights (Glowed): the sky sends its
    // light down. Where that is less than the wall's own pane behind it, the
    // pane is what is seen.
    const float sends = Dot(way, glow.facings.front()) < 0 ? std::max(0.0F, way[2]) : 0.0F;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        seen.at(channel) = std::max(seen.at(channel), sends * glow.colour.at(channel));
    }
    return {seen, falls, landed};
}

// How much of a texel a lamp's disc covers: all of it within the disc, the
// disc's share of it where the disc is the smaller, and a slope between at
// the disc's edge. `between` is the angle from the texel's middle to the
// disc's, `wide` the disc's own and `texel` that of a round as large as the
// texel is. One ray a texel would have a flame far off, a part of a texel
// across, wholly there or not at all.
float Covered(float between, float wide, float texel)
{
    const float edge = std::clamp((wide + texel - between) / (2 * std::min(wide, texel)), 0.0F, 1.0F);
    return edge * std::min(1.0F, (wide * wide) / (texel * texel));
}

// The picture of the level all round a place, and the light it is counted in.
struct Cube
{
    std::vector<Colour> texels; // six faces of `side` each way, face by face and row by row
    // The light that falls on what the picture sees, in the mean over all
    // the ways out of its middle that land on a lightmap, each colour by
    // itself; 1 where none does, or no light.
    Colour falls{1, 1, 1};
};

// The light of the lamps a cube's place sees straight, as it falls
// on a surface there in the mean of every way it may face: the mean of
// their discs all round (a disc shows a lamp's colour over the square of
// its radius, so that it lights a surface facing it as the lamp does;
// LampSeen). A surface's light where it mirrors the cube has its lamps
// in it (DaoLocal: a lightmap, a probe), and the cube's measure left
// them out: under a lamp the cube sees, every reflection was the lamp
// over the dim walls times a light that already had the lamp in it.
// Figor's floor, rough stone (a gloss of 0.05), mirrored the shaft's
// light and the torches as white patches (the user, 2026-10-12: "ground
// is too reflective").
// @param lampsSeen what of each texel of the cube is a lamp's disc
std::array<double, 3> LampsAllRound(std::span<const Colour> lampsSeen, std::uint32_t side)
{
    std::array<double, 3> light{};
    double all = 0;
    for (std::size_t i = 0; i < lampsSeen.size(); ++i)
    {
        const double out = TexelOf(i, side).out;
        const double share = 1 / (out * std::sqrt(out));
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            light.at(channel) += share * double{lampsSeen[i].at(channel)};
        }
        all += share;
    }
    for (double &channel : light)
    {
        channel = all > 0 ? channel / all : 0.0;
    }
    return light;
}

// The picture of the level all round a place: each texel the light seen
// along the way out through its middle, with the lamps in sight drawn over
// it, counted in the light that falls on what the picture sees (Cube::falls):
// a wall lit as the picture's walls are in the mean, and white, is 1.
//
// Counted so because a level has a cube or two and a surface mirrors its
// cube wherever it is: a gate in a dark corner mirrored the lit middle of
// its hall, and a floor in shade the hall's windows (seen 2026-10-12). The
// shader has the light that falls where the surface is, its lightmap's or a
// figure's probe's, and multiplies the cube by it (materials.hlsl's
// DaoLocal): the reflection is then the cube's picture, as bright as the
// place. It is what an engine with baked light does with a cube taken at
// one spot (Unreal's reflection captures, by the lightmap's brightness).
// Here each colour is counted by itself, so the reflection is in the
// place's light too: by brightness alone an iron gate with a candle before
// it mirrored the hall's cold windows, dimly, and nothing of the candle
// (seen 2026-10-12).
Cube CubeFrom(const Scene &scene, std::uint32_t room, const core::Point &from, std::uint32_t side,
              std::span<const LampSeen> lamps, const Options &options)
{
    std::vector<Colour> cube(std::size_t{side} * side * 6);
    std::vector<Colour> fell(cube.size());
    std::vector<Colour> lampsSeen(cube.size()); // what of a texel is a lamp's disc, over what is behind it
    std::vector<char> landed(cube.size(), 0);
    ForEach(cube.size(), [&](std::size_t i) {
        const auto [way, out] = TexelOf(i, side);
        const Sight sight = SeenAlong(scene, room, from, way, options);
        Colour seen = sight.seen;
        fell[i] = sight.falls;
        landed[i] = sight.landed ? 1 : 0;
        // The angle of a round as large as the texel is (CubeTexel::out).
        const float texel =
            (2 / static_cast<float>(side)) / std::sqrt(std::numbers::pi_v<float> * out * std::sqrt(out));
        for (const LampSeen &lamp : lamps)
        {
            const core::Point apart = Sub(way, lamp.way);
            const float between = 2 * std::asin(std::min(1.0F, std::sqrt(Dot(apart, apart)) / 2));
            const float covered = Covered(between, lamp.wide, texel);
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                seen.at(channel) = std::lerp(seen.at(channel), lamp.shows.at(channel), covered);
            }
        }
        cube[i] = seen;
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            lampsSeen[i].at(channel) = std::max(seen.at(channel) - sight.seen.at(channel), 0.0F);
        }
    });
    // The mean, each texel by how much of all round it is (CubeTexel::out).
    std::array<double, 3> light{};
    double round = 0;
    for (std::size_t i = 0; i < cube.size(); ++i)
    {
        if (landed[i] != 0)
        {
            const double out = TexelOf(i, side).out;
            const double share = 1 / (out * std::sqrt(out));
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                light.at(channel) += share * double{fell[i].at(channel)};
            }
            round += share;
        }
    }
    // And of the lamps it sees straight, which a surface it mirrors is lit by.
    const std::array<double, 3> lampLight = LampsAllRound(lampsSeen, side);
    constexpr double kNoLight = 1e-4;
    Colour falls{1, 1, 1};
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const double mean = (round > 0 ? light.at(channel) / round : 0.0) + lampLight.at(channel);
        if (mean > kNoLight)
        {
            falls.at(channel) = static_cast<float>(mean);
        }
    }
    for (Colour &texel : cube)
    {
        for (std::size_t channel = 0; channel < 3; ++channel)
        {
            texel.at(channel) /= falls.at(channel);
        }
    }
    return {std::move(cube), falls};
}

// A cube's picture as a DDS file as the game's own cubes are (read
// 2026-10-12: textures.erf's bad_cubemap.dds, 32 bits and not packed, and a
// level's own, DXT1): the header, then each face's pictures from the largest
// to one texel, the faces in Direct3D's order. Each smaller picture is the
// mean of the light, and is then kept as the lightmaps are (DdsOf), of
// kCubeMost.
std::vector<std::byte> DdsOfCube(const std::vector<Colour> &cube, std::uint32_t side)
{
    const auto pictures = static_cast<std::uint32_t>(std::bit_width(side));
    std::vector<std::byte> file(128);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(file.data() + at, &value, sizeof value); };
    put(0, 0x20534444);                                   // "DDS "
    put(4, 124);                                          // the header's size
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // caps, height, width, pixel format, mip count, linear size
    put(12, side);
    put(16, side);
    put(20, side * side * 4); // the largest picture's bytes, as the game's own cubes say it
    put(28, pictures);
    put(76, 32);         // the pixel format's size
    put(80, 0x41);       // RGB with alpha
    put(88, 32);         // bits a pixel
    put(92, 0x00FF0000); // red
    put(96, 0x0000FF00);
    put(100, 0x000000FF);
    put(104, 0xFF000000);
    put(108, 0x1000 | 0x8 | 0x400000); // a texture, complex, with mip maps
    put(112, 0x200 | 0xFC00);          // a cube map, all six of its faces
    const std::size_t aFace = std::size_t{side} * side;
    for (std::size_t face = 0; face < 6; ++face)
    {
        Picture picture(aFace);
        for (std::size_t i = 0; i < aFace; ++i)
        {
            const Colour &light = cube[(face * aFace) + i];
            picture[i] = {light[0], light[1], light[2], 1.0F};
        }
        for (std::uint32_t size = side; size >= 1; size /= 2)
        {
            for (const std::array<float, 4> &pixel : picture)
            {
                for (const std::size_t channel : {std::size_t{2}, std::size_t{1}, std::size_t{0}})
                {
                    file.push_back(static_cast<std::byte>(
                        NearestByte(std::sqrt(std::clamp(pixel.at(channel) / kCubeMost, 0.0F, 1.0F)) * 255.0F)));
                }
                file.push_back(std::byte{255});
            }
            picture = Halved(picture, size, size);
        }
    }
    return file;
}

// The texel of a cube's picture that a way out of its middle goes through:
// WayOfCube the other way about.
std::size_t TexelOfCube(const core::Point &way, std::uint32_t side)
{
    const std::array<float, 3> size{std::abs(way[0]), std::abs(way[1]), std::abs(way[2])};
    const std::size_t axis = static_cast<std::size_t>(std::ranges::max_element(size) - size.begin());
    const float most = std::max(size.at(axis), 1e-20F);
    const bool forward = way.at(axis) > 0;
    float across = 0;
    float down = 0;
    switch (axis)
    {
    case 0:
        across = (forward ? -way[2] : way[2]) / most;
        down = -way[1] / most;
        break;
    case 1:
        across = way[0] / most;
        down = (forward ? way[2] : -way[2]) / most;
        break;
    default:
        across = (forward ? way[0] : -way[0]) / most;
        down = -way[1] / most;
        break;
    }
    const auto texel = [side](float along) {
        return static_cast<std::size_t>(
            std::clamp((along + 1) / 2 * static_cast<float>(side), 0.0F, static_cast<float>(side - 1)));
    };
    const std::size_t face = (2 * axis) + (forward ? 0 : 1);
    return (((face * side) + texel(down)) * side) + texel(across);
}

// A cube's picture laid out for the eye, the tool's `cubeview`: four views
// from its middle side by side, each upright and each a quarter turn on from
// the one before (towards -y, +x, +y and -x), with the ceiling over the
// first and the floor under it; three bytes a pixel, 4 by 3 faces. They are
// the file's faces turned upright, neither mirrored, so what is right of
// what is as the file has it. The light is shown as the earlier look at the
// cubes showed it (2026-10-12): twice it over one more than twice it, a
// white wall at two thirds and a lamp at white, with no curve after, the
// game's shaders showing a cube's light as it is.
std::vector<std::uint8_t> CrossOf(const std::vector<Colour> &cube, std::uint32_t side)
{
    struct View
    {
        core::Point ahead{};
        core::Point right{};
        core::Point up{};
        std::uint32_t column = 0;
        std::uint32_t row = 0;
    };
    const std::array<View, 6> views{{{{0, -1, 0}, {1, 0, 0}, {0, 0, 1}, 0, 1},
                                     {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 1, 1},
                                     {{0, 1, 0}, {-1, 0, 0}, {0, 0, 1}, 2, 1},
                                     {{-1, 0, 0}, {0, -1, 0}, {0, 0, 1}, 3, 1},
                                     {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, 0, 0},
                                     {{0, 0, -1}, {1, 0, 0}, {0, -1, 0}, 0, 2}}};
    const std::size_t width = std::size_t{side} * 4;
    std::vector<std::uint8_t> picture(width * side * 3 * 3);
    for (const View &view : views)
    {
        for (std::uint32_t y = 0; y < side; ++y)
        {
            for (std::uint32_t x = 0; x < side; ++x)
            {
                const float across = (((static_cast<float>(x) + 0.5F) / static_cast<float>(side)) * 2) - 1;
                const float down = (((static_cast<float>(y) + 0.5F) / static_cast<float>(side)) * 2) - 1;
                core::Point way{};
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    way.at(axis) = view.ahead.at(axis) + (across * view.right.at(axis)) - (down * view.up.at(axis));
                }
                const Colour &light = cube[TexelOfCube(way, side)];
                std::uint8_t *pixel =
                    &picture[((((std::size_t{view.row} * side) + y) * width) + (std::size_t{view.column} * side) + x) *
                             3];
                for (std::size_t channel = 0; channel < 3; ++channel)
                {
                    const float shown = 2 * std::max(light.at(channel), 0.0F);
                    pixel[channel] = NearestByte(255.0F * shown / (1 + shown));
                }
            }
        }
    }
    return picture;
}

// A picture of three bytes a pixel as a PNG file. Not packed: the format's
// stream is zlib's, and zlib's has a way to say bytes as they are, 65535 at
// a time, which every reader takes; a cube's picture is half a megabyte so.
std::vector<std::byte> PngOf(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> file{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto big = [](std::vector<std::uint8_t> &to, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
        {
            to.push_back(static_cast<std::uint8_t>(value >> shift));
        }
    };
    // A piece of the file: its length, its kind and its bytes, and the CRC-32 of the last two.
    const auto piece = [&](std::string_view kind, std::span<const std::uint8_t> bytes) {
        big(file, static_cast<std::uint32_t>(bytes.size()));
        const std::size_t from = file.size();
        file.insert(file.end(), kind.begin(), kind.end());
        file.insert(file.end(), bytes.begin(), bytes.end());
        std::uint32_t crc = 0xFFFFFFFFU;
        for (std::size_t at = from; at < file.size(); ++at)
        {
            crc ^= file[at];
            for (int bit = 0; bit < 8; ++bit)
            {
                crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
            }
        }
        big(file, ~crc);
    };
    std::vector<std::uint8_t> head;
    big(head, width);
    big(head, height);
    head.insert(head.end(),
                {8, 2, 0, 0, 0}); // eight bits a channel, red green and blue, and the format's one way of each else
    piece("IHDR", head);
    // The rows, each after a byte that says it is kept as it is.
    std::vector<std::uint8_t> rows;
    rows.reserve(((std::size_t{width} * 3) + 1) * height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        rows.push_back(0);
        const auto row = rgb.subspan(std::size_t{y} * width * 3, std::size_t{width} * 3);
        rows.insert(rows.end(), row.begin(), row.end());
    }
    std::vector<std::uint8_t> stream{0x78, 0x01};
    std::uint32_t low = 1; // Adler-32, zlib's sum of the bytes it holds
    std::uint32_t high = 0;
    constexpr std::size_t kAtATime = 65535;
    for (std::size_t at = 0; at < rows.size(); at += kAtATime)
    {
        const std::size_t count = std::min(kAtATime, rows.size() - at);
        stream.push_back(at + count == rows.size() ? 1 : 0);
        for (const std::size_t length : {count, count ^ 0xFFFFU})
        {
            stream.push_back(static_cast<std::uint8_t>(length));
            stream.push_back(static_cast<std::uint8_t>(length >> 8));
        }
        for (std::size_t i = at; i < at + count; ++i)
        {
            stream.push_back(rows[i]);
            low = (low + rows[i]) % 65521;
            high = (high + low) % 65521;
        }
    }
    big(stream, (high << 16) | low);
    piece("IDAT", stream);
    piece("IEND", {});
    std::vector<std::byte> bytes(file.size());
    std::memcpy(bytes.data(), file.data(), file.size());
    return bytes;
}

// Makes the level's reflection cubes of a world that is lit, and writes each
// into the folder as a DDS of vanilla's own name, which in the plugin's
// override folder is the cube the game hands the shaders. Only a cube the
// level has a file for is made (CubesOf). Where a folder is named for it
// (Options::cubeview), each is written there too as a picture to look at,
// <the cube's name>.png.
// @param side a cube's face's, in texels
// @param ids the scene's rooms' names, for the log
void WriteCubes(const Scene &scene, std::span<const CubeAt> cubes, std::uint32_t side, const std::filesystem::path &out,
                const Options &options, const std::string &name, const std::vector<std::wstring> &ids)
{
    std::error_code error;
    if (!options.cubeview.empty())
    {
        std::filesystem::create_directories(options.cubeview, error);
    }
    for (const CubeAt &at : cubes)
    {
        const std::vector<LampSeen> lamps = LampsSeenFrom(scene, at.room, at.at, options);
        const Cube made = CubeFrom(scene, at.room, at.at, side, lamps, options);
        const std::vector<Colour> &cube = made.texels;
        const std::vector<std::byte> bytes = DdsOfCube(cube, side);
        std::ofstream stream(out / at.file, std::ios::binary);
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!options.cubeview.empty())
        {
            const std::vector<std::byte> png = PngOf(CrossOf(cube, side), side * 4, side * 3);
            std::ofstream view(options.cubeview / std::filesystem::path(at.file).replace_extension(L".png"),
                               std::ios::binary);
            view.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
        }
        double mean = 0;
        double most = 0;
        for (const Colour &light : cube)
        {
            const double luma =
                (kLuma[0] * double{light[0]}) + (kLuma[1] * double{light[1]}) + (kLuma[2] * double{light[2]});
            mean += luma / static_cast<double>(cube.size());
            most = std::max(most, luma);
        }
        spdlog::info("relight: {}: the cube {} from {:.2f} {:.2f} {:.2f}, room {}: {} of {} lamps in sight, "
                     "the light on what it sees {:.3f} {:.3f} {:.3f} in the mean; counted in that, its own {:.3f} in "
                     "the mean and {:.1f} at the most",
                     name, Text(at.file), at.at[0], at.at[1], at.at[2], Text(ids[at.room]), lamps.size(),
                     scene.lamps.size(), made.falls[0], made.falls[1], made.falls[2], mean, most);
    }
}

// Lights one world of a level's rooms and writes its lightmaps: the rooms
// `ids` are the world, each lit and each in the way of the others' light.
// @param sources the rooms whose definitions place the level's fittings
// @param withProbes whether the figures' probes are made of this world
// @param done the lightmaps written so far, by name: none is written twice
// @return how many lightmaps were written
std::size_t LightRooms(Level &level, const std::filesystem::path &levelRim, const std::filesystem::path &out,
                       const Options &options, const std::vector<std::wstring> &ids,
                       const std::vector<std::wstring> &sources, bool withProbes, std::set<std::wstring> &done,
                       std::optional<Colour> &gain, const PaintFiles &paints)
{
    const auto began = std::chrono::steady_clock::now();
    const std::string name = levelRim.stem().string();
    Scene scene;
    GatherRooms(level, scene, options, ids);
    const char *caster = "DAOLib's tree, a room at a time";
#ifdef ELGARA_EMBREE
    Embree embree;
    if (options.caster != 0 && CastByEmbree(embree, scene, name))
    {
        caster = "Embree, the world as one scene";
    }
#endif
    AddBeams(level, scene, options, ids, name);
    // The flames, after every lamp of the level's own is known.
    const std::size_t levelsOwn = scene.lamps.size();
    PlaceFlames(level, scene, options, ids, sources);
    spdlog::info("relight: {}: {} lamps of the level's own, {} more of its flames", name, levelsOwn,
                 scene.lamps.size() - levelsOwn);
    MakeMaps(level, scene, options);
    const Order order = LayRooms(scene);
    spdlog::info("relight: {}: {} rooms, {} lamps, {} glowing things, {} lightmaps, {} texels to light; rays by {}",
                 name, ids.size(), scene.lamps.size(), scene.glows.size(), scene.maps.size(), order.size(), caster);
    LightStraight(scene, order, options);
    for (int bounce = 0; bounce < options.bounces; ++bounce)
    {
        Bounce(scene, order, options, bounce);
    }
    MatchVanilla(scene, options, gain, name);
    const std::size_t written = WriteMaps(level, scene, out, options, done);
    if (withProbes && options.cube > 0)
    {
        // The level's cubes, of the light as the lightmaps were just written
        // with it, each surface in its own colour: the colours are found now,
        // the lightmaps written, and are the cubes' alone.
        if (!scene.shining.triangles.empty())
        {
            scene.shining.tree.emplace(scene.shining.triangles);
        }
        // A cube map's side is a power of two: the one at or under what was asked.
        const std::uint32_t side = std::bit_floor(options.cube);
        const std::vector<CubeAt> cubes = CubesOf(level, ids);
        const Painted painted = PaintSurfaces(level, scene, paints, options);
        BringToSurfaces(scene, painted, cubes, side, options, name);
        WriteCubes(scene, cubes, side, out, options, name, ids);
        spdlog::info("relight: {}: {} reflection cubes, {} texels a side", name, cubes.size(), side);
    }
    if (withProbes)
    {
        // A figure is lit by what is kept of vanilla's bake as a room's
        // probes always took it: each side of it as the surface behind is.
        // The probes take a surface to send on `surfaces` of its light, so
        // the kept light is put into the maps by one over that.
        if (options.kept > 0 && options.surfaces > 0)
        {
            for (auto &[atlas, map] : scene.maps)
            {
                for (std::size_t i = 0; i < map.whole.size(); ++i)
                {
                    for (std::size_t channel = 0; channel < 3; ++channel)
                    {
                        map.whole[i].at(channel) += options.kept * map.own[i].at(channel) / options.surfaces;
                    }
                }
            }
        }
        WriteProbes(level, scene, levelRim, out, options, ids);
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    spdlog::info("relight: {}: {} lightmaps written to {}, {} ms", name, written, out.string(), took.count());
    return written;
}

} // namespace

std::size_t Relight(const std::filesystem::path &levelRim, const std::filesystem::path &game,
                    const std::filesystem::path &out, const Options &options)
{
    const std::filesystem::path data = game / "packages" / "core" / "data";
    std::optional<Archive> rooms = Archive::Open(levelRim, true);
    std::optional<Archive> hierarchies = Archive::Open(data / "modelhierarchies.erf", true);
    std::optional<Archive> meshes = Archive::Open(data / "modelmeshdata.erf", true);
    if (!rooms || !hierarchies || !meshes)
    {
        spdlog::warn("relight: {} or the models' archives could not be read", levelRim.string());
        return 0;
    }
    Level level(levelRim.parent_path(), levelRim.stem().wstring(), std::move(*rooms), &*hierarchies, &*meshes);
    // Where the level's own lamps are, every one of its layouts', beside the
    // lightmaps (<level>.lamps: "ELGL", how many, three floats each). The
    // plugin puts those lamps out in a level lit again, their light being the
    // bake's to give or not; every other light, a spell's, a torch carried,
    // then lights the level as it does in vanilla (Graphics.cpp).
    {
        std::vector<float> places;
        const auto take = [&](const std::wstring &layout) {
            for (const Lamp &lamp : level.LampsOf(layout))
            {
                places.insert(places.end(), lamp.at.begin(), lamp.at.end());
            }
        };
        for (const std::wstring &id : level.Rooms())
        {
            take(id);
            if (level.Terrain() && id.ends_with(L"_0"))
            {
                take(id.substr(0, id.size() - 1) + L"1");
            }
        }
        take(L"");
        std::error_code error;
        std::filesystem::create_directories(out, error);
        std::ofstream stream(out / (levelRim.stem().wstring() + L".lamps"), std::ios::binary);
        const std::array<char, 4> magic{'E', 'L', 'G', 'L'};
        const auto count = static_cast<std::uint32_t>(places.size() / 3);
        stream.write(magic.data(), magic.size());
        stream.write(reinterpret_cast<const char *>(&count), sizeof count);
        const std::span<const std::byte> bytes = std::as_bytes(std::span(places));
        stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        spdlog::info("relight: {}: {} lamps of the level's own, said in its .lamps", levelRim.stem().string(), count);
    }
    std::set<std::wstring> done;
    std::optional<Colour> gain; // what the level's light is brought to vanilla's by, found by its first world
    // Where the surfaces' colours are, for the cubes; not opened where none is made.
    PaintFiles paints;
    if (options.cube > 0)
    {
        paints.materials = Archive::Open(data / "materialobjects.erf", true);
        paints.pack = Archive::Open(game / "packages" / "core" / "textures" / "high" / "texturepack.erf", true);
        paints.textures = Archive::Open(data / "textures.erf", true);
    }
    if (!level.Terrain())
    {
        return LightRooms(level, levelRim, out, options, level.Rooms(), level.Rooms(), true, done, gain, paints);
    }
    // A level of terrain is more than its tiles as they are from near. Each
    // tile has a far detail (<x>_<y>_1) with lightmaps of its own, drawn in
    // its stead from a distance; and the level has a room of its own, its
    // backdrop and the shell of its cave, with more. Left as the game baked
    // them, they are read as ours are written, the square of what is stored
    // by eight: Orzammar's far halls stood white and red as neon over a hall
    // lit again (the user, 2026-10-11: "did you even check this?"). So the
    // level is lit as two worlds, the near with the backdrop and the far
    // with the backdrop, by the same fittings.
    const std::vector<std::wstring> near = level.Rooms();
    std::vector<std::wstring> first = near;
    first.emplace_back();
    std::vector<std::wstring> far;
    far.reserve(near.size() + 1);
    for (const std::wstring &id : near)
    {
        far.push_back(id.substr(0, id.size() - 1) + L"1");
    }
    far.emplace_back();
    std::size_t written = LightRooms(level, levelRim, out, options, first, near, true, done, gain, paints);
    written += LightRooms(level, levelRim, out, options, far, near, false, done, gain, paints);
    return written;
}

} // namespace relighter
