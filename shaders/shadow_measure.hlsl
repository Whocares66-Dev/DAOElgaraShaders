// A kept level draw's bounds in the world, measured once on the GPU
// (LevelShadows.cpp): its vertices drawn as points by its measuring vertex
// shader (build_shaders.py's with_measured_place), each to the one pixel that
// is the draw's, with the place in the world it hands down; blended by
// taking the greatest, the first target keeps the draw's greatest x, y and z
// and the second its least, as the greatest of their negatives. Both held
// above 0 by kDaoMeasureOffset, the targets being cleared to 0.

static const float kDaoMeasureOffset = 65536;

struct Measured
{
    float4 most : COLOR0;
    float4 least : COLOR1;
};

Measured main(float4 place : TEXCOORD8)
{
    const float3 world = place.xyz / place.w;
    Measured measured;
    measured.most = float4(world + kDaoMeasureOffset, 1);
    measured.least = float4(kDaoMeasureOffset - world, 1);
    return measured;
}
