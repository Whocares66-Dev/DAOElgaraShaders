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

// The lights' shadows: what stands about each light drawn into six maps, a
// cube's faces, side by side in one atlas, a row of six to a light, in the
// order daoEffectLights has them (LevelShadows.cpp's DrawEffectShadows). A
// face looks along an axis of the world, +x, -x, +y, -y, +z, -z, from the
// light's place, a quarter turn wide and high, from kDaoEffectNear to the
// light's reach; it holds, for each texel, a + b / z of what is nearest the
// light, z the distance along the face's axis (the engine's shadow pass
// writes the depth over w). Across a face its x and y are the world's two
// other axes in order, y up the face.
static const float kDaoEffectTile = 256;
static const int kDaoEffectLightsMost = 16;
static const float kDaoEffectNear = 0.05;

// A point's place in light i's maps: the atlas's x and y, and its distance
// along the face's axis.
float3 DaoEffectPlace(int i, float3 world)
{
    const float4 light = daoEffectLights[2 * i];
    const float3 d = world - light.xyz;
    const float3 a = abs(d);
    float face;
    float z;
    float2 across;
    if (a.x >= a.y && a.x >= a.z)
    {
        face = d.x >= 0 ? 0 : 1;
        z = a.x;
        across = d.yz;
    }
    else if (a.y >= a.z)
    {
        face = d.y >= 0 ? 2 : 3;
        z = a.y;
        across = d.xz;
    }
    else
    {
        face = d.z >= 0 ? 4 : 5;
        z = a.z;
        across = d.xy;
    }
    const float2 uv = float2(0.5 + 0.5 * across.x / z, 0.5 - 0.5 * across.y / z);
    // Kept two texels inside the face's own tile, for the taps about it.
    const float2 inside = clamp(uv, 2 / kDaoEffectTile, 1 - 2 / kDaoEffectTile);
    return float3((face + inside.x) / 6, (i + inside.y) / kDaoEffectLightsMost, z);
}

// How much of light i reaches a point of the world, past what stands nearer
// the light than it: a three-texel square of the map about the point, each
// texel's answer by the distance it holds against the point's own, less a
// bias of two centimetres and three texels' width there (the maps' steps and
// the filter's breadth), weighed by how far the point lies across its own
// texel, so that the shadow's edge goes smoothly from texel to texel. Four
// taps a texel apart, each a plain yes or no, cut a figure's shadow from a
// fire into steps on its own body (the user, 2026-10-23: "shadows still very
// pixelated on chars' bodies").
float DaoEffectShadow(sampler2D map, int i, float3 world)
{
    const float far = daoEffectLights[2 * i].w;
    const float a = far / (far - kDaoEffectNear);
    const float b = -kDaoEffectNear * a;
    const float3 at = DaoEffectPlace(i, world);
    const float bias = 0.02 + 6 * at.z / kDaoEffectTile;
    const float2 size = float2(6 * kDaoEffectTile, kDaoEffectLightsMost * kDaoEffectTile);
    const float2 texels = at.xy * size - 0.5;
    const float2 f = frac(texels);
    const float2 corner = floor(texels);
    float lit = 0;
    [unroll] for (int y = -1; y <= 2; y++)
    {
        const float wy = y == -1 ? 1 - f.y : (y == 2 ? f.y : 1);
        [unroll] for (int x = -1; x <= 2; x++)
        {
            const float wx = x == -1 ? 1 - f.x : (x == 2 ? f.x : 1);
            const float2 tap = (corner + float2(x, y) + 0.5) / size;
            const float stored = tex2Dlod(map, float4(tap, 0, 0)).x;
            // The stored a + b / z back to z; nothing drawn there (1) is the reach.
            const float nearest = stored < 1 ? b / (stored - a) : far;
            lit += (nearest >= at.z - bias ? 1.0 : 0.0) * wx * wy;
        }
    }
    return lit / 9;
}

// A light's shadow at a point, where the shader has a sampler for the atlas;
// where it has none left (a face's: Direct3D 9's sixteen all taken), none.
#ifdef DAO_NO_EFFECT_SHADOWS
#define DAO_EFFECT_SHADOW(i, world) 1.0
#else
#define DAO_EFFECT_SHADOW(i, world) DaoEffectShadow(smpDaoEffectShadows, i, world)
#endif

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
        sum += fall * fall * saturate(dot(normal, way / distance)) * daoEffectLights[2 * i + 1].rgb *
               DAO_EFFECT_SHADOW(i, world);
    }
    return sum;
}

#endif
