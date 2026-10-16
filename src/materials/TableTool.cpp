// elgarashaders_materials: the materials' table, made and given to the plugin.
//
//   elgarashaders_materials table   [options]           data/materials.ini made again from the game
//   elgarashaders_materials objects <plugin> [options]  materials.answers, materials.names and the maps
//                                                       into a plugin's folder, from the table as it stands
//   elgarashaders_materials why <regex> [options]       each material so named: what its names say, the
//                                                       rule's share and the class, and its copies
//   elgarashaders_materials enciphered [type] [options] the files of a type (mao unless said) some copy
//                                                       of is in an enciphered archive, a line each
//   elgarashaders_materials unreadable <folder>         the textures the material objects in a folder name
//                                                       that the install has no copy of to read with no
//                                                       game, a line each (tools/dump_addins.py asks both)
//
// Options: --game <install> (DAO_GAME_DIR, or GOG's), --ini <table> (data/materials.ini), --names
// <tables> (data/material_names.txt), --addins <folder> (build/addins: the add-ins' files read out
// of the running game by tools/dump_addins.py, where the install's are enciphered).
//
// What tools/material_table.py did in Python until 2026-10-15 (Table.h has why it went).
#include "materials/Table.h"

#include <ShlObj.h>
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{

constexpr std::string_view kGog = R"(C:\Program Files (x86)\GOG Galaxy\Games\Dragon Age Origins)";

struct Settings
{
    std::string command;
    std::string argument;
    std::filesystem::path game;
    std::filesystem::path ini = "data/materials.ini";
    std::filesystem::path names = "data/material_names.txt";
    std::filesystem::path addins = "build/addins";
    std::filesystem::path documents;
};

std::optional<Settings> Parse(int argc, char **argv)
{
    Settings out;
    out.game = std::filesystem::path(kGog);
    char *fromEnvironment = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&fromEnvironment, &length, "DAO_GAME_DIR") == 0 && fromEnvironment != nullptr)
    {
        out.game = fromEnvironment;
    }
    std::free(fromEnvironment); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s's buffer is malloc's
    std::vector<std::string> words(argv + 1, argv + argc);
    for (std::size_t i = 0; i < words.size(); ++i)
    {
        const std::string &word = words[i];
        const bool valued = i + 1 < words.size();
        if (word == "--game" && valued)
        {
            out.game = words[++i];
        }
        else if (word == "--ini" && valued)
        {
            out.ini = words[++i];
        }
        else if (word == "--names" && valued)
        {
            out.names = words[++i];
        }
        else if (word == "--addins" && valued)
        {
            out.addins = words[++i];
        }
        else if (word == "--documents" && valued)
        {
            out.documents = words[++i];
        }
        else if (out.command.empty())
        {
            out.command = word;
        }
        else if (out.argument.empty())
        {
            out.argument = word;
        }
        else
        {
            return std::nullopt;
        }
    }
    const bool wantsArgument = out.command == "objects" || out.command == "why" || out.command == "unreadable";
    const bool bare = out.command == "table" || out.command == "enciphered";
    // enciphered's type is optional: material objects unless one is given
    if (out.command == "enciphered" && out.argument.empty())
    {
        out.argument = "mao";
    }
    if ((!bare && !wantsArgument) || (wantsArgument && out.argument.empty()))
    {
        return std::nullopt;
    }
    return out;
}

std::string ReadText(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

bool WriteText(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(file);
}

// The maps into the override folder, and those of an earlier run that are
// not asked for now taken away (materials.made lists what was written).
void WriteMaps(const std::filesystem::path &override, const materials::Objects &objects)
{
    std::filesystem::create_directories(override);
    const auto listed = override / "materials.made";
    std::set<std::string> before;
    {
        std::ifstream file(listed);
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty())
            {
                before.insert(line);
            }
        }
    }
    std::string made;
    std::set<std::string> now;
    for (const auto &[name, bytes] : objects.maps)
    {
        std::ofstream file(override / name, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        now.insert(name);
        made += name + "\n";
    }
    for (const auto &old : before)
    {
        std::error_code ignored;
        if (!now.contains(old))
        {
            std::filesystem::remove(override / old, ignored);
        }
    }
    WriteText(listed, made);
}

// Documents\BioWare\Dragon Age, whose AddIns.xml says which campaigns the
// install's add-ins extend: --documents, or the user's own.
std::filesystem::path Documents(const Settings &settings)
{
    if (!settings.documents.empty())
    {
        return settings.documents;
    }
    PWSTR known = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &known)))
    {
        out = std::filesystem::path(known) / "BioWare" / "Dragon Age";
    }
    CoTaskMemFree(known);
    return out;
}

// The textures the material objects in a folder name, in the slots the
// rule and the bumps read, that the install has no readable copy of.
int Unreadable(const materials::Catalogue &catalogue, const std::filesystem::path &folder)
{
    std::set<std::string> out;
    std::error_code failed;
    for (const auto &entry : std::filesystem::directory_iterator(folder, failed))
    {
        if (entry.path().extension() != ".mao")
        {
            continue;
        }
        const auto object = materials::ReadMaterialObject(entry.path().stem().string(), ReadText(entry.path()));
        for (const char *slot : {"mml_tDiffuse", "mml_tSpecularMask", "mml_tNormalMap", "mml_tTintMask"})
        {
            const std::string resource = object ? object->Texture(slot) : std::string{};
            if (!resource.empty() && !catalogue.Readable(resource))
            {
                out.insert(resource);
            }
        }
    }
    for (const auto &resource : out)
    {
        std::cout << resource << "\n";
    }
    return failed ? 1 : 0;
}

int Run(int argc, char **argv)
{
    const auto settings = Parse(argc, argv);
    if (!settings)
    {
        std::cerr
            << "elgarashaders_materials table | objects <plugin folder> | why <regex> | enciphered [type] |"
               " unreadable <folder>"
               " [--game <install>] [--documents <folder>] [--ini <table>] [--names <tables>] [--addins <folder>]\n";
        return 2;
    }
    materials::Names names;
    std::string error;
    if (!names.Parse(ReadText(settings->names), error))
    {
        std::cerr << settings->names.string() << ": " << error << "\n";
        return 2;
    }
    std::cerr << "reading " << settings->game.string() << "\n";
    const auto catalogue = materials::Catalogue::Load(settings->game, Documents(*settings), settings->addins, error);
    if (!catalogue)
    {
        std::cerr << error << "\n";
        return 2;
    }
    if (settings->command == "enciphered")
    {
        for (const auto &name : catalogue->Enciphered("." + settings->argument))
        {
            std::cout << name << "\n";
        }
        return 0;
    }
    if (settings->command == "unreadable")
    {
        return Unreadable(*catalogue, settings->argument);
    }
    std::cout << catalogue->Materials().size() << " solid materials\n";

    if (settings->command == "table")
    {
        const auto hands = materials::ReadHands(settings->ini);
        const std::string text = materials::MakeTable(*catalogue, names, hands, [](std::size_t done, std::size_t of) {
            std::cout << done << " of " << of << "\n";
        });
        if (!WriteText(settings->ini, text))
        {
            std::cerr << "could not write " << settings->ini.string() << "\n";
            return 1;
        }
        std::cout << "wrote " << settings->ini.string() << " (" << hands.size() << " lines by hand kept)\n";
        return 0;
    }
    if (settings->command == "objects")
    {
        const std::filesystem::path plugin = settings->argument;
        const auto objects = materials::MakeObjects(*catalogue, names, materials::ReadClasses(settings->ini));
        WriteMaps(plugin / "override", objects);
        const bool written = WriteText(plugin / "materials.answers", objects.answers) &&
                             WriteText(plugin / "materials.names", ReadText(settings->names));
        std::cout << "materials:";
        for (const auto &[what, count] : objects.counts)
        {
            std::cout << " " << count << " " << what << ";";
        }
        std::cout << " into " << plugin.string() << "\n";
        return written ? 0 : 1;
    }
    const std::regex wanted(settings->argument);
    for (const auto &[name, material] : catalogue->Materials())
    {
        if (!std::regex_search(name, wanted))
        {
            continue;
        }
        const auto classed = materials::Classify(*catalogue, names, material.main);
        std::printf("%-34s %-8s names say %s%s%s", name.c_str(), classed.cls.c_str(), classed.verdict.cls.c_str(),
                    classed.verdict.fallback.empty() ? "" : ", else ", classed.verdict.fallback.c_str());
        if (classed.share)
        {
            std::printf("; %.0f%% of it painted as metal", *classed.share * 100);
        }
        std::printf("\n");
        for (const auto &copy : material.copies)
        {
            std::printf("    %016llx %s (%s)\n", static_cast<unsigned long long>(copy.hash), copy.where.c_str(),
                        copy.campaign.c_str());
        }
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    // An exception, running out of memory above all, is said here in a line,
    // where the runtime would end the program with none; by the C library,
    // which throws nothing.
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception &error)
    {
        std::fputs("elgarashaders_materials: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return 3;
    }
}
