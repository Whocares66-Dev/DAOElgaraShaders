// A held piece's metal map made from its parts (issue 24): its texture's
// islands (Islands.h), each metal or not as a whole, by the item's kind or the
// texel rule's majority over it, and corrected by hand where those are wrong
// (data/material_parts.ini, read off review sheets). A weapon's blade, grip and
// guard, a shield's face and straps, a bow's limbs are islands of their own,
// so a whole island is the right unit; a worn piece's mesh is sewn in one
// island across leather and plate, so armor keeps the texel rule's map.
#pragma once

#include "materials/Islands.h"
#include "materials/Kind.h"
#include "materials/TextureRule.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace materials
{

// The corrections, by material object: each a word as data/material_parts.ini
// has it, in its order ("0,3" those islands not metal, "+2" metal, "~1" the
// texel rule's texels, "=m" or "=n" all).
using PartsFixes = std::map<std::string, std::vector<std::string>>;

// The corrections file read; an unreadable line is left out and named in `errors`.
PartsFixes ReadParts(const std::filesystem::path &file, std::vector<std::string> &errors);

// Each island's share of metal texels in `texels` (the texel rule's map at the
// islands' size, or null: none), 0 for an island that covers no texel.
std::vector<double> IslandShares(const Islands &islands, const Picture *texels);

// An island's default before the corrections: metal where the item's kind is,
// else where at least half its texels are.
bool DefaultMetal(Kind kind, double share);

// The map of a texture's islands at their size: the default for each island
// (metal where `kind` is metal, else where at least half its texels are metal
// in `texels`, which is the texel rule's map at the islands' size, or null),
// then `fixes` in their order. Texels no island covers are not metal.
Picture PartsMap(const Islands &islands, Kind kind, const Picture *texels, const std::vector<std::string> &fixes);

} // namespace materials
