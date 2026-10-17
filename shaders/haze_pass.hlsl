// The air between the eye and what it sees, lit by the sun and the sky: a
// pass of the colour grade's chain, after the depth in metres
// (occlusion_depth.hlsl) and before the anti-aliasing, so that an edge
// against the sky is smoothed with its haze on (build_shaders.py; the
// numbers are Outdoors.cpp's).
//
// The game fogs every surface and its sky towards one colour the area gives,
// whichever way one looks: in Redcliffe a dark grey-green, which is much of
// what makes its day dim and flat (measured 2026-10-16). Haze in daylight is
// brighter towards the sun. So where the engine fogged a surface towards the
// area's colour, the air's light seen that way is put in its place here, by
// the same amount of fog: what is added is the difference of the two, and a
// pixel whose fog is not known right is as the game drew it, wrong by no
// more than that difference. The sky and the clouds fog towards the same
// light in their own shaders (haze.hlsl's DaoSkyFog), and so do what writes
// no distance (DaoMeanFog).
//
// How much fog is the engine's own for each pixel: every surface's shader
// keeps how much of its light the fog left in the alpha of the distance it
// writes beside the picture (build_shaders.py's with_fog_kept). Reckoned
// again from the distance, exp(-w d^2) as terrain1.vsh has it, it was 20
// levels of 255 off on Redcliffe's hill: the engine reckons it per vertex
// and lays it across each triangle.

#include "haze.hlsl"

sampler2D smpScene : register(s0);
sampler2D smpMetres : register(s1);
sampler2D smpDepth : register(s2); // the game's distance target, its alpha the fog kept
float fFocalPlane;
float fDaoLensX;
float fDaoLensY;
float fDaoHaze; // 0 for none (the area's fog colour left as it is), 1 for ours
// The colour the engine fogs surfaces towards: the area's.
float fDaoFogR;
float fDaoFogG;
float fDaoFogB;
// The way to the sun in the camera's axes: x right, y down the picture, z
// ahead.
float fDaoSunX;
float fDaoSunY;
float fDaoSunZ;
// The air's light (haze.hlsl's DaoHazeLight): the part the air's molecules
// scatter, the part its haze scatters, and that's asymmetry.
float fDaoAirR;
float fDaoAirG;
float fDaoAirB;
float fDaoSunR;
float fDaoSunG;
float fDaoSunB;
float fDaoHazeG;
// What to show to check the haze by: 1, the fog as white air and no sun.
float fDaoHazeShow;

// The sky's distance as the game writes it (sky.psh's oC1, a constant) is
// 0.99998 of the focal plane, and the focal plane is over 4 km (seen in
// Redcliffe, 2026-10-16), at which no fog leaves anything to be seen. So what
// is that far is sky, which fogs itself, and is left alone here. What else
// wrote the sky's distance writes 0.9 of the focal plane now
// (build_shaders.py's with_far_distance).
static const float kDaoSkyFrom = 0.999;

bool DaoIsSky(float2 uv)
{
    return tex2Dlod(smpMetres, float4(uv, 0, 0)).r >= kDaoSkyFrom * fFocalPlane;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float4 scene = tex2D(smpScene, uv);
    if (fDaoHaze <= 0 || DaoIsSky(uv))
    {
        return scene;
    }
    const float2 at = (uv - 0.5) * float2(fDaoLensX, fDaoLensY);
    const float3 ray = normalize(float3(at, 1));
    const float fog = 1 - saturate(tex2Dlod(smpDepth, float4(uv, 0, 0)).a);
    if (fDaoHazeShow > 0)
    {
        return float4(scene.rgb + fog * (1 - float3(fDaoFogR, fDaoFogG, fDaoFogB)), scene.a);
    }
    const float3 light =
        DaoHazeLight(dot(ray, float3(fDaoSunX, fDaoSunY, fDaoSunZ)), float3(fDaoAirR, fDaoAirG, fDaoAirB),
                     float3(fDaoSunR, fDaoSunG, fDaoSunB), fDaoHazeG);
    return float4(max(scene.rgb + fog * (light - float3(fDaoFogR, fDaoFogG, fDaoFogB)), 0), scene.a);
}
