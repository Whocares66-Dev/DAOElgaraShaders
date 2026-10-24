// The air's light: what fog is lit by, seen one way (Outdoors.cpp has where
// its numbers come from). The air's light's pass puts it in place of the
// area's fog colour over every surface, by the fog the engine reckoned for
// it (haze_pass.hlsl), and the sky and the clouds fog towards it in their
// own shaders (build_shaders.py's with_lit_sky_fog), so the two meet at the
// horizon.
//
// `toSun` is the cosine between the way one looks and the way to the sun;
// `air` the part the air's molecules scatter, by Rayleigh's phase; `sun` the
// part its haze scatters, by Henyey-Greenstein's of asymmetry `g`. Each
// phase is times 4 pi, so that its mean over all ways is 1, and the two
// parts are the area's own fog colour together (Outdoors.cpp's HazeOf): the
// light is sent where the air sends it, and none is added.

#ifndef DAO_HAZE_HLSL
#define DAO_HAZE_HLSL

float3 DaoHazeLight(float toSun, float3 air, float3 sun, float g)
{
    const float rayleigh = 0.75 * (1 + toSun * toSun);
    const float mie = (1 - g * g) / pow(max(1 + g * g - 2 * g * toSun, 1e-4), 1.5);
    return air * rayleigh + sun * mie;
}

#ifdef DAO_LOOK_HLSL
// The colour the sky and the clouds fog towards, seen along `way` (any
// length) with the sun's light travelling along `sunGoing`: the air's light
// where the plugin says the haze is ours, `own` (the area's fog colour) where
// not. The plugin's numbers are in look.hlsl's registers, which are set on
// the device for every draw.
float4 DaoSkyFog(float3 way, float3 sunGoing, float4 own)
{
    if (DAO_HAZE <= 0)
    {
        return own;
    }
    const float3 light = DaoHazeLight(dot(normalize(way), -sunGoing), DAO_HAZE_AIR, DAO_HAZE_SUN, DAO_HAZE_G);
    return float4(light, own.w);
}

// The sky's colour as it is written: cut at white as the game cuts it, or,
// with the haze ours, left to the picture's roll-off above white.
float3 DaoSkyCut(float3 colour)
{
    return DAO_HAZE > 0 ? max(colour, 0) : saturate(colour);
}

// The view a surface's own shader reckons its haze by (Outdoors.h's
// HazeView): [0..2] the camera's axes in the world, right and down the
// picture and ahead, with one over the picture's width, one over its height
// and the lens's width over its focal length in their w; [3] the way to the
// sun in the world and the lens's height over its focal length; [4] the sky's
// fog's cap, its zenith in radians, the fog's intensity and 1 where the haze
// is ours. Every surface fogs itself so, in its own shader, with the game's
// anti-aliasing on as off: a pass reckoning it from the distance beside the
// picture had, with it on, only what the game's depth pass draws, and leaves,
// reeds, water and da* meshes have none (issue 57: a forest's ground a flat
// pale wall and its pines dark against it).
float4 daoHazeView[5] : register(c150);

// The way a pixel looks along, in the world, from its place on the picture
// (VPOS, its corner).
float3 DaoViewRay(float2 pixel)
{
    const float2 at =
        ((pixel + 0.5) * float2(daoHazeView[0].w, daoHazeView[1].w) - 0.5) * float2(daoHazeView[2].w, daoHazeView[3].w);
    return normalize(at.x * daoHazeView[0].xyz + at.y * daoHazeView[1].xyz + daoHazeView[2].xyz);
}

// How much the sky fogs itself along `ray` (the world's, its z up) by its own
// rule (sky.psh, read 2026-10-17): the fog's cap, less as the way rises, by
// exp(-z^3 / (0.0636620 zenith)). A zenith of 0.0015708 radians or less fogs
// none of the sky (lib-agent-2, DAOLib's dev/SKY.md, 2026-10-18), and so
// bounds no surface's fog: 1. As the sky's own, by exp2: fxc's optimizer does
// not settle on exp here.
float DaoSkyFogAlong(float3 ray)
{
    const float zenith = daoHazeView[4].y;
    if (zenith <= 0.0015708)
    {
        return 1;
    }
    return daoHazeView[4].x * saturate(exp2(ray.z * ray.z * ray.z * (-1.4426950 / (0.0636620 * zenith))));
}

// The air's light seen along `ray`, the sun's part all lit: what the sun's
// shadows in the air take away is the pass's (haze_pass.hlsl).
float3 DaoHazeAlong(float3 ray)
{
    return DaoHazeLight(dot(ray, daoHazeView[3].xyz), DAO_HAZE_AIR, DAO_HAZE_SUN, DAO_HAZE_G);
}

// What a surface fogged by the engine is to have added, where the haze is
// ours: of the fog the engine gave it (1 - `kept`, towards `fogColour`, the
// area's), as much as the sky's own fog along the same way, in the air's
// light seen that way and not the area's colour. The air out to a surface
// is a part of all the air the sky is fogged by, which thins as the way
// rises: the engine fogs by distance alone, and Haven's mountain, high over
// the eye and fogged as the horizon is, came out brighter than the sky about
// its peaks (the user, 2026-10-23, issue 53). What the engine fogged more
// is left in the area's colour, vanilla's: taken back to the surface's own,
// a mountain's came out many times brighter than it was lit.
float3 DaoSurfaceHaze(float2 pixel, float kept, float3 fogColour)
{
    if (DAO_HAZE <= 0 || daoHazeView[4].w <= 0)
    {
        return 0;
    }
    const float3 ray = DaoViewRay(pixel);
    const float fog = min(1 - saturate(kept), DaoSkyFogAlong(ray));
    return fog * (DaoHazeAlong(ray) - fogColour);
}

// The colour what writes no distance fogs towards: effects (a mist's cards,
// smoke, a spell), hair and props drawn blended (build_shaders.py's
// with_ray_haze_fog): the air's light seen the way the pixel looks.
float4 DaoRayFog(float2 pixel, float4 own)
{
    if (DAO_HAZE <= 0 || daoHazeView[4].w <= 0)
    {
        return own;
    }
    return float4(DaoHazeAlong(DaoViewRay(pixel)), own.w);
}
#endif

#endif
