// One material worked out from its own textures: its class, the lines it is
// given and its metal map, by the rule and names the table is made by. The
// table (Table.cpp) asks this of every material the install has, with the
// textures read from the install; the plugin asks it of a material the table
// does not know, a mod's, with the textures the running game hands it
// (MaterialLearn.cpp; issue 19, the user, 2026-10-15: "shouldn't we
// automatically detect & apply metallicity"). Nothing here touches the game
// or the process: the textures come in through `ImageOf`.
#pragma once

#include "materials/Names.h"
#include "materials/TextureRule.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace materials
{

// A texture by its resource name ("pn_arm_masa_0s.dds") as a picture, or
// nullopt where there is none or it does not decode.
using ImageOf = std::function<std::optional<Picture>(const std::string &resource)>;

// A texture file's bytes as a picture, or nullopt where they are no DDS.
std::optional<Picture> PictureOf(std::span<const std::byte> bytes);

// A metal map as the plugin is given it: one channel in a DXT1 DDS.
std::vector<std::byte> MapFile(const Picture &grey);

// The rule's map of a material and its share; nullopt where it has no
// diffuse and specular mask to ask.
std::optional<MetalMap> MapOf(const MaterialObject &object, const ImageOf &image);

// Its normal map's measure; nullopt where it has none to read.
std::optional<Bumps> BumpsOf(const MaterialObject &object, const ImageOf &image);

// A material's class and how it was reached: the share of metal the rule
// found and its map, where the names asked the textures.
struct Classed
{
    Verdict verdict;
    std::string cls;
    std::optional<double> share;
    std::optional<Picture> map;
};

Classed Classify(const MaterialObject &object, const Names &names, const ImageOf &image);

// What a material is given, worked out: its class, the lines (empty where
// it is given nothing), and its map's file and bytes where it is mixed and
// had textures to make one of.
struct Learned
{
    std::string cls;
    std::string said;
    std::string mapFile;
    std::vector<std::byte> map;
};

// A copy of a material the table does not know, worked out as the table
// works out one it does. `mapFile` names its map, where it has one; nullopt
// where the text is no solid material object.
std::optional<Learned> Learn(const std::string &name, std::string_view text, const Names &names, const ImageOf &image,
                             const std::string &mapFile);

} // namespace materials
