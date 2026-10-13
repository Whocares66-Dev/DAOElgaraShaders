// What a surface does with light: the standard model of a base colour, a
// metalness and a roughness (Burley 2012; Karis 2013), read from the game's
// own textures and from what each material says it is. Elgara Shaders puts
// this into every shader that lights a surface the game's one common way:
// bodies, armour, weapons, banners, props and a level's own surfaces
// (build_shaders.py's with_materials).
//
// What the game has for a surface: its colour (the diffuse texture, tinted),
// a "specular mask" whose colour it multiplies its highlights and its
// reflection by and whose alpha is the highlight's power from 1 to 256, and
// nothing that says whether the surface is metal. What the game does with
// them is pow(N.H, power) times 3 for each light whatever the power, the
// area's cube read by the power, the two summed and multiplied by the
// mask's colour and by a falloff made at each vertex. A stone bench painted
// with a mask of 0.8 mirrors as chrome does by that; it only never showed,
// the cube being dim.
//
// What this does, and where each number is from:
//
// - Metalness is DATA: mml_fDaoMetal, a number the material says of itself,
//   times a map of it, smpDaoMetal, where one texture is part metal and part
//   not (data/materials.ini and tools/material_table.py; a material that
//   says nothing is no metal). It was guessed here once, at each pixel from
//   the mask's gloss and brightness, and every wrong guess was on the screen:
//   a shield's painted face dark, a bench a mirror, gilt armour foil (the
//   user, 2026-10-12: "shouldn't all this be driven by materials? are we
//   just adding in hacks?").
// - A surface that is no metal scatters its colour and mirrors a few parts
//   in a hundred of what falls on it: a third of the mask's colour
//   (kDaoMirrorOfMask). The mask says how shiny on the game's own scale, and
//   the scale is measured: over the 638 masks of the game's stone, wood,
//   cloth, leather and skin the median of a mask's brightness is 0.12 (0.13,
//   0.13, 0.10, 0.11, 0.08 by class; tools/material_table.py's classes,
//   counted 2026-10-12), and an ordinary surface that is no metal mirrors
//   four parts in a hundred, so the one is taken for the other. It was 0.08
//   times the mask first, the common reading of a "specular" input that is a
//   half for an ordinary surface (Burley's and Unreal's), which these masks
//   are not: stone then mirrored one part in a hundred and less, and the
//   tower's hall was a third darker than by the game's formula.
// - A metal scatters nothing and mirrors in its own colour, which is the
//   surface's colour and the mask's together: the game paints a metal's
//   colour into both (gilt's yellow in its diffuse texture, dragonbone's red
//   in its mask's tint), and all a metal sends back, it sends back as a
//   mirror does. Where the two together are more than all that falls on it
//   (a sixth of the tower's iron gates), it is all of it, in their colour.
// - Roughness is the highlight's power: alpha squared is 2 / (power + 2),
//   the width at which a GGX lobe matches a Blinn-Phong lobe of that power
//   (Walter et al. 2007). The lobe is the whole of it, so a narrow one is as
//   bright as it is narrow. The cube is read at the mip that roughness
//   blurs to (Lagarde and de Rousiers 2014).
// - How much is mirrored grows towards the edge (Schlick 1994), at each
//   pixel, in place of the game's falloff at each vertex. A light's
//   highlight grows by the angle between the light and the eye (V.H), the
//   surroundings' reflection by the eye's angle to the surface and by its
//   roughness: the share of a cube a rough surface mirrors, as Karis fitted
//   it (2014; his split sum of 2013, in two lines). The edge mirrors all
//   that falls on it, but less where the mask says under two parts in a
//   hundred face on, which no surface is: that is shade painted into the
//   mask (Unreal's rule, 50 times the green). The edge was first one gain
//   by N.V for lights and cube alike and "what the roughness leaves" for a
//   rough surface: a lamp overhead then lit a floor seen at a low angle 75
//   times too brightly and the cube four times, which was the tower's floor
//   in white streaks (seen 2026-10-12, the floor's mask 0.08 and power 27).
// - The power is made less where a pixel holds more ways for the surface
//   to face than one: where the surface itself turns fast across the pixel
//   (Kaplanyan et al. 2016; Tokuyoshi and Kaplanyan 2019), and where the
//   pixel covers many texels of its normal map, by how far apart the map's
//   normals are over a patch that size. That is measured from each normal
//   map and said by the material (mml_vDaoBumps; tools/material_table.py's
//   bumps). The game's normal maps keep two of a normal's three numbers, so
//   their smaller pictures hold the mean way a patch faces and nothing of
//   how its facets differ; without it a flagstone floor seen low was a
//   smooth one in streaks, each pixel mirroring by whichever facet its
//   middle fell on (the tower's, seen 2026-10-12, and gone with every pixel
//   given the same width).
//
// DAO_MATERIALS (look.hlsl) is the look's switch: at 0, the Vanilla look,
// every function here is the game's own arithmetic. It wants the scene drawn
// into a 16-bit target with its highlights rolled off (Graphics.cpp,
// grade.hlsl), and the plugin holds it at 0 without one.

// What of the surroundings' reflection the surface last worked out sends to
// the eye (DaoMirror): for the ambient share, which is of what a surface
// really sends (look.hlsl's DaoShare; build_shaders.py's with_ambient_share).
static float3 gDaoEnvSent = 0;
float3 DaoEnvSent()
{
    return gDaoEnvSent;
}

// What the material says it is (build_shaders.py's MATERIAL_PARAMETERS puts
// both into the game's material definitions; a material object's own file
// sets them).
float mml_fDaoMetal;
sampler2D smpDaoMetal;
// And of its normal map: its width and height in texels, and then how far
// apart its normals are, in the mean, over a patch 2 and 4 texels across;
// over 8, 16, 32 and 64 in the second. Each is twice the larger of the two
// spreads of the normals' x and y in the patch, which is what the patch adds
// to the square of a lobe's width.
float4 mml_vDaoBumps;
float4 mml_vDaoBumpsFar;

// What a surface that is no metal mirrors face on, of its mask's colour.
static const float kDaoMirrorOfMask = 0.04 / 0.12;

// How much of the surface at `uv` mirrors as a metal does, 0 to 1: what of
// it is metal (the material's word and its map), times what the material
// says its reflection is of a whole one (`said`, mml_fSpecularReflectionMult,
// which the game multiplies the surroundings' reflection by and nothing
// else). The rest of a metal so dulled scatters its colour, as paint or
// tarnish does. Until 2026-10-12 a metal scattered nothing whatever the
// material said, so one the game's artists had dulled to a fifth (a kite
// shield's face, the axes and bows with iron on them: 4 in 100 of the
// part-metal materials say under a half) kept a fifth of a mirror and lost
// the rest of its light: the shield was a dark glass where the game has a
// matte bronze (issue 17; the user: "shield is a little too reflective").
float DaoMetal(float2 uv, float said)
{
    return saturate(mml_fDaoMetal * tex2D(smpDaoMetal, uv).g) * saturate(said) * saturate(DAO_MATERIALS);
}

// What the normal map at `uv` adds to the square of a lobe's width: the
// spread of its normals over as many texels as the pixel covers the long
// way, between the patches the material has the measure of. Nothing where
// a texel is a pixel or more, and nothing for a material that says nothing.
float DaoBumps(float2 uv)
{
    const float2 across = ddx(uv) * mml_vDaoBumps.xy;
    const float2 down = ddy(uv) * mml_vDaoBumps.xy;
    const float texels = sqrt(max(dot(across, across), dot(down, down)));
    const float level = min(log2(max(texels, 1.0)), 6.0);
    const float2 near = saturate(1 - abs(level - float2(1, 2)));
    const float4 far = saturate(1 - abs(level - float4(3, 4, 5, 6)));
    return dot(near, mml_vDaoBumps.zw) + dot(far, mml_vDaoBumpsFar);
}

// The power a surface's highlights and its reflection are made by: its own,
// less where a pixel holds more ways for the surface to face than the one
// its middle has, since what it mirrors is all of them, a wider lobe. Two
// things widen it. One is how fast the normal the pixel is lit by turns
// from this pixel to the next (`normal`, with its normal map): the paper's
// measure, half the turn's square, for a pixel's footprint a half across
// each way. The other is what the pixel cannot see at all, the map's
// normals inside it where it covers many texels (`bumps`: DaoBumps). 0.18
// of the width at the most, for both together.
//
// Where a texel is a pixel or more, nothing is added for the map: what it
// says there is seen, not averaged. A shield's rim in the sun shows its
// normal map's packing so, in dashes along a bevel two texels wide (DXT5:
// blocks of four texels by four), in the unmodified game too. A lobe
// widened by the spread of the map's normals two texels to each side hid
// them (tried 2026-10-12) and was taken out again: it measured the bevel
// itself and not the packing's error, and made every embossed surface
// rougher than its material says (the user: "is the highlight spreading the
// correct fix for the marching ants or just a hack?"). The fault is the
// data's, and the mending is the data's.
float DaoCalmed(float power, float3 normal, float bumps)
{
    const float3 across = ddx(normal);
    const float3 down = ddy(normal);
    const float turns = min(0.5 * (dot(across, across) + dot(down, down)) + bumps, 0.18);
    const float width = min(2 / (power + 2) + turns, 1.0);
    return lerp(power, max(2 / width - 2, 1.0), saturate(DAO_MATERIALS));
}

// A light's highlight, in place of the game's pow(N.H, power): the GGX lobe
// of that width, whole (D over four, in the game's own measure of a light,
// by which a white light makes a white surface facing it white). It is 3,
// the game's own number, at a power of 22. `facing` is N.H.
float DaoLobe(float facing, float power)
{
    const float theirs = pow(facing, power);
    const float width = 2 / (power + 2);
    const float under = facing * facing * (width - 1) + 1;
    const float ours = facing > 0 ? width / (4 * under * under) : 0;
    return lerp(theirs, ours, saturate(DAO_MATERIALS));
}

// What the lights' highlights are multiplied by where they join the
// reflection: the game's 3, or nothing of the kind with DaoLobe's lobes.
float DaoTripled()
{
    return lerp(3.0, 1.0, saturate(DAO_MATERIALS));
}

// The game's own falloff of the mirrored light by the angle, made at each
// vertex: left out, DaoMirror's being that at each pixel.
float DaoFalloff(float theirs)
{
    return lerp(theirs, 1.0, saturate(DAO_MATERIALS));
}

// What the light a surface scatters is multiplied by: a metal scatters none.
float DaoScattered(float metal)
{
    return 1 - saturate(metal);
}

// The mean of one of the game's own cubes, all round: its last picture, a
// texel a face, read along each of the six ways (a level past the last is
// the last).
float3 DaoCubeMean(samplerCUBE cube)
{
    const float far = 16;
    return (texCUBElod(cube, float4(1, 0, 0, far)).rgb + texCUBElod(cube, float4(-1, 0, 0, far)).rgb +
            texCUBElod(cube, float4(0, 1, 0, far)).rgb + texCUBElod(cube, float4(0, -1, 0, far)).rgb +
            texCUBElod(cube, float4(0, 0, 1, far)).rgb + texCUBElod(cube, float4(0, 0, -1, far)).rgb) /
           6;
}

// A cube of the area's as light, counted so that a surface mirroring it is
// to multiply it by the light where the surface is (DaoLocal).
//
// In a level lit again the cube is one of ours, in real light, counted in
// the light that falls on what it sees and kept as the lightmaps are: the
// square root of its share of kDaoCubeMost (Relighter.cpp's CubeFrom and
// WriteCubes; LevelFiles.h's kCubeMost).
//
// The game's own is a picture of eight bits taken at one spot, by what light
// nobody knows: dim beside a sunlit village, and the same for a figure in
// the sun and one in a cellar. Taken as it was, a metal out of doors, which
// scatters nothing and has only what it mirrors, was a dull grey thing
// beside the same suit's gilt (a plumed helm of steel, the user, 2026-10-12:
// "the helm looks gray", and in vanilla "about the same as armor"). So it is
// counted in its own mean, all round: what is left is the picture's shape,
// brighter here and dimmer there about 1, and the light where the surface
// is gives it its brightness and its colour. That light is the mean of what
// comes to the surface from all about it (a probe's light and a baked light
// are the light that falls, over pi), which is what a cube's mean should be.
// It is what is done with a reflection taken at one spot in a level whose
// light is baked (Lagarde and de Rousiers 2014; Call of Duty's "normalised"
// cubes, Lazarov 2013).
static const float kDaoCubeMost = 64.0;
float3 DaoCubeLight(samplerCUBE cube, float3 stored)
{
    const float3 counted = stored / max(DaoCubeMean(cube), 1.0 / 255);
    return lerp(lerp(stored, counted, saturate(DAO_MATERIALS)), stored * stored * kDaoCubeMost, saturate(DAO_RELIT));
}

// What a cube is multiplied by where a surface mirrors it: the light that
// falls where the surface is (`local`: a level's baked light there, a
// figure's or a prop's probe light), so that the reflection is as bright as
// the place and in its light, not as the spot the cube was taken at
// (DaoCubeLight). The game's own arithmetic takes its cube as it is.
float3 DaoLocal(float3 local)
{
    return lerp(1.0, max(local, 0), saturate(max(DAO_MATERIALS, DAO_RELIT)));
}

// The surroundings a surface mirrors, from the area's cube: `way` is where
// the reflection goes and, in w, the mip the game would read. Ours is the
// mip a surface of that roughness blurs the cube to: the cube's last mip
// times r (2 - r), r the square root of the lobe's width alpha (Lagarde and
// de Rousiers 2014). The game's cubes are 256 pixels a face, their last mip
// the eighth; ours are 128, a mip fewer.
float4 DaoReflected(samplerCUBE cube, float4 way, float power)
{
    const float rough = sqrt(sqrt(2 / (power + 2)));
    const float last = 8 - saturate(DAO_RELIT);
    float4 seen = texCUBElod(cube, float4(way.xyz, lerp(way.w, last * rough * (2 - rough), saturate(DAO_MATERIALS))));
    seen.rgb = DaoCubeLight(cube, seen.rgb);
    return seen;
}

// How far towards its edge's mirroring a light's highlight is: Schlick's
// weight. `along` is V.H, the same as L.H.
float DaoEdge(float along)
{
    return pow(1 - saturate(along), 5);
}

// The share of its surroundings a surface of roughness `rough` mirrors, seen
// at `facing` (N.V): x times what it mirrors face on, plus y times what it
// mirrors at its edge. Karis's fit (2014) of the split sum's second half,
// its numbers as he gives them.
float2 DaoSurround(float rough, float facing)
{
    const float4 byRough = float4(-1, -0.0275, -0.572, 0.022);
    const float4 smooth = float4(1, 0.0425, 1.04, -0.04);
    const float4 fit = rough * byRough + smooth;
    const float part = min(fit.x * fit.x, exp2(-9.28 * facing)) * fit.x + fit.y;
    return float2(-1.04, 1.04) * part + fit.zw;
}

// The light a surface mirrors: the lights' highlights and the surroundings'
// reflection, through what the surface mirrors face on (`mirror`, the mask's
// colour, and `own`, the surface's colour, by how much it is metal) and more
// towards its edge. `facing` is N.V, the surface's own and not its normal
// map's where the two are told apart (with_materials has why); `edge` is the
// lights' own weight
// towards the edge, DaoEdge's, the mean of it over the lights by their
// lobes (the lights are summed before this is reached; with one light, or
// lights the same way off, the mean is the thing itself).
// `metal` is DaoMetal's, what mirrors as metal, and `said` the material's
// word that went into it. `env` comes already times that word, as the game
// has it: so of the surroundings the metal's part is weighed by the metal
// there before the word (said once, not twice), and the rest's by what is
// left when the mirroring metal is taken away.
float3 DaoMirror(float3 lights, float3 env, float3 mirror, float3 own, float metal, float facing, float power,
                 float edge, float said)
{
    const float3 theirs = (lights + env) * mirror;
    const float3 both = own + mirror;
    const float3 asMetal = both / max(1.0, max(both.r, max(both.g, both.b)));
    const float mirrors = saturate(metal);
    const float whole = saturate(mirrors / max(saturate(said), 1e-3));
    const float3 unmetal = kDaoMirrorOfMask * mirror;
    const float3 faceOn = lerp(unmetal, asMetal, mirrors);
    const float3 faceOnEnv = (1 - mirrors) * unmetal + whole * asMetal;
    const float edgeOn = saturate(50 * faceOn.g);
    const float2 share = DaoSurround(sqrt(sqrt(2 / (power + 2))), saturate(facing));
    const float3 sent = env * (faceOnEnv * share.x + edgeOn * (1 - mirrors + whole) * share.y);
    gDaoEnvSent = lerp(env * mirror, sent, saturate(DAO_MATERIALS));
    return lerp(theirs, lights * (faceOn + (edgeOn - faceOn) * saturate(edge)) + sent, saturate(DAO_MATERIALS));
}
