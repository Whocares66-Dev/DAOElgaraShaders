// What a material is by its names, and by its names and its textures together
// (issue 19). The tables are data (data/material_names.txt, shipped beside the
// plugin as materials.names); what is here is the order they are asked in.
// Their patterns are regular expressions as Python writes them, which PCRE2
// reads the same. Nothing here touches the game or the process.
//
// HOW A MATERIAL IS CLASSED. By what it is called first: its own name, its
// diffuse texture's name and the project it was exported from (a comment in
// each .mao names it: ARM_MAS, ARM_LGT, CTH_COM, Prop_Kitchen ...), through
// the tables (data/material_names.txt). The footstep sound a level material
// carries is used only where the names say nothing. A name can say `metal`
// outright (an anvil, a chain). Where it says only that the thing has metal
// in it (armour, a weapon, a shield, a helmet, a suit on a stand, a heap of
// coin) the textures say how much, by the rule for a texel: nearly all of it
// and it is `metal`, some and it is `mixed`, none and it is what else its
// family is made of (leather, wood). The textures never make a metal of a
// material whose name does not allow one: a level's and a prop's gloss is one
// flat value and its painted mirror is as bright on stone as on iron, so
// paint alone cannot tell them.
//
// THE RULE FOR A TEXEL is TextureRule.cpp's.
//
// WHAT THIS CANNOT SEE. A shield's heraldry is not in its textures:
// Weapon.mat blends a heraldry map (mml_tHeraldryMap, set per item; the h_*
// material objects name no texture) over the shield at run time, so a steel
// shield's map says steel under the paint and the shader must take the
// paint's share off its metalness. A tint (mml_tTintMask and an item's
// colours) is likewise the game's at run time: the rule reads the diffuse as
// it is stored, grey under a tint of gold or of dragonbone's red. The
// add-ins' own material objects are in enciphered archives (ERF V2.2), which
// only the running game reads (tools/dump_addins.py).
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace materials
{

// What a name may say besides a class: that the textures say how much of the
// thing is metal (the tool's PAINT).
inline constexpr std::string_view kPaint = "paint";

// A material object (<name>.mao) as its file says it: its definition, its
// textures by slot, the project it was exported from, its footstep sound.
struct MaterialObject
{
    std::string name;                                          // lower case, no extension
    std::string definition;                                    // lower case: "character.mat"
    std::string project;                                       // as written: "ARM_MAS.matproj"
    std::vector<std::pair<std::string, std::string>> textures; // slot to resource, lower case
    std::optional<int> sound;

    [[nodiscard]] std::string Texture(const std::string &slot) const
    {
        for (const auto &[held, resource] : textures)
        {
            if (held == slot)
            {
                return resource;
            }
        }
        return {};
    }
    // Of the six solid, lit definitions the table classes.
    [[nodiscard]] bool Solid() const;
    // A suit of plate's: its project is ARM_MAS, ARM_HVY or ARM_DWP.
    [[nodiscard]] bool Plate() const;
};

// A material object's file read: nullopt where it is not one.
std::optional<MaterialObject> ReadMaterialObject(std::string_view name, std::string_view text);

// What the names say: a class, or kPaint with the class it falls back to.
struct Verdict
{
    std::string cls;
    std::string fallback; // empty where none
};

class Names
{
  public:
    Names();
    ~Names();
    Names(const Names &) = delete;
    Names &operator=(const Names &) = delete;
    Names(Names &&) = delete;
    Names &operator=(Names &&) = delete;

    // Reads materials.names's text into these. False, and a line in `error`,
    // where a line does not parse or a pattern does not compile.
    bool Parse(std::string_view text, std::string &error);

    // by_name: the verdict of the material's names alone.
    [[nodiscard]] Verdict ByName(const MaterialObject &material) const;

    // For a clear solid's class (glass, crystal, gem, ice), what it mirrors
    // face on and the power it shines by, as the tool writes them into a
    // material's lines; nullopt for any other class.
    [[nodiscard]] std::optional<std::pair<std::string, std::string>> Clear(const std::string &cls) const;

  private:
    struct Pattern;
    struct Row;
    bool ParseRow(const std::vector<std::string> &fields, std::string &error);
    [[nodiscard]] Verdict ByLevel(const std::string &name, const std::string &stem, std::string_view text,
                                  const MaterialObject &material) const;
    [[nodiscard]] const Row *LookUp(const std::string &table, std::string_view text) const;
    [[nodiscard]] Verdict ByThings(const std::string &name, std::string_view text,
                                   const std::optional<int> &sound) const;

    std::unordered_map<std::string, std::vector<Row>> tables_;
    std::vector<std::pair<std::string, std::vector<std::string>>> tilesets_;
    std::unordered_map<int, std::string> sounds_;
    std::unordered_map<std::string, std::string> clear_;
    std::string clearPower_;
    std::unique_ptr<Pattern> wielded_;
    std::unique_ptr<Pattern> wornTexture_;
};

// settle: the class of a material from its verdict and, where the names said
// kPaint, the rule's share of metal (nullopt where it had no textures to
// ask) and whether its map came out as specks.
std::string Settle(const Verdict &verdict, std::optional<double> share, bool ragged);

} // namespace materials
