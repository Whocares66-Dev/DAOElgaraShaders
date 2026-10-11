// Each pixel's distance in front of the camera, in metres, as one float: the
// first of the ambient occlusion's passes (build_shaders.py), which the other
// three read in place of the game's own depth.
//
// The game writes each pixel's distance into a second render target beside
// the picture (every lit shader's oC1: the distance over the focal plane,
// packed into the three bytes of a colour), for its depth of field. That
// target is anti-aliased like the picture, so wherever a pixel holds more than
// one surface its bytes are averaged, each on its own, and rounded:
//
// - at an outline the result is a distance between the figure's and the
//   wall's behind it, where nothing is;
// - where the samples straddle a step of the high byte the rounding alone
//   moves it by up to two metres.
//
// Read as they are, such pixels are specks hanging in the air, and the
// occlusion shaded what stands near them: a dark fringe about every figure
// against a building, which moved as the figure did (the user saw it shimmer,
// 2026-10-11; measured there as a third more change from frame to frame).
// So a pixel that stands apart from both its neighbours across or both down
// is given the nearer of the two's distance: it joins one surface or the
// other, as the larger part of it does.
//
// smpDepth is the game's target at sampler 1, which the pass's state reads
// unfiltered: a filtered reading would mix packed bytes again.

sampler2D smpDepth : register(s1);
float fFocalPlane;

// Written where nothing wrote a distance (the sky): further than anything.
static const float kDaoNoDepth = 10000.0;

float DaoPackedDepth(float2 uv)
{
    const float3 bytes = tex2Dlod(smpDepth, float4(uv, 0, 0)).xyz;
    const float metres = (bytes.z + bytes.y / 256 + bytes.x / 65536) * fFocalPlane;
    return metres < 0.05 ? kDaoNoDepth : metres;
}

// `centre` if it is within `apart` of `a` or of `b`, or not between them;
// else whichever of the two it is nearer.
float DaoJoin(float centre, float a, float b, float apart)
{
    const float low = min(a, b);
    const float high = max(a, b);
    const bool between = centre - low > apart && high - centre > apart;
    return between ? (centre - low < high - centre ? low : high) : centre;
}

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    float centre = DaoPackedDepth(uv);
    const float left = DaoPackedDepth(uv - float2(texel.x, 0));
    const float right = DaoPackedDepth(uv + float2(texel.x, 0));
    const float up = DaoPackedDepth(uv - float2(0, texel.y));
    const float down = DaoPackedDepth(uv + float2(0, texel.y));
    // What a surface seen almost edge on changes by from one pixel to the
    // next is under this; more is another surface.
    const float apart = 0.01 + 0.01 * min(centre, 100.0);
    // The high byte's rounding puts a pixel outside all four of its
    // neighbours: back among them first.
    centre = clamp(centre, min(min(left, right), min(up, down)) - apart, max(max(left, right), max(up, down)) + apart);
    centre = DaoJoin(centre, left, right, apart);
    centre = DaoJoin(centre, up, down, apart);
    return float4(centre, 0, 0, 1);
}
