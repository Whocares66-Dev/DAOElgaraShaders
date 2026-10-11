// The anti-aliasing's last pass (smaa.hlsl): the picture, each pixel on an
// edge mixed with its neighbour across it by the weight the second pass gave.
// The mixing is one filtered reading between the two pixels.

#include "smaa.hlsl"

sampler2D smpPicture : register(s0);
sampler2D smpWeights : register(s1);

float4 main(float2 pixel : VPOS) : COLOR0
{
    const float2 uv = DaoSmaaUv(pixel);
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    return SMAANeighborhoodBlendingPS(uv, offset, smpPicture, smpWeights);
}
