// A level lit again from the command line, with no game running: what
// Elgara Shaders' relighter (Relighter.h) makes of a level.
//
//   elgarashaders_relight <game folder> <level> <out folder> [<file .ini>] [name=value ...]
//
// The level is a folder's name under the game's packages\core\env, such as
// lak303d. The lightmaps are written into the out folder, each a DDS of its
// atlas's own name: put in a plugin's override folder, they are what the
// game lights the level by. The values, each with what it is when left out:
//
//   side=256       a lightmap is made no larger a side
//   lamps=1        what a lamp's own colour is multiplied by
//   reach=1        and its reach
//   furthest=60    a lamp that reaches further than this, in metres, is left out
//   surfaces=0.5   of the light on a surface, what it sends on
//   bounces=2      how many times light is followed off a surface
//   rays=48        cast from a texel for what comes back
//   kept=0         of the game's own baked light, what is laid under ours
//   caster=embree  what casts the rays: Embree, where the tool is built with
//                  it (the 64-bit one, tools/build-bake.ps1), or `tree`,
//                  DAOLib's own, a room at a time
//   glow=window:6,8,12   every part whose material's name has "window" in it
//                  glows that colour; as many of these as wanted. A fourth
//                  number, after another colon, is how much of that colour
//                  the thing's own surface is lit by (a crystal: its stone
//                  is lit by its lightmap; lava and glass are not)
//   flame=fire_cnd:0.5,0.3,0.12:12   every model placed whose name has
//                  "fire_cnd" in it is a flame of that light, carrying 12 m;
//                  flames together are one lamp, and their light falls off
//                  by the square of the distance
//
//   beam=window:0.5,0.6,0.85:0.5,0.3,-0.8   a light from far off, the moon,
//                  that travels that way and comes in through every part
//                  whose material's name has "window" in it
//   sky=window:2.2,2.8,4   the night sky's own light through the same
//                  openings, every one of them, into the room and downward
//   param= model= texture=   a level's tints, passed over here: they are
//                  tools/tint_level.py's, which tools/relight.ps1 runs after this
//
// A file (data/relight.ini) has the same values a line each, `name = value`,
// under [default] for every level and under [<level>] for one; the level's
// are read after the default's, and the command's own after both.
#include "Relighter.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

// A colour of three numbers with commas between, read from `at`, which is
// left where the colour ends.
std::array<float, 3> ColourAt(const char *&at)
{
    std::array<float, 3> colour{};
    for (float &channel : colour)
    {
        char *end = nullptr;
        channel = std::strtof(at, &end);
        at = *end == ',' ? end + 1 : end;
    }
    return colour;
}

// Takes one value into the options; false for a name there is none of.
bool Take(relighter::Options &options, const std::string &name, const std::string &value)
{
    const std::size_t colon = value.find(':');
    const char *after = colon == std::string::npos ? "" : value.c_str() + colon + 1;
    if (name == "side")
    {
        options.side = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    }
    else if (name == "lamps")
    {
        options.lamps = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "reach")
    {
        options.reach = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "furthest")
    {
        options.furthest = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "surfaces")
    {
        options.surfaces = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "bounces")
    {
        options.bounces = std::atoi(value.c_str());
    }
    else if (name == "rays")
    {
        options.rays = std::strtoul(value.c_str(), nullptr, 10);
    }
    else if (name == "caster")
    {
        options.caster = value == "tree" ? 0 : 1;
    }
    else if (name == "kept")
    {
        options.kept = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "flame")
    {
        relighter::Flame flame;
        flame.named = std::filesystem::path(value.substr(0, colon)).wstring();
        flame.colour = ColourAt(after);
        // The reach, then how far above its place it burns, then how far about it is clear.
        flame.reach = 12.0F;
        for (float *number : {&flame.reach, &flame.lift, &flame.clear})
        {
            if (*after != ':')
            {
                break;
            }
            char *end = nullptr;
            *number = std::strtof(after + 1, &end);
            after = end;
        }
        options.flames.push_back(std::move(flame));
    }
    else if (name == "beam")
    {
        relighter::Beam beam;
        beam.named = std::filesystem::path(value.substr(0, colon)).wstring();
        beam.colour = ColourAt(after);
        if (*after == ':')
        {
            ++after;
            beam.travels = ColourAt(after);
        }
        options.beams.push_back(std::move(beam));
    }
    else if (name == "sky")
    {
        // The sky of the openings a beam of that name has, or of a beam of no light.
        const std::wstring named = std::filesystem::path(value.substr(0, colon)).wstring();
        auto beam = std::ranges::find(options.beams, named, &relighter::Beam::named);
        if (beam == options.beams.end())
        {
            options.beams.push_back({named, {}, {0, 0, -1}, {}});
            beam = options.beams.end() - 1;
        }
        beam->sky = ColourAt(after);
    }
    else if (name == "param" || name == "model" || name == "texture")
    {
        // A level's tints, which are tools/tint_level.py's to read.
    }
    else if (name == "glow")
    {
        relighter::Glow glow;
        glow.named = std::filesystem::path(value.substr(0, colon)).wstring();
        glow.colour = ColourAt(after);
        glow.own = *after == ':' ? std::strtof(after + 1, nullptr) : 0.0F;
        options.glows.push_back(std::move(glow));
    }
    else
    {
        return false;
    }
    return true;
}

std::string Trimmed(const std::string &text)
{
    const std::size_t first = text.find_first_not_of(" \t\r");
    const std::size_t last = text.find_last_not_of(" \t\r");
    return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
}

// Takes a file's values for a level: [default]'s, then the level's own.
// @return whether the file has a section for the level
bool TakeFile(relighter::Options &options, const std::filesystem::path &file, const std::string &level)
{
    bool has = false;
    for (const std::string &wanted : {std::string("default"), level})
    {
        std::ifstream in(file);
        std::string section;
        for (std::string line; std::getline(in, line);)
        {
            line = Trimmed(line.substr(0, line.find(';')));
            if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                has = has || section == level;
                continue;
            }
            const std::size_t equals = line.find('=');
            if (section != wanted || equals == std::string::npos)
            {
                continue;
            }
            const std::string name = Trimmed(line.substr(0, equals));
            if (!Take(options, name, Trimmed(line.substr(equals + 1))))
            {
                spdlog::warn("{}: no such value: {}", file.string(), name);
            }
        }
    }
    return has;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        spdlog::error("usage: elgarashaders_relight <game folder> <level> <out folder> [<file .ini>] [name=value ...]");
        return 2;
    }
    const std::filesystem::path game(argv[1]);
    const std::filesystem::path level(argv[2]);
    relighter::Options options;
    for (int i = 4; i < argc; ++i)
    {
        const std::string word(argv[i]);
        if (word == "debug") // every opening, lamp and room said
        {
            spdlog::set_level(spdlog::level::debug);
            continue;
        }
        const std::size_t equals = word.find('=');
        if (equals == std::string::npos)
        {
            if (!TakeFile(options, word, level.string()))
            {
                spdlog::error("{} has nothing for {}", word, level.string());
                return 2;
            }
            continue;
        }
        if (!Take(options, word.substr(0, equals), word.substr(equals + 1)))
        {
            spdlog::error("no such value: {}", word.substr(0, equals));
            return 2;
        }
    }
    const std::size_t written = relighter::Relight(
        game / "packages" / "core" / "env" / level / (level.string() + ".rim"), game, argv[3], options);
    return written != 0 ? 0 : 1;
}
