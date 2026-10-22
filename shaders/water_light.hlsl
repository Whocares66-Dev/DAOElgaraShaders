// The hour's light on water (build_shaders.py's with_water_light): x how
// strong, over the level's own by day; y how much of its colour is kept
// (DayNight.h's Water). 1 and 1 where the clock is not running.
#ifndef DAO_WATER_LIGHT_HLSL
#define DAO_WATER_LIGHT_HLSL

float4 daoWaterLight : register(c159);

#endif
