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

// The glow the chain's passes before this one made of what is brighter than
// the picture holds (glow_bright.hlsl and the two after it), as light: it is
// kept as the square root of its light over glow_bright.hlsl's kDaoGlowMost.
sampler2D smpGlow;
float fDaoBloom; // how much of the glow is added: 0 to 4

float3 DaoGlow(float2 uv)
{
    const float3 kept = tex2D(smpGlow, uv).rgb;
    return kept * kept * 16.0 * fDaoBloom;
}

// The air between the eye and what it sees, lit by the sun. The game's fog
// is one colour whichever way one looks; haze in sunlight is bright towards
// the sun and dull away from it, and that is most of what says where the
// sun is and that the air has depth. So light is added by how much air a
// pixel is seen through (the depth in metres the occlusion's first pass
// made, occlusion_depth.hlsl; the sky is all of it) and by how nearly the
// pixel looks at the sun.
//
// Where the sun is in the picture is the plugin's to say (Graphics.cpp's
// KeepAir), each frame: fDaoSunX and fDaoSunY are its place on a plane one
// unit in front of the eye, or the place straight against it if it is behind
// (fDaoSunSide -1); fDaoLensX and fDaoLensY are the picture's width and
// height on that plane. The light's colour is the area's sun's, nothing under
// a moon, and fDaoHazeThick how much air a metre is.
sampler2D smpMetres;
float fDaoSunX;
float fDaoSunY;
float fDaoSunSide;
float fDaoLensX;
float fDaoLensY;
float fDaoHazeThick;
float fDaoSunRed;
float fDaoSunGreen;
float fDaoSunBlue;
float fDaoSunHaze; // how brightly: 0 to 4

// How much of the sun's light haze sends on to the side and back, of what it
// sends straight on; and how fast that falls off away from the sun.
static const float kDaoHazeAround = 0.04;
static const float kDaoHazeForward = 6.0;

float3 DaoAir(float2 uv)
{
    const float2 here = (uv - 0.5) * float2(fDaoLensX, fDaoLensY);
    const float2 sun = float2(fDaoSunX, fDaoSunY);
    const float toward = fDaoSunSide * (dot(here, sun) + 1) / sqrt((dot(here, here) + 1) * (dot(sun, sun) + 1));
    const float air = 1 - exp(-tex2D(smpMetres, uv).r * fDaoHazeThick);
    const float lit = lerp(kDaoHazeAround, 1.0, pow(saturate(0.5 + 0.5 * toward), kDaoHazeForward));
    return float3(fDaoSunRed, fDaoSunGreen, fDaoSunBlue) * (fDaoSunHaze * lit * air);
}

// How much of each, 0 for none: values of the chain (build_shaders.py's
// LOOK), which the plugin sets from graphics.ini.
float fDaoSharpen;    // 0 to 1
float fDaoVibrance;   // saturation given to the colours that have least
float fDaoContrast;   // of an S-curve over the middle tones; less than 0 flattens them
float fDaoSaturation; // the player's own: -1 is grey, 0 as it is, 1 twice the colour
float fDaoWarmth;     // the player's own: -1 cooler, 1 warmer, the brightness kept
float fDaoHighlights; // how far what is brighter than white is rolled off, not cut: 0 to 1
float fDaoExposure;   // stops the scene is opened up by before that, each one a doubling of its light: -3 to 3
float fDaoRange;      // the brightness that comes out white, in whites: 1 to 16

static const float3 kDaoLuma = float3(0.2126, 0.7152, 0.0722);
// A brightness as the picture can hold it: its own to a knee, and above it
// on a curve that leaves the knee at the same slope and is white at
// fDaoRange (Reinhard's curve with a white point, begun at the knee). The
// range is the plugin's to say, by the sun an area has (Graphics.cpp's
// KeepSun): a room's light is seldom much over white, and a shoulder long
// enough for ground in a sun four times the area's own bends a room's
// whites down to grey (the Circle was a third darker with it, measured
// 2026-10-11); a shoulder short enough for a room makes that ground one pale
// yellow (seen in Redcliffe the same day). The knee is lower the longer the
// range: 0.76 at a range of one and a half, 0.61 at four.
float DaoKnee()
{
    return 0.9 / (1 + 0.12 * max(fDaoRange, 1));
}

float3 DaoRolled(float3 light)
{
    const float knee = DaoKnee();
    const float3 over = max(light - knee, 0) / (1 - knee);
    const float white = (max(fDaoRange, 1.01) - knee) / (1 - knee);
    return min(light, knee) + (1 - knee) * saturate(over * (1 + over / (white * white)) / (1 + over));
}

// How bright a colour keeps its hue as it is rolled off, and from how bright
// it burns out towards white, in whites.
//
// These were parts of the range (0.4 of it to all of it), and a level lit
// again has a range of eight whites: a flame, drawn two and a half whites
// bright by the look's Fire, kept its hue whole and was a flat deep orange
// where vanilla's, cut at white a channel at a time, is yellow at its heart
// (the user, 2026-10-11: "fire looks too orange. dunno if we're manipulating
// the color balance, contrast, or what"). What burns out is a matter of how
// bright a thing is against white, not against the most the picture holds:
// from 0.8 of white to 2.2, whatever the range, which is what the old
// numbers came to out of doors at a range of two.
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
float3 DaoShoulder(float3 colour, float3 glow)
{
    colour = max(colour, 0) * exp2(fDaoExposure) + glow;
    const float peak = max(colour.r, max(colour.g, colour.b));
    const float3 hued = peak > DaoKnee() ? colour * (DaoRolled(peak).x / peak) : colour;
    const float3 burnt = DaoRolled(colour);
    // From its hue at kDaoBurnFrom whites to burnt out at kDaoBurnTo.
    const float3 kept = lerp(hued, burnt, saturate((peak - kDaoBurnFrom) / (kDaoBurnTo - kDaoBurnFrom)));
    return lerp(saturate(colour), kept, fDaoHighlights);
}

// The picture at `uv`, sharpened by its four neighbours: contrast-adaptive
// sharpening (AMD's FidelityFX CAS, the simple form), which sharpens flat
// detail most and an edge already hard least, so it rings very little.
float3 DaoSharpen(sampler2D picture, float2 uv, float3 glow)
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float3 here = DaoShoulder(tex2D(picture, uv).rgb, glow);
    const float3 up = DaoShoulder(tex2D(picture, uv - float2(0, texel.y)).rgb, glow);
    const float3 down = DaoShoulder(tex2D(picture, uv + float2(0, texel.y)).rgb, glow);
    const float3 left = DaoShoulder(tex2D(picture, uv - float2(texel.x, 0)).rgb, glow);
    const float3 right = DaoShoulder(tex2D(picture, uv + float2(texel.x, 0)).rgb, glow);
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
    // The player's own tuning (the options screen's sliders, kept a level at a time).
    colour = lerp(dot(colour, kDaoLuma), colour, 1 + fDaoSaturation);
    const float3 warm = float3(1 + 0.3 * fDaoWarmth, 1, 1 - 0.3 * fDaoWarmth);
    colour *= warm / dot(warm, kDaoLuma);
    colour = saturate(colour);
    colour = lerp(colour, colour * colour * (3 - 2 * colour), fDaoContrast);
    // Interleaved gradient noise (Jimenez 2014), a step of the 8-bit picture wide.
    const float grain = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    return colour + (grain - 0.5) / 255;
}
