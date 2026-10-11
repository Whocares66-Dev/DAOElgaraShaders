// The ambient occlusion of occlusion.hlsl smoothed along one axis of the
// screen: run across and then down, it takes out the grain the pass before
// has from turning its taps at every pixel. A neighbour counts for less the
// further its depth is from the centre's, so the shade under a figure does
// not bleed over the figure's outline.
//
// fAxis is 0 for across and 1 for down, set by the chain's two filters.
// smpDepth is occlusion_depth.hlsl's target, metres as one float.

sampler2D smpOcclusion : register(s0);
sampler2D smpDepth : register(s1); // unfiltered: see occlusion.hlsl
float fAxis;

static const int kDaoBlurReach = 4; // pixels each way
// How fast a neighbour stops counting, per part of the centre's depth.
static const float kDaoBlurSharpness = 40;

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float2 along = fAxis > 0.5 ? float2(0, texel.y) : float2(texel.x, 0);
    const float depth = tex2Dlod(smpDepth, float4(uv, 0, 0)).r;
    float sum = 0;
    float weights = 0;
    for (int i = -kDaoBlurReach; i <= kDaoBlurReach; i++)
    {
        const float2 at = uv + along * i;
        const float apart = abs(tex2Dlod(smpDepth, float4(at, 0, 0)).r - depth) / depth;
        const float weight = exp(-0.18 * i * i - kDaoBlurSharpness * apart);
        sum += tex2Dlod(smpOcclusion, float4(at, 0, 0)).r * weight;
        weights += weight;
    }
    // The centre always counts for 1, so there is always something to divide by.
    return float4(sum / weights, 1, 0, 1);
}
