// A level's baked light, shaped by the surface's normal map. Elgara Shaders
// puts this where a level's shader reads its lightmap (build_shaders.py).
//
// A level's shader lays down a baked lightmap and adds the area's sun to it.
// The sun is lit through the surface's normal map: every brick and plank has
// a lit side and a dark one. The bake is one colour a texel of the lightmap,
// worked out for the wall as a flat thing, and the normal map has no say in
// it; so wherever the sun does not reach, in any shade and in every room, a
// wall is its picture and nothing more. That is the flatness of the game's
// levels (the user, 2026-10-10: "the baked otherwise makes everything look
// flat").
//
// So the bake is given the relief the map holds: more where the map tilts
// the surface towards the light than the flat wall is, less where it tilts
// it away. Where the light comes from the bake does not say; it is taken
// from above, leaning to the sun's side, as the sky's light and the sun's
// bounce come outdoors and as lamps hang indoors. A flat stretch is left as
// bright as it was, so an area keeps the brightness it was made with.
//
// DAO_RELIEF (look.hlsl) is how strongly: 0 is the game's own.
//
// A figure's ambient light is the same case, a colour worked out a vertex
// with no word from its normal map, and is shaped the same way
// (build_shaders.py's with_ambient_relief), from straight above.
//
// Two forms came before this, both a share of the bake swapped for a light
// from the sky. The first took the fog's colour for that light, and an area
// with a dark fog came out a quarter darker than the game draws it; the
// second kept the bake's brightness and shaped it by whether a surface faces
// up, which the bake had already reckoned with (both measured in ten areas,
// 2026-10-11).

// How far the light leans from straight above to the sun's side.
static const float kDaoReliefLean = 0.6;
// The least and the most a point's baked light is multiplied by.
static const float kDaoReliefLeast = 0.45;
static const float kDaoReliefMost = 1.7;

// How much more a point faces the light for its normal map than the flat
// surface does: `mapped` the normal with the map, `flat` the vertex's, both
// in the world (z up); `toSun` the way to the sun, or nothing where there is
// none.
float DaoRelief(float3 mapped, float3 flat, float3 toSun)
{
    const float3 light = normalize(float3(toSun.xy * kDaoReliefLean, 1));
    return dot(normalize(mapped) - normalize(flat), light);
}

// What a light the normal map had no say in is multiplied by, for
// DaoRelief's answer.
float DaoReliefFactor(float relief)
{
    return clamp(1 + DAO_RELIEF * relief, kDaoReliefLeast, kDaoReliefMost);
}

// The least and the most the baked light is multiplied by for its contrast.
static const float kDaoContrastLeast = 0.3;
static const float kDaoContrastMost = 3.0;

// The lightmap's colour as the surface is to be lit by it. `baked` is the
// lightmap's colour, `relief` DaoRelief's answer.
//
// And the baked light's own contrast. A lightmap is eight bits from black to
// white, made for a picture of eight bits: the floor under a lamp is twice
// as bright as the far corner of its room where a lamp's light would make
// it twenty times, which is why a room reads as evenly grey. So light
// brighter than DAO_LIGHT_PIVOT is made brighter and light dimmer than it
// dimmer, by the power DAO_LIGHT_CONTRAST of how far it is from the pivot;
// at the pivot it stays, so a room keeps the brightness it was made with if
// the pivot is its middle. 0 is the game's own.
float3 DaoShapedBake(float3 baked, float relief)
{
    baked = lerp(baked, baked * baked * kDaoRelitMost, saturate(DAO_RELIT));
    const float luma = dot(baked, float3(0.2126, 0.7152, 0.0722));
    const float contrast =
        clamp(pow(max(luma, 1e-3) / max(DAO_LIGHT_PIVOT, 1e-3), DAO_LIGHT_CONTRAST), kDaoContrastLeast, kDaoContrastMost);
    // Out of doors the baked light is the sky's, and the sun is added to it
    // warm: shade that is the sun's own colour, only dimmer, is what makes a
    // sunlit village one sheet of yellow (the user, 2026-10-11: "I don't know
    // if the change is actually look better").
    return baked * DaoReliefFactor(relief) * contrast * lerp(1.0, kDaoSkyLight, DAO_COOL_SHADE) *
           (1 - saturate(DAO_BAKED_LESS));
}
