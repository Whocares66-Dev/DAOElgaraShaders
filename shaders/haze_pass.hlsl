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
// What to show to check the haze by: 1, the fog as white air and no sun; 2,
// the share of the fog the sun reaches in red and the fog in green.
float fDaoHazeShow;
// The fog's intensity, and the camera's place and axes in the world (x right,
// y down the picture, z ahead), for the sun's shadows in the air.
float fDaoFogIntensity;
float fDaoFogCap;
float fDaoFogZenith;
float fDaoEyeX;
float fDaoEyeY;
float fDaoEyeZ;
float fDaoRightX;
float fDaoRightY;
float fDaoRightZ;
float fDaoDownX;
float fDaoDownY;
float fDaoDownZ;
float fDaoAheadX;
float fDaoAheadY;
float fDaoAheadZ;

// The sun's shadow maps fitted to the view, as the surfaces that receive them
// read them (shadow.hlsl's daoShadowData; LevelShadows.cpp lends this pass
// the atlas and its registers): map i's texture matrix's first three rows at
// [3i..3i+2], in the tile at column i % 2 and row i / 2; [12].x 1 where the
// maps are there this frame, [12].y how far from the focus a map is read, as
// a share of its width; [13].xyz the focus.
sampler2D smpDaoSunMaps : register(s3);
float4 daoShadowData[14] : register(c200);

// How much nearer the sun than a point in the air a caster must be to shade
// it, in metres: the maps' own rounding, and no more, since nothing in the
// air is shaded by itself.
static const float kDaoAirBias = 0.05;

// Whether the sun reaches a point of the world: from the finest map that
// holds it, one sample; past them all, it does. A caster beyond a map's depth
// was pressed onto its ends as it was drawn (shadow_cast.hlsl), so a depth
// past them is held to them too.
float DaoSunAt(float3 world)
{
    const float4 point4 = float4(world, 1);
    const float4 focus = float4(daoShadowData[13].xyz, 1);
    float lit = 1;
    bool found = false;
    [loop] for (int map = 0; map < 4; map++)
    {
        const float4 m0 = daoShadowData[3 * map];
        const float4 m1 = daoShadowData[3 * map + 1];
        const float4 m2 = daoShadowData[3 * map + 2];
        const float2 at = float2(dot(m0, point4), dot(m1, point4));
        const float2 off = abs(at - float2(dot(m0, focus), dot(m1, focus)));
        if (!found && max(off.x, off.y) < daoShadowData[12].y)
        {
            found = true;
            const float depth = saturate(dot(m2, point4));
            const float2 uv = float2(map % 2, map / 2) * 0.5 + 0.5 * at;
            lit = tex2Dlod(smpDaoSunMaps, float4(uv, 0, 0)).x >= depth - kDaoAirBias * length(m2.xyz) ? 1 : 0;
        }
    }
    return lit;
}

// How many places along a view the sun's reach is asked at.
static const int kDaoAirSteps = 16;

// The share of a view's fog the sun reaches. The engine keeps
// exp(-(intensity t)^2) of the light from t metres off (terrain1.vsh, and
// every surface's: its mml_vFogParams[0].w is the intensity squared, read
// 2026-10-17), so the air between t and t + dt adds that much less and the
// fog `fog` of a view is made of the air out to where that has fallen to 1 -
// fog, every part of it adding as much as its fall. Its places are taken
// at equal shares of that fall, each pixel starting a part of a share along
// (`start`), so each counts the same.
float DaoSunInAir(float3 way, float metres, float fog, float start)
{
    const float3 eye = float3(fDaoEyeX, fDaoEyeY, fDaoEyeZ);
    const float3 worldWay = way.x * float3(fDaoRightX, fDaoRightY, fDaoRightZ) +
                            way.y * float3(fDaoDownX, fDaoDownY, fDaoDownZ) +
                            way.z * float3(fDaoAheadX, fDaoAheadY, fDaoAheadZ);
    float lit = 0;
    [loop] for (int i = 0; i < kDaoAirSteps; i++)
    {
        const float share = (i + start) / kDaoAirSteps * fog;
        const float t = min(sqrt(-log(max(1 - share, 1e-6))) / fDaoFogIntensity, metres);
        lit += DaoSunAt(eye + t * worldWay);
    }
    return lit / kDaoAirSteps;
}

// The sky's distance as the game writes it (sky.psh's oC1, a constant) is
// 0.99998 of the focal plane, and the focal plane is over 4 km (seen in
// Redcliffe, 2026-10-16), at which no fog leaves anything to be seen. So what
// is that far is sky, which fogs itself, and is left alone here. What else
// wrote the sky's distance writes 0.9 of the focal plane now
// (build_shaders.py's with_far_distance).
static const float kDaoSkyFrom = 0.999;

bool DaoIsSky(float depth)
{
    return depth >= kDaoSkyFrom * fFocalPlane;
}

// Whether the sun's shadows in the air are there to be read this frame.
bool DaoAirShaded()
{
    return daoShadowData[12].x > 0 && fDaoFogIntensity > 0;
}

// The haze's light seen along `ray` (the camera's axes) that the sun's
// shadows in the air take away: its sun's part, for the share of it that
// lies in shadow.
float3 DaoShadedHaze(float3 ray, float shaded)
{
    return shaded * DaoHazeLight(dot(ray, float3(fDaoSunX, fDaoSunY, fDaoSunZ)), 0,
                                 float3(fDaoSunR, fDaoSunG, fDaoSunB), fDaoHazeG);
}

// How much the sky fogs itself towards the air's light, by its own rule
// (sky.psh, read 2026-10-17): the fog's cap, less as the way rises, by
// exp(-z^3 / (0.0636620 zenith)), z the way's height over its length and the
// zenith in radians. Its fog is the air out to the cap's distance, and is
// shaded as a surface's is.
float DaoSkyFogOf(float3 ray)
{
    const float rise = ray.x * fDaoRightZ + ray.y * fDaoDownZ + ray.z * fDaoAheadZ;
    // As the sky's own, by exp2: fxc's optimizer does not settle on exp here.
    return fDaoFogCap * saturate(exp2(rise * rise * rise * (-1.4426950 / (0.0636620 * radians(fDaoFogZenith)))));
}

float4 main(float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0
{
    const float4 scene = tex2D(smpScene, uv);
    if (fDaoHaze <= 0)
    {
        return scene;
    }
    const float2 at = (uv - 0.5) * float2(fDaoLensX, fDaoLensY);
    const float3 ray = normalize(float3(at, 1));
    const float depth = tex2Dlod(smpMetres, float4(uv, 0, 0)).r;
    const bool sky = DaoIsSky(depth);
    // A surface's fog is the engine's for its pixel; the sky's, its own rule's,
    // which it has drawn with already, so that only its shade is to be taken.
    const float kept = saturate(tex2Dlod(smpDepth, float4(uv, 0, 0)).a);
    const float fog = sky ? DaoSkyFogOf(ray) : 1 - kept;
    if (fDaoHazeShow > 0 && fDaoHazeShow < 2)
    {
        return sky ? scene : float4(scene.rgb + fog * (1 - float3(fDaoFogR, fDaoFogG, fDaoFogB)), scene.a);
    }
    // The haze's part is the sun's light scattered on to the eye, so it is
    // there only as much as the sun reaches the air it is scattered in; the
    // air's part is the even light of the sky. The metres are along the
    // camera's axis.
    float sunLit = 1;
    if (DaoAirShaded() && fog > 0)
    {
        // Interleaved gradient noise (Jimenez 2014), as shadow.hlsl's: where
        // along its first share of the fog a pixel starts.
        const float start = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
        const float metres = sky ? 1e9 : depth * length(float3(at, 1));
        sunLit = DaoSunInAir(ray, metres, sky ? fDaoFogCap : fog, start);
    }
    if (fDaoHazeShow > 1)
    {
        return float4(sunLit, fog, 0, scene.a);
    }
    if (sky)
    {
        return float4(max(scene.rgb - fog * DaoShadedHaze(ray, 1 - sunLit), 0), scene.a);
    }
    const float3 light = DaoHazeLight(dot(ray, float3(fDaoSunX, fDaoSunY, fDaoSunZ)), float3(fDaoAirR, fDaoAirG, fDaoAirB),
                                      0, fDaoHazeG) +
                         DaoShadedHaze(ray, sunLit);
    return float4(max(scene.rgb + fog * (light - float3(fDaoFogR, fDaoFogG, fDaoFogB)), 0), scene.a);
}
