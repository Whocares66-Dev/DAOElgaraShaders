// The materials' table (data/materials.ini) and what the plugin is given of
// it (materials.answers and the metal maps), made by the same rule and names
// the plugin works a material out by (TextureRule.h, Names.h, Said.h), so there
// is one of each (issue 19; the user, 2026-10-15: "Let's have one set of
// functionality"). Until then tools/material_table.py made them in Python.
// The game's files are read through DAOLib's install index, which knows
// which copy of a file the game takes, in each campaign the install has.
#pragma once

#include "materials/Names.h"
#include "materials/TextureRule.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace dao::core::install
{
class Index;
}

namespace materials
{

// One copy of a material object: its file's text and what it says.
struct Copy
{
    std::string text;
    std::uint64_t hash = 0;
    MaterialObject object;
    std::string where;    // the archive or folder it was read from
    std::string campaign; // the campaign it is the game's in: "Single Player", "DAO_PRC_EP_1"
};

// A material: the copy the main campaign takes, and every copy of its name
// whose bytes differ (a comment alone, another campaign's, an add-in's).
struct Material
{
    std::string name;
    Copy main;
    std::vector<Copy> copies; // the main one among them
};

// The game's solid materials and their textures.
class Catalogue
{
  public:
    // Reads the install's material objects, as each campaign it has (the main
    // one and those AddIns.xml in `documents` names an add-in extending) takes
    // them; and those of a folder of the add-ins' files read out of the running
    // game (tools/dump_addins.py), where the install has none readable of a
    // name.
    static std::unique_ptr<Catalogue> Load(const std::filesystem::path &game, const std::filesystem::path &documents,
                                           const std::filesystem::path &addins, std::string &error);
    ~Catalogue();
    Catalogue(const Catalogue &) = delete;
    Catalogue &operator=(const Catalogue &) = delete;
    Catalogue(Catalogue &&) = delete;
    Catalogue &operator=(Catalogue &&) = delete;

    [[nodiscard]] const std::map<std::string, Material> &Materials() const
    {
        return materials_;
    }
    // A texture as the game takes it in a campaign, or nullopt where there is
    // none or it does not decode.
    [[nodiscard]] std::optional<Picture> Image(const std::string &resource, const std::string &campaign) const;

    // The names of the files of a type (".mao") that some copy of is in an
    // enciphered archive, which only the running game reads.
    [[nodiscard]] std::vector<std::string> Enciphered(std::string_view extension) const;
    // Whether any campaign has a copy of a file it can read with no game.
    [[nodiscard]] bool Readable(const std::string &resource) const;

  private:
    Catalogue();
    // The add-ins' material objects read out of the running game, where the
    // install has none readable of a name.
    void AddReadOut();
    // A file's bytes as a campaign takes it, or another's, or the read-out
    // folder's.
    [[nodiscard]] std::optional<std::vector<std::byte>> Bytes(const std::string &resource,
                                                              const std::string &campaign) const;
    // By campaign, the main one first.
    std::vector<std::pair<std::string, std::unique_ptr<dao::core::install::Index>>> indexes_;
    std::filesystem::path addins_;
    std::map<std::string, Material> materials_;
};

// A material's class and how it was reached: the share of metal the rule
// found and its map, where the names asked the textures.
struct Classed
{
    Verdict verdict;
    std::string cls;
    std::optional<double> share;
    std::optional<Picture> map;
};

Classed Classify(const Catalogue &catalogue, const Names &names, const Copy &copy);

// data/materials.ini's lines: by hand, `name = class ; by hand ...`, kept as
// written by every later run; and the table's classes.
struct Hand
{
    std::string cls;
    std::string line;
};
std::map<std::string, Hand> ReadHands(const std::filesystem::path &ini);
std::map<std::string, std::string> ReadClasses(const std::filesystem::path &ini);

// The table made again: every material classed, a level of detail's copy
// (_l2, _l3) given its base's class, a hand's line over the lot; as the
// file's text. `progress` is told how far it has got.
std::string MakeTable(const Catalogue &catalogue, const Names &names, const std::map<std::string, Hand> &hands,
                      const std::function<void(std::size_t, std::size_t)> &progress);

// What the plugin is given of the table as it stands: the answers' text (a
// line a copy of every material object, by its bytes' hash) and the maps,
// by their files' names. A copy that says other things than the table's (an
// other campaign's) is classed by itself. `counts` says what was made.
struct Objects
{
    std::string answers;
    std::vector<std::pair<std::string, std::vector<std::byte>>> maps;
    std::vector<std::pair<std::string, std::size_t>> counts;
};
Objects MakeObjects(const Catalogue &catalogue, const Names &names, const std::map<std::string, std::string> &classes);

} // namespace materials
