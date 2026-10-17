// A shadow map's cached level drawn back into it (LevelShadows.cpp; the
// pixel shader is shadow_copy.hlsl): a quad over the whole map, its corners
// given as places on the screen, and their places in the cached picture.

struct Corner
{
    float4 place : POSITION;
    float2 uv : TEXCOORD0;
};

Corner main(float4 place : POSITION, float2 uv : TEXCOORD0)
{
    Corner corner;
    corner.place = place;
    corner.uv = uv;
    return corner;
}
