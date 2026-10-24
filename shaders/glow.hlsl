// What a level's ground gives off of its own light, by the layers of its
// palette: lava. A terrain material blends up to three of its palette's eight
// layers at a pixel, each weighed by its masks (MaskA the first four layers',
// MaskA2 the last four's); mml_vDaoGlow is how strongly each layer glows, its
// own colour times that, which the plugin gives the materials data/glowing.ini
// names (MaterialPatch.cpp) and is 0 for every other. The glow is the layer's
// own: no light lights it and no hour dims it, as the lava on Haven's
// mountain top had gone dark with the night (the user, 2026-10-23: "let's
// also make the lava glow").

#ifndef DAO_GLOW_HLSL
#define DAO_GLOW_HLSL

float4 mml_vDaoGlow[2];

// How much of a pixel glows: its layers' weights, each times its layer's
// glow, over all their weights, as the shader weighs its layers' colours.
float DaoGlowShare(float4 maskA, float4 maskA2)
{
    const float all = dot(maskA, 1) + dot(maskA2, 1);
    return all > 0 ? (dot(maskA, mml_vDaoGlow[0]) + dot(maskA2, mml_vDaoGlow[1])) / all : 0;
}

#endif
