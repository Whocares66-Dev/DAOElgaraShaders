// A shadow map's cached level drawn back into it (LevelShadows.cpp): each
// texel's depth, read from the cache at its own texel, written as the map's
// depth in its red and as the depth the figures drawn after it are tested
// against.

sampler2D smpCached : register(s0); // point sampled

struct Copied
{
    float4 colour : COLOR0;
    float depth : DEPTH;
};

Copied main(float2 uv : TEXCOORD0)
{
    // A map's texel, its depth in red, or a colour (LevelShadows.cpp's
    // Reflect, with no depth to write).
    const float4 texel = tex2D(smpCached, uv);
    Copied copied;
    copied.colour = texel;
    copied.depth = texel.r;
    return copied;
}
