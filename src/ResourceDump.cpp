#include "ResourceDump.h"

#include "dao/Module.h"
#include "dao/Resources.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace resourcedump
{

namespace
{

constexpr std::string_view kListFile = "dump.txt";
constexpr std::string_view kDoneFile = "dump.done";
constexpr std::string_view kFolder = "dump";
// How many frames go by between looks for the list: two seconds or so.
constexpr int kEvery = 120;

int g_frames = 0;

// A resource's name as the game has them: letters, digits, an underscore, a
// hyphen, and a dot before its extension. Anything else in the list is not
// asked of the game and is not a file's name to write under.
bool IsName(std::string_view name)
{
    return !name.empty() && name.front() != '.' && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.';
    });
}

std::vector<std::string> NamesIn(const std::filesystem::path &list)
{
    std::vector<std::string> names;
    std::ifstream in(list);
    for (std::string line; std::getline(in, line);)
    {
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos)
        {
            continue;
        }
        const std::size_t last = line.find_last_not_of(" \t\r");
        names.push_back(line.substr(first, last - first + 1));
    }
    return names;
}

// Every name of a type the game has now (dao::resources::Names), written to
// names.<type>.txt beside the list, a name a line: how many.
std::size_t NamesWritten(const std::filesystem::path &beside, const std::string &type)
{
    if (!IsName(type))
    {
        return 0;
    }
    std::size_t count = 0;
    std::ofstream listed(beside / ("names." + type + ".txt"), std::ios::trunc);
    for (const std::wstring &name : dao::resources::Names(std::wstring(type.begin(), type.end())))
    {
        // The game's names are of its own narrow alphabet: a letter is a byte.
        std::string narrow;
        for (const wchar_t letter : name)
        {
            narrow.push_back(static_cast<char>(letter));
        }
        listed << narrow << '\n';
        ++count;
    }
    return count;
}

} // namespace

void Poll()
{
    if (++g_frames % kEvery != 0)
    {
        return;
    }
    const std::filesystem::path beside = dao::Module::OwnFolder();
    std::error_code error;
    if (!std::filesystem::exists(beside / kListFile, error))
    {
        return;
    }
    const std::vector<std::string> names = NamesIn(beside / kListFile);
    std::filesystem::create_directories(beside / kFolder, error);
    std::ofstream done(beside / kDoneFile, std::ios::trunc);
    std::size_t written = 0;
    for (const std::string &line : names)
    {
        // A name after a question mark is only asked after: whether the game has it.
        if (line.front() == '?')
        {
            const std::string asked = line.substr(1);
            const bool has = IsName(asked) && dao::resources::Exists(std::wstring(asked.begin(), asked.end()));
            done << "; " << (has ? "has" : "has not") << ": " << asked << '\n';
            continue;
        }
        // A type after a star asks for every name of that type the game has.
        if (line.front() == '*')
        {
            const std::string type = line.substr(1);
            done << "; names of " << type << ": " << NamesWritten(beside, type) << '\n';
            continue;
        }
        const std::string &name = line;
        std::optional<std::vector<std::byte>> bytes;
        if (IsName(name))
        {
            bytes = dao::resources::Read(std::wstring(name.begin(), name.end()));
        }
        if (!bytes)
        {
            done << "; none: " << name << '\n';
            continue;
        }
        std::ofstream out(beside / kFolder / name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        done << name << '\n';
        ++written;
    }
    done.close();
    std::filesystem::remove(beside / kListFile, error);
    spdlog::info("{}: {} of {} files written to {}", kListFile, written, names.size(), kFolder);
}

} // namespace resourcedump
