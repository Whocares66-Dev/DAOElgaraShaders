// The look's numbers that the level's and the figures' shaders read. Those
// shaders are the game's own, rebuilt (build_shaders.py), and the engine hands
// them what it has for them and nothing else; so the plugin puts these in
// the last four float registers, which no shader of the game's reaches (the
// highest any of them is handed is c60, counted 2026-10-11), on the game's Direct3D
// device (Graphics.cpp's KeepLookRegisters). With no plugin, or a look that
// leaves one out, a number is 0, and at 0 each of these is the game's own.

float4 daoLook[4] : register(c220);

// How strongly a level's baked light is shaped by its normal maps (bake.hlsl).
#define DAO_RELIEF daoLook[0].x
// How much the baked light's own contrast is raised, and the brightness of
// it that stays as it is (bake.hlsl's DaoShapedBake).
#define DAO_LIGHT_CONTRAST daoLook[1].w
#define DAO_LIGHT_PIVOT daoLook[2].x
// How far the light of the shade is the sky's colour, under a sun
// (bake.hlsl's DaoShapedBake): the look's CoolShade, nothing where the area
// has no sun or has a moon (Graphics.cpp's KeepSun).
#define DAO_COOL_SHADE daoLook[2].y
// Whether a surface's colour is left as bright as it is lit, 1, or cut at
// white as the game's shaders cut it, 0 (build_shaders.py's without_cut).
// Every lit surface but the ground ends its lighting with a cut at white,
// each channel on its own, which is all a picture of eight bits could hold:
// a wall in strong warm light went pale yellow there and then, before the
// picture's own roll-off could keep its colour (grade.hlsl). With the scene
// in a 16-bit target and the look's Highlights on, the roll-off has it.
#define DAO_UNCUT daoLook[2].z
float3 DaoCut(float3 colour)
{
    return lerp(saturate(colour), max(colour, 0), saturate(DAO_UNCUT));
}
// What a level's own lamps are multiplied by on its walls, floors and
// furnishings, and what is left of its ambient light where there is no sun
// (build_shaders.py's with_level_lights). A level has its torches, fires and
// braziers as real lights, dozens of them, each lighting what is near it by
// the way it faces; and one flat colour added to everything, which is what
// makes a hall lit by lava as even as an office. `sun` is the colour of the
// area's sun.
#define DAO_LEVEL_LAMPS daoLook[2].w
#define DAO_AMBIENT_LESS daoLook[3].x
// Whether the level's lightmaps are our own (Relighter.h), 1, or the game's,
// 0: the plugin's word, by the area (Graphics.cpp). Ours hold light up to
// kDaoRelitMost whites, kept as the square root of its share of that, so
// that a pool of candlelight can be brighter than a white wall and the dark
// beside it still has steps fine enough (bake.hlsl's DaoShapedBake).
#define DAO_RELIT daoLook[3].z
static const float kDaoRelitMost = 8.0;
// How much of the light baked into a level is taken away, to see the level
// by its real lights alone (bake.hlsl's DaoShapedBake): the look's BakedLess.
#define DAO_BAKED_LESS daoLook[3].y
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
float DaoLevelLamp()
{
    return 1 + DAO_LEVEL_LAMPS;
}
float DaoLevelAmbient(float3 sun)
{
    const float indoors = 1 - saturate(20 * max(sun.r, max(sun.g, sun.b)));
    return 1 - saturate(DAO_AMBIENT_LESS) * indoors;
}
// The sky's light beside the sun's, as a colour that keeps a grey's
// brightness: what is not lit by the sun out of doors is lit by the sky.
static const float3 kDaoSkyLight = float3(0.70, 0.95, 1.40);
// How far the figures' highlights are corrected, 0 to 1 (materials.hlsl).
#define DAO_MATERIALS daoLook[0].y
// How much of the sun's shadow falls on figures, 0 to 1 (build_shaders.py's
// with_sun_shadow).
#define DAO_SHADOW_ON_FIGURES daoLook[0].z
// What the colour of one of a figure's point lights is multiplied by
// (build_shaders.py's with_point_lights): `reach` is the light's, `sun` the
// colour of the area's sun on figures, `strength` the fourth component of the
// light's colour. The strength is 1 on every light of the game's, where no
// shader reads it as a number, and Graphics.cpp writes it on two kinds as a
// figure's lights are chosen. On a lamp the plugin offers a figure it is the
// look's Lamps: that is what tells such a lamp from a light an area has for
// its figures already, some of a lamp's reach. On a fill, a light the engine
// stands far off, hundreds of metres of reach, to light every figure of an
// area alike, it is what the look's FillsLess leaves, and it counts indoors
// alone, where the lamps do a fill's work: under a sun the fills stand for
// the sky's light, which nothing else gives a figure. Which far light is a
// fill is the plugin's to say, not the reach's: a portrait's own three
// lights reach as far.
float DaoPointLight(float reach, float3 sun, float strength)
{
    const float indoors = 1 - saturate(20 * max(sun.r, max(sun.g, sun.b)));
    return reach > 100 ? lerp(1, strength, indoors) : strength;
}
// How much brighter than its picture what burns is: a fire, a spell, lava
// (build_shaders.py's with_hot_light). The game draws a flame as a picture
// of one, no brighter at its heart than a white wall in a lamp's light, and
// a picture of eight bits had no room for more. With the scene in a 16-bit
// target there is room: the brighter a pixel of such a thing is drawn, the
// more it is taken for light itself and the brighter it is made, up to
// 1 + DAO_FIRE times; smoke and dust, drawn by the same shaders, are dark
// and stay as they are. So does what is bright and pale: the haze over
// Orzammar's lava is drawn by these shaders too, a sheet of pale pink across
// a whole hall's opening, and made brighter by its brightness alone it was
// a white wall (seen 2026-10-11). A flame, an ember, a spell and lava have a
// colour, and haze and steam have next to none. What comes of it is the
// grade's to bring down and the glow's to spread (grade.hlsl,
// glow_bright.hlsl).
#define DAO_FIRE daoLook[1].z
float3 DaoHot(float3 colour)
{
    const float peak = max(colour.r, max(colour.g, colour.b));
    const float coloured = (peak - min(colour.r, min(colour.g, colour.b))) / max(peak, 1e-4);
    return colour * (1 + DAO_FIRE * smoothstep(0.35, 0.9, peak) * smoothstep(0.25, 0.55, coloured));
}
// The look's Ambient, how much more of its ambient light a figure takes
// where there is no sun, is no shader's: Graphics.cpp lifts the light of a
// level's own probe as the engine hands it over, and not that of a probe of
// its own table.
