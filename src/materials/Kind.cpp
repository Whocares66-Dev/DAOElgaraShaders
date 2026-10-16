// Kind.h has what this is for. Read from the game's bitm_base, 2026-10-16:
// Type, ArmorType and EquippableSlots of every base item that has a model.
// Boots - Heavy carry ArmorType 4 and Boots - Massive 3, swapped against
// every other piece; both are metal, so the swap changes nothing here.
#include "materials/Kind.h"

namespace materials
{
namespace
{

constexpr std::int64_t kMelee = 2;
constexpr std::int64_t kWorn = 4;
constexpr std::int64_t kAmmunition = 7;

constexpr std::int64_t kLight = 1;
constexpr std::int64_t kMedium = 2;
constexpr std::int64_t kHeavy = 3;
constexpr std::int64_t kMassive = 4;
constexpr std::int64_t kCloth = 5;

constexpr std::int64_t kChest = 16;
constexpr std::int64_t kHead = 32;

} // namespace

Kind KindOf(const BaseItem &base)
{
    if (base.type == kMelee)
    {
        return Kind::Metal; // blades, axes, maces, mauls
    }
    if (base.type == kAmmunition)
    {
        return Kind::Leather; // a quiver is what is drawn
    }
    if (base.type != kWorn)
    {
        return Kind::None; // shields, staves, bows: wood or metal, the kind does not say
    }
    switch (base.armorType)
    {
    case kCloth:
        return Kind::Cloth;
    case kLight:
        return Kind::Leather;
    case kMassive:
        return Kind::Metal;
    case kHeavy:
        // a heavy chest is plate, mail and leather together; heavy helmets, gloves and boots are plate
        return (base.slots & kChest) != 0 ? Kind::Mixed : Kind::Metal;
    case kMedium:
        // a medium helmet is a steel cap; a medium chest, gloves or boots are leather with plate or mail
        return (base.slots & kHead) != 0 ? Kind::Metal : Kind::Mixed;
    default:
        return Kind::None;
    }
}

std::string_view NameOf(Kind kind)
{
    switch (kind)
    {
    case Kind::Metal:
        return "metal";
    case Kind::Mixed:
        return "mixed";
    case Kind::Leather:
        return "leather";
    case Kind::Cloth:
        return "cloth";
    case Kind::None:
        break;
    }
    return "";
}

} // namespace materials
