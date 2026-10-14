// The last word on the whole picture: what Elgara Shaders adds to the
// game's own colour grade (build_shaders.py).
//
// The game ends a frame with da_hsvmatrix, one pass over the whole picture
// (hslmatrix.psh, read 2026-10-10): the picture through a colour matrix (the
// area's hue, saturation, brightness and contrast) and then each channel
// through a curve kept in a texture. This rides in that pass, so it costs no
// pass of its own and needs nothing of the engine: the picture is darkened
// by the ambient occlusion and brought down from the 16-bit scene as it is
// read, and after the game's grade its smooth gradients are given a grain
// too fine to see, which is what keeps a sky or a fog from showing bands.

// The ambient occlusion the chain's passes before this one made
// (occlusion.hlsl, occlusion_blur.hlsl): 1 where a point is open.
sampler2D smpOcclusion;

// What the picture is multiplied by: the openness, on the ambient share of
// the pixel's light and no more (the pass's green: issue 8). A wall in the
// sun keeps its creases' light; a corner in shade loses it.
float DaoOcclusion(float2 uv)
{
    const float2 kept = tex2D(smpOcclusion, uv).rg;
    return lerp(1, kept.r, kept.g);
}

// How far what is brighter than white is rolled off, not cut: a value of
// the chain (build_shaders.py's LOOK), 1 with Improved materials on.
float fDaoHighlights;

// The brightness that comes out white, in whites. The scene is the game's
// own light (nothing here makes it brighter), which is seldom much over
// white: a shoulder long enough for more bends a room's whites down to grey
// (the Circle was a third darker at a range of four, measured 2026-10-11).
static const float kDaoRange = 1.5;

// A brightness as the picture can hold it: its own to a knee, and above it
// on a curve that leaves the knee at the same slope and is white at
// kDaoRange (Reinhard's curve with a white point, begun at the knee).
float DaoKnee()
{
    return 0.9 / (1 + 0.12 * kDaoRange);
}

float3 DaoRolled(float3 light)
{
    const float knee = DaoKnee();
    const float3 over = max(light - knee, 0) / (1 - knee);
    const float white = (kDaoRange - knee) / (1 - knee);
    return min(light, knee) + (1 - knee) * saturate(over * (1 + over / (white * white)) / (1 + over));
}

// How bright a colour keeps its hue as it is rolled off, and from how bright
// it burns out towards white, in whites: from 0.8 of white to 2.2. A flame
// held to its hue whole is a flat deep orange where vanilla's, cut at white
// a channel at a time, is yellow at its heart (the user, 2026-10-11: "fire
// looks too orange").
static const float kDaoBurnFrom = 0.8;
static const float kDaoBurnTo = 2.2;

// A colour of the scene as the picture can hold it. With the scene drawn
// into a 16-bit target (Graphics.cpp's own scene target) a colour may be
// brighter than white, and this is the one place it comes down: the game's
// grade after this reads a table by the colour and knows nothing above 1.
// Cut at white a channel at a time, as eight bits cut it, a bright gold goes
// pale: red and green both stop at 1 and blue catches up. So a colour a
// little over white keeps its hue: its strongest channel is rolled off and
// the others keep their share of it. A colour far over white is another
// thing, a fire or a spell, layer on layer of light added up: held to its
// hue, all of it comes down to one flat colour, and the fires of Denerim's
// market were pale boxes (the user, 2026-10-11). Such a colour burns out to
// white as it does on film and as eight bits made it, each channel rolled
// off on its own. At fDaoHighlights of 0 it is the cut.
float3 DaoShoulder(float3 colour)
{
    colour = max(colour, 0);
    const float peak = max(colour.r, max(colour.g, colour.b));
    const float3 hued = peak > DaoKnee() ? colour * (DaoRolled(peak).x / peak) : colour;
    const float3 burnt = DaoRolled(colour);
    // From its hue at kDaoBurnFrom whites to burnt out at kDaoBurnTo.
    const float3 kept = lerp(hued, burnt, saturate((peak - kDaoBurnFrom) / (kDaoBurnTo - kDaoBurnFrom)));
    return lerp(saturate(colour), kept, fDaoHighlights);
}

// The picture at `uv`, as the grade is to read it.
float3 DaoPicture(sampler2D picture, float2 uv)
{
    return DaoShoulder(tex2D(picture, uv).rgb);
}

// The graded picture's colour made ours. `pixel` is its place on the screen.
float3 DaoGrade(float3 colour, float2 pixel)
{
    // Interleaved gradient noise (Jimenez 2014), a step of the 8-bit picture wide.
    const float grain = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return saturate(colour) + (grain - 0.5) / 255;
}
