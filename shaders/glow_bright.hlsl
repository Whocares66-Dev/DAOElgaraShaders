// What of the picture is bright enough to glow, a quarter of its size a side:
// the first of the glow's passes (build_shaders.py), which the next blur and
// the colour grade adds back to the picture before it brings it down to what
// eight bits hold.
//
// The game's own bloom takes whatever is past the middle of an eight-bit
// picture, a pale wall as soon as a fire. With the scene in a 16-bit target
// (Graphics.cpp) a fire, a spell, the sky behind a roof and snow in the sun
// are brighter than white by what they are, and those are what glow in a
// lens or an eye: so the glow is of what is over kDaoGlowFrom of the
// picture's range (grade.hlsl's fDaoRange, the brightness that comes out
// white), after the exposure, each colour by how far its strongest channel
// is over.
//
// The target is eight bits, so what is written is the square root of the
// light over kDaoGlowMost: fine steps where the glow is faint, which is
// where steps would show.

sampler2D smpPicture : register(s0);
float fDaoExposure;
float fDaoRange;

static const float kDaoGlowFrom = 0.55; // of the range
static const float kDaoGlowMost = 16.0; // whites: the most a glow's light is kept as

float3 DaoBright(float2 uv)
{
    const float3 colour = max(tex2Dlod(smpPicture, float4(uv, 0, 0)).rgb, 0) * exp2(fDaoExposure);
    const float peak = max(colour.r, max(colour.g, colour.b));
    const float from = kDaoGlowFrom * max(fDaoRange, 1);
    return colour * (max(peak - from, 0) / max(peak, 1e-4));
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    // Four readings between the picture's texels, each the mean of four of
    // them: the sixteen this pixel stands for. Each counts for less the
    // brighter it is, so one glinting pixel does not flicker as a whole blob
    // of glow (Karis's average).
    const float2 texel = 0.25 * float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    float3 sum = 0;
    float weights = 0;
    for (int i = 0; i < 4; i++)
    {
        const float2 at = uv + texel * float2(i % 2 ? 1 : -1, i / 2 ? 1 : -1);
        const float3 bright = DaoBright(at);
        const float weight = 1 / (1 + max(bright.r, max(bright.g, bright.b)));
        sum += bright * weight;
        weights += weight;
    }
    return float4(sqrt(saturate(sum / weights / kDaoGlowMost)), 1);
}
