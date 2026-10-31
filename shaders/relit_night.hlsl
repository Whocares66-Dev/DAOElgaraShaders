// An interior's night by its own lamps and candles (build_shaders.py's
// with_relit_night): beside the makers' lightmap a level's shader reads the
// relighter's of the same room, lit by its fittings alone, and goes to it as
// the daylight goes (DayNight.h's RelitNight). The makers baked the candles'
// light into the same texels as their moonlight, so kept to a share at night
// the candles were dimmed with the moon and lit nothing (the user,
// 2026-10-29: "candles don't actually have light").
// x the night's share, 0 where no night lightmap is bound for the draw
// (NightLightmaps.cpp); y what the relit light is multiplied by (1, the
// fittings' strengths as the relighter has them; outdoors.tune's RelitNight).
// The makers' bake stays under it as the hour has it, kept at night to its
// file's `fill` in the night's colour: the night's blue with the lamps on it
// (the user, 2026-10-30: "some combination of the blue as night look with the
// more principled lighting attempt ... maybe something like 25% to 50% fill
// with night color").
#ifndef DAO_RELIT_NIGHT_HLSL
#define DAO_RELIT_NIGHT_HLSL

float4 daoRelitNight : register(c157);
sampler2D smpDaoNightLightmap;

// The relighter keeps the root of the light's share of eight whites
// (Relighter.cpp), so that a candle's pool is more than a flat patch in eight
// bits.
static const float kDaoRelitMost = 8;

float3 DaoRelitNight(float3 stored)
{
    return stored * stored * kDaoRelitMost * daoRelitNight.y;
}

// The makers' bake as the hour has it (`baked`) with the night's lamps on it,
// `made` the makers' bake as it is in the lightmap. Where the lamps' light is
// as bright as the makers' bake, it is the light the makers baked there: the
// lamps' takes its place, and the bake is kept, in the night's blue, only
// where the lamps' light is less. Added to it whole, a candle's pool on a
// pillar was the makers' own candle glow kept in the night's violet with the
// candle's orange on top, a pale cream (the library's pillar at midnight
// 121, 70, 76 of 255, the lamps adding 64, 46, 26: the user, 2026-10-30, "the
// blue fill shouldn't reduce the orange of fire light").
float3 DaoWithRelitNight(float3 baked, float3 made, float3 stored)
{
    const float3 lamps = DaoRelitNight(stored);
    const float3 luminance = float3(0.2126, 0.7152, 0.0722);
    const float theirs = saturate(dot(lamps, luminance) / max(dot(made, luminance), 0.0001));
    return (baked * (1 - (daoRelitNight.x * theirs))) + (lamps * daoRelitNight.x);
}

#endif
