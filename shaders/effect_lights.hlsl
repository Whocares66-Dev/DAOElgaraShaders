// The lights of effects (a flaming blade, a spell, a fire's glow), as the
// plugin hands every lit surface them each frame (EffectLights.cpp): each
// light as the engine hands a shader one of its point lights, its place and
// reach at [2i] and its colour at [2i + 1], and how many there are in
// daoEffectCount.x. Each shader runs its own reckoning of one point light
// once for each (build_shaders.py's with_effect_lights), so an effect lights
// every surface it reaches by the surface's own material, not the few meshes
// the engine would have chosen it for.

#ifndef DAO_EFFECT_LIGHTS_HLSL
#define DAO_EFFECT_LIGHTS_HLSL

float4 daoEffectLights[32] : register(c160);
float4 daoEffectCount : register(c192);

// A texture read in a light's reckoning, run in a loop as long as the lights:
// at its first level, a loop of no fixed length allowing no gradient. What a
// reckoning reads is a table (a face's scattering by the light's angle).
float4 DaoTexFirstLevel(sampler2D map, float2 at)
{
    return tex2Dlod(map, float4(at, 0, 0));
}

// The light the effects scatter from a surface at `world` facing `normal`,
// for a shader that reckons no point light of its own (the ground's: the
// engine gives terrain the sun alone). One light as the level's shaders
// reckon it (static1.psh, read 2026-10-17): its colour by how the surface
// faces it and by cos^2 of a quarter turn times the distance over its reach,
// the distance held to a millimetre at the least.
float3 DaoEffectDiffuse(float3 world, float3 normal)
{
    float3 sum = 0;
    [loop] for (int i = 0; i < daoEffectCount.x; i++)
    {
        const float4 place = daoEffectLights[2 * i];
        const float3 way = place.xyz - world;
        const float distance = max(length(way), 0.001);
        const float fall = cos(1.5707963 * saturate(distance / place.w));
        sum += fall * fall * saturate(dot(normal, way / distance)) * daoEffectLights[2 * i + 1].rgb;
    }
    return sum;
}

#endif
