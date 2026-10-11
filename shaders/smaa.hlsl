// What the three passes of the anti-aliasing share: SMAA 1x (Jimenez et al.
// 2012, smaa/SMAA.hlsl, as its authors publish it) set up for the game's
// Direct3D 9, and a pixel's place in the picture.
//
// The game's own anti-aliasing is multisampling, which smooths the edges of
// polygons and nothing else: not hair, leaves and fences cut out of a texture,
// not a shader's own sparkle. SMAA works on the finished picture instead, so
// it sees every edge alike: smaa_edges.hlsl finds them, smaa_weights.hlsl
// measures the shape each is a part of, and smaa_blend.hlsl mixes each pixel
// with its neighbour across the edge by that much. The passes are put before
// the game's colour grade (smaa.py), which is before the interface is drawn.

// The picture's width and height in pixels, given by the chain to each pass.
float fWidth;
float fHeight;

#define SMAA_RT_METRICS float4(1.0 / fWidth, 1.0 / fHeight, fWidth, fHeight)
#define SMAA_HLSL_3
#define SMAA_PRESET_ULTRA
// smaa.py writes the area texture with its two channels in red and green.
#define SMAA_AREATEX_SELECT(sample) sample.rg
#include "smaa/SMAA.hlsl"

// Where a pixel is in a texture the size of the picture, at its centre. Taken
// from the pixel's own place, not from the quad's texture coordinate: SMAA
// counts on reading exactly at and exactly between texels, and Direct3D 9
// puts a quad's coordinates half a pixel off unless the quad was moved for it.
float2 DaoSmaaUv(float2 pixel)
{
    return (pixel + 0.5) * SMAA_RT_METRICS.xy;
}
