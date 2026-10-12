// How open the way to the sun is from each pixel of the picture, a quarter
// of its size: what makes shafts of light. Air lit by the sun is bright only
// where the sun reaches it, and what stands before the sun, a tree, a tower,
// a roof's edge, leaves the air behind it dark: seen from the ground that is
// a fan of light and shade spreading from the sun. Nothing here knows the
// air's own shadows; what is known is the picture, so each pixel looks along
// the picture towards the sun's place in it and counts how much of that way
// is open sky or far haze (Mitchell's light shafts of 2007, of the depth and
// not of the colours).
//
// The sun's place is the plugin's to say (grade.hlsl's DaoAir has what the
// values are). smpDepth is occlusion_depth.hlsl's target, metres as one
// float, at sampler 1, which the pass's state reads unfiltered.

sampler2D smpDepth : register(s1);
float fDaoSunX;
float fDaoSunY;
float fDaoSunSide;
float fDaoLensX;
float fDaoLensY;

static const int kDaoShaftSteps = 28;
// How far towards the sun a pixel looks, of the picture's height.
static const float kDaoShaftReach = 0.9;
// What each step further counts for, of the one before.
static const float kDaoShaftFade = 0.94;
// The distance from which what is seen counts as open, and by which wholly.
static const float kDaoOpenFrom = 150.0;
static const float kDaoOpenBy = 600.0;

float4 main(float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0
{
    if (fDaoSunSide <= 0)
    {
        return 0; // the sun is behind: no shafts come this way
    }
    const float2 sun = 0.5 + float2(fDaoSunX / max(fDaoLensX, 1e-4), fDaoSunY / max(fDaoLensY, 1e-4));
    const float2 toSun = sun - uv;
    // The way's length in heights of the picture, for a reach the same along and across.
    const float far = length(toSun * float2(fDaoLensX / max(fDaoLensY, 1e-4), 1));
    const float2 step = toSun * (min(1.0, kDaoShaftReach / max(far, 1e-4)) / kDaoShaftSteps);
    // Interleaved gradient noise (Jimenez 2014): each pixel starts a part of a
    // step along, so the steps show as grain and not as rings.
    const float start = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float open = 0;
    float weights = 0;
    float weight = 1;
    for (int i = 0; i < kDaoShaftSteps; i++)
    {
        const float2 at = uv + step * (i + start);
        const float metres = tex2Dlod(smpDepth, float4(at, 0, 0)).r;
        open += weight * saturate((metres - kDaoOpenFrom) / (kDaoOpenBy - kDaoOpenFrom));
        weights += weight;
        weight *= kDaoShaftFade;
    }
    return float4((open / weights).xxx, 1);
}
