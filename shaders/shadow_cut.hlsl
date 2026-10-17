// A cut-out surface of the level, a tree's fronds and leaves, drawn into the
// engine's sun shadow maps (LevelShadows.cpp; build_shaders.py's
// with_shadow_place keeps its texture's place for it): what shadow_cast.hlsl
// writes, where the texture is not cut away as the draw itself cuts it, by
// its alpha against the draw's own alpha test's reference.

sampler2D smpDiffuse : register(s0);
float fDaoAlphaReference : register(c216); // the draw's D3DRS_ALPHAREF, of 1

struct Cast
{
    float4 colour : COLOR0;
    float4 surface : COLOR1; // as shadow_cast.hlsl's
    float depth : DEPTH;
};

Cast main(float2 uv : TEXCOORD0, float4 place : TEXCOORD8)
{
    clip(tex2D(smpDiffuse, uv).a - fDaoAlphaReference);
    const float depth = saturate(place.z / place.w);
    Cast cast;
    cast.colour = float4(depth, 0, 0, 0);
    cast.surface = float4(tex2Dlod(smpDiffuse, float4(0.5, 0.5, 0, 16)).rgb, 1);
    cast.depth = depth;
    return cast;
}
