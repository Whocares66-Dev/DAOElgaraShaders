// A Direct3D 9 shader's byte code as the game hands it to the device: its
// length, and the constants its compiler's table (the CTAB comment) names.
#pragma once

#include <Windows.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shadercode
{

// A constant of a shader's table: its name, first register and how many.
struct Constant
{
    std::string name;
    UINT first = 0;
    UINT count = 0;
};

// Returns the byte code up to and with its end token.
std::vector<std::byte> CodeOf(const DWORD *code);

// Returns the constants a shader's table names; none for code with no table.
std::vector<Constant> ConstantsOf(const std::vector<std::byte> &code);

// Returns the first register of the constant of this name, if the table names it.
std::optional<UINT> RegisterOf(const std::vector<std::byte> &code, std::string_view name);

// Returns whether a pixel shader's code writes its colour output `index`
// (oC0, oC1, ...): whether an instruction has it for its destination.
bool WritesColour(const std::vector<std::byte> &code, UINT index);

} // namespace shadercode
