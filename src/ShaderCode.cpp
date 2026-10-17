#include "ShaderCode.h"

#include <cstdint>
#include <cstring>

namespace shadercode
{

// The constants a shader's table (the compiler's CTAB comment) names.
std::vector<Constant> ConstantsOf(const std::vector<std::byte> &code)
{
    std::vector<Constant> constants;
    const std::string_view text(reinterpret_cast<const char *>(code.data()), code.size());
    const std::size_t tag = text.find("CTAB");
    if (tag == std::string_view::npos)
    {
        return constants;
    }
    // D3DXSHADER_CONSTANTTABLE after the tag: its size, creator, version, how
    // many constants and where their D3DXSHADER_CONSTANTINFO are, offsets
    // from the table's start; each of those its name's offset, register set
    // and index (two halves), count and reserved (two halves), type and
    // default.
    const std::size_t table = tag + 4;
    const auto word = [&](std::size_t at) -> std::uint32_t {
        std::uint32_t value = 0;
        if (at + sizeof value <= code.size())
        {
            std::memcpy(&value, code.data() + at, sizeof value);
        }
        return value;
    };
    const std::uint32_t count = word(table + 12);
    const std::uint32_t infos = word(table + 16);
    constexpr std::size_t kInfoSize = 20;
    for (std::uint32_t i = 0; i < count && i < 512; ++i)
    {
        const std::size_t info = table + infos + (i * kInfoSize);
        const std::size_t named = table + word(info);
        if (named >= code.size())
        {
            continue;
        }
        constants.push_back({std::string(text.data() + named, std::strlen(text.data() + named)), word(info + 4) >> 16,
                             word(info + 8) & 0xFFFF});
    }
    return constants;
}

// The register a shader's constant table gives a constant of this name, if it
// names it.
std::optional<UINT> RegisterOf(const std::vector<std::byte> &code, std::string_view name)
{
    for (const Constant &constant : ConstantsOf(code))
    {
        if (constant.name == name)
        {
            return constant.first;
        }
    }
    return std::nullopt;
}

// The byte code up to its end token.
std::vector<std::byte> CodeOf(const DWORD *code)
{
    constexpr DWORD kEnd = 0x0000FFFF;
    constexpr std::size_t kMost = 1 << 20;
    std::size_t words = 0;
    while (words < kMost && code[words] != kEnd)
    {
        ++words;
    }
    const auto *bytes = reinterpret_cast<const std::byte *>(code);
    return {bytes, bytes + ((words + 1) * sizeof(DWORD))};
}

// Notes on WritesColour: Direct3D 9's shader tokens (d3d9types.h). After the
// version token each instruction is an opcode token, its parameters' count in
// bits 24 to 27 from shader model 2 on, and a comment (0xFFFE) its length in
// bits 16 to 30; the end is 0x0000FFFF. A parameter's register type is bits 28
// to 30 and 11 to 12 (the high two), its number bits 0 to 10. The destination
// is an instruction's first parameter, but a declaration's second (dcl), and
// a def's first is followed by its literal numbers, which are not registers.
bool WritesColour(const std::vector<std::byte> &code, UINT index)
{
    constexpr std::uint32_t kComment = 0xFFFE;
    constexpr std::uint32_t kEnd = 0x0000FFFF;
    constexpr std::uint32_t kDcl = 0x1F;
    constexpr std::uint32_t kColourOut = 8; // D3DSPR_COLOROUT
    const std::size_t words = code.size() / sizeof(std::uint32_t);
    const auto word = [&](std::size_t at) {
        std::uint32_t value = 0;
        std::memcpy(&value, code.data() + (at * sizeof value), sizeof value);
        return value;
    };
    for (std::size_t at = 1; at < words;)
    {
        const std::uint32_t token = word(at);
        if (token == kEnd)
        {
            break;
        }
        const std::uint32_t opcode = token & 0xFFFF;
        if (opcode == kComment)
        {
            at += 1 + ((token >> 16) & 0x7FFF);
            continue;
        }
        const std::uint32_t length = (token >> 24) & 0xF;
        const std::size_t destination = at + (opcode == kDcl ? 2 : 1);
        if (length > 0 && destination < words && destination <= at + length)
        {
            const std::uint32_t parameter = word(destination);
            const std::uint32_t type = ((parameter >> 28) & 0x7) | ((parameter >> 8) & 0x18);
            if (type == kColourOut && (parameter & 0x7FF) == index)
            {
                return true;
            }
        }
        at += 1 + length;
    }
    return false;
}

} // namespace shadercode
