"""A compiled Direct3D 9 shader as HLSL that compiles back to the same thing.

The game's shaders (packages\\core\\data\\shaders.erf) are compiled bytecode,
shader model 2 and 3, with no source. `fxc /dumpbin` lists one as assembly,
with its constant table in a comment: each parameter's name, type and
register. This turns that listing into HLSL, a statement per instruction over
the same registers, with the parameters declared as they were, so the result
compiles (fxc again) to a shader the engine binds the same way: it finds a
parameter by its name in the constant table. A statement can then be replaced
by HLSL of our own (Shader.replace), which is how a plugin changes what the
game draws: shadows filtered better, another lighting model.

    python tools/shader_hlsl.py listing.asm            HLSL on stdout
    python tools/shader_hlsl.py --profile listing.asm  the profile to compile it for

What an instruction means is Direct3D 9's shader assembly reference; the
places where HLSL's function of the same name differs are noted in OPERATIONS.
"""

import re
import sys

COMPONENTS = "xyzw"

# A semantic for each register a shader model 2 shader does not declare one for.
OUTPUT_SEMANTICS = {
    "oPos": "POSITION",
    "oPts": "PSIZE",
    "oFog": "FOG",
    "oDepth": "DEPTH",
}
OUTPUT_PREFIXES = {"oC": "COLOR", "oT": "TEXCOORD", "oD": "COLOR"}
INPUT_PREFIXES = {"t": "TEXCOORD", "v": "COLOR"}
SAMPLER_FUNCTIONS = {
    "sampler2D": ("tex2D", 2),
    "samplerCUBE": ("texCUBE", 3),
    "sampler3D": ("tex3D", 3),
}

# An instruction whose result's every component comes from the same component
# of its sources, as HLSL over those sources. The assembly's exp and log are
# base 2. Its rsq and pow take the absolute value of their source, as the
# hardware does for HLSL's too: fxc compiles rsqrt and pow to them unchanged.
COMPONENTWISE = {
    "mov": "{0}",
    "add": "{0} + {1}",
    "sub": "{0} - {1}",
    "mul": "{0} * {1}",
    "mad": "{0} * {1} + {2}",
    "max": "max({0}, {1})",
    "min": "min({0}, {1})",
    "frc": "frac({0})",
    "abs": "abs({0})",
    "rcp": "1 / {0}",
    "rsq": "rsqrt({0})",
    "exp": "exp2({0})",
    "log": "log2({0})",
    "pow": "pow({0}, {1})",
    "sge": "({0} >= {1} ? 1.0 : 0.0)",
    "slt": "({0} < {1} ? 1.0 : 0.0)",
    "cmp": "({0} >= 0 ? {1} : {2})",
    "lrp": "lerp({2}, {1}, {0})",
    "dsx": "ddx({0})",
    "dsy": "ddy({0})",
}

# One whose result is one number, from fixed components of its sources: the
# count of components each source gives.
SCALAR = {
    "dp3": ("dot({0}, {1})", (3, 3)),
    "dp4": ("dot({0}, {1})", (4, 4)),
    "dp2add": ("dot({0}, {1}) + {2}", (2, 2, 1)),
}

COMPARISONS = {"gt": ">", "lt": "<", "ge": ">=", "le": "<=", "eq": "==", "ne": "!="}

OPERAND = re.compile(
    r"^(-)?([A-Za-z]+\d*)(_abs)?(?:\[(a\d+)\.([xyzw])\])?(?:\.([xyzw]{1,4}))?$"
)
PARAMETER = re.compile(r"^//\s+(row_major\s+)?(\w+)\s+(\w+)(?:\[(\d+)\])?;")
REGISTER = re.compile(r"^//\s+(\w+)\s+([cbis])(\d+)\s+(\d+)\s*$")
DECLARATION = re.compile(
    r"^dcl(?:_(?!pp|centroid)([a-z]+?)(\d*))?((?:_pp|_centroid)*)$"
)


class ShaderError(ValueError):
    """A listing this does not understand."""


class Parameter:
    """One row of the constant table: a uniform and the registers it holds."""

    def __init__(self, name, kind, row_major, count):
        self.name = name
        self.kind = kind
        self.row_major = row_major
        self.count = count  # the array's declared length, None if not an array
        self.set = None  # c, b, i or s
        self.base = 0
        self.size = 0

    def declaration(self):
        prefix = "row_major " if self.row_major else ""
        array = f"[{self.count}]" if self.count is not None else ""
        return (
            f"{prefix}{self.kind} {self.name}{array} : register({self.set}{self.base});"
        )

    def register(self, index, relative=None):
        """HLSL for the float register `index` past this parameter's first, and
        the components it has."""
        rows = self.kind == "float4x4" and self.row_major
        if relative is not None:
            if rows and self.count is not None:
                # An array of matrices, a register a row: the address is taken
                # to step by whole matrices, as SpeedTree's wind matrices are
                # stepped (the index times four), so the row stays the one
                # the instruction names.
                return (
                    f"{self.name}[{index // 4} + {relative} / 4][{index % 4}]",
                    COMPONENTS,
                )
            if not (self.kind == "float4" and self.count is not None) and not (
                rows and self.count is None
            ):
                raise ShaderError(
                    f"{self.name} is indexed by {relative} and is not an array of float4"
                )
            return f"{self.name}[{index} + {relative}]", COMPONENTS
        if rows and self.count is not None:
            return f"{self.name}[{index // 4}][{index % 4}]", COMPONENTS
        if self.kind in ("float", "int"):
            return self.name, "x"
        if self.kind in ("float2", "float3", "int2", "int3"):
            return self.name, COMPONENTS[: int(self.kind[-1])]
        if self.kind == "float4":
            return (
                f"{self.name}[{index}]" if self.count is not None else self.name
            ), COMPONENTS
        if self.kind == "float4x4" and self.count is None:
            if self.row_major:
                return f"{self.name}[{index}]", COMPONENTS
            # Packed by column: register i is column i.
            return "float4(" + ", ".join(
                f"{self.name}[{row}][{index}]" for row in range(4)
            ) + ")", None
        raise ShaderError(f"{self.name}: no reading of a {self.kind} by register")


class Statement:
    """An instruction and the HLSL it became."""

    def __init__(self, asm, hlsl, depth):
        self.asm = asm
        self.hlsl = hlsl
        self.depth = depth


class Shader:
    """A listing read: `statements` are its body, `render()` the HLSL."""

    def __init__(self, listing):
        self.parameters = {}
        self.profile = None
        self.definitions = {}  # a def'd constant register: its four numbers
        self.loop_counts = {}
        self.inputs = []  # (field, type, semantic, register, mask)
        self.extra_inputs = []  # (field, type, semantic): read as input.<field> by added statements
        self.extra_outputs = []  # (field, type, semantic, the HLSL it is given)
        self.outputs = []
        self.statements = []
        self._registers = {}  # set letter: {number: (parameter, index)}
        self._temporaries = set()
        self._tables = {}  # a defined register indexed by the address register: how many follow it
        self._written = {}  # an output register of shader model 2: its mask
        self._depth = 0
        self._loops = 0
        for line in listing.splitlines():
            self._line(line.strip())
        if self.profile is None:
            raise ShaderError("no shader version in the listing")

    @property
    def is_pixel(self):
        return self.profile.startswith("ps")

    def _line(self, line):
        if line.startswith("//"):
            self._comment(line)
        elif line and not line.startswith("#"):
            self._instruction(line.split("//")[0].strip())

    def _comment(self, line):
        match = PARAMETER.match(line)
        if match:
            row_major, kind, name, count = match.groups()
            self.parameters[name] = Parameter(
                name, kind, bool(row_major), int(count) if count else None
            )
            return
        match = REGISTER.match(line)
        if match and match.group(1) in self.parameters:
            parameter = self.parameters[match.group(1)]
            parameter.set, parameter.base, parameter.size = (
                match.group(2),
                int(match.group(3)),
                int(match.group(4)),
            )
            for index in range(parameter.size):
                self._registers.setdefault(parameter.set, {})[
                    parameter.base + index
                ] = (parameter, index)

    def _instruction(self, text):
        name, _, rest = text.partition(" ")
        operands = [o.strip() for o in rest.split(",")] if rest.strip() else []
        if re.fullmatch(r"[pv]s_\d_\w", name):
            self.profile = name
        elif name == "def":
            self.definitions[operands[0]] = operands[1:]
        elif name == "defi":
            self.loop_counts[operands[0]] = int(operands[1])
        elif name.startswith("dcl"):
            self._declare(name, operands[0])
        else:
            self._statement(text, name, operands)

    def _declare(self, name, operand):
        register, _, mask = operand.partition(".")
        if register.startswith("s"):
            return  # a sampler: the constant table has its type
        usage, index, _ = DECLARATION.match(name).groups()
        if usage is None:  # shader model 2's pixel inputs: the register says which
            usage, index = INPUT_PREFIXES[register[0]].lower(), register[1:]
        mask = mask or COMPONENTS
        # A declaration's components keep their places in the value: `.xyw`
        # is the first, second and fourth of four, not three in a row. Read
        # as three, the sky's shader took a z for the w it divides the sun's
        # place by, and drew the sun several times its size (seen in play
        # 2026-10-11). So the field is as wide as its last component.
        width = COMPONENTS.index(mask[-1]) + 1
        kind = "float" if width == 1 else f"float{width}"
        entry = (
            f"{register}_{mask}",
            kind,
            f"{usage.upper()}{index or 0}",
            register,
            mask,
        )
        (self.outputs if register.startswith("o") else self.inputs).append(entry)

    # Operands

    def _source(self, operand, components):
        """HLSL for a source operand's components at `components` (positions
        in its four-component swizzle)."""
        match = OPERAND.match(operand)
        if not match:
            raise ShaderError(f"operand {operand}")
        negate, register, absolute, relative, relative_component, swizzle = (
            match.groups()
        )
        swizzle = swizzle or COMPONENTS
        swizzle += swizzle[-1] * (4 - len(swizzle))
        wanted = "".join(swizzle[i] for i in components)
        if relative:
            relative = f"{relative}.{relative_component}"
        base, has = self._register(register, relative)
        if has is None or any(c not in has for c in wanted):
            text = (
                f"float4({base}{', 0' * (4 - len(has))}).{wanted}"
                if has
                else f"{base}.{wanted}"
            )
        elif has == "x":
            text = base if len(wanted) == 1 else f"{base}.{wanted}"
        else:
            text = f"{base}.{wanted}"
        if absolute:
            text = f"abs({text})"
        return f"-{text}" if negate else text

    def _register(self, register, relative):
        letter, number = re.fullmatch(r"([A-Za-z]+)(\d*)", register).groups()
        if letter == "r":
            self._temporaries.add(register)
            return register, COMPONENTS
        if letter in ("v", "t"):
            return register, COMPONENTS
        if letter == "c":
            if register in self.definitions and relative is None:
                return register, COMPONENTS
            if register in self.definitions:
                # A table the compiler laid out in defined constants: this
                # one and every one defined straight after it.
                count = 1
                while f"c{int(number) + count}" in self.definitions:
                    count += 1
                self._tables[register] = count
                return f"{register}_table({relative})", COMPONENTS
            found = self._registers.get("c", {}).get(int(number))
            if found is None:
                raise ShaderError(f"{register} is neither defined nor a parameter's")
            return found[0].register(found[1], relative)
        raise ShaderError(f"{register} read as a source")

    def _destination(self, operand):
        register, _, mask = operand.partition(".")
        mask = mask or COMPONENTS
        if register.startswith("r"):
            self._temporaries.add(register)
        elif register.startswith("o") and not re.fullmatch(r"o\d+", register):
            seen = self._written.get(register, "")
            self._written[register] = "".join(
                c for c in COMPONENTS if c in seen or c in mask
            )
        return register, mask

    def _sampler(self, operand):
        found = self._registers.get("s", {}).get(int(operand[1:]))
        if found is None:
            raise ShaderError(f"{operand} has no name in the constant table")
        return found[0].name, found[0].kind

    # Statements

    def _emit(self, asm, hlsl):
        self.statements.append(Statement(asm, hlsl, self._depth))

    def _statement(self, asm, name, operands):
        base, _, modifiers = name.partition("_")
        if base == "if":
            self._emit(asm, f"if ({self._condition(modifiers, operands)}) {{")
            self._depth += 1
        elif base == "else":
            self._depth -= 1
            self._emit(asm, "} else {")
            self._depth += 1
        elif base in ("endif", "endrep"):
            self._depth -= 1
            self._emit(asm, "}")
        elif base == "rep":
            self._loops += 1
            count = self.loop_counts[operands[0]]
            self._emit(
                asm,
                f"for (int loop{self._loops} = 0; loop{self._loops} < {count}; loop{self._loops}++) {{",
            )
            self._depth += 1
        elif base == "texkill":
            register, mask = self._destination(operands[0])
            self._emit(
                asm,
                f"clip({register}.{mask});"
                if mask != COMPONENTS
                else f"clip({register});",
            )
        else:
            self._assignment(asm, base, modifiers.split("_"), operands)

    def _condition(self, comparison, operands):
        if comparison:
            left, right = (self._source(o, [0]) for o in operands)
            return f"{left} {COMPARISONS[comparison]} {right}"
        parameter, index = self._registers["b"][int(operands[0][1:])]
        return (
            f"{parameter.name}[{index}]"
            if parameter.count is not None
            else parameter.name
        )

    def _assignment(self, asm, base, modifiers, operands):
        register, mask = self._destination(operands[0])
        value = self._value(base, mask, operands[1:])
        if "sat" in modifiers:
            value = f"saturate({value})"
        target = register if mask == COMPONENTS else f"{register}.{mask}"
        self._emit(asm, f"{target} = {value};")

    def _value(self, base, mask, sources):
        positions = [COMPONENTS.index(c) for c in mask]
        if base in COMPONENTWISE:
            return COMPONENTWISE[base].format(
                *(self._source(s, positions) for s in sources)
            )
        if base in SCALAR:
            form, counts = SCALAR[base]
            return form.format(
                *(
                    self._source(s, range(n))
                    for s, n in zip(sources, counts, strict=True)
                )
            )
        if base == "nrm":
            return f"normalize({self._source(sources[0], range(3))}).{mask}"
        if base == "sincos":
            angle = self._source(sources[0], [0])
            parts = {"x": f"cos({angle})", "y": f"sin({angle})"}
            return (
                parts[mask] if len(mask) == 1 else f"float2({parts['x']}, {parts['y']})"
            )
        if base == "mova":
            # The address register takes the nearest integer.
            return f"floor({self._source(sources[0], positions)} + 0.5)"
        if base.startswith("texld"):
            return self._texture(base, mask, sources)
        raise ShaderError(f"instruction {base}")

    def _texture(self, base, mask, sources):
        name, kind = self._sampler(sources[1])
        function, count = SAMPLER_FUNCTIONS[kind]
        if base == "texld":
            call = f"{function}({name}, {self._source(sources[0], range(count))})"
        elif base in ("texldl", "texldb"):
            call = f"{function}{'lod' if base == 'texldl' else 'bias'}({name}, {self._source(sources[0], range(4))})"
        elif base == "texldd":
            coordinate, dx, dy = (
                self._source(s, range(count))
                for s in (sources[0], sources[2], sources[3])
            )
            call = f"{function}grad({name}, {coordinate}, {dx}, {dy})"
        else:
            raise ShaderError(f"instruction {base}")
        return call if mask == COMPONENTS else f"{call}.{mask}"

    # Changing it

    def find(self, pattern, start=0):
        """The index of the first statement at or after `start` whose assembly
        matches a regular expression."""
        for index in range(start, len(self.statements)):
            if re.search(pattern, self.statements[index].asm):
                return index
        raise ShaderError(f"no instruction matches {pattern}")

    def replace(self, first, last, lines):
        """Put HLSL lines in place of the statements `first` to `last`, both
        included."""
        depth = self.statements[first].depth
        self.statements[first : last + 1] = [
            Statement("", line, depth) for line in lines
        ]

    def insert(self, after, lines):
        """Put HLSL lines after the statement `after`, or before the first
        with an `after` of -1. What they set is then what the rest reads, and
        the statements that set it before are dropped by the compiler with
        all only they needed."""
        depth = self.statements[after].depth if after >= 0 else 0
        self.statements[after + 1 : after + 1] = [
            Statement("", line, depth) for line in lines
        ]

    def add_input(self, field, kind, semantic):
        """Another input, which added statements read as `input.<field>`: the
        pixel's place on the screen (VPOS), say. Asked for twice, it is there
        once."""
        if (field, kind, semantic) not in self.extra_inputs:
            self.extra_inputs.append((field, kind, semantic))

    def add_output(self, field, kind, semantic, value):
        """Another output, given the HLSL `value` when the shader is done: a
        vertex shader handing the pixel shader one more thing, say."""
        self.extra_outputs.append((field, kind, semantic, value))

    # Writing it out

    def _output_fields(self):
        if self.outputs:  # shader model 3's vertex outputs, declared
            return self.outputs
        fields = []
        for register, mask in sorted(self._written.items()):
            width = COMPONENTS.index(mask[-1]) + 1
            if register in OUTPUT_SEMANTICS:
                semantic = OUTPUT_SEMANTICS[register]
            else:
                prefix, number = re.fullmatch(r"([A-Za-z]+)(\d+)", register).groups()
                semantic = f"{OUTPUT_PREFIXES[prefix]}{number}"
            if register == "oDepth":
                width = 1
            elif semantic == "POSITION" or semantic.startswith("COLOR"):
                width = 4
            kind = "float" if width == 1 else f"float{width}"
            fields.append((register, kind, semantic, register, COMPONENTS[:width]))
        return fields

    @staticmethod
    def _width(kind):
        return 1 if kind == "float" else int(kind[-1])

    def _part(self, kind, mask):
        """The components of a field of type `kind` that `mask` names: none
        to write where the field is exactly those."""
        return "" if self._width(kind) == len(mask) else f".{mask}"

    def render(self, preamble=""):
        """The whole shader as HLSL. `preamble` goes before `main`: functions
        the replaced statements call."""
        outputs = self._output_fields()
        out = [
            "// Written by tools/shader_hlsl.py from the game's own shader; not kept in the repository.",
            "#pragma warning(disable : 3571)  // pow of a negative: the assembly's pow is of the absolute value",
        ]
        out += [p.declaration() for p in self.parameters.values() if p.set is not None]
        out.append("struct Input {")
        out += [
            f"    {kind} {field} : {semantic};"
            for field, kind, semantic, _, _ in self.inputs
        ]
        out += [
            f"    {kind} {field} : {semantic};"
            for field, kind, semantic in self.extra_inputs
        ]
        out.append("};")
        out.append("struct Output {")
        out += [
            f"    {kind} {field} : {semantic};"
            for field, kind, semantic, _, _ in outputs
        ]
        out += [
            f"    {kind} {field} : {semantic};"
            for field, kind, semantic, _ in self.extra_outputs
        ]
        out.append("};")
        if preamble:
            out.append(preamble)
        # A table the address register indexes, as a choice of its rows by the
        # index: as an array of the defined constants, inside main or out,
        # fxc 10.0.22621 packed it into fewer components than are read (the
        # leaf cards' corner offsets, c60[a0.x].yz, were read as .x and .x,
        # and a tree's leaves were lost: 2026-10-22).
        for register, count in self._tables.items():
            first = int(register[1:])
            rows = [f"float4({', '.join(self.definitions[f'c{first + i}'])})" for i in range(count)]
            chosen = rows[-1]
            for i in range(count - 2, -1, -1):
                chosen = f"(i == {i} ? {rows[i]} : {chosen})"
            out.append(f"float4 {register}_table(int i) {{ return {chosen}; }}")
        out.append("Output main(Input input)")
        out.append("{")
        for register in sorted({r for _, _, _, r, _ in self.inputs}):
            out.append(f"    float4 {register} = 0;")
        out += [
            f"    {register}.{mask} = input.{field}{self._part(kind, mask)};"
            for field, kind, _, register, mask in self.inputs
        ]
        for register in sorted({r for _, _, _, r, _ in outputs}):
            out.append(f"    float4 {register} = 0;")
        for register in sorted(self._temporaries, key=lambda r: int(r[1:])):
            out.append(f"    float4 {register} = 0;")
        if any("a0" in s.hlsl for s in self.statements):
            out.append("    int4 a0 = 0;")
        for register, numbers in self.definitions.items():
            out.append(f"    const float4 {register} = float4({', '.join(numbers)});")
        for statement in self.statements:
            comment = f"  // {statement.asm}" if statement.asm else ""
            out.append(f"    {'    ' * statement.depth}{statement.hlsl}{comment}")
        out.append("    Output output;")
        out += [
            f"    output.{field} = {register}.{COMPONENTS[: self._width(kind)]};"
            for field, kind, _, register, _ in outputs
        ]
        out += [
            f"    output.{field} = {value};"
            for field, _, _, value in self.extra_outputs
        ]
        out.append("    return output;")
        out.append("}")
        return "\n".join(out) + "\n"


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    profile_only = argv[0] == "--profile"
    with open(argv[-1], encoding="utf-8") as f:
        shader = Shader(f.read())
    print(
        shader.profile if profile_only else shader.render(),
        end="\n" if profile_only else "",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
