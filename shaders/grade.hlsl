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

// The picture's exposure, a factor on the scene's light before it is rolled
// off: the options screen's Exposure, -0.5 to +0.5 EV, 0 by default (a
// value of the chain, build_shaders.py's LOOK).
float fDaoExposure;

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

// How much brighter the shade is made, in stops: a value of the chain
// (build_shaders.py's LOOK), 0 with Improved lighting off. The sun under one
// rule and the level's own shadows left a forest's floor and a party under a
// timber house near black (the user, 2026-10-17: "We just don't want the
// shadows to be pure black"). It is laid on the dark tones alone: whole on
// what is a sixteenth of white and darker, none on what is a quarter of white
// and brighter, and between them less the brighter a colour is, so a sunlit
// street is not taken up with the shade beside it; it multiplies, so black
// stays black. Fading over two stops, a lift under a stop never makes a darker
// colour the brighter. (An eye's exposure, measured from the picture each
// frame and eased, did it for a day: it lifted the forest's floor by 0.24 of a
// stop and Denerim's shaded house by 0.35, and the party picker's figures
// came up over seconds as it caught up; a lift that is the same everywhere
// does what was wanted of it, with nothing to lag.)
float fDaoLift;
static const float kDaoLiftWhole = -4;
static const float kDaoLiftNone = -2;

// The picture at `uv`, as the grade is to read it, its shade lifted by
// `lift` stops (fDaoLift, or 0); rolled off at white where `rolled` (the
// fading form, which has no curve after it), else left as bright as it is
// for DaoIntoCurve.
float3 DaoPicture(sampler2D picture, float2 uv, float lift, bool rolled)
{
    const float3 colour = tex2D(picture, uv).rgb;
    const float stopsFromWhite = log2(max(dot(colour, float3(0.2126, 0.7152, 0.0722)), 1.0 / 4096));
    const float dark = saturate((kDaoLiftNone - stopsFromWhite) / (kDaoLiftNone - kDaoLiftWhole));
    const float3 lit = colour * (fDaoExposure * exp2(lift * dark));
    return rolled ? DaoShoulder(lit) : lit;
}

// Where an area's curve reaches white, a channel at a time: the least of its
// input that comes out white, found by halving (eight times, to a 256th: the
// curve is 256 wide). The grade's last step is a curve for each channel,
// each area's own (cc_<area>.dds; DAOLib's dev/TODO.md, read 2026-10-17), and
// many are an S that is white well short of the top: the Dead Trenches' at
// 0.59, 0.78 and 0.66 of red, green and blue, the Circle's at 0.52, 0.54 and
// 0.40, the Dalish camp's near 1.
float3 DaoCurveWhite(sampler2D curve)
{
    float3 low = 0;
    float3 high = 1;
    [unroll] for (int i = 0; i < 8; i++)
    {
        const float3 middle = (low + high) / 2;
        const float3 seen =
            float3(tex2Dlod(curve, float4(middle.r, 0.5, 0, 0)).r, tex2Dlod(curve, float4(middle.g, 0.5, 0, 0)).g,
                   tex2Dlod(curve, float4(middle.b, 0.5, 0, 0)).b);
        const float3 white = step(254.5 / 255, seen);
        high = lerp(high, middle, white);
        low = lerp(middle, low, white);
    }
    return max(high, 1.0 / 255);
}

// The graded colour (the picture through the area's colour matrix, as bright
// as it is) brought into what the area's curve can show, each channel's
// white the curve's: rolled off by DaoShoulder as a share of it. What the
// curve would have cut at its white comes down on the shoulder instead, and
// below the knee the curve has the colour as it had it. The roll-off ran on
// the picture before the matrix, white at 1, and an effect's flash in the
// Dead Trenches, rolled off to 0.9 and more, was cut flat white by the
// curve's 0.59 (the user, 2026-10-17: "we're blowing out the image with
// effect lighting").
float3 DaoIntoCurve(float3 graded, sampler2D curve)
{
    const float3 white = DaoCurveWhite(curve);
    return DaoShoulder(graded / white) * white;
}

// The graded picture's colour made ours. `pixel` is its place on the screen.
float3 DaoGrade(float3 colour, float2 pixel)
{
    // Interleaved gradient noise (Jimenez 2014), a step of the 8-bit picture wide.
    const float grain = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return saturate(colour) + (grain - 0.5) / 255;
}
