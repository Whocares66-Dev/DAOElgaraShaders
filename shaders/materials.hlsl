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

// The brightest a highlight is scaled: the picture is eight bits, and what is
// past white is only lost.
static const float kDaoHighlightMost = 12.0;

// What the lights' highlights are multiplied by, in place of the game's 3:
// the Blinn-Phong lobe's own measure, (power + 2) / 8, which is 3 at a power
// of 22, and never less than the game's. Below 22 the measure would dim a
// wide sheen the armour was painted to have: gilded plate went dull with it
// (seen in Redcliffe, 2026-10-11).
float DaoHighlight(float power)
{
    return lerp(3.0, clamp((power + 2) / 8, 3.0, kDaoHighlightMost), saturate(DAO_MATERIALS));
}

// How much a surface is metal, 0 to 1. The game's textures have no map that
// says so, and two that between them do: the specular mask's alpha, the
// gloss, is high on plate and low on leather and cloth, rivets apart (seen
// as a picture on four suits, 2026-10-11), and its colour, the mirror
// colour, is dark where a surface mirrors nothing. A surface is metal as far
// as it is both glossy and mirroring.
static const float kDaoMetalGlossFrom = 0.4;
static const float kDaoMetalGlossTo = 0.75;
static const float kDaoMetalMirrorFrom = 0.12;
static const float kDaoMetalMirrorTo = 0.35;
// How much less light a full metal scatters, and how much more of its
// surroundings it mirrors. A metal has no colour of its own to scatter: its
// colour is the colour of what it mirrors. The game gives gilded plate a
// bright yellow of its own under a soft highlight, which is how plastic is
// lit (the user, 2026-10-11: "looks like yellow plastic ... it's supposed to
// be metallic gold").
static const float kDaoMetalScatters = 0.25;
static const float kDaoMetalMirrors = 4.5;

float DaoMetal(float3 mirror, float power)
{
    const float bright = max(mirror.r, max(mirror.g, mirror.b));
    return DAO_MATERIALS * smoothstep(kDaoMetalGlossFrom, kDaoMetalGlossTo, power / 256) *
           smoothstep(kDaoMetalMirrorFrom, kDaoMetalMirrorTo, bright);
}

// What the light a surface scatters is multiplied by.
float DaoScattered(float metal)
{
    return lerp(1.0, kDaoMetalScatters, saturate(metal));
}

// The mirror colour as seen at an angle: `colour` face on, towards the
// surface's polish (never less than its colour) edge on. `facing` is N.V, 1
// face on; `power` the highlight's, whose share of 256 is the polish.
float3 DaoFresnel(float3 colour, float facing, float power)
{
    const float edge = pow(1 - saturate(facing), 5);
    const float polish = saturate(power / 256);
    return colour + (max(colour, polish) - colour) * edge * saturate(DAO_MATERIALS);
}

// The light a surface mirrors: the lights' highlights and the surroundings'
// reflection (`env`, from the area's cubemap), the last more for a metal,
// through the mirror colour.
float3 DaoSpecular(float3 lights, float3 env, float3 colour, float metal, float facing, float power)
{
    return (lights + env * lerp(1.0, kDaoMetalMirrors, saturate(metal))) * DaoFresnel(colour, facing, power);
}
