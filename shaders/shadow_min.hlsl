// A level of the sun's atlas's min-max depth chain made from the level above
// it (LevelShadows.cpp's MakeNearest): each texel's red the least depth, the
// nearest the sun, of the four it covers, and its green the most. The
// atlas's own first level has its depth in red alone (z = 1: the level above
// is that one). The receivers read it to bound the soft
// shadows' search for what shades them by the nearest caster actually within
// the search (shadow.hlsl's DaoNearestIn), not by the map's near end, which a
// low sun put up to 2 km away (2026-10-28, #61).

sampler2D smpAbove : register(s0); // point sampled, its mips point sampled
// x: the level above's number; y: a texel of this level, as uv; z: 1 where
// the level above is the atlas's first.
float4 daoMinLevel : register(c0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    const float lod = daoMinLevel.x;
    const float h = daoMinLevel.y * 0.25; // the centres of the four above, a quarter texel of this level off its own
    const float2 a = tex2Dlod(smpAbove, float4(uv.x - h, uv.y - h, 0, lod)).xy;
    const float2 b = tex2Dlod(smpAbove, float4(uv.x + h, uv.y - h, 0, lod)).xy;
    const float2 c = tex2Dlod(smpAbove, float4(uv.x - h, uv.y + h, 0, lod)).xy;
    const float2 d = tex2Dlod(smpAbove, float4(uv.x + h, uv.y + h, 0, lod)).xy;
    const float4 most = daoMinLevel.z > 0 ? float4(a.x, b.x, c.x, d.x) : float4(a.y, b.y, c.y, d.y);
    return float4(min(min(a.x, b.x), min(c.x, d.x)), max(max(most.x, most.y), max(most.z, most.w)), 0, 0);
}
