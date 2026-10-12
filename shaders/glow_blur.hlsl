// The glow of glow_bright.hlsl spread along one axis of the screen. Run
// across and then down at a quarter of the picture's size it is the glow
// close about a light; run again from that into a sixteenth of the size, it
// is the wide one, a haze about a fire or a window as far as a hand at
// arm's length.
//
// fAxis is 0 for across and 1 for down, set by the chain's filters. The
// glow is kept as the square root of its light, as glow_bright.hlsl says, and
// is spread as light.

sampler2D smpGlow : register(s0);
float fAxis;

// A bell nine texels wide of this pass's own target, read as five between
// the texels of what it reads.
static const float kDaoOffsets[3] = {0.0, 1.3846153846, 3.2307692308};
static const float kDaoWeights[3] = {0.2270270270, 0.3162162162, 0.0702702703};

float3 DaoGlowAt(float2 uv)
{
    const float3 kept = tex2Dlod(smpGlow, float4(uv, 0, 0)).rgb;
    return kept * kept;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float2 along = fAxis > 0.5 ? float2(0, texel.y) : float2(texel.x, 0);
    float3 sum = DaoGlowAt(uv) * kDaoWeights[0];
    for (int i = 1; i < 3; i++)
    {
        sum += (DaoGlowAt(uv + along * kDaoOffsets[i]) + DaoGlowAt(uv - along * kDaoOffsets[i])) * kDaoWeights[i];
    }
    return float4(sqrt(saturate(sum)), 1);
}
