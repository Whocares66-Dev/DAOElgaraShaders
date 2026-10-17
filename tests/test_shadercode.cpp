#include "../src/ShaderCode.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace
{

std::vector<std::byte> Code(std::initializer_list<std::uint32_t> tokens)
{
    std::vector<std::byte> code(tokens.size() * sizeof(std::uint32_t));
    std::memcpy(code.data(), std::data(tokens), code.size());
    return code;
}

// ps_3_0, then `mov oC<n>, c0` (oC is register type 8: its high two bits at 11
// and 12), then the end.
constexpr std::uint32_t kVersion = 0xFFFF0300;
constexpr std::uint32_t kMov = 0x02000001;
constexpr std::uint32_t kConstantC0 = 0xA0E40000;
constexpr std::uint32_t kEnd = 0x0000FFFF;
constexpr std::uint32_t ColourOut(std::uint32_t index)
{
    return 0x800F0800 | index;
}

} // namespace

TEST_CASE("a pixel shader's colour outputs are read from its instructions' destinations")
{
    const auto second = Code({kVersion, kMov, ColourOut(1), kConstantC0, kEnd});
    CHECK(shadercode::WritesColour(second, 1));
    CHECK_FALSE(shadercode::WritesColour(second, 0));

    const auto first = Code({kVersion, kMov, ColourOut(0), kConstantC0, kEnd});
    CHECK(shadercode::WritesColour(first, 0));
    CHECK_FALSE(shadercode::WritesColour(first, 1));
}

TEST_CASE("a comment's words and a def's numbers are not read as registers")
{
    // A comment of two words, the second shaped as oC1, then a def of c0 whose
    // literal x is shaped as oC1 too: neither writes it.
    const auto code = Code({kVersion, 0x0002FFFE, 0, ColourOut(1), 0x05000051, 0xA00F0000, ColourOut(1), 0, 0, 0, kMov,
                            ColourOut(0), kConstantC0, kEnd});
    CHECK_FALSE(shadercode::WritesColour(code, 1));
    CHECK(shadercode::WritesColour(code, 0));
}
