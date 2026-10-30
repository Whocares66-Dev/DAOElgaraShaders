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

// The makers' bake as the hour has it (`baked`) with the night's lamps on it.
float3 DaoWithRelitNight(float3 baked, float3 stored)
{
    return baked + DaoRelitNight(stored) * daoRelitNight.x;
}

#endif
