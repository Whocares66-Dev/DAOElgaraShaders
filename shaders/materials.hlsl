// What armour, clothing and weapons do with the light that glances off them.
// Elgara Shaders puts this into the character shaders that build their
// highlight the one common way (build_shaders.py's with_materials).
//
// The game's highlight is pow(N.H, power) times 3, whatever the power, which
// the specular mask's alpha sets from 1 to 256. A highlight that is narrower
// gathers the same light into a smaller place and should be brighter by as
// much; at a fixed 3, polished plate has a pinpoint no brighter than cloth's
// wide sheen, so metal reads as plastic and cloth as wet. And the mirror
// colour is the same seen face on as edge on, where every real surface
// reflects more.
//
// DAO_MATERIALS (look.hlsl), 0 to 1, is how far towards the corrected forms:
// 0 is vanilla. It wants the scene drawn into a 16-bit target with its
// highlights rolled off (Graphics.cpp, grade.hlsl), and the plugin holds it
// at 0 without one: in eight bits a highlight brighter than vanilla's runs
// into white a channel at a time and washes gilded plate pale (seen close,
// in a conversation, 2026-10-11).

// The brightest a highlight's lobe is at its middle, in whites of the light:
// a mirror's is past any picture, and what is past the picture's range is
// only bloom.
static const float kDaoHighlightMost = 6.0;
static const float kDaoSharpestPower = 96.0;

// A light's highlight on a surface, in place of the game's pow(N.H, power):
// the GGX lobe (Walter et al. 2007, Trowbridge and Reitz), which is what a
// rough surface's highlight is shaped like, a bright middle with a long soft
// skirt where the game's falls to nothing. Its width is the power's:
// alpha squared is 2 / (power + 2), the width a Blinn-Phong lobe of that
// power has. And it is the lobe whole, pi D / 4, so a narrower one is
// brighter by as much as it is narrower: (power + 2) / 8 at its middle, which
// is the game's 3 at a power of 22. The game's own tripling is taken off
// where it joins the sum (DaoTripled).
float DaoLobe(float facing, float power)
{
    const float theirs = pow(facing, power);
    // No narrower than a power of 96 makes it: a blade's bevel is painted at
    // a gloss of 1 a texel wide, and a lobe that narrow is on one pixel and
    // off the next along the edge, dashes that crawl as the blade moves (the
    // user, 2026-10-12: "it's like a marching ants effect").
    const float width = 2 / (min(power, kDaoSharpestPower) + 2);
    const float under = facing * facing * (width - 1) + 1;
    const float ours = facing > 0 ? min(width / (4 * under * under), kDaoHighlightMost) : 0;
    return lerp(theirs, ours, saturate(DAO_MATERIALS));
}

// What the lights' highlights are multiplied by where they join the
// reflection: the game's 3, or nothing of the kind with DaoLobe's lobes.
float DaoTripled()
{
    return lerp(3.0, 1.0, saturate(DAO_MATERIALS));
}

// The game's own falling off of a surface's mirror light by the angle it is
// seen at, made at each vertex with the vertex's normal (0.39 face on to 1.5
// edge on, for nearly every material: read 2026-10-12): left out, DaoFresnel
// being that at each pixel with the pixel's normal.
float DaoFalloff(float theirs)
{
    return lerp(theirs, 1.0, saturate(DAO_MATERIALS));
}

// How much a surface is metal, 0 to 1. The game's textures have no map that
// says so, and two that between them do: the specular mask's alpha, the
// gloss, and its colour, the mirror colour. Measured pixel by pixel over the
// game's armour (2026-10-12): two fifths of plate's pixels have a gloss over
// 0.2 with a mirror colour over 0.2, a seventh of mail's and scale's and a
// tenth of leather's and cloth's. There is no clean line between them, so
// it is a matter of degree, and a material it is wrong for wants saying so
// itself.
// (Wide, since the user's first look, 2026-10-12: from 0.15 to 0.40 and 0.15
// to 0.35 a suit's pixels were metal or not by the mask's own grain, one dark
// and mirroring beside one painted, and the surface crawled: "lots of dark
// flickering on the surface".)
static const float kDaoMetalGlossFrom = 0.10;
static const float kDaoMetalGlossTo = 0.55;
static const float kDaoMetalMirrorFrom = 0.10;
static const float kDaoMetalMirrorTo = 0.50;
// How much less light a full metal scatters: it has no colour of its own to
// scatter, its colour is the colour of what it mirrors. The game gives gilded
// plate a bright yellow of its own under a soft highlight, which is how
// plastic is lit (the user, 2026-10-11: "looks like yellow plastic ... it's
// supposed to be metallic gold"). Half, not the quarter it was: the game's
// metals are painted, their dents and dirt and trim are in the colour they
// scatter, and at a quarter a suit lost them.
// (A fifth for a surface sure to be metal was tried for an hour, and was
// tin foil: "let's have the metal take more of its base color, so it doesn't
// look tin-foil", the user, 2026-10-12. The game's metals are coloured, red
// steel and gilt and silverite, and that colour is in what they scatter.)
static const float kDaoMetalScatters = 0.8;
// How far a metal's mirror colour takes the hue of what it scatters, which
// is what a coloured metal's does.
static const float kDaoMetalOwnHue = 0.85;
// And how much more it mirrors than it is painted to: the mirror colours are
// dim, plate's a fifth to two fifths where steel's is over a half.
static const float kDaoMetalMirrors = 1.25;

// How far a colour is from grey, 0 to 1.
float DaoColoured(float3 colour)
{
    const float most = max(colour.r, max(colour.g, colour.b));
    return (most - min(colour.r, min(colour.g, colour.b))) / max(most, 1e-3);
}

// `own` is the colour the surface scatters, which says what the mask cannot:
// paint. A shield's arms are lacquered, glossy and bright in the mask as
// plate is, and taken for metal the white of its field went dark and its
// lions dull (seen in Redcliffe, 2026-10-12). Paint is a colour the mirror
// colour does not share (a gilded thing mirrors its own yellow; a painted
// one mirrors grey), or a white brighter than a metal is painted.
float DaoMetal(float3 mirror, float power, float3 own)
{
    const float bright = max(mirror.r, max(mirror.g, mirror.b));
    const float painted = max(saturate((DaoColoured(own) - DaoColoured(mirror) - 0.1) * 4),
                              smoothstep(0.55, 0.8, max(own.r, max(own.g, own.b))));
    return DAO_MATERIALS * smoothstep(kDaoMetalGlossFrom, kDaoMetalGlossTo, power / 256) *
           smoothstep(kDaoMetalMirrorFrom, kDaoMetalMirrorTo, bright) * (1 - painted);
}

// What the light a surface scatters is multiplied by.
float DaoScattered(float metal)
{
    return lerp(1.0, kDaoMetalScatters, saturate(metal));
}

// The surroundings a surface mirrors, from the area's cube: `way` is where
// the reflection goes and, in w, the mip the game would read, nine times
// what the gloss lacks of 1. The cube is a picture of the room from where
// its probe stands, 256 pixels a face (read 2026-10-12: its walls, windows
// and lamps are all there), and the game reads plate, whose gloss is a fifth
// to a half, at its seventh mip of nine: two pixels a face, one flat colour,
// which is why its metal has nothing in it to be metal by. So a glossy
// surface reads it sharper, by the square of what the gloss lacks (plate
// from the fifth mip to the second), and what it reads is set further from
// the room's mean than the picture has it: the cube is eight bits and a lamp
// in it is no brighter than a white wall, where to a mirror it is many times
// so. Sharper and further apart than this (the cube of what it lacks, two and
// a half times apart) every dent of the normal map read another part of the
// room, dark beside bright, and the armour crawled as it moved.
static const float kDaoCubeMips = 8.0;
// The sharpest it is read, whatever the gloss: its fourth mip, 32 pixels a
// face. A helm's dome painted at a gloss of 1 read the cube whole, and was a
// picture of the room so clear that it looked like glass (the user,
// 2026-10-12: "cailan's helmet looks transparent for some reason"). The cube
// is one picture from one place, good for the light and dark of a room and
// not for its image.
static const float kDaoCubeSharpest = 3.0;
static const float kDaoMirrorApart = 1.25;

float4 DaoReflected(samplerCUBE cube, float4 way, float power)
{
    const float much = saturate(DAO_MATERIALS);
    const float gloss = saturate((power - 1) / 255);
    const float lacks = 1 - gloss;
    float4 seen = texCUBElod(cube, float4(way.xyz, lerp(way.w, max(kDaoCubeMips * lacks * lacks, kDaoCubeSharpest), much)));
    const float3 mean = texCUBElod(cube, float4(way.xyz, kDaoCubeMips - 1)).rgb;
    const float apart = lerp(1.0, kDaoMirrorApart, smoothstep(0.1, 0.4, gloss) * much);
    // Its brightness is set apart, not its colour: with the colours set
    // apart too, gilded plate under a blue sky wore patches of green and
    // cyan (seen in Redcliffe, 2026-10-12).
    const float was = dot(seen.rgb, float3(0.2126, 0.7152, 0.0722));
    const float room = dot(mean, float3(0.2126, 0.7152, 0.0722));
    seen.rgb *= max(room + (was - room) * apart, 0) / max(was, 1e-4);
    return seen;
}

// The mirror colour as seen at an angle (Schlick 1994): `colour` face on,
// and edge on more, by the surface's polish. `facing` is N.V, 1 face on;
// `power` the highlight's, whose share of 256 is the polish.
//
// Edge on it is no white mirror, as the formula has it for a perfect
// surface: it is two fifths at most and in the mirror colour's own hue. A
// helm's dome and a blade's bevel are painted at a gloss of 1, and with a
// white mirror at their edges the side of the dome that turned from the eye
// was a clear picture of the sky where the sun did not reach it, with a hard
// edge where the sun began (the user, 2026-10-12: "cailan's helmet looks
// transparent", and of a sword, "only looks bad when viewed from sharp
// angle"). Found by taking the change out of the shaders a few at a time:
// the patch is the unlit side of the dome, and what was seen there was the
// reflection alone.
static const float kDaoEdgeMost = 0.4;

float3 DaoFresnel(float3 colour, float facing, float power)
{
    const float edge = pow(1 - saturate(facing), 5);
    const float polish = min(saturate(power / 256), kDaoEdgeMost);
    const float bright = max(colour.r, max(colour.g, colour.b));
    const float3 rim = max(colour, colour * (polish / max(bright, 1e-3)));
    return colour + (rim - colour) * edge * saturate(DAO_MATERIALS);
}

// The light a surface mirrors: the lights' highlights and the surroundings'
// reflection (`env`, from the area's cubemap), through the mirror colour.
// A metal has more of that than it is painted with, in the same hue (a
// mirror colour scaled past white went white, and gilt was foil), and it
// takes the hue of `own`, the colour the surface scatters.
float3 DaoSpecular(float3 lights, float3 env, float3 colour, float metal, float facing, float power, float3 own)
{
    const float sure = saturate(metal);
    const float bright = max(colour.r, max(colour.g, colour.b));
    float3 mirror = colour * min(lerp(1.0, kDaoMetalMirrors, sure), 1 / max(bright, 1e-3));
    // By how bright the mirror colour is, not by how sure the metal: a
    // gilded pauldron the telling was unsure of mirrored a white sky in grey
    // and was silver, where a mirror colour that bright is a metal's whatever
    // its gloss (seen in Redcliffe, 2026-10-12).
    // Unless the mirror colour has a hue of its own, which is then the
    // metal's: dragonbone plate's red is in its mirror colour (an armour's
    // tint gives it one) over a dull brown, and given the brown's hue its
    // red went, twice over (the user, 2026-10-12, after the first try:
    // "reflection seems same to me").
    const float3 ownHue = own / max(max(own.r, max(own.g, own.b)), 1e-3);
    const float3 hue = lerp(ownHue, colour / max(bright, 1e-3), saturate(DaoColoured(colour) * 4));
    // And by its gloss as much as by its mirror colour: dragonbone plate is
    // a wine red with a dim grey mirror colour and a high gloss, and what it
    // mirrored it mirrored grey, a steel suit with red beads (the user,
    // 2026-10-12: "the metal can / should reflect light more in accordance
    // w/ its base color").
    const float glossy = smoothstep(0.1, 0.4, saturate(power / 256));
    const float tinted = kDaoMetalOwnHue * max(smoothstep(0.08, 0.3, bright), glossy) * saturate(DAO_MATERIALS);
    mirror = lerp(mirror, hue * max(mirror.r, max(mirror.g, mirror.b)), tinted);
    return (lights + env) * DaoFresnel(mirror, facing, power);
}
