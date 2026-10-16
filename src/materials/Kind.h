// What a piece of equipment is made of, by the kind of item it is (issue 24):
// the game records no metalness, but its base items (bitm_base) say what each
// kind is and where it is worn, and that decides a piece's substance better
// than its material's tier, its sound class or its texels (scored over every
// equipment material object of every campaign, 2026-10-16: 12 exceptions in
// 267 where the texels decide the kinds that say nothing, against 16 to 54
// for the others). A metal kind is metal over its whole texture; a mixed one
// is split by its texels; the rest keep their names' and texels' say.
#pragma once

#include <cstdint>
#include <string_view>

namespace materials
{

enum class Kind : std::uint8_t
{
    None,    // the kind says nothing: shields, staves, bows, crossbows, or no item
    Metal,   // massive pieces; heavy helmets, gloves and boots; medium helmets; blades, axes, maces
    Mixed,   // medium and heavy chests, medium gloves and boots: plate or mail with leather
    Leather, // light pieces, quivers
    Cloth,   // clothing, robes, mage hoods
};

// A base item's columns that say what it is.
struct BaseItem
{
    std::int64_t type = 0;      // Type: 2 melee weapon, 3 shield, 4 worn, 5 staff, 6 bow, 7 ammunition
    std::int64_t armorType = 0; // ArmorType: 1 light, 2 medium, 3 heavy, 4 massive, 5 cloth
    std::int64_t slots = 0;     // EquippableSlots: 16 chest, 32 head, 64 feet, 128 hands
};

// What a base item's kind says its pieces are made of.
Kind KindOf(const BaseItem &base);

// The kind's name: "metal", "mixed", "leather", "cloth", or "" for None.
std::string_view NameOf(Kind kind);

} // namespace materials
