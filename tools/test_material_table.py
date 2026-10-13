"""tools/material_table.py: its rules on names and texels written here, and, where the
game is installed, a dozen of the game's materials whose class is beyond doubt.

    python tools/test_material_table.py

The names and the rule for a texel need no game; the rule needs numpy and Pillow, and is
skipped without them, as the game's materials are skipped without the game.
"""

import io
import os
import tempfile
import unittest

import material_table as mt

HAVE_NUMPY = mt.np is not None
HAVE_GAME = HAVE_NUMPY and os.path.isdir(os.path.join(mt.GAME, "packages", "core", "data"))


class NamesTest(unittest.TestCase):
    """What a material is called, with no texture read."""

    def verdict(self, name, definition, diffuse="", project="", sound=None):
        return mt.by_name(name, definition, diffuse, project, sound).cls

    def test_worn(self):
        self.assertEqual(self.verdict("pn_arm_masa", "character.mat", "pn_arm_masa_0d.dds", "ARM_MAS.matproj"), mt.PAINT)
        self.assertEqual(self.verdict("pm_arm_lgtd_l2", "character.mat", "pm_arm_lgtd_0dl2.dds", "ARM_LGT.matproj"), mt.PAINT)
        self.assertEqual(self.verdict("hm_arm_nuda", "character.mat", "hm_arm_nuda_0d.dds", "ARM_NUD.matproj"), "skin")
        self.assertEqual(self.verdict("pf_rob_appa", "character.mat", "pf_rob_appa_0d.dds", "ROB_APP.matproj"), "cloth")
        self.assertEqual(self.verdict("pm_cth_coma", "character.mat", "pm_cth_coma_0d.dds", "CTH_COM.matproj"), "cloth")
        self.assertEqual(self.verdict("pn_hlh_crn", "character.mat", "pn_hlh_crn_0d.dds"), "metal")

    def test_a_fallback_is_what_else_the_family_is_made_of(self):
        self.assertEqual(mt.by_name("pm_arm_lgtb", "character.mat", "pm_arm_lgtb_0d.dds").fallback, "leather")
        self.assertEqual(mt.by_name("w_shd_kita", "weapon.mat", "w_shd_kita_0d.dds").fallback, "wood")
        self.assertEqual(mt.by_name("w_lsw_ls01a_0", "weapon.mat", "w_lsw_ls01a_0d.dds").fallback, "other")

    def test_creatures(self):
        self.assertEqual(self.verdict("c_wolfa", "character.mat", "c_wolfa_0d.dds", "C_WOLF.matproj"), "skin")
        self.assertEqual(self.verdict("cn_golem", "character.mat", "cn_bdy_stga_0d.dds"), "stone")
        self.assertEqual(self.verdict("cn_bdy_glka", "character.mat", "cn_bdy_glka_0d.dds"), mt.PAINT)
        # a head is never asked of the paint: the rule marked horns, teeth and a wet face
        self.assertEqual(self.verdict("cn_hed_glka", "character.mat", "cn_hed_glka_0d.dds"), "skin")
        self.assertEqual(self.verdict("cn_hed_aoga", "character.mat", "cn_hed_aoga_0d.dds"), "skin")

    def test_weapons(self):
        self.assertEqual(self.verdict("w_lsw_ls01a_0", "weapon.mat", "w_lsw_ls01a_0d.dds"), mt.PAINT)
        self.assertEqual(self.verdict("w_shd_srda", "weapon.mat", "w_shd_srda_0d.dds"), mt.PAINT)
        self.assertEqual(self.verdict("w_stf_st01a_0", "weapon.mat", "w_stf_st01a_0d.dds"), "wood")
        self.assertEqual(self.verdict("w_qui_qu01", "weapon.mat", "w_qui_qu01a_0d.dds"), "leather")

    def test_other_definitions(self):
        self.assertEqual(self.verdict("cn_lea_gra", "canopy.mat", "cn_lea_graa_0d.dds"), "foliage")
        self.assertEqual(self.verdict("flagking", "cloth.mat", "king_tent_flag.dds"), "cloth")

    def test_things(self):
        static = "static.mat"
        self.assertEqual(self.verdict("tti_master", static, "tti_master_d.dds", "TTI_Master.matproj", 0), "stone")
        self.assertEqual(self.verdict("prp_tevbench1", static, "p_tevbench1_01d.dds", "Prop_Tevinter.matproj", 0), "stone")
        self.assertEqual(self.verdict("prp_tevtable1", static, "p_tevtable1_01d.dds", "Prop_Tevinter.matproj", 0), "stone")
        self.assertEqual(self.verdict("plc_benchmarble01", "prop.mat", "p_bench_marble_01d.dds"), "stone")
        self.assertEqual(self.verdict("fci_exteriorwall01", static, "fci_ewall1.dds", "FereldenCastleInterior.matproj", 5), "stone")
        self.assertEqual(self.verdict("fci_endwoodbeam", static, "fci_endwoodbm.dds", "FereldenCastleInterior.matproj", 1), "wood")
        self.assertEqual(self.verdict("benchlog_01", static, "p_bench_log_01d.dds", "Prop_Kitchen.matproj", 0), "wood")
        self.assertEqual(self.verdict("redrug", static, "prp_rugred.dds", "Prop_Bedroom.matproj", 15), "cloth")
        self.assertEqual(self.verdict("anvil_01", static, "p_anvil_01d.dds", "Prop_Other.matproj", 0), "metal")
        self.assertEqual(self.verdict("plc_chainshackles01", "prop.mat", "p_chain_shackles_01d.dds"), "metal")
        self.assertEqual(self.verdict("prp_ivywall01", static, "prp_ivywall01_d.dds", "TerrainMaster.matproj", 0), "foliage")

    def test_words_inside_words(self):
        static = "static.mat"
        # the Anvil of the Void is a level, not an anvil; a butcher's board is not a herb; cracked is not a rack
        self.assertEqual(self.verdict("hro_anvil_wall", static, "hro_anvil_wall01d.dds", "hro_anvil.matproj", 0), "stone")
        self.assertEqual(self.verdict("prp_butcherboard01", static, "p_butcher_boardd.dds", "OutsourcedProps01.matproj", 0), "wood")
        self.assertEqual(self.verdict("fci_crackedstuccowall05", static, "fci_iwall4.dds", "FereldenCastleInterior.matproj", 5), "stone")
        # an armour stand is a stand of planks (its texture seen 2026-10-12), not armour
        self.assertEqual(self.verdict("plc_armorstand", "prop.mat", "p_armor_stand_01d.dds", "Prop_Armory.matproj"), "wood")

    def test_a_prop_in_a_weapons_texture_is_asked_as_the_weapon(self):
        self.assertEqual(self.verdict("plc_lsw_ls01a_0", "prop.mat", "w_lsw_ls01a_0d.dds", "Prop_Armory.matproj"), mt.PAINT)

    def test_no_name_is_other(self):
        self.assertEqual(self.verdict("prp_cline", "static.mat", "prp_cline0.dds", "Prop_Other.matproj", 0), "other")
        self.assertEqual(self.verdict("h_aoredclifa", "static.mat", "", "HERALDRY.matproj", 0), "other")
        # a metal footstep alone does not make a metal
        self.assertEqual(self.verdict("prp_whatsit", "static.mat", "p_whatsit_01d.dds", "", 13), "other")

    def test_settle(self):
        plate = mt.by_name("pn_arm_masa", "character.mat", "pn_arm_masa_0d.dds")
        leather = mt.by_name("pm_arm_lgtb", "character.mat", "pm_arm_lgtb_0d.dds")
        self.assertEqual(mt.settle(plate, 0.5)[0], "mixed")
        self.assertEqual(mt.settle(plate, 0.95)[0], "metal")
        self.assertEqual(mt.settle(plate, 0.0)[0], "other")
        self.assertEqual(mt.settle(leather, 0.005)[0], "leather")
        self.assertEqual(mt.settle(leather, 0.06)[0], "mixed")
        self.assertEqual(mt.settle(leather, None)[0], "leather")
        hide = mt.by_name("cn_bdy_glka", "character.mat", "cn_bdy_glka_0d.dds")
        self.assertEqual(mt.settle(hide, 0.2, ragged=True)[0], "skin")

    def test_map_name(self):
        self.assertEqual(mt.map_name("pn_arm_masa_0s.dds"), "pn_arm_masa_0_m.dds")
        self.assertEqual(mt.map_name("PN_ARM_MASA_0sl2.dds"), "pn_arm_masa_0l2_m.dds")
        self.assertEqual(mt.map_name("tti_master_s.dds"), "tti_master_m.dds")
        self.assertEqual(mt.map_name("plc_doorlg_01_spec.dds"), "plc_doorlg_01_m.dds")

    def test_a_line_by_hand_is_kept(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "materials.ini")
            with open(path, "w", encoding="utf-8") as f:
                f.write("; a header\nanvil_01 = metal\nprp_tevbench1 = wood ; by hand: it is oak in my mod\nredrug=stone;By Hand\n")
            hands = mt.hand_edits(path)
        self.assertEqual(sorted(hands), ["prp_tevbench1", "redrug"])
        self.assertEqual(hands["prp_tevbench1"], ("wood", "prp_tevbench1 = wood ; by hand: it is oak in my mod"))

        class Stub:
            def __init__(self, cls):
                self.cls, self.share, self.material = cls, None, None

        text = mt.table_text({"anvil_01": Stub("metal"), "prp_tevbench1": Stub("stone"), "redrug": Stub("cloth")}, hands)
        self.assertIn("\nprp_tevbench1 = wood ; by hand: it is oak in my mod\n", text)
        self.assertIn("\nredrug=stone;By Hand\n", text)
        self.assertRegex(text, r"\nanvil_01 += metal\n")


@unittest.skipUnless(HAVE_NUMPY, "numpy and Pillow are not installed")
class TexelsTest(unittest.TestCase):
    """The rule for a texel, on colours written here: (diffuse, mirror, gloss)."""

    def metal(self, diffuse, mirror, gloss, surely=False):
        np = mt.np
        one = np.array([[diffuse]], dtype=np.float32), np.array([[mirror]], dtype=np.float32), np.array([[gloss]], dtype=np.float32)
        return bool(mt.metal_texels(*one, surely=surely)[0, 0])

    def test_metals(self):
        self.assertTrue(self.metal((0.31, 0.31, 0.30), (0.25, 0.25, 0.25), 0.25))  # massive plate's median
        self.assertTrue(self.metal((0.02, 0.02, 0.02), (0.40, 0.40, 0.40), 0.30))  # dark, with a bright mirror
        self.assertTrue(self.metal((0.50, 0.38, 0.15), (0.30, 0.22, 0.12), 0.15))  # gold that mirrors gold
        self.assertTrue(self.metal((0.60, 0.48, 0.15), (0.90, 0.90, 0.90), 0.90))  # a brass stud under a white mirror
        self.assertTrue(self.metal((0.22, 0.24, 0.23), (0.36, 0.40, 0.44), 0.21))  # a cold iron rim

    def test_not_metals(self):
        self.assertFalse(self.metal((0.24, 0.16, 0.10), (0.06, 0.06, 0.06), 0.05))  # leather
        self.assertFalse(self.metal((0.60, 0.10, 0.10), (0.50, 0.50, 0.50), 0.50))  # red paint with a white highlight
        self.assertFalse(self.metal((0.20, 0.14, 0.09), (0.16, 0.19, 0.25), 0.15))  # brown leather with a blue sheen
        self.assertFalse(self.metal((0.20, 0.20, 0.21), (0.22, 0.24, 0.27), 0.12))  # grey leather with one
        self.assertFalse(self.metal((0.90, 0.88, 0.85), (0.20, 0.20, 0.20), 0.30))  # bone, white cloth: a dim mirror under a bright colour
        self.assertFalse(self.metal((0.30, 0.30, 0.30), (0.30, 0.30, 0.30), 0.04))  # bright but matte
        self.assertFalse(self.metal((0.40, 0.26, 0.14), (0.25, 0.25, 0.25), 0.15))  # wood with a grey mirror that only just passes

    def test_surely(self):
        self.assertTrue(self.metal((0.30, 0.30, 0.30), (0.90, 0.90, 0.90), 0.90, surely=True))
        self.assertFalse(self.metal((0.31, 0.31, 0.30), (0.25, 0.25, 0.25), 0.25, surely=True))

    def test_a_stud_outlives_the_majority_and_a_speck_does_not(self):
        np = mt.np
        diffuse = np.zeros((128, 128, 4), dtype=np.float32)
        diffuse[..., :3] = (0.24, 0.16, 0.10)
        specular = np.zeros((128, 128, 4), dtype=np.float32)
        specular[..., :3], specular[..., 3] = 0.05, 0.05
        specular[20:24, 20:24] = 1.0  # a stud: white, and glossy
        specular[80, 80] = (0.2, 0.2, 0.2, 0.12)  # a speck that only just passes
        diffuse[80, 80, :3] = 0.25
        metal, share = mt.metal_map(diffuse, specular)
        self.assertEqual(metal.shape, (32, 32))
        self.assertGreater(metal[5, 5], 0.2)
        self.assertEqual(metal[20, 20], 0.0)
        self.assertLess(share, 0.01)

    def test_map_size(self):
        self.assertEqual(mt.map_size(512, 512), (128, 128))
        self.assertEqual(mt.map_size(128, 256), (32, 64))
        self.assertEqual(mt.map_size(4096, 1024), (256, 64))
        self.assertEqual(mt.map_size(8, 8), (4, 4))

    def test_the_map_as_a_file(self):
        np = mt.np
        from PIL import Image

        grey = np.zeros((16, 32), dtype=np.float32)
        grey[:, 16:] = 1.0
        grey[4:8, 4:8] = 0.5
        body = mt.dds_dxt1(grey)
        self.assertEqual(body[:4], b"DDS ")
        self.assertEqual(body[84:88], b"DXT1")
        self.assertEqual(int.from_bytes(body[28:32], "little"), 6)  # 32x16 down to 1x1
        self.assertEqual(len(body), 128 + 8 * (8 * 4 + 4 * 2 + 2 + 1 + 1 + 1))
        back = np.asarray(Image.open(io.BytesIO(body)).convert("RGB"), dtype=np.float32)[..., 1] / 255
        self.assertEqual(back.shape, (16, 32))
        self.assertLess(float(np.abs(back - grey).max()), 0.02)


@unittest.skipUnless(HAVE_NUMPY, "numpy and Pillow are not installed")
class ShadeTest(unittest.TestCase):
    """Shade painted over a metal is not another material (metal_map's joined)."""

    def test_joined_goes_through_what_may_and_no_further(self):
        seeds = mt.np.zeros((5, 7), dtype=bool)
        seeds[2, 0] = True
        may = mt.np.zeros((5, 7), dtype=bool)
        may[2, 1:4] = True  # a run from the seed
        may[2, 5:] = True  # and one past a gap
        may[0, 0] = True  # and one that touches nothing
        grown = mt.joined(seeds, may)
        self.assertTrue(grown[2, :4].all())
        self.assertFalse(grown[2, 4:].any())
        self.assertFalse(grown[0, 0])

    def test_a_plate_shaded_to_one_side_is_metal_all_over(self):
        # steel, lit at the left and painted darker to the right, to a third; a dark strap beside it
        size = 64
        shade = mt.np.linspace(1.0, 0.33, size, dtype=mt.np.float32)[None, :].repeat(size, axis=0)
        diffuse = mt.np.ones((size, size, 4), dtype=mt.np.float32)
        specular = mt.np.ones((size, size, 4), dtype=mt.np.float32)
        diffuse[..., :3] = 0.45 * shade[..., None]
        specular[..., :3] = 0.36 * shade[..., None]
        specular[..., 3] = 0.24 * shade
        diffuse[48:, :, :3] = (0.20, 0.12, 0.08)  # leather: a dim mirror under its colour
        specular[48:, :, :3] = 0.04
        specular[48:, :, 3] = 0.06
        metal, share = mt.metal_map(diffuse, specular, plate=True)
        plain, same = mt.metal_map(diffuse, specular)
        self.assertEqual(share, same)  # the share, and so the class, is of the rule without it
        self.assertLess(plain[: int(44 * plain.shape[0] / size), -2:].mean(), 0.5)
        scale = metal.shape[0] / size
        plate = metal[: int(44 * scale)]
        self.assertGreater(plate[:, -2:].mean(), 0.9)  # the shaded end too
        self.assertLess(metal[int(52 * scale) :].mean(), 0.05)  # and not the strap


@unittest.skipUnless(HAVE_GAME, "the game is not installed (DAO_GAME_DIR), or numpy and Pillow are not")
class GameTest(unittest.TestCase):
    """A dozen of the game's own materials, each beyond doubt, by what makes it so."""

    @classmethod
    def setUpClass(cls):
        cls.game = mt.Game(mt.GAME, levels=False)

    def row(self, name):
        return mt.classify(self.game, self.game.materials[name])

    def test_the_game_has_its_materials(self):
        counts = {d: sum(1 for m in self.game.materials.values() if m.definition == d) for d in mt.SOLID}
        self.assertGreater(counts["static.mat"], 900)
        self.assertGreater(counts["character.mat"], 900)
        self.assertGreater(self.game.skipped["vfx.mat"], 1000)

    def test_stone(self):
        # the mages' tower's one sheet: the user's ruling, 2026-10-12, after its walls were mirrors
        self.assertEqual(self.row("tti_master").cls, "stone")
        # a castle's rubble wall: named a wall, given a stone footstep
        self.assertEqual(self.row("fci_exteriorwall01").cls, "stone")
        # the tower's benches and tables, whose bright painted mirror made them metal to a guess
        for name in ("prp_tevbench1", "prp_tevtable1", "plc_tevtable_02", "prp_benchmarble01"):
            self.assertEqual(self.row(name).cls, "stone", name)

    def test_wood(self):
        # a beam named wood with a wooden footstep; a staff
        self.assertEqual(self.row("fci_endwoodbeam").cls, "wood")
        self.assertEqual(self.row("w_stf_st01a_0").cls, "wood")
        # the armour stand: planks
        self.assertEqual(self.row("plc_armorstand").cls, "wood")

    def test_cloth(self):
        # a rug with a carpet's footstep; a robe; a banner on the cloth simulation
        self.assertEqual(self.row("redrug").cls, "cloth")
        self.assertEqual(self.row("pf_rob_appa").cls, "cloth")
        self.assertEqual(self.row("flagking").cls, "cloth")

    def test_skin_and_foliage(self):
        self.assertEqual(self.row("hm_arm_nuda").cls, "skin")
        self.assertEqual(self.row("c_wolfa").cls, "skin")
        self.assertEqual(self.row("cn_lea_gra").cls, "foliage")

    def test_metal(self):
        # an anvil and a chain: iron by their names, black iron in their textures
        self.assertEqual(self.row("anvil_01").cls, "metal")
        self.assertEqual(self.row("plc_chainshackles01").cls, "metal")

    def test_massive_plate_is_mixed_and_mostly_metal(self):
        row = self.row("pn_arm_masa")
        self.assertEqual(row.cls, "mixed")
        self.assertGreater(row.share, 0.35)
        self.assertLess(row.share, 0.75)
        self.assertEqual(row.map.shape, (128, 128))

    def test_a_sword_is_mixed(self):
        row = self.row("w_lsw_ls01a_0")
        self.assertEqual(row.cls, "mixed")
        self.assertGreater(row.share, 0.3)

    def test_a_steel_shield_is_metal_nearly_all(self):
        row = self.row("w_shd_kitb")
        self.assertEqual(row.cls, "mixed")
        self.assertGreater(row.share, 0.6)

    def test_a_wooden_shield_keeps_its_wood_and_has_its_iron(self):
        # the round shield: planks, an iron rim and boss
        row = self.row("w_shd_srda")
        self.assertEqual(row.cls, "mixed")
        self.assertGreater(row.share, 0.08)
        self.assertLess(row.share, 0.35)
        self.assertEqual(row.map.shape, (64, 32))
        self.assertLess(float(row.map[5:10, 8:22].mean()), 0.02)  # planks, between the rim and the boss
        self.assertGreater(float(row.map[13:18, 13:18].mean()), 0.5)  # the boss
        self.assertGreater(float(row.map[0, 11:19].mean()), 0.5)  # the rim

    def test_leather_is_not_metal(self):
        # studded leather: its buckles and studs, and no more
        row = self.row("pm_arm_lgtd")
        self.assertEqual(row.cls, "mixed")
        self.assertLess(row.share, 0.15)
        # a leather suit with a blue sheen, which a guess took for metal
        row = self.row("pm_arm_lgtb")
        self.assertIn(row.cls, ("leather", "mixed"))
        self.assertLess(row.share, 0.08)


@unittest.skipUnless(HAVE_NUMPY, "numpy and Pillow are not installed")
class ObjectsTest(unittest.TestCase):
    """The table as the game reads it (--objects)."""

    @staticmethod
    def normal_map(x, y):
        """A normal map as the game keeps one: x in the alpha, y in the green."""
        picture = mt.np.zeros((*x.shape, 4), dtype=mt.np.float32)
        picture[..., 3] = (x + 1) / 2
        picture[..., 1] = (y + 1) / 2
        return picture

    def test_a_flat_map_adds_nothing(self):
        flat = mt.np.zeros((64, 64))
        side, spreads = mt.bumps(self.normal_map(flat, flat))
        self.assertEqual(side, (64, 64))
        self.assertEqual(len(spreads), mt.BUMP_PATCHES)
        self.assertTrue(all(abs(v) < 1e-9 for v in spreads))

    def test_ridges_a_texel_wide_add_twice_their_spread(self):
        # columns leaning half one way and half the other: in any patch the x's spread
        # is a quarter and the y's nothing, and a lobe's width squared takes twice that
        x = mt.np.tile(mt.np.array([0.5, -0.5]), (32, 16))
        side, spreads = mt.bumps(self.normal_map(x, mt.np.zeros_like(x)))
        self.assertEqual(side, (32, 32))
        for value in spreads:
            self.assertAlmostEqual(value, 0.5, places=6)

    def test_wide_ridges_show_only_in_wide_patches(self):
        x = mt.np.tile(mt.np.repeat(mt.np.array([0.5, -0.5]), 8), (64, 4))  # ridges 8 texels wide
        _, spreads = mt.bumps(self.normal_map(x, mt.np.zeros_like(x)))
        self.assertAlmostEqual(spreads[0], 0.0, places=6)  # every patch of 2 lies on one slope
        self.assertLess(spreads[1], spreads[3])
        self.assertAlmostEqual(spreads[3], 0.5, places=6)  # a patch of 16 holds both

    def test_a_material_object_says_what_it_is(self):
        material = mt.Material(
            "m", '<?xml version="1.0"?>\n<MaterialObject Name="m">\n\t<Material Name="Prop.mat"></Material>\n</MaterialObject>', "a.erf"
        )
        text = mt.object_text(material, True, "m_m.dds", ((256, 512), [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]))
        self.assertIn('<Float Name="mml_fDaoMetal" value="1.00"></Float>', text)
        self.assertIn('<Texture Name="mml_tDaoMetal" ResName="m_m.dds"></Texture>', text)
        self.assertIn('<Vector4f Name="mml_vDaoBumps" value="256.00 512.00 0.10000 0.20000"></Vector4f>', text)
        self.assertIn('<Vector4f Name="mml_vDaoBumpsFar" value="0.30000 0.40000 0.50000 0.60000"></Vector4f>', text)
        self.assertTrue(text.rstrip().endswith("</MaterialObject>"))
        self.assertLess(text.index("mml_fDaoMetal"), text.index("</MaterialObject>"))
        plain = mt.object_text(material, False, None, ((64, 64), [0.0] * 6))
        self.assertNotIn("mml_fDaoMetal", plain)
        self.assertIn("mml_vDaoBumps", plain)

    def test_a_material_drawn_blended_is_left_blended(self):
        helm = mt.Material(
            "helm",
            '<MaterialObject Name="helm">\n\t<Material Name="Character.mat"></Material>\n'
            '\t<DefaultSemantic Name="AlphaNoTint"></DefaultSemantic>\n</MaterialObject>',
            "a.erf",
        )
        self.assertIn('<DefaultSemantic Name="AlphaNoTint">', mt.object_text(helm, True, None, None))

    def test_the_table_is_read_as_written(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "materials.ini")
            with open(path, "w", encoding="utf-8") as f:
                f.write("; a comment = metal\nanvil_01 = metal\npn_arm_masa   = mixed  ; 51% metal\nrug = cloth ; by hand\nodd = nonsense\n")
            self.assertEqual(mt.table_classes(path), {"anvil_01": "metal", "pn_arm_masa": "mixed", "rug": "cloth"})


if __name__ == "__main__":
    unittest.main()
