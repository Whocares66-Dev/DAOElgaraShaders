// The lines a material object is given for its class (issue 19): that it is
// metal, its map, its normal map's measure, a clear solid's mirror. The table
// and a material the plugin works out itself are given them in one form.
// Nothing here touches the game or the process.
#pragma once

#include "materials/Names.h"
#include "materials/TextureRule.h"

#include <optional>
#include <string>
#include <string_view>

namespace materials
{

// The map's file for a specular mask (the tool's map_name): its s, _s or
// _spec taken off and a level of detail's mark kept, and _m.dds put on:
// pn_arm_masa_0s.dds -> pn_arm_masa_0_m.dds.
std::string MapName(std::string_view specular);

// What a material of class `cls` is given: for metal, that it is; for mixed,
// that it is and `mapFile`, its map (empty where it had no textures to make
// one of, and then no metal); for a clear solid, what it mirrors and how
// smooth it is; and for any class, its normal map's measure where it has one.
// Empty where it is given nothing.
std::string SaidFor(const std::string &cls, const Names &names, const std::string &mapFile,
                    const std::optional<Bumps> &bumps);

} // namespace materials
