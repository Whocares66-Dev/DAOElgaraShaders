// How much of the sun reaches a point, from the game's two shadow maps: what
// Elgara Shaders puts in place of the game's own few taps (build_shaders.py).
//
// The game draws the sun's shadows into two maps, a near one and a far one
// (parallel-split shadow maps), and each shader that receives them compares
// its point's depth in a map with the depth stored there. Its own filter is
// four or five taps at fixed places, which is what makes a shadow's edge
// blocky. This takes sixteen, on a disc turned by a different angle at every
// pixel, so the steps of the map become a fine grain, and compares each with
// the depth the surface has at that tap, not at the centre, so a wide disc
// does not shadow a slope with itself. That slope is one triangle's: where
// two meet at an angle the taps over the edge are judged by the wrong one,
// which drew a dark line along the terrain's creases (seen in Redcliffe,
// 2026-10-10), so every tap is also let off kDaoBias of depth.
//
// g_vShadowData is the game's, as its shaders read it (static1.psh, read
// 2026-10-10): [0..3] the near map's matrix by rows, [4..6] the far map's
// first three and [7].xyz its fourth with a w of 1, [7].w the far map's
// reach, and [8] the plane that divides the two.

// The plugin's maps, fitted around the camera's focus (LevelShadows.cpp,
// Cascades.h), four in one atlas that the receivers read on the engine's near
// map's sampler, map i in the tile at column i % 2 and row i / 2; in
// registers of their own: map i's texture matrix's first three rows, to its
// tile's 0 to 1, at [3i..3i+2] (its fourth is 0, 0, 0, 1: they are square to
// the sun); [12].x 1
// where they are the plugin's, [12].y how far from what they were fitted
// around a map is read, as a share of its width; and [13].xyz that point, the
// camera's focus. Where [12].x is 0, the game's g_vShadowData is read, its
// maps fitted to its figures.
float4 daoShadowData[14] : register(c200);
// The shadows' blend (LevelShadows.cpp's FadeData): where the sun has just
// been aimed anew, the old aim's maps are kept, on the engine's far map's
// sampler, and a pixel reads the old or the new as daoShadowData[12].z goes
// from 0 to 1 (0, no fade): the old where its own number is past it, so the
// old edge dissolves into the new over the fade. Each pixel reads one map: a
// mix of both in one loop ran the figures' shaders out of their 32 temporary
// registers (2026-10-21), and costs a second lookup. [0..2] are the turn from
// the new aim to the old, by rows: a point turned by them and placed by a new
// map's rows is where the old map has it, but for [3 + map]'s offsets across
// and down (x, y) and its depth's scale and offset (z, w).
float4 daoShadowFade[7] : register(c193);
#define DAO_SHADOW_FADE daoShadowData[12].z

// The texels a side of each map of the plugin's atlas (LevelShadows.cpp's
// kTile), whatever size the engine's own maps are.
static const float kDaoAtlasTile = 2048;
// The penumbra's radius in the world, in metres, where the maps are the
// game's (the plugin's own have the sun's, DaoAtlasShadow), and the least it
// may be in texels of the map, under which the map's own steps show.
static const float kDaoPenumbra = 0.035;
static const float kDaoLeastTexels = 1.5;
// How much nearer the light than a tap's own surface a caster must be to
// shadow it, in metres: a centimetre, and half the disc for a crease.
static const float kDaoBias = 0.01 + 0.5 * kDaoPenumbra;

static const float2 kDaoDisc[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),  float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),   float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590),  float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790),
};

// The world place's slopes over the screen, taken by DaoTakeSlopes where
// every pixel of a quad runs: derivatives cannot be taken inside a branch
// the pixels of a quad may take apart, and fxc ran all of DaoShadow, both
// its ways, on every pixel to take them where they were asked (some 370
// instructions of static1.psh's 683 that ran whatever the options said,
// read 2026-10-22). Taken first, the shadow is behind a branch of its own.
static float3 daoWorldDx = 0;
static float3 daoWorldDy = 0;
void DaoTakeSlopes(float3 world)
{
    daoWorldDx = ddx(world);
    daoWorldDy = ddy(world);
}

// The depth's slope over a map from the map's place's slopes over the
// screen, `dx` and `dy`.
float2 DaoSlope(float3 dx, float3 dy)
{
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float2 slope = float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z);
    return abs(det) > 1e-12 ? slope / det : 0;
}

// The share of a filter's taps that read the new aim's maps: all but while
// the shadows fade, when the rest read the old aim's (daoShadowFade).
float DaoNewShare()
{
    return DAO_SHADOW_FADE > 0 ? DAO_SHADOW_FADE : 1;
}

// A map's depth at `uv`. The old aim's atlas is read as if it lay beside the
// new one, at x from 1 to 2 (DaoTapAt), so the one filter serves both: a
// shader with a second filter ran out of its 32 temporary registers (the
// scenery's and the figures', 2026-10-21). The engine's own maps are read
// below 1, never faded.
float DaoDepthAt(sampler2D map, float2 uv)
{
    if (uv.x >= 1)
    {
        return tex2Dlod(smpShadowPartition2, float4(uv.x - 1, uv.y, 0, 0)).x;
    }
    return tex2Dlod(map, float4(uv, 0, 0)).x;
}

// Where tap `i` of a filter looks, x and y in the atlas and z the depth to
// compare: the new map's place `at`, or, for the taps past the new aim's
// share while the shadows fade, the old map's `oldAt` in the old atlas. So
// each pixel goes from the old shadow to the new by sixteenths: a dissolve
// of whole pixels between the two showed as a checkered edge (the user,
// 2026-10-21: "the shadows dither when transitioning").
float3 DaoTapAt(int i, float3 at, float3 oldAt, float share, float2 origin, float scale, float2 tap)
{
    const float edge = 0.5 / kDaoAtlasTile;
    const bool old = (i + 0.5) / 16 > share;
    const float3 from = old ? oldAt : at;
    return float3(origin + float2(old ? 1 : 0, 0) + scale * clamp(from.xy + tap, edge, 1 - edge), from.z);
}

// The lit part of a disc around `at` (x, y in the map, z the depth there),
// whose depth has `slope` over the map. `bias` is kDaoBias as the map's depth.
// The map is at `origin` of the texture, `scale` of it wide: a tile of the
// atlas, its taps kept in it. While the shadows fade, the taps past `share`
// read the old map at `oldAt` (DaoTapAt).
float DaoShadowMap(sampler2D map, float3 at, float3 oldAt, float share, float2 slope, float2 turn, float radius,
                   float bias, float2 origin, float scale)
{
    const float reach = 2 * radius; // no tap's depth moves further than a surface at 63 degrees would
    float lit = 0;
    [loop] for (int i = 0; i < 16; i++)
    {
        const float2 tap =
            float2(kDaoDisc[i].x * turn.x - kDaoDisc[i].y * turn.y, kDaoDisc[i].x * turn.y + kDaoDisc[i].y * turn.x) *
            radius;
        const float3 tapAt = DaoTapAt(i, at, oldAt, share, origin, scale, tap);
        lit += DaoDepthAt(map, tapAt.xy) >= tapAt.z + clamp(dot(tap, slope), -reach, reach) - bias ? 1.0 : 0.0;
    }
    return lit / 16;
}

// The lit part of a disc around `at` as the game's maps are read, `m0` to
// `m2` its matrix's first three rows, over `w`.
float DaoShadowMap(sampler2D map, float3 at, float4 m0, float4 m1, float4 m2, float w, float2 turn, float radius,
                   float bias)
{
    const float3 dx = float3(dot(m0.xyz, daoWorldDx), dot(m1.xyz, daoWorldDx), dot(m2.xyz, daoWorldDx)) / w;
    const float3 dy = float3(dot(m0.xyz, daoWorldDy), dot(m1.xyz, daoWorldDy), dot(m2.xyz, daoWorldDy)) / w;
    return DaoShadowMap(map, at, at, 1, DaoSlope(dx, dy), turn, radius, bias, 0, 1);
}

// The tangent of the sun's angular radius (0.267 degrees, as seen from the
// Earth): a shadow's edge is as soft as the sun's disc is wide seen from the
// ground, so the disc a point is filtered over has this times the distance
// from what casts its shadow to it for its radius. A post's foot is sharp and
// its top's shadow soft, and a crown's dapples ten metres up are blurred by a
// hand's breadth.
static const float kDaoSunRadius = 0.00466;

// The mean depth of what is nearer the sun than `at` within `radius` of it,
// and how much of the disc that is: the first half of percentage-closer soft
// shadows (Fernando 2005).
float2 DaoBlocker(sampler2D map, float3 at, float3 oldAt, float share, float2 slope, float2 turn, float radius,
                  float bias, float2 origin, float scale)
{
    const float reach = 2 * radius;
    float sum = 0;
    float count = 0;
    [loop] for (int i = 0; i < 16; i++)
    {
        const float2 tap =
            float2(kDaoDisc[i].x * turn.x - kDaoDisc[i].y * turn.y, kDaoDisc[i].x * turn.y + kDaoDisc[i].y * turn.x) *
            radius;
        const float3 tapAt = DaoTapAt(i, at, oldAt, share, origin, scale, tap);
        const float depth = DaoDepthAt(map, tapAt.xy);
        if (depth < tapAt.z + clamp(dot(tap, slope), -reach, reach) - bias)
        {
            sum += depth;
            count += 1;
        }
    }
    return float2(count > 0 ? sum / count : at.z, count / 16);
}

// A point's place in a map (x, y its texture's, z its depth), the disc's
// radius there and the bias, by the map's matrix's rows.
void DaoMapPlace(float4 m0, float4 m1, float4 m2, float4 point4, float least, out float3 at, out float radius,
                 out float bias, float size)
{
    const float w = 1;
    at = float3(dot(m0, point4), dot(m1, point4), dot(m2, point4));
    at.z = saturate(at.z);
    radius = max(kDaoPenumbra * length(m0.xyz) / w, least);
    // A texel's width more than kDaoBias: a map fitted to the view has
    // texels of centimetres (some 7 in the far one at 2048), where the
    // game's, fitted to its figures, had millimetres; and a leaf or a blade
    // at a slant to the sun was shaded by its own neighbour in the map (the
    // trees over Redcliffe went dark, 2026-10-16).
    const float texel = 1 / (size * length(m0.xyz) / w);
    bias = (kDaoBias + texel) * length(m2.xyz) / w;
}

// How much the maps fitted to the view say of where the sun reaches at the
// point DaoShadow was last asked of: 1 within them, falling to 0 over the
// last one's edge; what is left the lightmap's alpha says (build_shaders.py's
// with_our_shadow). 0 where the maps are the game's.
static float daoShadowCovered = 0;

// How wide the band at a map's edge is over which a point goes over to the
// next map, as a share of the map's width: a hard edge was a line across the
// ground where the shadows' detail changed, which moved, and popped, as the
// camera did (the user, 2026-10-16).
static const float kDaoBand = 0.1;

// A point's place in one of the maps fitted to the view, with its disc and
// bias there, and how far it is in: 1 a band's width and a disc's inside the
// map's edge, 0 at its edge.
struct DaoInMap
{
    float3 at;
    float3 oldAt; // where the old aim's map has the point while the shadows fade (daoShadowFade), else `at`
    float radius;
    float bias;
    float inside;
    // The map's texture units, across and in depth, a metre.
    float across;
    float deep;
};

// The nearest the sun (x) and the farthest (y) anything is within `search`
// of `at` in the tile at `origin` (both in the tile's 0 to 1), from the
// atlas's min-max depth chain (LevelShadows.cpp's MakeNearest): the four
// texels of the first level whose texel is as wide as the search's disc,
// which hold the disc whole. Kept to the tile, whose texels are whole at
// every level to its own single one. The first level has its depth in red
// alone.
float2 DaoDepthsIn(float2 origin, float2 at, float search)
{
    const float across = 2 * search * kDaoAtlasTile; // the disc's width in the first level's texels
    const float level = clamp(ceil(log2(max(across, 1))), 0, 11);
    const float size = 2 * kDaoAtlasTile / exp2(level); // the atlas's texels a side at that level
    const float2 low = origin * size;
    const float2 high = low + 0.5 * size - 1;
    const float2 first = clamp(floor((origin + 0.5 * at) * size - 0.5), low, high);
    const float2 second = min(first + 1, high);
    float2 a = tex2Dlod(smpShadowPartition1, float4((first + 0.5) / size, 0, level)).xy;
    float2 b = tex2Dlod(smpShadowPartition1, float4((float2(second.x, first.y) + 0.5) / size, 0, level)).xy;
    float2 c = tex2Dlod(smpShadowPartition1, float4((float2(first.x, second.y) + 0.5) / size, 0, level)).xy;
    float2 d = tex2Dlod(smpShadowPartition1, float4((second + 0.5) / size, 0, level)).xy;
    if (level < 1)
    {
        a.y = a.x;
        b.y = b.x;
        c.y = c.x;
        d.y = d.x;
    }
    return float2(min(min(a.x, b.x), min(c.x, d.x)), max(max(a.y, b.y), max(c.y, d.y)));
}

// The lit part at `placed` in map `tile` of the atlas, its edge as soft as the
// sun makes it there: what may shade the point is looked for as far round it
// as the sun's disc reaches from the map's sunward side, and the point is
// filtered over the penumbra that what was found casts, no less than the
// map's own steps (the disc's least radius, placed.radius).
float DaoAtlasShadow(int tile, DaoInMap placed, float2 turn)
{
    const float2 origin = float2(tile % 2, tile / 2) * 0.5;
    // The old aim's map stays where it was as the party walks on, and a point
    // the new map holds may be off the old one's edge: its taps there read the
    // old atlas's edge, and a block of shade came and went before the party
    // as the shadows faded (the user, 2026-10-22). Off the old map, the new
    // map's alone.
    const bool oldHolds = all(placed.oldAt.xy > 0) && all(placed.oldAt.xy < 1);
    const float share = oldHolds ? DaoNewShare() : 1;
    // The depth's slope over the map, by the map's rows from the world's
    // slopes over the screen.
    const float4 m0 = daoShadowData[tile * 3];
    const float4 m1 = daoShadowData[tile * 3 + 1];
    const float4 m2 = daoShadowData[tile * 3 + 2];
    const float2 slope = DaoSlope(float3(dot(m0.xyz, daoWorldDx), dot(m1.xyz, daoWorldDx), dot(m2.xyz, daoWorldDx)),
                                  float3(dot(m0.xyz, daoWorldDy), dot(m1.xyz, daoWorldDy), dot(m2.xyz, daoWorldDy)));
    // As far round as the sun's disc reaches from the map's sunward end at
    // most, and then, twice, no further than it reaches from the nearest
    // thing within that: the map's end is the farthest caster's, up to 2 km
    // away along a low sun, and the search was metres wide on a map of six,
    // its sixteen taps strewn over the tile, thirty-two on most pixels
    // (2026-10-28, #61: 2.9 ms a frame at Redcliffe at 21:00). Nothing
    // within reach nearer the sun than the point less the bias and the
    // slope's allowance, and it is lit; everything within it nearer the sun
    // by more than those, and every tap of the filter, whose disc is no
    // wider than the search, is shaded: it is in the umbra (under a low sun,
    // most of a valley in a far hill's shade).
    float search = max(kDaoSunRadius * (placed.at.z / placed.deep) * placed.across, placed.radius);
    for (int round = 0; round < 2; round++)
    {
        const float2 depths = DaoDepthsIn(origin, placed.at.xy, search);
        if (depths.x >= placed.at.z + 2 * search - placed.bias)
        {
            return 1;
        }
        if (depths.y < placed.at.z - 2 * search - placed.bias)
        {
            return 0;
        }
        search = max(min(search, kDaoSunRadius * (max(placed.at.z - depths.x, 0) / placed.deep) * placed.across),
                     placed.radius);
    }
    const float2 blocker =
        DaoBlocker(smpShadowPartition1, placed.at, placed.oldAt, share, slope, turn, search, placed.bias, origin, 0.5);
    if (blocker.y <= 0)
    {
        return 1;
    }
    const float penumbra = kDaoSunRadius * ((placed.at.z - blocker.x) / placed.deep) * placed.across;
    return DaoShadowMap(smpShadowPartition1, placed.at, placed.oldAt, share, slope, turn, max(penumbra, placed.radius),
                        placed.bias, origin, 0.5);
}

// How far a point is looked up off its surface, along its facing towards the
// sun, in the map's texels: a surface read at its own place in the map is
// judged by itself, and the ferns' thin cards, at a slant to the sun, shaded
// themselves dark (the tactical view, 2026-10-16). Off it by a texel and a
// half, a surface is past its own depth and not past what shades it (normal
// offset, Holbert 2011).
static const float kDaoNormalOffset = 1.5;

DaoInMap DaoPlaceIn(int first, float4 point4, float3 facing, float least)
{
    DaoInMap placed;
    const float texel = 1 / (kDaoAtlasTile * length(daoShadowData[first].xyz));
    const float4 offPoint = point4 + float4(facing * (kDaoNormalOffset * texel), 0);
    DaoMapPlace(daoShadowData[first], daoShadowData[first + 1], daoShadowData[first + 2], offPoint, least, placed.at,
                placed.radius, placed.bias, kDaoAtlasTile);
    // The least the disc may be is the map's steps: the sun's own softness
    // is DaoAtlasShadow's.
    placed.radius = least;
    placed.across = length(daoShadowData[first].xyz);
    placed.deep = length(daoShadowData[first + 2].xyz);
    // By how far the point is from the focus across the map, not from the
    // map's edge: a map moves in steps, and its edge, and the change of
    // detail there, jumped with it as the party walked (the user,
    // 2026-10-16); the focus moves smoothly, and every map holds all within
    // daoShadowData[12].y of its width of it.
    const float4 focus = float4(daoShadowData[13].xyz, 1);
    const float2 focusAt = float2(dot(daoShadowData[first], focus), dot(daoShadowData[first + 1], focus));
    const float2 off = abs(placed.at.xy - focusAt);
    placed.inside = saturate((daoShadowData[12].y - max(off.x, off.y) - 1.5 * placed.radius) / kDaoBand);
    // And within its depth: a map is as deep along the sun as it is wide, and
    // a surface nearer the sun than its near side, a hillside over the party,
    // was clamped to that side and read as lit, a bright patch in the hill's
    // shade with the fine map's edges (the user, 2026-10-16).
    const float depth = dot(daoShadowData[first + 2], point4);
    placed.inside *= saturate(min(depth, 1 - depth) / kDaoBand);
    placed.oldAt = placed.at;
    if (DAO_SHADOW_FADE > 0)
    {
        // Where the old aim's map has the point (daoShadowFade).
        const float4 turned = float4(dot(daoShadowFade[0].xyz, offPoint.xyz), dot(daoShadowFade[1].xyz, offPoint.xyz),
                                     dot(daoShadowFade[2].xyz, offPoint.xyz), 1);
        const float4 shift = daoShadowFade[3 + first / 3];
        placed.oldAt =
            float3(dot(daoShadowData[first], turned) + shift.x, dot(daoShadowData[first + 1], turned) + shift.y,
                   saturate(dot(daoShadowData[first + 2], turned) * shift.z + shift.w));
    }
    return placed;
}

// The lit part at a point of the world from the maps fitted to the view: the
// finest that holds it, each of them holding the whole level inside its
// bounds, going over to the next over a band at its edge; past the last, lit,
// where the level's own lightmaps say what is shadowed. A map is chosen by
// what holds the point, not by how far ahead of the camera it is: the
// tactical camera looks down from 15 to 25 m, and the party's ground was
// read from the far map, its texels a hand wide (the user, 2026-10-16:
// "blocky & pixelated").
float DaoViewShadow(float4 point4, float3 facing, float2 turn, float least)
{
    // Only how far the point is inside each map is kept while the maps are
    // chosen, and a map's placing is made again where it is read: the four
    // placings held at once, behind a branch, left the level's shaders no
    // registers (fxc's X4505, 2026-10-22).
    daoShadowCovered = 1;
    const float inside0 = DaoPlaceIn(0, point4, facing, least).inside;
    if (inside0 >= 1)
    {
        return DaoAtlasShadow(0, DaoPlaceIn(0, point4, facing, least), turn);
    }
    const float inside1 = DaoPlaceIn(3, point4, facing, least).inside;
    float lit = 1;
    float covered = 0;
    if (inside1 < 1)
    {
        const float inside2 = DaoPlaceIn(6, point4, facing, least).inside;
        if (inside2 < 1)
        {
            const float inside3 = DaoPlaceIn(9, point4, facing, least).inside;
            if (inside3 > 0)
            {
                lit = lerp(1, DaoAtlasShadow(3, DaoPlaceIn(9, point4, facing, least), turn), inside3);
                covered = inside3;
            }
        }
        if (inside2 > 0)
        {
            lit = lerp(lit, DaoAtlasShadow(2, DaoPlaceIn(6, point4, facing, least), turn), inside2);
            covered = lerp(covered, 1, inside2);
        }
    }
    if (inside1 > 0)
    {
        lit = lerp(lit, DaoAtlasShadow(1, DaoPlaceIn(3, point4, facing, least), turn), inside1);
        covered = lerp(covered, 1, inside1);
    }
    if (inside0 > 0)
    {
        lit = lerp(lit, DaoAtlasShadow(0, DaoPlaceIn(0, point4, facing, least), turn), inside0);
        covered = lerp(covered, 1, inside0);
    }
    daoShadowCovered = covered;
    return lit;
}

// The lit part at a point of the world, as the game's shaders choose between
// the two maps and fade the far one out. `pixel` is the pixel's place on the
// screen (VPOS). DaoTakeSlopes must have been given the point first.
float DaoShadow(float3 world, float2 pixel)
{
    const float4 point4 = float4(world, 1);
    // Interleaved gradient noise (Jimenez 2014): an angle per pixel.
    const float angle = 6.2831853 * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const float2 turn = float2(cos(angle), sin(angle));
    const float least = kDaoLeastTexels / DAO_SHADOW_MAP_SIZE;
    [branch] if (daoShadowData[12].x > 0)
    {
        // The surface's facing from its own slopes over the screen, turned
        // to the sun's side: the side a surface is looked up off.
        const float3 across = cross(daoWorldDx, daoWorldDy);
        const float3 toSun = -normalize(daoShadowData[2].xyz);
        float3 facing = dot(across, across) > 0 ? normalize(across) : toSun;
        facing = dot(facing, toSun) < 0 ? -facing : facing;
        return DaoViewShadow(point4, facing, turn, kDaoLeastTexels / kDaoAtlasTile);
    }

    const float nearW = dot(g_vShadowData[3], point4);
    float3 nearAt =
        float3(dot(g_vShadowData[0], point4), dot(g_vShadowData[1], point4), dot(g_vShadowData[2], point4)) / nearW;
    nearAt.z = saturate(nearAt.z);
    const float nearRadius = max(kDaoPenumbra * length(g_vShadowData[0].xyz) / nearW, least);
    const float nearBias = kDaoBias * length(g_vShadowData[2].xyz) / nearW;

    const float farW = dot(float4(g_vShadowData[7].xyz, 1), point4);
    float3 farAt =
        float3(dot(g_vShadowData[4], point4), dot(g_vShadowData[5], point4), dot(g_vShadowData[6], point4)) / farW;
    farAt.z = saturate(farAt.z);
    const float farRadius = max(kDaoPenumbra * length(g_vShadowData[4].xyz) / farW, least);
    const float farBias = kDaoBias * length(g_vShadowData[6].xyz) / farW;

    const float nearLit = DaoShadowMap(smpShadowPartition1, nearAt, g_vShadowData[0], g_vShadowData[1],
                                       g_vShadowData[2], nearW, turn, nearRadius, nearBias);
    const float farLit = DaoShadowMap(smpShadowPartition2, farAt, g_vShadowData[4], g_vShadowData[5], g_vShadowData[6],
                                      farW, turn, farRadius, farBias);

    const float side = dot(world, g_vShadowData[8].xyz) + g_vShadowData[8].w;
    const float fade = saturate((g_vShadowData[7].w - side) / (0.25 * g_vShadowData[7].w));
    // The near map is left a disc's width before its edge: a tap past the
    // edge reads nothing that was drawn, and the join between the two maps
    // showed as a dark line across the ground (seen in the tactical view,
    // 2026-10-11).
    const float2 margin = min(nearAt.xy, 1 - nearAt.xy);
    const bool inside = min(margin.x, margin.y) > 1.5 * nearRadius;
    return side > 0 && inside ? nearLit : lerp(1, farLit, fade);
}
