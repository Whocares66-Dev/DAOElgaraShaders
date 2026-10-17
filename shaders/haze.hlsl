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

// The colour what writes no distance fogs towards: effects (a mist's cards,
// smoke, a spell), hair and props drawn blended, which the air's light's
// pass cannot find (build_shaders.py's with_mean_haze_fog). The air's light
// on the mean over all ways, its sun's part's phase being 1 on the mean.
float4 DaoMeanFog(float4 own)
{
    return DAO_HAZE > 0 ? float4(DAO_HAZE_AIR + DAO_HAZE_SUN, own.w) : own;
}
#endif

#endif
