// The anti-aliasing's first pass (smaa.hlsl): where the picture has an edge.
// Red is an edge on a pixel's left, green one above it, by the difference in
// colour, not in brightness alone, which misses an edge between two colours
// equally bright. A pixel with no edge is not written, so the chain clears
// the target first.
//
// It is also the anti-aliasing's switch (smaa.py's SWITCH): with fDaoSmaa at
// 0 no pixel is written, the second pass finds no edge to weigh, and the last
// hands the picture on as it came.

#include "smaa.hlsl"

sampler2D smpPicture : register(s0);
float fDaoSmaa;

float4 main(float2 pixel : VPOS) : COLOR0
{
    clip(fDaoSmaa - 0.5);
    const float2 uv = DaoSmaaUv(pixel);
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    return float4(SMAAColorEdgeDetectionPS(uv, offset, smpPicture), 0, 0);
}
