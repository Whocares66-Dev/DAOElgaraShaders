// The anti-aliasing's second pass (smaa.hlsl): how much of each neighbour a
// pixel on an edge is to take. Each edge is followed both ways to its ends,
// and the shape found (a step, a diagonal, a corner) is looked up in the area
// texture for how much of the pixel the true line would have covered.
//
// The edges and the area texture are read between texels, which is how one
// reading tells two texels apart, so their samplers are filtered; the search
// texture is read at its texels.

#include "smaa.hlsl"

sampler2D smpEdges : register(s0);
sampler2D smpArea : register(s1);
sampler2D smpSearch : register(s2);

float4 main(float2 pixel : VPOS) : COLOR0
{
    const float2 uv = DaoSmaaUv(pixel);
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    return SMAABlendingWeightCalculationPS(uv, pixcoord, offset, smpEdges, smpArea, smpSearch, 0);
}
