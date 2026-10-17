// How strong an area's fills are on figures and props (build_shaders.py's
// with_fill_strength; the number is Outdoors.cpp's).
//
// The engine lights every figure of an area by three far-off lights it stands
// about the level, a kilometre and more of reach (Redcliffe by day: 1100 m, at
// 9 m up, 0.39, 0.50, 0.39 together beside a sun on figures of 0.64, 0.52,
// 0.33; the docks of Lake Calenhad by night: 450 m, at 65 m, two blue and one
// orange over the town, together brighter than the moon; read from a
// figure's registers 2026-10-16). Nearly as strong as the sun or the moon and
// from every side, they light a figure flat: the side away from the light is
// lit almost as well as the side towards it. Turned down, with the sun or the
// moon turned up, the light's way reads on a figure.
//
// They are not the sky: lit from above by their sum in their place, the party
// at the docks was lit more brightly than the moon could, in white
// (2026-10-16).

#ifndef DAO_FILLS_HLSL
#define DAO_FILLS_HLSL

// x: the fills' strength, 1 for vanilla's. Set by the plugin; 1 in a
// portrait and the inventory's figure, which are lit by lights of their own.
float4 daoFill : register(c217);

// A light reaching further than this is a fill: an area's are 450 m and more,
// a lamp's some metres to tens. A portrait's and the inventory's figure's own
// lights reach some 200 m, and are drawn while daoFill says 1.
static const float kDaoFillReach = 100;

// What one of a figure's point lights is multiplied by.
float DaoFillKept(float reach)
{
    return reach > kDaoFillReach ? daoFill.x : 1;
}

#endif
