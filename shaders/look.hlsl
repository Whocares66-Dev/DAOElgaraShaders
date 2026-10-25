// The options' numbers that the level's and the figures' shaders read. Those
// shaders are the game's own, rebuilt (build_shaders.py), and the engine hands
// them what it has for them and nothing else; so the plugin puts these in
// the last four float registers, which no shader of the game's reaches (the
// highest any of them is handed is c60, counted 2026-10-11), on the game's Direct3D
// device (Graphics.cpp's KeepLookRegisters). With no plugin, or an option
// off, a number is 0, and at 0 each of these is the game's own.

#ifndef DAO_LOOK_HLSL
#define DAO_LOOK_HLSL

float4 daoLook[4] : register(c220);

// How strongly a level's baked light is shaped by its normal maps (bake.hlsl):
// Improved materials.
#define DAO_RELIEF daoLook[0].x
// How far the figures' highlights are corrected, 0 to 1 (materials.hlsl):
// Improved materials.
#define DAO_MATERIALS daoLook[0].y
// How much of the sun's shadow falls on figures, 0 to 1 (build_shaders.py's
// with_sun_shadow): Improved lighting.
#define DAO_SHADOW_ON_FIGURES daoLook[0].z
// Whether the sun's shadows are filtered by shadow.hlsl, 1, or by the
// game's own few taps, 0 (build_shaders.py's with_our_shadow): Improved
// shadows.
#define DAO_SHADOWS daoLook[0].w
// The shadow maps' size, texels a side, as the engine was asked for them
// (Graphics.cpp's SetPartitions): the plugin's own size with Improved
// shadows on as the game started, else the game's.
#define DAO_SHADOW_MAP_SIZE (daoLook[1].x > 0 ? daoLook[1].x : 1024.0)
// Whether a surface's colour is left as bright as it is lit, 1, or cut at
// white as the game's shaders cut it, 0 (build_shaders.py's without_cut).
// Every lit surface but the ground ends its lighting with a cut at white,
// each channel on its own, which is all a picture of eight bits could hold:
// a wall in strong warm light went pale yellow there and then, before the
// picture's own roll-off could keep its colour (grade.hlsl). With the scene
// in a 16-bit target and Improved materials on, the roll-off has it.
#define DAO_UNCUT daoLook[1].y
// A developer's look at the sun's shadow (build_shaders.py's
// with_shadow_show): 1 has a surface that receives it show the lightmap's
// word of where the sun reaches as red and the shadow maps' as green, yellow
// where they agree that it does and black where they agree it does not.
#define DAO_SHADOW_SHOW daoLook[1].z
// What a level's baked light is multiplied by, each colour (build_shaders.py's
// with_baked_light): the sky's light of the hour where a level has a day and
// a night (DayNight.h), else 1.
#define DAO_BAKED_LIGHT (1 + daoLook[2].xyz)
float3 DaoCut(float3 colour)
{
    return lerp(saturate(colour), max(colour, 0), saturate(DAO_UNCUT));
}
// The air's light every fogged surface, the sky, the clouds and the effects
// fog toward (haze.hlsl; Outdoors.cpp), two registers of their own below
// daoLook, set as they are: the part the sky's light makes and whether it is
// ours (1, or the area's fog color, 0); the part the sun or the moon makes
// and how its glow leans toward the light.
float4 daoHaze[2] : register(c218);
#define DAO_HAZE daoHaze[0].w
#define DAO_HAZE_SKY daoHaze[0].xyz
#define DAO_HAZE_SUN daoHaze[1].xyz
#define DAO_HAZE_G daoHaze[1].w
// Whether what is drawn now goes into the frame's own picture, 1, or into
// some other target, 0: the plugin's word, as the device's target changes
// (Graphics.cpp's SetRenderTarget). A share is written to the alpha of the
// first and never of the second: a portrait and the record's or the
// inventory's figure are drawn into small targets of their own, whose alpha
// is their opacity when the interface lays them on the screen, and with a
// share there a face was gone from every portrait and the figure from the
// record (the user, 2026-10-12: "portraits are broken", "so are char
// records"; a share is 0 where no light is ambient, which is all of such a
// scene).
#define DAO_SHARE_ASKED daoLook[3].w
// How much of a surface's light is ambient, 0 to 1: what the ambient
// occlusion may shade (issue 8). `ambient` is what the surface sends of its
// ambient light, scattered (a figure's probe light, a level's baked light,
// through its own colour) and mirrored (its surroundings' reflection), and
// `lit` all it sends to the eye. Written to the picture's own alpha, which
// in vanilla's solid shaders is their opacity, 1, and blended by nothing
// (build_shaders.py's with_ambient_share has where it was first and why not).
float DaoShare(float3 ambient, float3 lit)
{
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    return saturate(dot(max(ambient, 0), luma) / max(dot(max(lit, 0), luma), 1e-4));
}

#endif
