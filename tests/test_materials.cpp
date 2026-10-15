// The materials' rule, names, lines and table (src/materials): what was
// tools/test_material_table.py until 2026-10-15, with the same cases. The
// names are read from data/material_names.txt, the file the plugin ships. The
// cases tagged [game] read the installed game (DAO_GAME_DIR, or GOG's) and
// pass with nothing asked where it is not there.
#include "materials/Answers.h"
#include "materials/Names.h"
#include "materials/Said.h"
#include "materials/Table.h"
#include "materials/TextureRule.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using materials::Picture;

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

const materials::Names &TheNames()
{
    static const auto names = [] {
        auto out = std::make_unique<materials::Names>();
        std::string error;
        if (!out->Parse(ReadText(ELGARA_NAMES_FILE), error))
        {
            FAIL(error);
        }
        return out;
    }();
    return *names;
}

materials::MaterialObject Object(std::string name, std::string definition, std::string diffuse = {},
                                 std::string project = {}, std::optional<int> sound = std::nullopt)
{
    materials::MaterialObject out;
    out.name = std::move(name);
    out.definition = std::move(definition);
    out.project = std::move(project);
    if (!diffuse.empty())
    {
        out.textures.emplace_back("mml_tDiffuse", std::move(diffuse));
    }
    out.sound = sound;
    return out;
}

std::string Verdict(std::string name, std::string definition, std::string diffuse = {}, std::string project = {},
                    std::optional<int> sound = std::nullopt)
{
    return TheNames()
        .ByName(Object(std::move(name), std::move(definition), std::move(diffuse), std::move(project), sound))
        .cls;
}

constexpr std::string_view kPaint = materials::kPaint;

} // namespace

TEST_CASE("the worn are classed by the code in their name", "[names]")
{
    CHECK(Verdict("pn_arm_masa", "character.mat", "pn_arm_masa_0d.dds", "ARM_MAS.matproj") == kPaint);
    CHECK(Verdict("pm_arm_lgtd_l2", "character.mat", "pm_arm_lgtd_0dl2.dds", "ARM_LGT.matproj") == kPaint);
    CHECK(Verdict("hm_arm_nuda", "character.mat", "hm_arm_nuda_0d.dds", "ARM_NUD.matproj") == "skin");
    CHECK(Verdict("pf_rob_appa", "character.mat", "pf_rob_appa_0d.dds", "ROB_APP.matproj") == "cloth");
    CHECK(Verdict("pm_cth_coma", "character.mat", "pm_cth_coma_0d.dds", "CTH_COM.matproj") == "cloth");
    CHECK(Verdict("pn_hlh_crn", "character.mat", "pn_hlh_crn_0d.dds") == "metal");
}

TEST_CASE("a fallback is what else the family is made of", "[names]")
{
    const auto &names = TheNames();
    CHECK(names.ByName(Object("pm_arm_lgtb", "character.mat", "pm_arm_lgtb_0d.dds")).fallback == "leather");
    CHECK(names.ByName(Object("w_shd_kita", "weapon.mat", "w_shd_kita_0d.dds")).fallback == "wood");
    CHECK(names.ByName(Object("w_lsw_ls01a_0", "weapon.mat", "w_lsw_ls01a_0d.dds")).fallback == "other");
}

TEST_CASE("creatures: bare bodies, and a head is never asked of the paint", "[names]")
{
    CHECK(Verdict("c_wolfa", "character.mat", "c_wolfa_0d.dds", "C_WOLF.matproj") == "skin");
    CHECK(Verdict("cn_golem", "character.mat", "cn_bdy_stga_0d.dds") == "stone");
    CHECK(Verdict("cn_bdy_glka", "character.mat", "cn_bdy_glka_0d.dds") == kPaint);
    // the rule marked horns, teeth and a wet face
    CHECK(Verdict("cn_hed_glka", "character.mat", "cn_hed_glka_0d.dds") == "skin");
    CHECK(Verdict("cn_hed_aoga", "character.mat", "cn_hed_aoga_0d.dds") == "skin");
}

TEST_CASE("weapons, and the definitions that say what a thing is", "[names]")
{
    CHECK(Verdict("w_lsw_ls01a_0", "weapon.mat", "w_lsw_ls01a_0d.dds") == kPaint);
    CHECK(Verdict("w_shd_srda", "weapon.mat", "w_shd_srda_0d.dds") == kPaint);
    CHECK(Verdict("w_stf_st01a_0", "weapon.mat", "w_stf_st01a_0d.dds") == "wood");
    CHECK(Verdict("w_qui_qu01", "weapon.mat", "w_qui_qu01a_0d.dds") == "leather");
    CHECK(Verdict("cn_lea_gra", "canopy.mat", "cn_lea_graa_0d.dds") == "foliage");
    CHECK(Verdict("flagking", "cloth.mat", "king_tent_flag.dds") == "cloth");
}

TEST_CASE("things, by the words in their names and their footsteps", "[names]")
{
    const std::string s = "static.mat";
    CHECK(Verdict("tti_master", s, "tti_master_d.dds", "TTI_Master.matproj", 0) == "stone");
    CHECK(Verdict("prp_tevbench1", s, "p_tevbench1_01d.dds", "Prop_Tevinter.matproj", 0) == "stone");
    CHECK(Verdict("prp_tevtable1", s, "p_tevtable1_01d.dds", "Prop_Tevinter.matproj", 0) == "stone");
    CHECK(Verdict("plc_benchmarble01", "prop.mat", "p_bench_marble_01d.dds") == "stone");
    CHECK(Verdict("fci_exteriorwall01", s, "fci_ewall1.dds", "FereldenCastleInterior.matproj", 5) == "stone");
    CHECK(Verdict("fci_endwoodbeam", s, "fci_endwoodbm.dds", "FereldenCastleInterior.matproj", 1) == "wood");
    CHECK(Verdict("benchlog_01", s, "p_bench_log_01d.dds", "Prop_Kitchen.matproj", 0) == "wood");
    CHECK(Verdict("redrug", s, "prp_rugred.dds", "Prop_Bedroom.matproj", 15) == "cloth");
    CHECK(Verdict("anvil_01", s, "p_anvil_01d.dds", "Prop_Other.matproj", 0) == "metal");
    CHECK(Verdict("plc_chainshackles01", "prop.mat", "p_chain_shackles_01d.dds") == "metal");
    CHECK(Verdict("prp_ivywall01", s, "prp_ivywall01_d.dds", "TerrainMaster.matproj", 0) == "foliage");
}

TEST_CASE("words inside words", "[names]")
{
    const std::string s = "static.mat";
    // the Anvil of the Void is a level, not an anvil; a butcher's board is not a herb; cracked is not a rack
    CHECK(Verdict("hro_anvil_wall", s, "hro_anvil_wall01d.dds", "hro_anvil.matproj", 0) == "stone");
    CHECK(Verdict("prp_butcherboard01", s, "p_butcher_boardd.dds", "OutsourcedProps01.matproj", 0) == "wood");
    CHECK(Verdict("fci_crackedstuccowall05", s, "fci_iwall4.dds", "FereldenCastleInterior.matproj", 5) == "stone");
    // an armour stand is a stand of planks (its texture seen 2026-10-12), not armour
    CHECK(Verdict("plc_armorstand", "prop.mat", "p_armor_stand_01d.dds", "Prop_Armory.matproj") == "wood");
}

TEST_CASE("a prop in a weapon's texture is asked as the weapon; no name is other", "[names]")
{
    CHECK(Verdict("plc_lsw_ls01a_0", "prop.mat", "w_lsw_ls01a_0d.dds", "Prop_Armory.matproj") == kPaint);
    CHECK(Verdict("prp_cline", "static.mat", "prp_cline0.dds", "Prop_Other.matproj", 0) == "other");
    CHECK(Verdict("h_aoredclifa", "static.mat", "", "HERALDRY.matproj", 0) == "other");
    // a metal footstep alone does not make a metal
    CHECK(Verdict("prp_whatsit", "static.mat", "p_whatsit_01d.dds", "", 13) == "other");
}

TEST_CASE("a material object's file says what it is", "[names]")
{
    const std::string text = "<?xml version=\"1.0\"?>\n<!-- Exported by Material Editor --><!-- "
                             "dc:source=file:///C:/BioWareDrive/ArtContent/MATERIAL_PROJECTS/EP1/ARM_MAS.matproj -->"
                             "<MaterialObject Name=\"PN_HLF_S01b\">\n\t<Material Name=\"Character.mat\"></Material>\n"
                             "\t<Texture Name=\"mml_tDiffuse\" ResName=\"PN_HLF_S01C_0d.dds\"></Texture>\n"
                             "\t<Integer Name=\"mml_iSoundMaterialType\" value=\"5\"></Integer>\n</MaterialObject>";
    const auto read = materials::ReadMaterialObject("PN_HLF_S01b", text);
    REQUIRE(read.has_value());
    const auto &object = read.value();
    CHECK(object.name == "pn_hlf_s01b");
    CHECK(object.definition == "character.mat");
    CHECK(object.project == "ARM_MAS.matproj");
    CHECK(object.Texture("mml_tDiffuse") == "pn_hlf_s01c_0d.dds");
    CHECK(object.sound == 5);
    CHECK(object.Solid());
    CHECK(object.Plate());
    CHECK_FALSE(materials::ReadMaterialObject("x", "no object"));
}

TEST_CASE("settle: the share of metal says the class of a material its names say is painted", "[names]")
{
    const auto &names = TheNames();
    const auto plate = names.ByName(Object("pn_arm_masa", "character.mat", "pn_arm_masa_0d.dds"));
    const auto leather = names.ByName(Object("pm_arm_lgtb", "character.mat", "pm_arm_lgtb_0d.dds"));
    CHECK(materials::Settle(plate, 0.5, false) == "mixed");
    CHECK(materials::Settle(plate, 0.95, false) == "metal");
    CHECK(materials::Settle(plate, 0.0, false) == "other");
    CHECK(materials::Settle(leather, 0.005, false) == "leather");
    CHECK(materials::Settle(leather, 0.06, false) == "mixed");
    CHECK(materials::Settle(leather, std::nullopt, false) == "leather");
    const auto hide = names.ByName(Object("cn_bdy_glka", "character.mat", "cn_bdy_glka_0d.dds"));
    CHECK(materials::Settle(hide, 0.2, true) == "skin");
}

TEST_CASE("the rule for a texel: metals", "[rule]")
{
    CHECK(materials::IsMetalTexel({0.31F, 0.31F, 0.30F}, {0.25F, 0.25F, 0.25F}, 0.25F)); // massive plate's median
    CHECK(materials::IsMetalTexel({0.02F, 0.02F, 0.02F}, {0.40F, 0.40F, 0.40F}, 0.30F)); // dark, a bright mirror
    CHECK(materials::IsMetalTexel({0.50F, 0.38F, 0.15F}, {0.30F, 0.22F, 0.12F}, 0.15F)); // gold that mirrors gold
    CHECK(materials::IsMetalTexel({0.60F, 0.48F, 0.15F}, {0.90F, 0.90F, 0.90F}, 0.90F)); // brass under white
    CHECK(materials::IsMetalTexel({0.22F, 0.24F, 0.23F}, {0.36F, 0.40F, 0.44F}, 0.21F)); // a cold iron rim
}

TEST_CASE("the rule for a texel: not metals", "[rule]")
{
    CHECK_FALSE(materials::IsMetalTexel({0.24F, 0.16F, 0.10F}, {0.06F, 0.06F, 0.06F}, 0.05F)); // leather
    CHECK_FALSE(materials::IsMetalTexel({0.60F, 0.10F, 0.10F}, {0.50F, 0.50F, 0.50F}, 0.50F)); // red paint
    CHECK_FALSE(materials::IsMetalTexel({0.20F, 0.14F, 0.09F}, {0.16F, 0.19F, 0.25F}, 0.15F)); // a blue sheen
    CHECK_FALSE(materials::IsMetalTexel({0.20F, 0.20F, 0.21F}, {0.22F, 0.24F, 0.27F}, 0.12F)); // grey leather, one
    CHECK_FALSE(materials::IsMetalTexel({0.90F, 0.88F, 0.85F}, {0.20F, 0.20F, 0.20F}, 0.30F)); // bone, white cloth
    CHECK_FALSE(materials::IsMetalTexel({0.30F, 0.30F, 0.30F}, {0.30F, 0.30F, 0.30F}, 0.04F)); // bright, matte
    CHECK_FALSE(materials::IsMetalTexel({0.40F, 0.26F, 0.14F}, {0.25F, 0.25F, 0.25F}, 0.15F)); // wood, grey mirror
    CHECK(materials::IsMetalTexel({0.30F, 0.30F, 0.30F}, {0.90F, 0.90F, 0.90F}, 0.90F, true));
    CHECK_FALSE(materials::IsMetalTexel({0.31F, 0.31F, 0.30F}, {0.25F, 0.25F, 0.25F}, 0.25F, true));
}

namespace
{

Picture Filled(std::uint32_t side, std::array<float, 4> value)
{
    Picture out{side, side, 4, {}};
    for (std::uint32_t i = 0; i < side * side; ++i)
    {
        out.values.insert(out.values.end(), value.begin(), value.end());
    }
    return out;
}

void Set(Picture &picture, std::uint32_t x, std::uint32_t y, std::array<float, 4> value)
{
    for (std::uint32_t c = 0; c < 4; ++c)
    {
        picture.values[(((static_cast<std::size_t>(y) * picture.width) + x) * 4) + c] = value[c];
    }
}

} // namespace

TEST_CASE("a stud outlives the majority and a speck does not", "[rule]")
{
    Picture diffuse = Filled(128, {0.24F, 0.16F, 0.10F, 0});
    Picture specular = Filled(128, {0.05F, 0.05F, 0.05F, 0.05F});
    for (std::uint32_t y = 20; y < 24; ++y)
    {
        for (std::uint32_t x = 20; x < 24; ++x)
        {
            Set(specular, x, y, {1, 1, 1, 1}); // a stud: white, and glossy
        }
    }
    Set(specular, 80, 80, {0.2F, 0.2F, 0.2F, 0.12F}); // a speck that only just passes
    Set(diffuse, 80, 80, {0.25F, 0.25F, 0.25F, 0});
    const auto map = materials::MakeMetalMap(diffuse, specular, false);
    REQUIRE(map.metal.width == 32);
    REQUIRE(map.metal.height == 32);
    CHECK(map.metal.At(5, 5, 0) > 0.2F);
    CHECK(map.metal.At(20, 20, 0) == 0.0F);
    CHECK(map.share < 0.01);
}

TEST_CASE("a plate shaded to one side is metal all over, and its strap is not", "[rule]")
{
    // steel, lit at the left and painted darker to the right, to a third; a dark strap beside it
    constexpr std::uint32_t kSide = 64;
    Picture diffuse = Filled(kSide, {0, 0, 0, 1});
    Picture specular = Filled(kSide, {0, 0, 0, 1});
    for (std::uint32_t y = 0; y < kSide; ++y)
    {
        for (std::uint32_t x = 0; x < kSide; ++x)
        {
            const float shade = 1.0F - (0.67F * static_cast<float>(x) / (kSide - 1));
            if (y < 48)
            {
                Set(diffuse, x, y, {0.45F * shade, 0.45F * shade, 0.45F * shade, 1});
                Set(specular, x, y, {0.36F * shade, 0.36F * shade, 0.36F * shade, 0.24F * shade});
            }
            else
            {
                Set(diffuse, x, y, {0.20F, 0.12F, 0.08F, 1}); // leather: a dim mirror under its colour
                Set(specular, x, y, {0.04F, 0.04F, 0.04F, 0.06F});
            }
        }
    }
    const auto plate = materials::MakeMetalMap(diffuse, specular, true);
    const auto plain = materials::MakeMetalMap(diffuse, specular, false);
    CHECK(plate.share == plain.share); // the share, and so the class, is of the rule without it
    const auto mean = [](const Picture &p, std::uint32_t x0, std::uint32_t x1, std::uint32_t y0, std::uint32_t y1) {
        float sum = 0;
        for (std::uint32_t y = y0; y < y1; ++y)
        {
            for (std::uint32_t x = x0; x < x1; ++x)
            {
                sum += p.At(x, y, 0);
            }
        }
        return sum / static_cast<float>((x1 - x0) * (y1 - y0));
    };
    const std::uint32_t side = plate.metal.width; // 16
    const auto rows = static_cast<std::uint32_t>(44 * side / kSide);
    CHECK(mean(plain.metal, side - 2, side, 0, rows) < 0.5F);
    CHECK(mean(plate.metal, side - 2, side, 0, rows) > 0.9F);           // the shaded end too
    CHECK(mean(plate.metal, 0, side, 52 * side / kSide, side) < 0.05F); // and not the strap
}

TEST_CASE("a map is a quarter of its mask's side, 256 at the most and 4 at the least", "[rule]")
{
    CHECK(materials::MapSize(512, 512) == std::array<std::uint32_t, 2>{128, 128});
    CHECK(materials::MapSize(128, 256) == std::array<std::uint32_t, 2>{32, 64});
    CHECK(materials::MapSize(4096, 1024) == std::array<std::uint32_t, 2>{256, 64});
    CHECK(materials::MapSize(8, 8) == std::array<std::uint32_t, 2>{4, 4});
    CHECK(materials::MapName("pn_arm_masa_0s.dds") == "pn_arm_masa_0_m.dds");
    CHECK(materials::MapName("PN_ARM_MASA_0sl2.dds") == "pn_arm_masa_0l2_m.dds");
    CHECK(materials::MapName("tti_master_s.dds") == "tti_master_m.dds");
    CHECK(materials::MapName("plc_doorlg_01_spec.dds") == "plc_doorlg_01_m.dds");
}

namespace
{

// A normal map as the game keeps one: x in the alpha, y in the green.
Picture NormalMap(std::uint32_t width, std::uint32_t height, const std::function<float(std::uint32_t)> &xOfColumn)
{
    Picture out{width, height, 4, {}};
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            out.values.insert(out.values.end(), {0, 0.5F, 0, (xOfColumn(x) + 1) / 2});
        }
    }
    return out;
}

} // namespace

TEST_CASE("bumps: a flat map adds nothing, ridges add twice their spread where a patch holds them", "[rule]")
{
    const auto flat = materials::MeasureBumps(NormalMap(64, 64, [](std::uint32_t) { return 0.0F; }));
    CHECK(flat.width == 64);
    for (const double v : flat.spreads)
    {
        CHECK(v == Catch::Approx(0.0).margin(1e-9));
    }
    // columns leaning half one way and half the other: in any patch the x's
    // spread is a quarter and the y's nothing, and a width squared takes twice
    const auto narrow =
        materials::MeasureBumps(NormalMap(32, 32, [](std::uint32_t x) { return x % 2 == 0 ? 0.5F : -0.5F; }));
    for (const double v : narrow.spreads)
    {
        CHECK(v == Catch::Approx(0.5).margin(1e-6));
    }
    const auto wide =
        materials::MeasureBumps(NormalMap(64, 64, [](std::uint32_t x) { return (x / 8) % 2 == 0 ? 0.5F : -0.5F; }));
    CHECK(wide.spreads[0] == Catch::Approx(0.0).margin(1e-6)); // every patch of 2 lies on one slope
    CHECK(wide.spreads[1] < wide.spreads[3]);
    CHECK(wide.spreads[3] == Catch::Approx(0.5).margin(1e-6)); // a patch of 16 holds both
}

TEST_CASE("a material is known by its bytes, and its answer goes before its end", "[answers]")
{
    // FNV-1a of 64 bits, as its authors give it for "" and "a"
    CHECK(materials::Fnv1a64({}) == 0xcbf29ce484222325ULL);
    const std::byte a{'a'};
    CHECK(materials::Fnv1a64({&a, 1}) == 0xaf63dc4c8601ec8cULL);

    const std::string object = "<MaterialObject Name=\"M\">\n</MaterialObject>\n";
    const auto *bytes = reinterpret_cast<const std::byte *>(object.data());
    const std::span<const std::byte> span(bytes, object.size());
    char hex[17] = {};
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(materials::Fnv1a64(span)));
    materials::Answers answers;
    answers.Parse(std::string("; a comment\n") + hex +
                  " m <Float Name=\"mml_fDaoMetal\" value=\"1.00\"></Float>\nbad line\n");
    CHECK(answers.Count() == 1);
    CHECK(answers.Unparsed() == 1);
    const auto patched = answers.Patched(span);
    REQUIRE(patched.has_value());
    const auto &bytesOut = patched.value();
    const std::string text(reinterpret_cast<const char *>(bytesOut.data()), bytesOut.size());
    CHECK(text ==
          "<MaterialObject Name=\"M\">\n<Float Name=\"mml_fDaoMetal\" value=\"1.00\"></Float></MaterialObject>\n");
    const std::string other = "<MaterialObject/>";
    CHECK_FALSE(answers.Patched({reinterpret_cast<const std::byte *>(other.data()), other.size()}));
}

TEST_CASE("the lines a material is given by its class", "[answers]")
{
    const auto &names = TheNames();
    CHECK(materials::SaidFor("metal", names, {}, std::nullopt) ==
          R"(<Float Name="mml_fDaoMetal" value="1.00"></Float>)");
    CHECK(
        materials::SaidFor("mixed", names, "m_m.dds", std::nullopt) ==
        R"(<Float Name="mml_fDaoMetal" value="1.00"></Float><Texture Name="mml_tDaoMetal" ResName="m_m.dds"></Texture>)");
    CHECK(materials::SaidFor("mixed", names, {}, std::nullopt).empty()); // no textures to make a map of: no metal
    CHECK(materials::SaidFor("stone", names, {}, std::nullopt).empty());
    materials::Bumps bumps{256, 512, {0.1, 0.2, 0.3, 0.4, 0.5, 0.6}};
    CHECK(materials::SaidFor("stone", names, {}, bumps) ==
          R"(<Vector4f Name="mml_vDaoBumps" value="256.00 512.00 0.10000 0.20000"></Vector4f>)"
          R"(<Vector4f Name="mml_vDaoBumpsFar" value="0.30000 0.40000 0.50000 0.60000"></Vector4f>)");
    CHECK(materials::SaidFor("glass", names, {}, std::nullopt) ==
          R"(<Vector4f Name="mml_vDaoClear" value="0.0400 256.00 0.00 0.00"></Vector4f>)");
}

TEST_CASE("the table is read as written, a hand's lines kept", "[table]")
{
    const auto path = std::filesystem::temp_directory_path() / "elgara_test_materials.ini";
    {
        std::ofstream file(path);
        file << "; a comment = metal\nanvil_01 = metal\npn_arm_masa   = mixed  ; 51% metal\n"
                "prp_tevbench1 = wood ; by hand: it is oak in my mod\nredrug=stone;By Hand\nodd = nonsense\n";
    }
    const auto classes = materials::ReadClasses(path);
    CHECK(classes ==
          std::map<std::string, std::string>{
              {"anvil_01", "metal"}, {"pn_arm_masa", "mixed"}, {"prp_tevbench1", "wood"}, {"redrug", "stone"}});
    const auto hands = materials::ReadHands(path);
    REQUIRE(hands.size() == 2);
    CHECK(hands.at("prp_tevbench1").cls == "wood");
    CHECK(hands.at("prp_tevbench1").line == "prp_tevbench1 = wood ; by hand: it is oak in my mod");
    CHECK(hands.at("redrug").line == "redrug=stone;By Hand");
    std::filesystem::remove(path);
}

namespace
{

// The installed game's catalogue, once; null where there is no game.
const materials::Catalogue *TheGame()
{
    static const auto catalogue = []() -> std::unique_ptr<materials::Catalogue> {
        std::filesystem::path game = R"(C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins)";
        char *fromEnvironment = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&fromEnvironment, &length, "DAO_GAME_DIR") == 0 && fromEnvironment != nullptr)
        {
            game = fromEnvironment;
        }
        std::free(fromEnvironment); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s's buffer is malloc's
        std::error_code missing;
        if (!std::filesystem::is_directory(game / "packages" / "core" / "data", missing))
        {
            return nullptr;
        }
        std::string error;
        return materials::Catalogue::Load(game, {}, {}, error);
    }();
    return catalogue.get();
}

materials::Classed Row(const std::string &name)
{
    const auto &material = TheGame()->Materials().at(name);
    return materials::Classify(*TheGame(), TheNames(), material.main);
}

} // namespace

TEST_CASE("the game's own materials, each beyond doubt", "[game]")
{
    if (TheGame() == nullptr)
    {
        SKIP("the game is not installed (DAO_GAME_DIR)");
    }
    CHECK(TheGame()->Materials().size() > 2000);
    // the mages' tower's one sheet: the user's ruling, 2026-10-12, after its walls were mirrors
    CHECK(Row("tti_master").cls == "stone");
    CHECK(Row("fci_exteriorwall01").cls == "stone");
    // the tower's benches and tables, whose bright painted mirror made them metal to a guess
    for (const char *name : {"prp_tevbench1", "prp_tevtable1", "plc_tevtable_02", "prp_benchmarble01"})
    {
        CHECK(Row(name).cls == "stone");
    }
    CHECK(Row("fci_endwoodbeam").cls == "wood");
    CHECK(Row("w_stf_st01a_0").cls == "wood");
    CHECK(Row("plc_armorstand").cls == "wood");
    CHECK(Row("redrug").cls == "cloth");
    CHECK(Row("pf_rob_appa").cls == "cloth");
    CHECK(Row("flagking").cls == "cloth");
    CHECK(Row("hm_arm_nuda").cls == "skin");
    CHECK(Row("c_wolfa").cls == "skin");
    CHECK(Row("cn_lea_gra").cls == "foliage");
    CHECK(Row("anvil_01").cls == "metal");
    CHECK(Row("plc_chainshackles01").cls == "metal");
}

TEST_CASE("the game's metal in part: plate, a sword, shields, leather", "[game]")
{
    if (TheGame() == nullptr)
    {
        SKIP("the game is not installed (DAO_GAME_DIR)");
    }
    const auto plate = Row("pn_arm_masa");
    CHECK(plate.cls == "mixed");
    CHECK(plate.share.value() > 0.35);
    CHECK(plate.share.value() < 0.75);
    CHECK(plate.map.value().width == 128);
    CHECK(Row("w_lsw_ls01a_0").cls == "mixed");
    CHECK(Row("w_lsw_ls01a_0").share.value() > 0.3);
    CHECK(Row("w_shd_kitb").share.value() > 0.6);
    // the round shield: planks, an iron rim and boss
    const auto round = Row("w_shd_srda");
    CHECK(round.cls == "mixed");
    CHECK(round.share.value() > 0.08);
    CHECK(round.share.value() < 0.35);
    // studded leather: its buckles and studs and no more; a leather suit with a blue sheen
    CHECK(Row("pm_arm_lgtd").share.value() < 0.15);
    CHECK(Row("pm_arm_lgtb").share.value() < 0.08);
    // Massive Helmet 1B, Cailan's: the main campaign's own copy, not Awakening's (2026-10-14)
    const auto helm = TheGame()->Materials().at("pn_hlf_s01b").main;
    CHECK(helm.object.Texture("mml_tDiffuse") == "pn_hlf_s01b_0d.dds");
    CHECK(helm.campaign == "Single Player");
}
