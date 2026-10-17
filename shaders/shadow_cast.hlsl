// The level's surfaces drawn into the engine's sun shadow maps (Graphics.cpp's
// level shadows; build_shaders.py's with_shadow_place makes the vertex
// shaders): what the engine's own pass (pssm.psh) writes, the depth in the
// map's red, from the place the vertex shader hands down.
//
// The engine fits each map's depth to what it draws into it, its figures
// (2026-10-16: of the level drawn into a map, 1375 of 262144 samples were
// written, the rest beyond the depth's ends). So the vertex shader presses
// what lies beyond onto the ends, and here the depth tested and written is
// the place's own, held to the ends: a roof above every figure is nearest of
// all, ground below them farthest, as the map is cleared.

// The draw's colours, on the level's own sampler of them (s0: smpDecal, or
// a tree's smpDiffuseMap, or a terrain's palette of its layers).
sampler2D smpColours : register(s0);

struct Cast
{
    float4 colour : COLOR0;
    float4 surface : COLOR1; // the draw's colour, where the cache has a target for it (LevelShadows.cpp)
    float depth : DEPTH;
};

Cast main(float4 place : TEXCOORD8)
{
    const float depth = saturate(place.z / place.w);
    Cast cast;
    cast.colour = float4(depth, 0, 0, 0);
    // Its texture's smallest picture: the mean of its colours.
    cast.surface = float4(tex2Dlod(smpColours, float4(0.5, 0.5, 0, 16)).rgb, 1);
    cast.depth = depth;
    return cast;
}
