// The two glows and the sun's shafts made one, the picture's own size: the
// last of the glow's passes. The close glow, the wide one and the shafts are
// small pictures, and the colour grade's own pass reads its second texture
// and those after it at their texels, unfiltered (the game's
// HSVMatrixBlend.sta), which would lay them on in squares; this pass reads
// all three between their texels (a state of our own, build_shaders.py's
// GLOW_STATE) and writes what the grade then reads texel for texel.
//
// The shafts (glow_shafts.hlsl) are how open the way to the sun is; here
// they become light: the sun's colour, by the look's Shafts, and by how
// nearly the pixel looks at the sun, as grade.hlsl's DaoAir reckons it.
//
// Kept as the square root of the light, as glow_bright.hlsl says.

sampler2D smpGlow : register(s0);
sampler2D smpGlowWide : register(s1);
sampler2D smpShafts : register(s2);
float fDaoSunX;
float fDaoSunY;
float fDaoSunSide;
float fDaoLensX;
float fDaoLensY;
float fDaoSunRed;
float fDaoSunGreen;
float fDaoSunBlue;
float fDaoShafts;

// How much of the whole is the wide glow's.
static const float kDaoWide = 0.5;
// How fast the shafts fall off away from the sun.
static const float kDaoShaftForward = 5.0;

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float3 close = tex2Dlod(smpGlow, float4(uv, 0, 0)).rgb;
    const float3 wide = tex2Dlod(smpGlowWide, float4(uv, 0, 0)).rgb;
    const float2 here = (uv - 0.5) * float2(fDaoLensX, fDaoLensY);
    const float2 sun = float2(fDaoSunX, fDaoSunY);
    const float toward = fDaoSunSide * (dot(here, sun) + 1) / sqrt((dot(here, here) + 1) * (dot(sun, sun) + 1));
    const float shafts = tex2Dlod(smpShafts, float4(uv, 0, 0)).r * pow(saturate(toward), kDaoShaftForward) * fDaoShafts;
    const float3 light = lerp(close * close, wide * wide, kDaoWide) +
                         float3(fDaoSunRed, fDaoSunGreen, fDaoSunBlue) * shafts / 16.0;
    return float4(sqrt(saturate(light)), 1);
}
