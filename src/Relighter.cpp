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

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <numbers>
#include <set>
#include <thread>

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

constexpr float kOffSurface = 0.03F;  // metres a ray starts off its surface
constexpr float kShortOfLamp = 0.35F; // metres a shadow's ray stops short of a lamp, which stands in its own fitting
constexpr float kGlowReach = 45.0F;   // metres from which a glowing thing is not counted
constexpr std::size_t kGlowTried = 16; // points of each glowing thing tried from a texel
constexpr float kGlowNearest = 1.0F;   // metres: a point nearer than this lights as if it were this far
constexpr float kGlowPointsAMetre = 6.0F; // points spread over a square metre of a glowing thing
constexpr float kOwnPointsAMetre = 70.0F; // and of one that is lit by its own glow: some 0.12 m apart
constexpr int kFilled = 4;            // texels the light is spread out past the triangles' edges

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
    std::vector<Colour> own; // the game's baked light
    std::vector<std::uint8_t> sun; // its alpha
    std::vector<Colour> straight; // the light that came straight from a lamp or a glow
    std::vector<Colour> glowed;   // the glows' part of it, before it is smoothed and added
    std::vector<Colour> came;     // the light of the bounce before, to be read by the next
    std::vector<Colour> whole;    // all of it
};

// Something that glows, as points over its surface.
struct Glowing
{
    std::vector<core::Point> points;
    std::vector<core::Point> facings;
    core::Point middle{};
    float reach = 0;  // how far its furthest point is from its middle
    float patch = 0;  // the square metres a point stands for
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

struct Scene
{
    std::vector<RoomLit> rooms;
    std::vector<Lamp> lamps;
    std::vector<Glowing> glows;
    std::vector<Beaming> beams;
    std::map<std::uint32_t, Map> maps; // by the atlas's number
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
        auto *corners = static_cast<float *>(rtcSetNewGeometryBuffer(mesh, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3,
                                                                    3 * sizeof(float), 3 * count));
        auto *indices = static_cast<unsigned *>(rtcSetNewGeometryBuffer(mesh, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3,
                                                                       3 * sizeof(unsigned), count));
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
                sum.at(axis) += (triangle.corners.a.at(axis) + triangle.corners.b.at(axis) + triangle.corners.c.at(axis)) / 3;
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
        const bool lit = std::ranges::any_of(scene.lamps.begin(), scene.lamps.begin() + static_cast<std::ptrdiff_t>(levelsOwn),
                                             [&](const Lamp &has) {
                                                 const core::Point apart = Sub(has.at, middle);
                                                 return Dot(apart, apart) < kLampsOwn * kLampsOwn;
                                             });
        if (lit)
        {
            continue;
        }
        const float many = std::min(group.count, kFlamesMost);
        scene.lamps.push_back({{middle[0], middle[1], middle[2] + flame.lift},
                               {flame.colour[0] * many, flame.colour[1] * many, flame.colour[2] * many},
                               flame.reach,
                               kCore,
                               flame.clear});
    }
}

// What a ray lands on first among a room's own and the rooms about it.
struct Landing
{
    const Geometry *geometry = nullptr;
    core::RayHit hit;
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
                       core::RayHit{cast.hit.primID, cast.ray.tfar, cast.hit.u, cast.hit.v}};
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
            nearest = Landing{lit.geometry, *found};
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
    const unsigned threads = std::max(1U, std::thread::hardware_concurrency() > 3 ? std::thread::hardware_concurrency() - 2 : 1U);
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

// Lays a room's triangles into the lightmaps: each texel whose middle a
// triangle covers is given its place on the triangle and the triangle's
// facing.
void Lay(Scene &scene, std::uint32_t room, const Geometry &geometry)
{
    std::vector<bool> ground(geometry.lit.size());
    for (const auto &[from, to] : geometry.ground)
    {
        std::fill(ground.begin() + static_cast<std::ptrdiff_t>(from), ground.begin() + static_cast<std::ptrdiff_t>(to), true);
    }
    for (std::size_t index = 0; index < geometry.lit.size(); ++index)
    {
        const level::LitTriangle &triangle = geometry.lit[index];
        const auto found = scene.maps.find(triangle.atlas);
        if (found == scene.maps.end())
        {
            continue;
        }
        Map &map = found->second;
        core::Point facing = Unit(Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a)));
        if (ground[index] && facing[2] < 0)
        {
            facing = {-facing[0], -facing[1], -facing[2]};
        }
        const auto w = static_cast<float>(map.width);
        const auto h = static_cast<float>(map.height);
        const std::array<std::array<float, 2>, 3> uv{{{triangle.uv[0][0] * w, triangle.uv[0][1] * h},
                                                      {triangle.uv[1][0] * w, triangle.uv[1][1] * h},
                                                      {triangle.uv[2][0] * w, triangle.uv[2][1] * h}}};
        const float area = ((uv[1][0] - uv[0][0]) * (uv[2][1] - uv[0][1])) - ((uv[2][0] - uv[0][0]) * (uv[1][1] - uv[0][1]));
        if (std::abs(area) < 1e-9F)
        {
            continue;
        }
        const int left = std::max(0, static_cast<int>(std::floor(std::min({uv[0][0], uv[1][0], uv[2][0]}) - 0.5F)));
        const int right = std::min(static_cast<int>(map.width) - 1, static_cast<int>(std::ceil(std::max({uv[0][0], uv[1][0], uv[2][0]}) + 0.5F)));
        const int top = std::max(0, static_cast<int>(std::floor(std::min({uv[0][1], uv[1][1], uv[2][1]}) - 0.5F)));
        const int bottom = std::min(static_cast<int>(map.height) - 1, static_cast<int>(std::ceil(std::max({uv[0][1], uv[1][1], uv[2][1]}) + 0.5F)));
        for (int y = top; y <= bottom; ++y)
        {
            for (int x = left; x <= right; ++x)
            {
                const float px = static_cast<float>(x) + 0.5F;
                const float py = static_cast<float>(y) + 0.5F;
                // The texel's middle as shares of the three corners; a little
                // outside still counts, so a triangle's edge is not left bare.
                const float b = (((px - uv[0][0]) * (uv[2][1] - uv[0][1])) - ((uv[2][0] - uv[0][0]) * (py - uv[0][1]))) / area;
                const float c = (((uv[1][0] - uv[0][0]) * (py - uv[0][1])) - ((px - uv[0][0]) * (uv[1][1] - uv[0][1]))) / area;
                const float a = 1 - b - c;
                constexpr float kOutside = -0.02F;
                Texel &texel = map.texels[(static_cast<std::size_t>(y) * map.width) + static_cast<std::size_t>(x)];
                std::array<float, 3> shares{a, b, c};
                if (a < kOutside || b < kOutside || c < kOutside)
                {
                    // A triangle that covers no texel's middle, the riser of a
                    // step a hand high, still has texels drawn from: left
                    // bare, they were filled from whatever lay beside them in
                    // the lightmap, and a platform's edge shone as lava does
                    // (the user, 2026-10-11: "why's the platform edge glowing?
                    // that's not lava"). So a texel no triangle covers takes
                    // the nearest place on one that comes within its square.
                    if (texel.covered)
                    {
                        continue;
                    }
                    float nearest = 0.55F; // texels, squared: within the texel's own square, about
                    bool near = false;
                    for (std::size_t edge = 0; edge < 3; ++edge)
                    {
                        const std::array<float, 2> &from = uv.at(edge);
                        const std::array<float, 2> &to = uv.at((edge + 1) % 3);
                        const float ex = to[0] - from[0];
                        const float ey = to[1] - from[1];
                        const float along =
                            std::clamp((((px - from[0]) * ex) + ((py - from[1]) * ey)) / std::max((ex * ex) + (ey * ey), 1e-12F),
                                       0.0F, 1.0F);
                        const float dx = from[0] + (along * ex) - px;
                        const float dy = from[1] + (along * ey) - py;
                        if ((dx * dx) + (dy * dy) < nearest)
                        {
                            nearest = (dx * dx) + (dy * dy);
                            shares = {0, 0, 0};
                            shares.at(edge) = 1 - along;
                            shares.at((edge + 1) % 3) = along;
                            near = true;
                        }
                    }
                    if (!near)
                    {
                        continue;
                    }
                    texel.loose = true;
                }
                else
                {
                    texel.loose = false;
                }
                texel.covered = true;
                texel.room = room;
                texel.facing = facing;
                for (std::size_t axis = 0; axis < 3; ++axis)
                {
                    texel.at.at(axis) = (shares[0] * triangle.corners.a.at(axis)) + (shares[1] * triangle.corners.b.at(axis)) +
                                        (shares[2] * triangle.corners.c.at(axis));
                }
            }
        }
    }
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
            continue;
        }
        const core::Point way{to[0] / far, to[1] / far, to[2] / far};
        if (facing != nullptr && Dot(*facing, way) <= 0)
        {
            continue;
        }
        const float shortOf = std::max(kShortOfLamp, lamp.clear);
        if (far > shortOf && Blocked(scene, room, from, way, far - shortOf))
        {
            continue;
        }
        const float within = 1 - ((far / reach) * (far / reach));
        const float share = (ours ? 1.0F : options.lamps) * within * within / ((far * far) + (core * core));
        seen(way, Colour{share * lamp.colour[0], share * lamp.colour[1], share * lamp.colour[2]});
    }
    for (const Beaming &beam : scene.beams)
    {
        constexpr float kBeamFar = 120.0F; // metres an opening may be from what it lights
        if (!beam.openings || (facing != nullptr && Dot(*facing, beam.toSource) <= 0))
        {
            continue;
        }
        const std::optional<core::RayHit> out = beam.openings->Cast(from, beam.toSource, 1e-3F, kBeamFar);
        if (!out || (out->distance > 0.1F && Blocked(scene, room, from, beam.toSource, out->distance - 0.05F)))
        {
            continue;
        }
        // The light comes in by an opening, from outside: a window in the wall
        // the light is travelling towards lets none in, and with no word on
        // which side is out, the wall about such a window was lit through it
        // from the room (the user, 2026-10-11: "windows seem to have light
        // shining on them instead of through them").
        if (Dot(beam.toSource, beam.outward[out->triangle / 2]) <= 0)
        {
            continue;
        }
        // (The tracery's own shadow is not cast: a window's model has its glass
        // in it with its bars, and a pane that stops the light stops all of it.)
        seen(beam.toSource, beam.colour);
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
    const auto x = static_cast<std::size_t>(std::clamp(u * static_cast<float>(map.width), 0.0F, static_cast<float>(map.width - 1)));
    const auto y = static_cast<std::size_t>(std::clamp(v * static_cast<float>(map.height), 0.0F, static_cast<float>(map.height - 1)));
    return light[(y * map.width) + x];
}

// What comes back to a texel off the surfaces about: the mean, over rays
// cast out the way it faces and more of them the nearer that way, of the
// light where each lands, by what a surface sends on.
Colour Back(const Scene &scene, const Texel &texel, std::uint32_t seed, const Options &options)
{
    const core::Point from = Along(texel.at, texel.facing, kOffSurface);
    const core::Point aside = Unit(std::abs(texel.facing[2]) < 0.9F ? Cross(texel.facing, {0, 0, 1}) : Cross(texel.facing, {1, 0, 0}));
    const core::Point other = Cross(texel.facing, aside);
    Colour sum{};
    for (std::size_t ray = 0; ray < options.rays; ++ray)
    {
        const float turn = 2 * std::numbers::pi_v<float> * Chance(seed, static_cast<std::uint32_t>(ray), 7);
        const float out = std::sqrt(Chance(seed, static_cast<std::uint32_t>(ray), 13));
        const float up = std::sqrt(std::max(0.0F, 1 - (out * out)));
        const core::Point way{(aside[0] * out * std::cos(turn)) + (other[0] * out * std::sin(turn)) + (texel.facing[0] * up),
                              (aside[1] * out * std::cos(turn)) + (other[1] * out * std::sin(turn)) + (texel.facing[1] * up),
                              (aside[2] * out * std::cos(turn)) + (other[2] * out * std::sin(turn)) + (texel.facing[2] * up)};
        const std::optional<Landing> landing = Cast(scene, texel.room, from, way, std::numeric_limits<float>::infinity());
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
        const Colour &there = At(map->second, map->second.came,
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
                if (!other.covered || Dot(other.facing, texel.facing) < kSameFacing || Dot(apart, apart) > kNearby * kNearby)
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
                Colour sum{};
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int nx = static_cast<int>(x) + dx;
                        const int ny = static_cast<int>(y) + dy;
                        if (nx < 0 || ny < 0 || nx >= static_cast<int>(map.width) || ny >= static_cast<int>(map.height))
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
                if (count != 0)
                {
                    filled[at] = {sum[0] / static_cast<float>(count), sum[1] / static_cast<float>(count), sum[2] / static_cast<float>(count)};
                    next[at] = true;
                }
            }
        }
        has = std::move(next);
        light = std::move(filled);
    }
}

// A lightmap as a DDS file, not packed, with its smaller pictures after it.
std::vector<std::byte> DdsOf(const Map &map, const Options &options)
{
    std::uint32_t width = map.width;
    std::uint32_t height = map.height;
    std::vector<std::array<float, 4>> picture(std::size_t{width} * height);
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
                file.push_back(static_cast<std::byte>(static_cast<std::uint8_t>((pixel.at(channel) * 255.0F) + 0.5F)));
            }
        }
        const std::uint32_t nextWidth = std::max(width / 2, 1U);
        const std::uint32_t nextHeight = std::max(height / 2, 1U);
        std::vector<std::array<float, 4>> half(std::size_t{nextWidth} * nextHeight);
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
                            picture[(std::size_t{std::min((2 * y) + dy, height - 1)} * width) + std::min((2 * x) + dx, width - 1)];
                        for (std::size_t channel = 0; channel < 4; ++channel)
                        {
                            sum.at(channel) += from.at(channel) / 4;
                        }
                    }
                }
                half[(std::size_t{y} * nextWidth) + x] = sum;
            }
        }
        picture = std::move(half);
        width = nextWidth;
        height = nextHeight;
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
                                            std::min((x * across) + dx, image.width - 1)) * 4;
                    for (std::size_t channel = 0; channel < 4; ++channel)
                    {
                        sum.at(channel) += static_cast<float>(image.rgba[at + channel]);
                    }
                }
            }
            const float count = static_cast<float>(across * down) * 255.0F;
            const std::size_t to = (std::size_t{y} * map.width) + x;
            map.own[to] = {sum[0] / count, sum[1] / count, sum[2] / count};
            map.sun[to] = static_cast<std::uint8_t>((sum[3] / static_cast<float>(across * down)) + 0.5F);
        }
    }
}

// What glows among the models a room places, each model one glowing thing.
void FindGlows(Level &level, const std::wstring &id, const Options &options, std::vector<Glowing> &out)
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
            Glowing glow;
            glow.colour = rule.colour;
            glow.own = rule.own;
            float area = 0;
            std::vector<std::pair<float, const Level::Drawn *>> parts;
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
                const core::Point across = Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a));
                area += 0.5F * std::sqrt(Dot(across, across));
                parts.emplace_back(area, &triangle);
            }
            if (parts.empty() || area < 1e-4F)
            {
                continue;
            }
            // A thing lit by its own glow is found again by its points
            // (Straight), and wants them close together.
            const float density = rule.own > 0 ? kOwnPointsAMetre : kGlowPointsAMetre;
            const auto count = static_cast<std::size_t>(std::clamp(area * density, 4.0F, 256.0F));
            glow.patch = area / static_cast<float>(count);
            const auto seed = static_cast<std::uint32_t>(out.size());
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
                glow.facings.push_back(Unit(Cross(Sub(triangle.corners.b, triangle.corners.a), Sub(triangle.corners.c, triangle.corners.a))));
            }
            for (const core::Point &point : glow.points)
            {
                const core::Point apart = Sub(point, glow.middle);
                glow.reach = std::max(glow.reach, std::sqrt(Dot(apart, apart)));
            }
            out.push_back(std::move(glow));
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
        const Colour &there = At(map->second, map->second.whole,
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

// Lights one world of a level's rooms and writes its lightmaps: the rooms
// `ids` are the world, each lit and each in the way of the others' light.
// @param sources the rooms whose definitions place the level's fittings
// @param withProbes whether the figures' probes are made of this world
// @param done the lightmaps written so far, by name: none is written twice
// @return how many lightmaps were written
std::size_t LightRooms(Level &level, const std::filesystem::path &levelRim, const std::filesystem::path &out,
                       const Options &options, const std::vector<std::wstring> &ids,
                       const std::vector<std::wstring> &sources, bool withProbes, std::set<std::wstring> &done)
{
    const auto began = std::chrono::steady_clock::now();
    Scene scene;
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
        lit.about.push_back(i);
        for (const std::wstring &seen : level.SeenFrom(ids[i]))
        {
            if (const auto found = numbers.find(seen); found != numbers.end())
            {
                lit.about.push_back(found->second);
            }
        }
        // A tile's far detail has the tiles about it by its name; and the
        // level's own room, its backdrop and the shell of its cave, is about
        // every room as every room is about it.
        if (const std::optional<Tile> tile = level.Terrain() && !TileOf(ids[i]) ? TileOfAnyDetail(ids[i]) : std::nullopt)
        {
            for (std::size_t other = 0; other < ids.size(); ++other)
            {
                const std::optional<Tile> there = other != i ? TileOfAnyDetail(ids[other]) : std::nullopt;
                if (there && std::abs(there->x - tile->x) <= kTilesAbout && std::abs(there->y - tile->y) <= kTilesAbout)
                {
                    lit.about.push_back(other);
                }
            }
        }
        for (std::size_t other = 0; other < ids.size(); ++other)
        {
            if (other != i && (ids[i].empty() || ids[other].empty()))
            {
                lit.about.push_back(other);
            }
        }
        // A tile's lamps are in the layout its definition places, and in a
        // terrain level that is often the layout of the tile's other detail
        // alone (Orzammar's Commons: <x>_<y>_1_ll in the level's own archive,
        // 60 lamps in all, of which the tiles seen from near name 27).
        std::vector<Lamp> lamps = level.LampsOf(ids[i]);
        if (level.Terrain() && ids[i].ends_with(L"_0"))
        {
            for (const Lamp &lamp : level.LampsOf(ids[i].substr(0, ids[i].size() - 1) + L"1"))
            {
                lamps.push_back(lamp);
            }
        }
        for (const Lamp &lamp : lamps)
        {
            AddLamp(scene, lamp, options);
        }
        FindGlows(level, ids[i], options, scene.glows);
    }
    for (const Lamp &lamp : level.LampsOf(L"")) // the level's own layout, where it has one
    {
        AddLamp(scene, lamp, options);
    }
    const char *caster = "DAOLib's tree, a room at a time";
#ifdef ELGARA_EMBREE
    Embree embree;
    if (options.caster != 0)
    {
        const auto before = std::chrono::steady_clock::now();
        scene.embree = MakeEmbree(embree, scene);
        if (scene.embree != nullptr)
        {
            caster = "Embree, the world as one scene";
            spdlog::info("relight: {}: Embree's scene made in {} ms", levelRim.stem().string(),
                         std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count());
        }
    }
#endif
    // The level's middle: the middle of its rooms' boxes together.
    Box all;
    for (const RoomLit &lit : scene.rooms)
    {
        if (lit.geometry != nullptr && !lit.geometry->lit.empty())
        {
            all.Add(lit.box.low);
            all.Add(lit.box.high);
        }
    }
    const core::Point levelMiddle{(all.low[0] + all.high[0]) / 2, (all.low[1] + all.high[1]) / 2,
                                  (all.low[2] + all.high[2]) / 2};
    // The openings of each light from far off, the whole level's together. A
    // window as the level has it is a model of its tracery alone, gilded bars
    // before a dark pane that is the wall's own: with the bars themselves
    // taken for the opening, the moon lit slivers of sill and arch beside
    // them and nothing else (the user, 2026-10-11: "where's this random blue
    // from?"). So a window's opening is the flat of its model, as wide and as
    // high as the model is, a little in from its edges; and the bars are
    // what stands in it, and shade what is behind them.
    for (const Beam &beam : options.beams)
    {
        constexpr float kInFromTheEdge = 0.92F;
        std::vector<core::Triangle> openings;
        std::vector<core::Triangle> bars;
        std::vector<core::Point> outward;
        std::size_t faced = 0; // openings with a face of the wall before them
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
                // The model's own three ways in the level, and how far it
                // goes along each: the least is its thickness.
                const core::Point origin = level::InLevel(*room, placed, {0, 0, 0});
                std::array<core::Point, 3> ways{Unit(Sub(level::InLevel(*room, placed, {1, 0, 0}), origin)),
                                                Unit(Sub(level::InLevel(*room, placed, {0, 1, 0}), origin)),
                                                Unit(Sub(level::InLevel(*room, placed, {0, 0, 1}), origin))};
                std::array<float, 3> low{};
                std::array<float, 3> high{};
                low.fill(std::numeric_limits<float>::infinity());
                high.fill(-std::numeric_limits<float>::infinity());
                for (const core::Triangle &triangle : own)
                {
                    for (const core::Point *corner : {&triangle.a, &triangle.b, &triangle.c})
                    {
                        for (std::size_t axis = 0; axis < 3; ++axis)
                        {
                            const float along = Dot(Sub(*corner, origin), ways.at(axis));
                            low.at(axis) = std::min(low.at(axis), along);
                            high.at(axis) = std::max(high.at(axis), along);
                        }
                    }
                }
                std::size_t thin = 0;
                for (std::size_t axis = 1; axis < 3; ++axis)
                {
                    thin = high.at(axis) - low.at(axis) < high.at(thin) - low.at(thin) ? axis : thin;
                }
                const std::size_t u = (thin + 1) % 3;
                const std::size_t v = (thin + 2) % 3;
                const auto corner = [&](float su, float sv, float off) {
                    const float alongU = ((low.at(u) + high.at(u)) / 2) + (su * kInFromTheEdge * (high.at(u) - low.at(u)) / 2);
                    const float alongV = ((low.at(v) + high.at(v)) / 2) + (sv * kInFromTheEdge * (high.at(v) - low.at(v)) / 2);
                    const float alongThin = ((low.at(thin) + high.at(thin)) / 2) + off;
                    core::Point at = origin;
                    for (std::size_t axis = 0; axis < 3; ++axis)
                    {
                        at.at(axis) += (ways.at(u).at(axis) * alongU) + (ways.at(v).at(axis) * alongV) +
                                       (ways.at(thin).at(axis) * alongThin);
                    }
                    return at;
                };
                // Which side is the room: the one that is closed in. Outside a
                // level there is nothing at all, so rays cast from the window's
                // middle land on something on the room's side and on nothing on
                // the other. (Taken the other way about, by where rays went
                // furthest, every window faced out and no light came in:
                // 2026-10-11.) A window stands in the thickness of its wall, in
                // this level a little behind the wall's own face: where there is
                // a face close before it on the room's side, the opening is put
                // just clear of that; else just clear of the model.
                const core::Point across = ways.at(thin);
                const core::Point back{-across[0], -across[1], -across[2]};
                constexpr float kLooked = 60.0F;
                const core::Point middle = corner(0, 0, 0);
                int ahead = 0;
                int behind = 0;
                for (const core::Point &ray : probes::Directions(128))
                {
                    if (Cast(scene, static_cast<std::uint32_t>(index), middle, ray, kLooked))
                    {
                        ++(Dot(ray, across) > 0 ? ahead : behind);
                    }
                }
                // And where rays land on both sides, as they do from a window
                // high in a tower's outer wall, with the level's other rooms
                // behind it across the round: a window is in a level's outer
                // wall, and the room is towards the level's middle.
                const bool roomAhead = std::abs(ahead - behind) > 48 ? ahead > behind
                                                                     : Dot(Sub(levelMiddle, middle), across) > 0;
                const core::Point inward = roomAhead ? across : back;
                constexpr float kFaceWithin = 1.5F; // metres: a face of the wall before the window
                constexpr float kClear = 0.15F;     // metres the opening stands before it
                const std::optional<Landing> face =
                    Cast(scene, static_cast<std::uint32_t>(index), middle, inward, kFaceWithin);
                const float proud =
                    face ? face->hit.distance + kClear : ((high.at(thin) - low.at(thin)) / 2) + kClear;
                faced += face ? 1 : 0;
                const float shift = roomAhead ? proud : -proud;
                openings.push_back({corner(-1, -1, shift), corner(1, -1, shift), corner(1, 1, shift)});
                openings.push_back({corner(-1, -1, shift), corner(1, 1, shift), corner(-1, 1, shift)});
                outward.push_back({-inward[0], -inward[1], -inward[2]});
                if (std::max({beam.sky[0], beam.sky[1], beam.sky[2]}) > 0)
                {
                    constexpr int kAcross = 6;
                    constexpr int kUp = 8;
                    Glowing glow;
                    glow.sky = true;
                    glow.colour = beam.sky;
                    glow.middle = corner(0, 0, shift);
                    glow.patch = kInFromTheEdge * kInFromTheEdge * (high.at(u) - low.at(u)) * (high.at(v) - low.at(v)) /
                                 static_cast<float>(kAcross * kUp);
                    for (int i = 0; i < kAcross; ++i)
                    {
                        for (int j = 0; j < kUp; ++j)
                        {
                            const core::Point at = corner(((static_cast<float>(i) + 0.5F) / kAcross * 2) - 1,
                                                          ((static_cast<float>(j) + 0.5F) / kUp * 2) - 1, shift);
                            glow.points.push_back(at);
                            glow.facings.push_back(inward);
                            const core::Point apart = Sub(at, glow.middle);
                            glow.reach = std::max(glow.reach, std::sqrt(Dot(apart, apart)));
                        }
                    }
                    scene.glows.push_back(std::move(glow));
                }
                spdlog::debug("relight: an opening at {:.1f} {:.1f} {:.1f}, {:.1f} by {:.1f} m, rays landing {} to {}, "
                              "a face {:.2f} m before it",
                              middle[0], middle[1], middle[2], high.at(u) - low.at(u), high.at(v) - low.at(v),
                              std::max(ahead, behind), std::min(ahead, behind), face ? face->hit.distance : -1.0F);
                bars.insert(bars.end(), own.begin(), own.end());
            }
        }
        Beaming beaming;
        beaming.toSource = Unit({-beam.travels[0], -beam.travels[1], -beam.travels[2]});
        beaming.colour = beam.colour;
        spdlog::info("relight: {}: a light from far off through {} openings of {}, {} of them behind a face of the wall",
                     levelRim.stem().string(), openings.size() / 2, Text(beam.named), faced);
        if (!openings.empty())
        {
            beaming.openings.emplace(openings);
            beaming.bars.emplace(bars);
            beaming.outward = std::move(outward);
            scene.beams.push_back(std::move(beaming));
        }
    }
    // Where the level's makers put a light, whether or not it is taken as one:
    // it says where a fitting's flame is.
    std::vector<core::Point> hints;
    for (const std::wstring &id : sources)
    {
        for (const std::wstring &layout : {id, level.Terrain() && id.ends_with(L"_0") ? id.substr(0, id.size() - 1) + L"1" : id})
        {
            for (const Lamp &lamp : level.LampsOf(layout))
            {
                hints.push_back(lamp.at);
            }
        }
    }
    // The flames, after every lamp of the level's own is known.
    const std::size_t levelsOwn = scene.lamps.size();
    for (const Flame &flame : options.flames)
    {
        std::vector<core::Point> flames;
        for (const std::wstring &id : sources)
        {
            const level::Room *room = level.RoomOf(id);
            for (const level::Placed &placed : room != nullptr ? room->models : std::vector<level::Placed>{})
            {
                if (Lower(placed.file).find(Lower(flame.named)) != std::wstring::npos)
                {
                    core::Point at = WhereItBurns(level, id, *room, placed, hints);
                    if (flame.lands > 0)
                    {
                        constexpr float kFrom = 0.5F;   // metres over the place the look down starts from
                        constexpr float kDeep = 200.0F; // and how far down it looks
                        const auto index = static_cast<std::uint32_t>(
                            std::ranges::find(ids, id) != ids.end() ? std::ranges::find(ids, id) - ids.begin() : 0);
                        const core::Point from{at[0], at[1], at[2] + kFrom};
                        if (const std::optional<Landing> floor = Cast(scene, index, from, {0, 0, -1}, kDeep))
                        {
                            at[2] = from[2] - floor->hit.distance;
                        }
                    }
                    flames.push_back(at);
                }
            }
        }
        // What an earlier rule lit is this rule's "already lit": a lantern's
        // rule before the candles' keeps the candle inside it from counting twice.
        AddFlames(scene, flames, flame, scene.lamps.size());
    }
    spdlog::info("relight: {}: {} lamps of the level's own, {} more of its flames", levelRim.stem().string(), levelsOwn,
                 scene.lamps.size() - levelsOwn);
    // The lightmaps, each at its own shape, no larger a side than asked.
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
    std::size_t covered = 0;
    for (std::size_t i = 0; i < ids.size(); ++i)
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
    std::vector<std::pair<Map *, std::uint32_t>> order; // every covered texel, for the threads to share out
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
        covered += static_cast<std::size_t>(std::ranges::count_if(map.texels, [](const Texel &texel) { return texel.covered; }));
    }
    spdlog::info("relight: {}: {} rooms, {} lamps, {} glowing things, {} lightmaps, {} texels to light; rays by {}",
                 levelRim.stem().string(), ids.size(), scene.lamps.size(), scene.glows.size(), scene.maps.size(), covered,
                 caster);
    ForEach(order.size(), [&](std::size_t i) {
        Map &map = *order[i].first;
        map.straight[order[i].second] = Straight(scene, map.texels[order[i].second], static_cast<std::uint32_t>(i), options,
                                                 map.glowed[order[i].second]);
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
    for (int bounce = 0; bounce < options.bounces; ++bounce)
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
    // And the figures' probes, of the same lights, beside the lightmaps: the
    // plugin takes them as they are in place of baking its own.
    if (withProbes)
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
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    spdlog::info("relight: {}: {} lightmaps written to {}, {} ms", levelRim.stem().string(), written, out.string(),
                 took.count());
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
        stream.write(reinterpret_cast<const char *>(places.data()),
                     static_cast<std::streamsize>(places.size() * sizeof(float)));
        spdlog::info("relight: {}: {} lamps of the level's own, said in its .lamps", levelRim.stem().string(), count);
    }
    std::set<std::wstring> done;
    if (!level.Terrain())
    {
        return LightRooms(level, levelRim, out, options, level.Rooms(), level.Rooms(), true, done);
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
    for (const std::wstring &id : near)
    {
        far.push_back(id.substr(0, id.size() - 1) + L"1");
    }
    far.emplace_back();
    std::size_t written = LightRooms(level, levelRim, out, options, first, near, true, done);
    written += LightRooms(level, levelRim, out, options, far, near, false, done);
    return written;
}

} // namespace relighter
