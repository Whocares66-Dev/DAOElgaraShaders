// How open each point of the picture is to the sky around it, from the scene's
// depth alone: ambient occlusion, the second of the four passes
// Elgara Shaders adds to the game's colour-grade chain (build_shaders.py).
// occlusion_depth.hlsl writes each pixel's distance, this one the openness,
// occlusion_blur.hlsl smooths it across and then down, and the grade
// multiplies it into the picture.
//
// It rebuilds where each pixel is in front of the camera and darkens a point
// by how much of its neighbourhood rises above its own surface (Scalable
// Ambient Obscurance, McGuire et al. 2012): the corners where two walls meet,
// the ground under a body, the folds of armour.
//
// smpDepth is occlusion_depth.hlsl's target, metres as one float, at sampler
// 1, which the pass's state reads unfiltered.

sampler2D smpDepth : register(s1);

// Values of the chain (build_shaders.py's LOOK), set from graphics.ini.
float fDaoOcclusion;       // how dark, 0 for none
float fDaoOcclusionRadius; // metres around a point that can shade it

static const float kDaoAoFar = 60.0; // metres from the camera at which it has faded out
static const int kDaoAoTaps = 14;
// The tangent of half the camera's vertical field of view. Not read from the
// engine yet: a guess, and one that only stretches the neighbourhood sideways.
static const float kDaoAoHalfFov = 0.45;
// The most of the screen's height the radius may span.
static const float kDaoAoMostOfScreen = 0.2;

// Where a pixel is: x right, y up, z into the screen, in metres.
float3 DaoViewPoint(float2 uv, float aspect)
{
    const float depth = tex2Dlod(smpDepth, float4(uv, 0, 0)).r;
    return float3((uv.x * 2 - 1) * kDaoAoHalfFov * aspect, (1 - uv.y * 2) * kDaoAoHalfFov, 1) * depth;
}

// How the surface runs from `here` along one axis of the screen, by the
// neighbour on the side it changes less towards: at an outline one neighbour
// is another surface, and the first form of this pass, which took the
// change across whichever pixel the hardware paired this one with, got a
// normal there that belonged to neither, and a fringe that moved with the
// figure (seen in play 2026-10-11).
float3 DaoAlong(float3 here, float3 before, float3 after)
{
    const float3 back = here - before;
    const float3 on = after - here;
    return abs(back.z) < abs(on.z) ? back : on;
}

// 1 where a point is open, towards 0 where it is closed in. `pixel` is the
// point's place on the screen.
float4 main(float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float aspect = texel.y / texel.x;
    const float2 across = float2(texel.x, 0);
    const float2 down = float2(0, texel.y);
    const float3 here = DaoViewPoint(uv, aspect);
    float3 normal = normalize(cross(DaoAlong(here, DaoViewPoint(uv - across, aspect), DaoViewPoint(uv + across, aspect)),
                                    DaoAlong(here, DaoViewPoint(uv - down, aspect), DaoViewPoint(uv + down, aspect))));
    normal = normal.z > 0 ? -normal : normal;
    // The radius, no more than kDaoAoMostOfScreen of the screen's height: seen
    // from a metre, as a face is in a conversation, 0.7 m spans most of the
    // picture, and a face was shaded a fifth darker by its own shoulders and
    // hair (seen 2026-10-11). Held to a part of the screen, the radius about
    // a near thing is small, and what is shaded there is its creases.
    const float radius = min(fDaoOcclusionRadius, kDaoAoMostOfScreen * 2 * kDaoAoHalfFov * here.z);
    const float reach = radius / (2 * kDaoAoHalfFov * here.z); // the radius as a part of the screen's height
    const float turn = 6.2831853 * frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    float closed = 0;
    for (int i = 0; i < kDaoAoTaps; i++)
    {
        const float along = (i + 0.5) / kDaoAoTaps;
        const float angle = turn + i * 2.3999632; // the golden angle: a spiral
        const float2 offset = float2(cos(angle) / aspect, sin(angle)) * reach * sqrt(along);
        const float3 to = DaoViewPoint(uv + offset, aspect) - here;
        const float distance2 = dot(to, to);
        const float rise = max(dot(to, normal) - 0.004 - 0.001 * here.z, 0);
        closed += distance2 < radius * radius ? rise / (distance2 + 0.0004) : 0;
    }
    float open = saturate(1 - fDaoOcclusion * closed * radius / kDaoAoTaps);
    // Too far for the radius to span two pixels: nothing to tell there.
    open = reach < 2 * texel.y ? 1 : open;
    open = lerp(open, 1, saturate(here.z / kDaoAoFar));
    return float4(open, 1, 0, 1);
}
