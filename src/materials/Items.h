// What the game records of what its equipment is made of (issue 24): every
// item template of every campaign, the kind its base item says it is
// (Kind.h), the model its variation table gives it, and the material objects
// that model's meshes are drawn with. The game has no metalness of its own (no
// field says a texel or a material is metal, read 2026-10-15); a material
// object's class is worked out from its items' kind.
#pragma once

#include "materials/Kind.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace dao::core::install
{
class Index;
}

namespace materials
{

// Where the inventory reads its files.
struct Files
{
    // the campaigns' indexes, the main campaign's first
    std::vector<std::pair<std::string, const dao::core::install::Index *>> campaigns;
    // a file's bytes as a campaign takes it; nullopt where none can be read
    std::function<std::optional<std::vector<std::byte>>(const std::string &file, const std::string &campaign)> bytes;
    // a folder of files read out of the running game, or none
    std::filesystem::path readOut;
};

// An item template, and the model it is drawn with.
struct Item
{
    std::string name;                 // the template, no extension
    Kind kind = Kind::None;           // what its base item's kind says it is made of
    std::string model;                // ModelType, ModelSubType and ModelVariation: "hlf_gwra", or empty
    std::vector<std::string> objects; // the material objects its model's meshes are drawn with, sorted
};

struct Inventory
{
    std::vector<Item> items;
};

// Reads every campaign's items, tables and models.
Inventory TakeInventory(const Files &files);

// Each material object's kind: the one every item drawn with it agrees on, None where they differ.
std::map<std::string, Kind> KindsOf(const Inventory &inventory);

} // namespace materials
