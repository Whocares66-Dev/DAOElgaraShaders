// The last word on the whole picture: what Elgara Shaders adds to the
// game's own colour grade (build_shaders.py).
//
// The game ends a frame with da_hsvmatrix, one pass over the whole picture
// (hslmatrix.psh, read 2026-10-10): the picture through a colour matrix (the
// area's hue, saturation, brightness and contrast) and then each channel
// through a curve kept in a texture. This rides in that pass, so it costs no
// pass of its own and needs nothing of the engine: the picture is sharpened
// as it is read, and after the game's grade its colours are given more life,
// its middle tones more contrast, and its smooth gradients a grain too fine
// to see, which is what keeps a sky or a fog from showing bands.

// The ambient occlusion the chain's passes before this one made
// (occlusion.hlsl, occlusion_blur.hlsl): 1 where a point is open.
sampler2D smpOcclusion;

float DaoOcclusion(float2 uv)
{
    return tex2D(smpOcclusion, uv).r;
}

// How much of each, 0 for none: values of the chain (build_shaders.py's
// LOOK), which the plugin sets from graphics.ini.
float fDaoSharpen;    // 0 to 1
float fDaoVibrance;   // saturation given to the colours that have least
float fDaoContrast;   // of an S-curve over the middle tones
float fDaoHighlights; // how far what is brighter than white is rolled off, not cut: 0 to 1

static const float3 kDaoLuma = float3(0.2126, 0.7152, 0.0722);
// The brightness a colour's strongest channel keeps as it is; above it the
// channel is bent towards white, which it reaches only at no end of light.
static const float kDaoKnee = 0.8;
// How bright a colour keeps its hue as it is rolled off, and from how bright
// it burns out to white, in whites.
static const float kDaoBurnFrom = 1.15;
static const float kDaoBurnTo = 1.8;

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
    const float rolled = kDaoKnee + (1 - kDaoKnee) * (1 - exp(-(peak - kDaoKnee) / (1 - kDaoKnee)));
    const float3 hued = peak > kDaoKnee ? colour * (rolled / peak) : colour;
    const float3 each = kDaoKnee + (1 - kDaoKnee) * (1 - exp(-(colour - kDaoKnee) / (1 - kDaoKnee)));
    const float3 burnt = float3(colour.r > kDaoKnee ? each.r : colour.r, colour.g > kDaoKnee ? each.g : colour.g,
                                colour.b > kDaoKnee ? each.b : colour.b);
    // From its hue at kDaoBurnFrom times white to burnt out at kDaoBurnTo.
    const float3 kept = lerp(hued, burnt, saturate((peak - kDaoBurnFrom) / (kDaoBurnTo - kDaoBurnFrom)));
    return lerp(saturate(colour), kept, fDaoHighlights);
}

// The picture at `uv`, sharpened by its four neighbours: contrast-adaptive
// sharpening (AMD's FidelityFX CAS, the simple form), which sharpens flat
// detail most and an edge already hard least, so it rings very little.
float3 DaoSharpen(sampler2D picture, float2 uv)
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float3 here = DaoShoulder(tex2D(picture, uv).rgb);
    const float3 up = DaoShoulder(tex2D(picture, uv - float2(0, texel.y)).rgb);
    const float3 down = DaoShoulder(tex2D(picture, uv + float2(0, texel.y)).rgb);
    const float3 left = DaoShoulder(tex2D(picture, uv - float2(texel.x, 0)).rgb);
    const float3 right = DaoShoulder(tex2D(picture, uv + float2(texel.x, 0)).rgb);
    const float3 least = min(here, min(min(up, down), min(left, right)));
    const float3 most = max(here, max(max(up, down), max(left, right)));
    const float3 room = sqrt(saturate(min(least, 1 - most) / max(most, 1e-4)));
    const float3 weight = room * (-1 / lerp(8.0, 5.0, saturate(fDaoSharpen)));
    return saturate(((up + down + left + right) * weight + here) / (1 + 4 * weight));
}

// The graded picture's colour made ours. `pixel` is its place on the screen.
float3 DaoGrade(float3 colour, float2 pixel)
{
    const float luma = dot(colour, kDaoLuma);
    const float spread = max(colour.r, max(colour.g, colour.b)) - min(colour.r, min(colour.g, colour.b));
    colour = lerp(luma, colour, 1 + fDaoVibrance * (1 - spread));
    colour = saturate(colour);
    colour = lerp(colour, colour * colour * (3 - 2 * colour), fDaoContrast);
    // Interleaved gradient noise (Jimenez 2014), a step of the 8-bit picture wide.
    const float grain = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return colour + (grain - 0.5) / 255;
}
