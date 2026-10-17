// The game's folder: the one its executable's bin_ship is in. Not the
// plugin's, which a test loads from a folder of its own.
#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

inline std::filesystem::path GameFolder()
{
    std::wstring exe(MAX_PATH, wchar_t{});
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));
    return std::filesystem::path(exe).parent_path().parent_path();
}
