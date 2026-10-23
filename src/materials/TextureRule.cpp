// The rule for which texels of a material are metal, and the measure of a
// normal map's roughness. Written in Python first (tools/material_table.py,
// 2026-10-12) and ported here, checked against it over every material of the
// game before it was taken out (2026-10-15: 2632 of 2632 the same, the shares
// at most 0.0004 apart); the names of its parts there are kept beside each
// function here.
#include "materials/TextureRule.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <utility>

namespace materials
{

namespace
{

// The rule for a texel. All of it is here, in C++ since 2026-10-15; no material has a number of its own.
//
// Measured over the game's armour, weapons and shields, 2026-10-12 (the mirror is the
// specular mask's brightest channel, the surface's own colour the diffuse's):
//   plate, blades, rims, bosses, buckles and studs are painted with a mirror of 0.2 to 0.9
//   and a gloss of 0.15 to 0.6, and their mirror is a grey copy of their own colour, 0.5
//   to 1.5 times as bright (massive plate's median 0.7 to 0.9, a kite shield's 0.67);
//   leather, cloth, wood and skin have a mirror under 0.15 and a gloss under 0.1 nearly
//   everywhere, and a mirror a tenth to two fifths of their own colour;
//   the exceptions are sheens. Gold and bronze have a mirror tinted their own hue. Some
//   leather suits (pm_arm_lgta, lgtb, pf_arm_lgtc) have a BLUE mirror as bright as mail's
//   (0.22, with a gloss of 0.12) over brown or grey leather, its saturation 0.13 to 0.5;
//   steel's mirror is grey (0.02), but a cold steel's can be as blue as 0.3 (a round
//   shield's iron rim, 0.17), and that has a gloss of 0.2 and a mirror of 0.4.
// a metal mirrors brightly
constexpr float kMirrorMin = 0.16F;
// and is glossy (a power of 26 and up, of the game's 1 to 256)
constexpr float kGlossMin = 0.10F;
// its mirror is no dimmer than this share of its own colour
constexpr float kMirrorOfOwn = 0.35F;
// a saturation from which a surface's own colour is a colour, not a grey
constexpr float kColoured = 0.30F;
// a mirror's saturation from which it has a tint
constexpr float kTinted = 0.12F;
// and from which it is a colour of its own
constexpr float kSheen = 0.30F;
// the gloss and the mirror from which a mirror tinted cool is cold steel's,
constexpr float kGlossSteel = 0.16F;
// not a sheen
constexpr float kMirrorSteel = 0.30F;
// degrees between a surface's hue and its mirror's that make them two colours
constexpr float kHueApart = 45.0F;
// the hues, in degrees, of copper, bronze, brass and gold, from
constexpr float kWarmFrom = 15.0F;
// to
constexpr float kWarmTo = 65.0F;
// a colour darker than this has no hue to speak of
constexpr float kSeen = 0.12F;
// a texel black in both textures is the atlas's unused space
constexpr float kUsed = 0.04F;
// a texel past the least mirror and gloss by this factor is metal with no neighbour's say
constexpr float kSure = 2.0F;

// A specular mask past this side is a level's atlas, asked at a quarter.
constexpr std::uint32_t kAtlasSide = 1024;

enum class Ask
{
    Plain,
    Surely,
    Shaded,
};

struct Hue
{
    float hue = 0;        // degrees
    float saturation = 0; // of the brightest channel
    float top = 0;        // the brightest channel
};

// hue_saturation: numpy's order of the cases, red first where two channels tie.
Hue HueOf(float red, float green, float blue)
{
    const float top = std::max({red, green, blue});
    const float low = std::min({red, green, blue});
    const float spread = std::max(top - low, 1e-6F);
    float hue = 0;
    if (top == red)
    {
        hue = std::fmod((green - blue) / spread, 6.0F);
        if (hue < 0)
        {
            hue += 6.0F;
        }
    }
    else if (top == green)
    {
        hue = ((blue - red) / spread) + 2;
    }
    else
    {
        hue = ((red - green) / spread) + 4;
    }
    return {hue * 60, (top - low) / std::max(top, 1e-4F), top};
}

float PythonMod(float a, float b)
{
    const float r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}

// metal_texels for one texel: the surface's colour, the mirror's, the gloss.
//
// Which texels are metal: a mask, from the diffuse colour (h, w, 3), the
// specular mask's colour (h, w, 3) and its gloss (h, w), all 0 to 1 and of
// one size.
//
// A texel is metal where ALL of these hold:
//   1. it mirrors brightly: its mirror is MIRROR_MIN or more;
//   2. it is glossy: GLOSS_MIN or more;
//   3. its mirror is as bright as its own colour (MIRROR_OF_OWN of it or more): a metal
//      was painted with a mirror copied from its colour, a cloth or a bone with a dim
//      one under a bright colour. A dark surface with a bright mirror passes, which is
//      what a metal is;
//   4. its mirror is its own colour:
//      where the surface is coloured, the mirror is tinted the same hue (gold mirrors
//      gold) -- a paint, a dye, a leather or a wood mirrors white, or another colour.
//      But brass studs and gilt trim are painted yellow under a WHITE mirror, so a
//      surface of a warm colour (WARM: orange to yellow, the hues of copper, bronze,
//      brass and gold) with a grey mirror is metal too where that mirror is bright and
//      glossy twice over (SURE): as no leather's or wood's is;
//      where the surface is grey or too dark to have a hue, the mirror is grey, or
//      tinted warm, or tinted cool (green to violet) only a little (under SHEEN) and
//      then with a steel's gloss and brightness (GLOSS_STEEL, MIRROR_STEEL): a cool
//      tint on a dull or a dim mirror is the sheen painted on leather, and no metal's
//      mirror is blue outright.
// Why not gloss and mirror alone: they pass painted heraldry, dyed cloth with a satin
// highlight and oiled leather, which were the bugs.
//
// With surely, the same asked twice as hard: SURE times the least mirror and
// the least gloss, and no cool tint at all. That is a stud, a rivet, a
// buckle: a white dot on a black mask, which needs no neighbour to be
// believed (metal_map).
//
// With shaded, the same asked half as hard of the mirror and the gloss (1 /
// SURE of the least; what it is beside its own colour, and its hue, as ever):
// a metal with shade painted over it, which is believed only where it joins
// metal (metal_map).
bool IsMetal(const float *own, const float *mirror, float gloss, Ask ask)
{
    const Hue surface = HueOf(own[0], own[1], own[2]);
    const Hue shine = HueOf(mirror[0], mirror[1], mirror[2]);
    const bool coloured = surface.saturation >= kColoured && surface.top >= kSeen;
    const float apart = std::abs(PythonMod(surface.hue - shine.hue + 180, 360) - 180);
    const bool sameHue = shine.saturation >= kTinted && apart <= kHueApart;
    const bool cool = shine.saturation >= kTinted && shine.hue > 65 && shine.hue < 345;
    const bool sheen = cool && (shine.saturation >= kSheen || gloss < kGlossSteel || shine.top < kMirrorSteel);
    const bool twice = shine.top >= kSure * kMirrorMin && gloss >= kSure * kGlossMin;
    const bool gilt = surface.hue >= kWarmFrom && surface.hue <= kWarmTo && shine.saturation < kTinted && twice;
    // In plate's second look (Shaded) a gray surface's cool tint is not a
    // leather's sheen: what is asked there joins plate already found, and the
    // templars' plate is painted with a violet mirror (0.15 of saturation, a
    // gloss of 0.18) that the sheen's test left matte beside their metal
    // gauntlets (issue 32).
    const bool tintAllowed = ask == Ask::Shaded;
    const bool ownColour =
        coloured ? (sameHue || gilt) : (tintAllowed || !(ask == Ask::Surely ? cool : sheen));
    float least = 1.0F;
    if (ask == Ask::Surely)
    {
        least = kSure;
    }
    else if (ask == Ask::Shaded)
    {
        least = 1.0F / kSure;
    }
    return shine.top >= least * kMirrorMin && gloss >= least * kGlossMin && shine.top >= kMirrorOfOwn * surface.top &&
           ownColour;
}

// One channel of a picture, or the first `count` channels of it.
Picture Channels(const Picture &picture, std::uint32_t first, std::uint32_t count)
{
    Picture out{picture.width, picture.height, count, {}};
    out.values.resize(static_cast<std::size_t>(picture.width) * picture.height * count);
    for (std::size_t i = 0; i < static_cast<std::size_t>(picture.width) * picture.height; ++i)
    {
        for (std::uint32_t c = 0; c < count; ++c)
        {
            out.values[(i * count) + c] = picture.values[(i * picture.channels) + first + c];
        }
    }
    return out;
}

// box: each texel's mean over the square of that odd side about it, the edge
// repeated.
Picture Box(const Picture &picture, std::uint32_t side)
{
    const auto half = static_cast<std::int64_t>(side / 2);
    const auto width = static_cast<std::int64_t>(picture.width);
    const auto height = static_cast<std::int64_t>(picture.height);
    Picture out{picture.width, picture.height, picture.channels, {}};
    out.values.assign(picture.values.size(), 0.0F);
    const float count = static_cast<float>(side) * static_cast<float>(side);
    for (std::int64_t y = 0; y < height; ++y)
    {
        for (std::int64_t x = 0; x < width; ++x)
        {
            for (std::uint32_t c = 0; c < picture.channels; ++c)
            {
                float sum = 0;
                for (std::int64_t dy = -half; dy <= half; ++dy)
                {
                    const auto yy = static_cast<std::uint32_t>(std::clamp(y + dy, std::int64_t{0}, height - 1));
                    for (std::int64_t dx = -half; dx <= half; ++dx)
                    {
                        const auto xx = static_cast<std::uint32_t>(std::clamp(x + dx, std::int64_t{0}, width - 1));
                        sum += picture.At(xx, yy, c);
                    }
                }
                out.values[(((static_cast<std::size_t>(y) * picture.width) + static_cast<std::size_t>(x)) *
                            picture.channels) +
                           c] = sum / count;
            }
        }
    }
    return out;
}

// The weights Pillow's resampler gives each output texel along one side, for
// its box filter (libImaging/Resample.c, precompute_coeffs): the input texels
// whose middles fall within the output texel's span, all alike.
struct Coefficients
{
    std::vector<std::uint32_t> first;
    std::vector<std::vector<double>> weights;
};

Coefficients Coefficients1D(std::uint32_t inSize, std::uint32_t outSize)
{
    const double scale = static_cast<double>(inSize) / outSize;
    const double filterScale = std::max(scale, 1.0);
    const double support = 0.5 * filterScale;
    Coefficients out;
    out.first.resize(outSize);
    out.weights.resize(outSize);
    for (std::uint32_t xx = 0; xx < outSize; ++xx)
    {
        const double centre = (xx + 0.5) * scale;
        const double ss = 1.0 / filterScale;
        // Pillow truncates (int)(x + 0.5); the same as the floor here, a
        // negative start being held to 0 either way.
        auto xmin = static_cast<std::int64_t>(std::floor(centre - support + 0.5));
        xmin = std::max<std::int64_t>(xmin, 0);
        auto xmax = static_cast<std::int64_t>(std::floor(centre + support + 0.5));
        xmax = std::min<std::int64_t>(xmax, inSize) - xmin;
        std::vector<double> k(static_cast<std::size_t>(std::max<std::int64_t>(xmax, 0)));
        double total = 0;
        for (std::size_t x = 0; x < k.size(); ++x)
        {
            const double at = (static_cast<double>(x) + static_cast<double>(xmin) - centre + 0.5) * ss;
            k[x] = (at > -0.5 && at <= 0.5) ? 1.0 : 0.0;
            total += k[x];
        }
        if (total != 0)
        {
            for (auto &w : k)
            {
                w /= total;
            }
        }
        out.first[xx] = static_cast<std::uint32_t>(xmin);
        out.weights[xx] = std::move(k);
    }
    return out;
}

// metal_texels over a picture: the surface's colour, the mirror's, the gloss,
// each already boxed.
std::vector<std::uint8_t> MetalTexels(const Picture &own, const Picture &mirror, const Picture &gloss, Ask ask)
{
    const std::size_t count = static_cast<std::size_t>(own.width) * own.height;
    std::vector<std::uint8_t> out(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        out[i] = IsMetal(&own.values[i * 3], &mirror.values[i * 3], gloss.values[i], ask) ? 1 : 0;
    }
    return out;
}

// joined: `seeds` and every texel of `may` joined to one of them, edge to
// edge, through texels of `may`.
std::vector<std::uint8_t> Joined(std::vector<std::uint8_t> seeds, const std::vector<std::uint8_t> &may,
                                 std::uint32_t width, std::uint32_t height)
{
    std::deque<std::size_t> open;
    for (std::size_t i = 0; i < seeds.size(); ++i)
    {
        if (seeds[i] != 0)
        {
            open.push_back(i);
        }
    }
    const auto visit = [&](std::size_t i) {
        if (seeds[i] == 0 && may[i] != 0)
        {
            seeds[i] = 1;
            open.push_back(i);
        }
    };
    while (!open.empty())
    {
        const std::size_t i = open.front();
        open.pop_front();
        const std::size_t x = i % width;
        const std::size_t y = i / width;
        if (x > 0)
        {
            visit(i - 1);
        }
        if (x + 1 < width)
        {
            visit(i + 1);
        }
        if (y > 0)
        {
            visit(i - width);
        }
        if (y + 1 < height)
        {
            visit(i + width);
        }
    }
    return seeds;
}

Picture FromMask(const std::vector<std::uint8_t> &mask, std::uint32_t width, std::uint32_t height)
{
    Picture out{width, height, 1, {}};
    out.values.assign(mask.begin(), mask.end());
    return out;
}

} // namespace

bool IsMetalTexel(const std::array<float, 3> &own, const std::array<float, 3> &mirror, float gloss, bool surely)
{
    return IsMetal(own.data(), mirror.data(), gloss, surely ? Ask::Surely : Ask::Plain);
}

Picture FromRgba8(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba)
{
    Picture out{width, height, 4, {}};
    out.values.resize(rgba.size());
    std::ranges::transform(rgba, out.values.begin(), [](std::uint8_t v) { return static_cast<float>(v) / 255.0F; });
    return out;
}

Picture Resized(const Picture &picture, std::uint32_t width, std::uint32_t height)
{
    if (picture.width == width && picture.height == height)
    {
        return picture;
    }
    // Across first, then down, as Pillow does; in doubles, rounded to floats
    // between the passes as its 'F' pictures are.
    const Coefficients across = Coefficients1D(picture.width, width);
    Picture wide{width, picture.height, picture.channels, {}};
    wide.values.resize(static_cast<std::size_t>(width) * picture.height * picture.channels);
    for (std::uint32_t y = 0; y < picture.height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            for (std::uint32_t c = 0; c < picture.channels; ++c)
            {
                double sum = 0;
                for (std::size_t k = 0; k < across.weights[x].size(); ++k)
                {
                    sum += picture.At(across.first[x] + static_cast<std::uint32_t>(k), y, c) * across.weights[x][k];
                }
                wide.values[(((static_cast<std::size_t>(y) * width) + x) * picture.channels) + c] =
                    static_cast<float>(sum);
            }
        }
    }
    const Coefficients down = Coefficients1D(picture.height, height);
    Picture out{width, height, picture.channels, {}};
    out.values.resize(static_cast<std::size_t>(width) * height * picture.channels);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            for (std::uint32_t c = 0; c < picture.channels; ++c)
            {
                double sum = 0;
                for (std::size_t k = 0; k < down.weights[y].size(); ++k)
                {
                    sum += wide.At(x, down.first[y] + static_cast<std::uint32_t>(k), c) * down.weights[y][k];
                }
                out.values[(((static_cast<std::size_t>(y) * width) + x) * picture.channels) + c] =
                    static_cast<float>(sum);
            }
        }
    }
    return out;
}

bool Ragged(const Picture &metal)
{
    std::size_t some = 0;
    std::size_t edge = 0;
    for (const float v : metal.values)
    {
        if (v > 0.15F)
        {
            ++some;
            edge += v < 0.85F ? 1 : 0;
        }
    }
    return some != 0 && static_cast<double>(edge) / static_cast<double>(some) > 0.6;
}

std::array<std::uint32_t, 2> MapSize(std::uint32_t width, std::uint32_t height)
{
    width = std::max(4U, width / 4);
    height = std::max(4U, height / 4);
    while (std::max(width, height) > 256)
    {
        width = std::max(4U, width / 2);
        height = std::max(4U, height / 2);
    }
    return {width, height};
}

// metal_map:
//
// The metalness of a material from its diffuse and its specular mask (h, w, 4
// each, 0 to 1): (map, share). The map is at map_size, 0 to 1; the share is
// of the texels in use. The rule is asked at the specular mask's own size, of
// colours each averaged with its neighbours (DXT's noise is a texel wide). A
// texel that passes then goes with the majority of its neighbourhood, a
// square about a 128th of the texture across (3 texels of a mask of 256, 5 of
// 512, 9 of 1024), which takes out the specks of a paint that only just
// passes and fills the pinholes of one that only just fails; but a texel that
// passes twice over (surely) stands whatever is about it, or the majority
// would take every stud off studded leather (it did, 2026-10-12). The mask is
// then averaged down to the map, which is all the blur there is: an edge is
// one map texel wide, and a stud smaller than a map texel is a grey one, not
// lost.
//
// And shade is not a material. The game's textures have shade painted into
// them, colour, mirror and gloss alike: a helm's skull is dark where it goes
// under its brow plate. By the least mirror and gloss alone the shaded half
// of that skull was no metal, drawn matte and pale beside the rest's dark
// mirror, with an edge where the paint's shade began, and in play the edge
// read as the helm's plume seen through it (the user, 2026-10-12, a fifth
// time). So what passes for metal but for being dim (metal_texels' `shaded`)
// is metal where it joins metal that passes outright, through texels like
// itself: the brush's shade leads back to the lit metal it was laid on. A dim
// thing that touches no metal is what it was, a strap, a cloth, the dark
// between plates.
//
// Only with `plate`, in a suit of plate (is_plate). Shade cannot be told from
// a dark thing by the textures: tried for every material, the same rule made
// metal of a leather boot's dark glossy hide where it met its gilt trim, and
// of a shield's dark ground. In plate what is dim, glossy and joined to plate
// is plate. The share is of the rule without it, so no material's class moves
// by it.
MetalMap MakeMetalMap(const Picture &diffuse, const Picture &specularIn, bool plate)
{
    std::uint32_t width = specularIn.width;
    std::uint32_t height = specularIn.height;
    Picture specular = specularIn;
    if (std::max(width, height) > kAtlasSide)
    {
        width /= 4;
        height /= 4;
        specular = Resized(specular, width, height);
    }
    const Picture own = Resized(Channels(diffuse, 0, 3), width, height);
    const Picture mirror = Channels(specular, 0, 3);
    const Picture gloss = Channels(specular, 3, 1);
    const std::size_t count = static_cast<std::size_t>(width) * height;

    std::vector<std::uint8_t> used(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const float ownTop = std::max({own.values[i * 3], own.values[(i * 3) + 1], own.values[(i * 3) + 2]});
        const float mirrorTop =
            std::max({mirror.values[i * 3], mirror.values[(i * 3) + 1], mirror.values[(i * 3) + 2]});
        used[i] = (ownTop > kUsed || mirrorTop > kUsed) ? 1 : 0;
    }
    const Picture ownBox = Box(own, 3);
    const Picture mirrorBox = Box(mirror, 3);
    const Picture glossBox = Box(gloss, 3);

    // A texel that passes goes with the majority of a square about it of a
    // 128th of the texture across (odd, 3 at the least); one that passes
    // twice over stands whatever is about it.
    const std::uint32_t side = std::max(3U, ((std::max(width, height) + 127) / 128) | 1U);
    const Picture majority = Box(FromMask(MetalTexels(ownBox, mirrorBox, glossBox, Ask::Plain), width, height), side);
    const std::vector<std::uint8_t> surely = MetalTexels(ownBox, mirrorBox, glossBox, Ask::Surely);
    std::vector<std::uint8_t> metal(count);
    std::size_t inUse = 0;
    std::size_t metalInUse = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        metal[i] = (majority.values[i] > 0.5F || surely[i] != 0) ? 1 : 0;
        if (used[i] != 0)
        {
            ++inUse;
            metalInUse += metal[i];
        }
    }
    const double share = inUse == 0 ? 0.0 : static_cast<double>(metalInUse) / static_cast<double>(inUse);
    if (plate)
    {
        std::vector<std::uint8_t> shaded = MetalTexels(ownBox, mirrorBox, glossBox, Ask::Shaded);
        for (std::size_t i = 0; i < count; ++i)
        {
            shaded[i] = (shaded[i] != 0 && used[i] != 0) ? 1 : 0;
        }
        metal = Joined(std::move(metal), shaded, width, height);
    }
    const auto size = MapSize(width, height);
    return {Resized(FromMask(metal, width, height), size[0], size[1]), share};
}

// bumps:
//
// A normal map's width and height, and how far apart its normals are over
// patches of 2, 4, ... 64 texels across, in the mean over the map: for each,
// twice the larger of the two spreads (the larger eigenvalue of the
// covariance) of the normals' x and y within a patch, which is what a pixel
// covering such a patch adds to the square of its lobe's width (the shader's
// DaoBumps; Tokuyoshi and Kaplanyan 2019 take the same of a surface's
// turning). The game keeps a normal's x in the alpha and its y in the green,
// each from -1 to 1 over the byte (static1.psh, read 2026-10-12). A map
// smaller than a patch has the measure of the largest patch it holds.
Bumps MeasureBumps(const Picture &normalMap)
{
    Bumps out{normalMap.width, normalMap.height, {}};
    for (std::size_t patch = 1; patch <= out.spreads.size(); ++patch)
    {
        const std::uint32_t side = 1U << patch;
        const std::uint32_t down = normalMap.height / side;
        const std::uint32_t across = normalMap.width / side;
        if (down == 0 || across == 0)
        {
            out.spreads[patch - 1] = patch > 1 ? out.spreads[patch - 2] : 0.0;
            continue;
        }
        double total = 0;
        const double n = static_cast<double>(side) * side;
        for (std::uint32_t py = 0; py < down; ++py)
        {
            for (std::uint32_t px = 0; px < across; ++px)
            {
                double sx = 0;
                double sy = 0;
                double sxx = 0;
                double syy = 0;
                double sxy = 0;
                for (std::uint32_t y = py * side; y < (py + 1) * side; ++y)
                {
                    for (std::uint32_t x = px * side; x < (px + 1) * side; ++x)
                    {
                        const double nx = (static_cast<double>(normalMap.At(x, y, 3)) * 2) - 1;
                        const double ny = (static_cast<double>(normalMap.At(x, y, 1)) * 2) - 1;
                        sx += nx;
                        sy += ny;
                        sxx += nx * nx;
                        syy += ny * ny;
                        sxy += nx * ny;
                    }
                }
                const double mx = sx / n;
                const double my = sy / n;
                const double xx = (sxx / n) - (mx * mx);
                const double yy = (syy / n) - (my * my);
                const double xy = (sxy / n) - (mx * my);
                total += ((xx + yy) / 2) + std::sqrt((((xx - yy) / 2) * ((xx - yy) / 2)) + (xy * xy));
            }
        }
        out.spreads[patch - 1] = 2 * total / (static_cast<double>(down) * across);
    }
    return out;
}

} // namespace materials
