// The night's light on what a level's bake lights (build_shaders.py's
// with_baked_light). At night the day level's bake, its sky's and its sun's
// light where they fell by day, gives way to the night sky's even light: the
// day's bake dimmed kept the day's light where it fell, the windmill and the
// houses lit from the front with nothing to light them, and the night
// level's own bake, its lightmaps laid out apart from the day's, could not be
// read through the day's atlases without mistakes (the user, 2026-10-21:
// "just go back to using a day level, but suppress the baked lighting").
// x is the night's share, 0 by day and 1 at night (DayNight.h's NightShare);
// y, z, w the night sky's light, as the lightmaps keep light (the day
// level's lightmaps' mean is its sky's light, about 0.16, 0.20, 0.16).
#ifndef DAO_NIGHT_SKY_HLSL
#define DAO_NIGHT_SKY_HLSL

float4 daoNightSky : register(c215);

// An interior's bake by day (DayNight.h's BakeHue): xyz the bake's mean
// colour at a luminance of 1, w how far each texel takes that colour at its
// own luminance, before the hour's multiplier (DAO_BAKED_LIGHT) turns that
// colour the daylight's. Its makers lit the Circle tower's floors as a night,
// violet stone and orange pools by its candles, and in the daylight's colour
// multiplied each channel at a time the pools went red-brown and the stone
// gray: the hall by the children at noon 42.6, 17.6, 15.0 of 255 (2026-10-25).
float4 daoBakeHue : register(c155);

float3 DaoBakeHued(float3 baked)
{
    return lerp(baked, dot(baked, float3(0.2126, 0.7152, 0.0722)) * daoBakeHue.xyz, daoBakeHue.w);
}

#endif
