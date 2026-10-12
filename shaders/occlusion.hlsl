// How open each point of the picture is to the light around it, from the
// scene's depth alone: ambient occlusion, the second of the four passes
// Elgara Shaders adds to the game's colour-grade chain (build_shaders.py).
// occlusion_depth.hlsl writes each pixel's distance, this one the openness,
// occlusion_blur.hlsl smooths it across and then down, and the grade
// multiplies it into the picture.
//
// Ground-truth ambient occlusion (Jimenez, Wu, Pesce and Jarabo 2016,
// "Practical Real-Time Strategies for Accurate Indirect Occlusion"; the
// arithmetic as Intel's XeGTAO has it), asked for by name (the user,
// 2026-10-12: "implement gtao"). The hemisphere over a point is cut into
// slices through the eye; in each slice the pass walks out both ways along
// the screen and finds how high the scene rises, the two horizons; what of a
// cosine-weighted light comes in between them is known in closed form. That
// is what a point really sees of an even light, where the pass this replaces
// (Scalable Ambient Obscurance, McGuire et al. 2012) summed how much of the
// neighbourhood stood above the surface, which has no such meaning and
// darkened open creases as much as closed ones.
//
// smpDepth is occlusion_depth.hlsl's target, metres as one float, at sampler
// 1, which the pass's state reads unfiltered.

sampler2D smpDepth : register(s1);
// The picture, for its alpha alone: how much of the pixel's light is
// ambient, as its shader said (look.hlsl's DaoShare). It goes on in this
// pass's green, beside the openness in its red.
sampler2D smpScene : register(s2);

// Values of the chain (build_shaders.py's LOOK), set from graphics.ini and by
// the options screen's slider.
float fDaoOcclusion;       // how dark, 0 for none: twice this is the power the openness is raised to
float fDaoOcclusionRadius; // metres around a point that can shade it
// The picture's width and height on a plane one unit in front of the eye
// (Graphics.cpp's KeepLens, each frame), which the first form of this pass
// guessed.
float fDaoLensX;
float fDaoLensY;

static const float kDaoAoFar = 60.0; // metres from the camera at which it has faded out
// Four slices of three steps each way: 24 readings of the depth. There is no
// frame before this one to add to, as there is where this method is at home,
// so the slices are turned at every pixel and the blur after takes the grain.
static const int kDaoAoSlices = 4;
static const int kDaoAoSteps = 3;
// The most of the screen's height the radius may span.
static const float kDaoAoMostOfScreen = 0.2;
// Of the radius, the outer part over which a thing stops counting.
static const float kDaoAoFalloff = 0.615;
// The least of its light this pass leaves a point.
static const float kDaoAoLeast = 0.3;
static const float kDaoPi = 3.14159265;
static const float kDaoHalfPi = 1.57079633;

// Where a pixel is: x right, y up, z into the screen, in metres.
float3 DaoViewPoint(float2 uv)
{
    const float depth = tex2Dlod(smpDepth, float4(uv, 0, 0)).r;
    return float3((uv.x - 0.5) * fDaoLensX, (0.5 - uv.y) * fDaoLensY, 1) * depth;
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

// The cosine of how high the scene rises at `to` (from the point, in metres)
// against the way to the eye, or `low`, the surface's own, by how far off it
// is and whether it rises above the surface at all. `rise` is what a thing
// must stand above the surface by to count: the depth has steps of its own,
// and a floor seen aslant would shade itself by them.
//
// A thing that stands off towards the eye or away from it counts for less
// than one beside the point: its distance along the view is taken for more
// than it is (XeGTAO's compensation for thin occluders). The depth has only
// the front of each thing, and a plume's cards a hand's breadth behind a
// helm are, to this search, a wall as deep as the scene: the side of the
// helm that faced them went black, with the cards' stepped outline (the
// user, 2026-10-12: "cailan's helmet looks transparent for some reason";
// found on a frozen frame, where the patch went with this pass off and with
// nothing else, and the depth itself was sound: build_shaders.py's
// show-depth).
static const float kDaoAoThin = 2.0;

float DaoHorizon(float3 to, float3 view, float3 normal, float low, float radius, float rise)
{
    const float far = length(to);
    const float along = dot(to, view);
    const float range = kDaoAoFalloff * radius;
    float counts = saturate((radius - length(to + view * (along * kDaoAoThin))) / range);
    counts = dot(to, normal) > rise ? counts : 0;
    return lerp(low, dot(to / max(far, 1e-5), view), counts);
}

// The light that comes in between a slice's two horizons, of all a cosine-
// weighted light would bring it: `n` is the angle of the surface's normal in
// the slice from the way to the eye.
float DaoArc(float horizon, float n, float cosN)
{
    const float h = n + clamp(horizon - n, -kDaoHalfPi, kDaoHalfPi);
    return (cosN + 2 * h * sin(n) - cos(2 * h - n)) / 4;
}

// 1 where a point is open, towards 0 where it is closed in. `pixel` is the
// point's place on the screen.
float4 main(float2 uv : TEXCOORD0, float2 pixel : VPOS) : COLOR0
{
    const float2 texel = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    const float2 across = float2(texel.x, 0);
    const float2 down = float2(0, texel.y);
    const float3 here = DaoViewPoint(uv);
    float3 normal = normalize(cross(DaoAlong(here, DaoViewPoint(uv - across), DaoViewPoint(uv + across)),
                                    DaoAlong(here, DaoViewPoint(uv - down), DaoViewPoint(uv + down))));
    normal = normal.z > 0 ? -normal : normal;
    const float3 view = normalize(-here);
    // The radius, no more than kDaoAoMostOfScreen of the screen's height: seen
    // from a metre, as a face is in a conversation, 0.7 m spans most of the
    // picture, and a face was shaded a fifth darker by its own shoulders and
    // hair (seen 2026-10-11). Held to a part of the screen, the radius about
    // a near thing is small, and what is shaded there is its creases.
    const float radius = min(fDaoOcclusionRadius, kDaoAoMostOfScreen * fDaoLensY * here.z);
    const float reach = radius / (fDaoLensY * here.z); // the radius as a part of the screen's height
    const float rise = 0.004 + 0.001 * here.z;
    // Interleaved gradient noise (Jimenez 2014), twice: the slices' turn and where the steps fall.
    const float turn = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
    const float shift = frac(52.9829189 * frac(dot(pixel + 47.0, float2(0.06711056, 0.00583715))));
    float open = 0;
    for (int slice = 0; slice < kDaoAoSlices; slice++)
    {
        const float angle = (slice + turn) / kDaoAoSlices * kDaoPi;
        const float2 way = float2(cos(angle), sin(angle)); // on the plane before the eye, y up
        // The slice's plane holds the way to the eye and `way`; the normal's
        // part in it is what the slice's light falls on.
        const float3 flat = float3(way, 0) - dot(float3(way, 0), view) * view;
        const float3 axis = normalize(cross(flat, view));
        const float3 within = normal - axis * dot(normal, axis);
        const float share = length(within) + 1e-5;
        const float cosN = saturate(dot(within, view) / share);
        const float n = sign(dot(flat, within)) * acos(cosN);
        // The surface's own horizons, either way, are where the search starts.
        const float lowOn = cos(n + kDaoHalfPi);
        const float lowBack = cos(n - kDaoHalfPi);
        float on = lowOn;
        float back = lowBack;
        const float2 along = float2(way.x * fDaoLensY / fDaoLensX, -way.y) * reach;
        for (int step = 0; step < kDaoAoSteps; step++)
        {
            // Nearer the point more often than far: the square of an even walk, and never less than a pixel.
            const float part = (step + shift) / kDaoAoSteps;
            const float2 offset = along * max(part * part, 1.5 * texel.y / max(reach, 1e-5));
            on = max(on, DaoHorizon(DaoViewPoint(uv + offset) - here, view, normal, lowOn, radius, rise));
            back = max(back, DaoHorizon(DaoViewPoint(uv - offset) - here, view, normal, lowBack, radius, rise));
        }
        open += share * (DaoArc(-acos(clamp(back, -1, 1)), n, cosN) + DaoArc(acos(clamp(on, -1, 1)), n, cosN));
    }
    // And no point's ambient light is less by this pass than kDaoAoLeast:
    // where the search is wrong it is not to be black. (0.45 while the pass
    // shaded the lights' own light too; it shades the ambient share alone
    // since issue 8, and a wrong answer costs a lit surface little.)
    open = max(pow(saturate(open / kDaoAoSlices), 2 * fDaoOcclusion), kDaoAoLeast);
    // Too far for the radius to span two pixels: nothing to tell there.
    open = reach < 2 * texel.y ? 1 : open;
    open = lerp(open, 1, saturate(here.z / kDaoAoFar));
    return float4(open, saturate(tex2Dlod(smpScene, float4(uv, 0, 0)).a), 0, 1);
}
