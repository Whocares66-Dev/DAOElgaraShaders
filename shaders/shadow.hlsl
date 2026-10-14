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

// The penumbra's radius in the world, in metres, and the least it may be in
// texels of the map, under which the map's own steps show.
static const float kDaoPenumbra = 0.035;
static const float kDaoLeastTexels = 1.5;
// How much nearer the light than a tap's own surface a caster must be to
// shadow it, in metres: a centimetre, and half the disc for a crease.
static const float kDaoBias = 0.01 + 0.5 * kDaoPenumbra;

static const float2 kDaoDisc[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),  float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790),
};

// The lit part of a disc around `at` (x, y in the map, z the depth there).
// `bias` is kDaoBias as the map's depth.
float DaoShadowMap(sampler2D map, float3 at, float2 turn, float radius, float bias)
{
    // The depth's slope over the map, from the three's slopes over the screen.
    const float3 dx = ddx(at);
    const float3 dy = ddy(at);
    const float det = dx.x * dy.y - dx.y * dy.x;
    float2 slope = float2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z);
    slope = abs(det) > 1e-12 ? slope / det : 0;
    const float reach = 2 * radius; // no tap's depth moves further than a surface at 63 degrees would
    float lit = 0;
    for (int i = 0; i < 16; i++)
    {
        const float2 tap = float2(kDaoDisc[i].x * turn.x - kDaoDisc[i].y * turn.y,
                                  kDaoDisc[i].x * turn.y + kDaoDisc[i].y * turn.x) * radius;
        const float depth = at.z + clamp(dot(tap, slope), -reach, reach) - bias;
        lit += tex2Dlod(map, float4(at.xy + tap, 0, 0)).x >= depth ? 1.0 : 0.0;
    }
    return lit / 16;
}

// The lit part at a point of the world, as the game's shaders choose between
// the two maps and fade the far one out. `pixel` is the pixel's place on the
// screen (VPOS).
float DaoShadow(float3 world, float2 pixel)
{
    const float4 point4 = float4(world, 1);
    // Interleaved gradient noise (Jimenez 2014): an angle per pixel.
    const float angle = 6.2831853 * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const float2 turn = float2(cos(angle), sin(angle));
    const float least = kDaoLeastTexels / DAO_SHADOW_MAP_SIZE;

    const float nearW = dot(g_vShadowData[3], point4);
    float3 nearAt = float3(dot(g_vShadowData[0], point4), dot(g_vShadowData[1], point4),
                           dot(g_vShadowData[2], point4)) / nearW;
    nearAt.z = saturate(nearAt.z);
    const float nearRadius = max(kDaoPenumbra * length(g_vShadowData[0].xyz) / nearW, least);
    const float nearBias = kDaoBias * length(g_vShadowData[2].xyz) / nearW;

    const float farW = dot(float4(g_vShadowData[7].xyz, 1), point4);
    float3 farAt = float3(dot(g_vShadowData[4], point4), dot(g_vShadowData[5], point4),
                          dot(g_vShadowData[6], point4)) / farW;
    farAt.z = saturate(farAt.z);
    const float farRadius = max(kDaoPenumbra * length(g_vShadowData[4].xyz) / farW, least);
    const float farBias = kDaoBias * length(g_vShadowData[6].xyz) / farW;

    const float nearLit = DaoShadowMap(smpShadowPartition1, nearAt, turn, nearRadius, nearBias);
    const float farLit = DaoShadowMap(smpShadowPartition2, farAt, turn, farRadius, farBias);

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
