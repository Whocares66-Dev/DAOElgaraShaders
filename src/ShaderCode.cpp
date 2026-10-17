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

} // namespace shadercode
