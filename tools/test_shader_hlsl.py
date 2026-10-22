"""tools/shader_hlsl.py on listings written here, as fxc /dumpbin writes them."""

import unittest

import shader_hlsl

PIXEL = """
//
// Parameters:
//
//   bool g_bShaderControlBooleans[6];
//   float g_fFocalPlane;
//   float4 g_vLightParams[13];
//   row_major float4x4 mRows;
//   float4x4 mColumns;
//   samplerCUBE smpCubemap0;
//   sampler2D smpDecal;
//
//
// Registers:
//
//   Name                     Reg   Size
//   ------------------------ ----- ----
//   g_bShaderControlBooleans b0       4
//   g_vLightParams           c0      12
//   g_fFocalPlane            c12      1
//   mRows                    c13      4
//   mColumns                 c17      4
//   smpDecal                 s0       1
//   smpCubemap0              s10      1
//

    ps_3_0
    def c29, 2, -1, 1, 255
    defi i0, 3, 0, 0, 0
    dcl_texcoord v0.xy
    dcl_texcoord1 v0.zw
    dcl_texcoord4 v4
    dcl_2d s0
    dcl_cube s10
    texld_pp r1, v0, s0
    mad r1.xy, r1.wyzw, c29.x, c29.y
    dp2add r1.z, r1, -r1, c29.z
    mul_sat r2.xyz, r1.y, v4
    if b3
      texldl r0, r2, s10
    else
      mov r0, c11
    endif
    rep i0
      add r0.x, r0.x, c29.z
    endrep
    rcp r3.x, c12.x
    dp4 r3.y, c14, v4
    dp4 r3.z, c18, v4
    sincos r4.xy, r3.x
    cmp r0.w, -r3_abs.x, c29.z, r0.w
    texkill r0
    mov oC0, r0
    mov oDepth, r3.x

// approximately 20 instruction slots used
"""

VERTEX = """
// Parameters:
//
//   float4 bsm_bones[201];
//   row_major float4x4 g_mLightParams[3];
//
// Registers:
//
//   Name           Reg   Size
//   -------------- ----- ----
//   bsm_bones      c0     201
//   g_mLightParams c201    12
//
    vs_3_0
    dcl_position v0
    dcl_blendindices v4
    dcl_position o0
    dcl_texcoord o1.xy
    mova a0, v4
    dp4 o0.x, c1[a0.y], v0
    dp4 o0.y, c206, v0
    mov o0.zw, v0
    mov o1.xy, v0
"""


class PixelShaderTest(unittest.TestCase):
    def setUp(self):
        self.shader = shader_hlsl.Shader(PIXEL)
        self.hlsl = self.shader.render()

    def statement(self, asm):
        return self.shader.statements[self.shader.find(asm)].hlsl

    def test_the_parameters_keep_their_names_types_and_registers(self):
        self.assertEqual(self.shader.profile, "ps_3_0")
        self.assertIn("float4 g_vLightParams[13] : register(c0);", self.hlsl)
        self.assertIn("bool g_bShaderControlBooleans[6] : register(b0);", self.hlsl)
        self.assertIn("row_major float4x4 mRows : register(c13);", self.hlsl)
        self.assertIn("samplerCUBE smpCubemap0 : register(s10);", self.hlsl)

    def test_an_inputs_components_keep_their_places(self):
        self.assertIn("float2 v0_xy : TEXCOORD0;", self.hlsl)
        self.assertIn("v0.xy = input.v0_xy;", self.hlsl)
        # The third and fourth of TEXCOORD1, not its first two.
        self.assertIn("float4 v0_zw : TEXCOORD1;", self.hlsl)
        self.assertIn("v0.zw = input.v0_zw.zw;", self.hlsl)

    def test_components_with_a_gap(self):
        shader = shader_hlsl.Shader(
            "    ps_3_0\n    dcl_texcoord2 v1.xyw\n    dcl_texcoord3 v2.z\n    mov oC0, v1\n"
        )
        hlsl = shader.render()
        self.assertIn("float4 v1_xyw : TEXCOORD2;", hlsl)
        self.assertIn("v1.xyw = input.v1_xyw.xyw;", hlsl)
        self.assertIn("float3 v2_z : TEXCOORD3;", hlsl)
        self.assertIn("v2.z = input.v2_z.z;", hlsl)

    def test_a_source_gives_the_components_the_destination_takes(self):
        self.assertEqual(
            self.statement("^mad r1.xy"), "r1.xy = r1.wy * c29.xx + c29.yy;"
        )
        self.assertEqual(
            self.statement("^mul_sat"), "r2.xyz = saturate(r1.yyy * v4.xyz);"
        )

    def test_a_dot_product_takes_its_own_components(self):
        self.assertEqual(
            self.statement("^dp2add"), "r1.z = dot(r1.xy, -r1.xy) + c29.z;"
        )

    def test_a_constant_register_is_read_through_its_parameter(self):
        self.assertEqual(
            self.statement("^mov r0, c11"), "r0 = g_vLightParams[11].xyzw;"
        )
        self.assertEqual(self.statement("^rcp"), "r3.x = 1 / g_fFocalPlane;")
        self.assertEqual(
            self.statement("^dp4 r3.y"), "r3.y = dot(mRows[1].xyzw, v4.xyzw);"
        )
        # A matrix packed by column: register 1 of it is its second column.
        self.assertIn(
            "float4(mColumns[0][1], mColumns[1][1], mColumns[2][1], mColumns[3][1]).xyzw",
            self.statement("^dp4 r3.z"),
        )

    def test_flow_and_textures(self):
        self.assertEqual(self.statement("^if b3"), "if (g_bShaderControlBooleans[3]) {")
        self.assertEqual(
            self.statement("^texldl"), "r0 = texCUBElod(smpCubemap0, r2.xyzw);"
        )
        self.assertEqual(self.statement("^texld_pp"), "r1 = tex2D(smpDecal, v0.xy);")
        self.assertIn("loop1 < 3", self.statement("^rep"))
        self.assertEqual(self.statement("^texkill"), "clip(r0);")

    def test_modifiers_of_a_source(self):
        self.assertEqual(
            self.statement("^cmp"), "r0.w = (-abs(r3.x) >= 0 ? c29.z : r0.w);"
        )
        self.assertEqual(
            self.statement("^sincos"), "r4.xy = float2(cos(r3.x), sin(r3.x));"
        )

    def test_the_outputs_are_the_registers_written(self):
        self.assertIn("float4 oC0 : COLOR0;", self.hlsl)
        self.assertIn("float oDepth : DEPTH;", self.hlsl)
        self.assertIn("output.oDepth = oDepth.x;", self.hlsl)

    def test_lines_put_after_a_statement_and_an_input_of_our_own(self):
        at = self.shader.find("^dp4 r3.z")
        self.shader.add_input("pixel", "float2", "VPOS")
        self.shader.insert(at, ["r3.z = Ours(input.pixel);"])
        self.assertEqual(
            self.shader.statements[at + 1].hlsl, "r3.z = Ours(input.pixel);"
        )
        hlsl = self.shader.render("float Ours(float2 p) { return p.x; }")
        self.assertIn("float2 pixel : VPOS;", hlsl)
        self.assertLess(hlsl.index("float Ours"), hlsl.index("Output main"))

    def test_lines_put_before_the_first_statement(self):
        self.shader.insert(-1, ["float daoFirst = 0;"])
        self.assertEqual(self.shader.statements[0].hlsl, "float daoFirst = 0;")
        self.assertEqual(self.shader.statements[0].depth, 0)

    def test_statements_replaced(self):
        first, last = self.shader.find("^rep"), self.shader.find("^endrep")
        self.shader.replace(first, last, ["r0.x = r0.x + 3;"])
        self.assertNotIn("loop1", self.shader.render())


class VertexShaderTest(unittest.TestCase):
    def setUp(self):
        self.shader = shader_hlsl.Shader(VERTEX)
        self.hlsl = self.shader.render()

    def test_a_register_indexed_by_the_address_register(self):
        at = self.shader.find(r"c1\[a0.y\]")
        self.assertEqual(
            self.shader.statements[at].hlsl,
            "o0.x = dot(bsm_bones[1 + a0.y].xyzw, v0.xyzw);",
        )
        self.assertIn("int4 a0 = 0;", self.hlsl)

    def test_a_matrix_of_an_array_indexed_by_the_address_register(self):
        shader = shader_hlsl.Shader(
            "// Parameters:\n//\n//   row_major float4x4 g_amWindMatrices[7];\n//\n"
            "// Registers:\n//\n//   Name             Reg   Size\n"
            "//   ---------------- ----- ----\n//   g_amWindMatrices c0      28\n//\n"
            "    vs_3_0\n    dcl_position v0\n    dcl_position o0\n"
            "    mova a0.x, v0.x\n    mul o0, v0.y, c1[a0.x]\n"
        )
        # The second row of the matrix the address steps to, four registers
        # a matrix.
        self.assertEqual(
            shader.statements[shader.find("c1")].hlsl,
            "o0 = v0.yyyy * g_amWindMatrices[0 + a0.x / 4][1].xyzw;",
        )

    def test_defined_constants_indexed_by_the_address_register_are_a_table(self):
        shader = shader_hlsl.Shader(
            "    vs_3_0\n    def c60, 0, 0.5, 0.5, 6\n    def c61, 0, -0.5, 0.5, 4\n"
            "    def c63, 1, 2, 3, 4\n"
            "    dcl_position v0\n    dcl_position o0\n"
            "    mova a0.x, v0.x\n    add o0, v0, c60[a0.x]\n"
        )
        hlsl = shader.render()
        self.assertEqual(
            shader.statements[shader.find("c60")].hlsl,
            "o0 = v0.xyzw + c60_table(a0.x).xyzw;",
        )
        # c60 and c61, which follows it; c63 does not. Chosen by the index,
        # not an array, which fxc packed into fewer components than are read.
        self.assertIn(
            "float4 c60_table(int i) { return (i == 0 ? float4(0, 0.5, 0.5, 6) : float4(0, -0.5, 0.5, 4)); }",
            hlsl,
        )

    def test_a_row_of_a_matrix_in_an_array(self):
        at = self.shader.find("c206")
        self.assertEqual(
            self.shader.statements[at].hlsl,
            "o0.y = dot(g_mLightParams[1][1].xyzw, v0.xyzw);",
        )

    def test_an_output_of_our_own(self):
        self.shader.add_input("uv", "float2", "TEXCOORD0")
        self.shader.add_output("uv", "float2", "TEXCOORD1", "input.uv")
        hlsl = self.shader.render()
        self.assertIn("float2 uv : TEXCOORD1;", hlsl)
        self.assertIn("output.uv = input.uv;", hlsl)

    def test_the_outputs_are_the_ones_declared(self):
        self.assertIn("float4 o0_xyzw : POSITION0;", self.hlsl)
        self.assertIn("float2 o1_xy : TEXCOORD0;", self.hlsl)
        self.assertIn("output.o1_xy = o1.xy;", self.hlsl)


class RefusalTest(unittest.TestCase):
    def test_a_listing_with_no_version(self):
        with self.assertRaises(shader_hlsl.ShaderError):
            shader_hlsl.Shader("    mov r0, r1\n")

    def test_a_register_nothing_names(self):
        with self.assertRaises(shader_hlsl.ShaderError):
            shader_hlsl.Shader("    ps_3_0\n    mov oC0, c5\n")

    def test_an_instruction_not_known(self):
        with self.assertRaises(shader_hlsl.ShaderError):
            shader_hlsl.Shader(
                "    ps_3_0\n    def c0, 1, 1, 1, 1\n    m4x4 oC0, c0, c0\n"
            )


if __name__ == "__main__":
    unittest.main()
