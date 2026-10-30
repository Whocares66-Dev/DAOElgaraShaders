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
//   match=1        how far the level's light is brought to vanilla's bake in its
//                  mean brightness and colour; 0 leaves it as the fittings give it
//   bright=0       how far its brightness is brought to vanilla's bake too, by the bright end
//   caster=embree  what casts the rays: Embree, where the tool is built with
//                  it (the 64-bit one, tools/build-bake.ps1), or `tree`,
//                  DAOLib's own, a room at a time
//   cube=128       how many pixels a side the level's reflection cubes are
//                  made, each a DDS of vanilla's own name beside the
//                  lightmaps; 0 makes none
//   cubeview=<folder>   each cube written there too as a picture to look at
//                  with no game, <the cube's name>.png: four views from where
//                  the cube stands side by side (towards -y, +x, +y and -x),
//                  the ceiling over the first and the floor under it
//   glow=window:6,8,12   every part whose material's name has "window" in it
//                  glows that colour; as many of these as wanted. A fourth
//                  number, after another colon, is how much of that colour
//                  the thing's own surface is lit by (a crystal: its stone
//                  is lit by its lightmap; lava and glass are not)
//   flame=fire_cnd:0.5,0.3,0.12:12   every model placed whose name has
//                  "fire_cnd" in it is a flame of that light, carrying 12 m;
//                  flames together are one lamp, and their light falls off
//                  by the square of the distance; after the reach, how far
//                  above its place it burns, how far about it is clear, 1
//                  for a light that lands, and how many flames together are
//                  summed (10 if not said)
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
#include <cstdio>
#include <cstdlib>
#include <exception>
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

// Takes a value that is one number into the options; false for a name that
// is not one of those.
bool TakeNumber(relighter::Options &options, const std::string &name, const std::string &value)
{
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
        options.bounces = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
    }
    else if (name == "rays")
    {
        options.rays = std::strtoul(value.c_str(), nullptr, 10);
    }
    else if (name == "match")
    {
        options.match = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "bright")
    {
        options.bright = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "caster")
    {
        options.caster = value == "tree" ? 0 : 1;
    }
    else if (name == "kept")
    {
        options.kept = std::strtof(value.c_str(), nullptr);
    }
    else if (name == "cube")
    {
        options.cube = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    }
    else
    {
        return false;
    }
    return true;
}

// A value that says what it is of by a name, `<name>:<numbers>`: the name,
// and where in the value what follows its colon starts ("" with no colon).
struct Named
{
    std::wstring named;
    const char *after = "";
};

Named NamedIn(const std::string &value)
{
    const std::size_t colon = value.find(':');
    return {std::filesystem::path(value.substr(0, colon)).wstring(),
            colon == std::string::npos ? "" : value.c_str() + colon + 1};
}

relighter::Flame FlameOf(const std::string &value)
{
    const Named named = NamedIn(value);
    const char *after = named.after;
    relighter::Flame flame;
    flame.named = named.named;
    flame.colour = ColourAt(after);
    // The reach, then how far above its place it burns, then how far about it is clear.
    flame.reach = 12.0F;
    // And a fourth, 1 for a light that lands (a shaft through a roof), and a
    // fifth, how many of its flames together are summed: Relighter.h's Flame.
    for (float *number : {&flame.reach, &flame.lift, &flame.clear, &flame.lands, &flame.most})
    {
        if (*after != ':')
        {
            break;
        }
        char *end = nullptr;
        *number = std::strtof(after + 1, &end);
        after = end;
    }
    return flame;
}

relighter::Beam BeamOf(const std::string &value)
{
    const Named named = NamedIn(value);
    const char *after = named.after;
    relighter::Beam beam;
    beam.named = named.named;
    beam.colour = ColourAt(after);
    if (*after == ':')
    {
        ++after;
        beam.travels = ColourAt(after);
    }
    return beam;
}

// Takes the sky of the openings a beam of that name has, or of a beam of no light.
void TakeSky(relighter::Options &options, const std::string &value)
{
    const Named named = NamedIn(value);
    const char *after = named.after;
    auto beam = std::ranges::find(options.beams, named.named, &relighter::Beam::named);
    if (beam == options.beams.end())
    {
        options.beams.push_back({named.named, {}, {0, 0, -1}, {}});
        beam = options.beams.end() - 1;
    }
    beam->sky = ColourAt(after);
}

relighter::Glow GlowOf(const std::string &value)
{
    const Named named = NamedIn(value);
    const char *after = named.after;
    relighter::Glow glow;
    glow.named = named.named;
    glow.colour = ColourAt(after);
    glow.own = *after == ':' ? std::strtof(after + 1, nullptr) : 0.0F;
    return glow;
}

// Takes one value into the options; false for a name there is none of.
bool Take(relighter::Options &options, const std::string &name, const std::string &value)
{
    if (TakeNumber(options, name, value))
    {
        return true;
    }
    if (name == "flame")
    {
        options.flames.push_back(FlameOf(value));
    }
    else if (name == "beam")
    {
        options.beams.push_back(BeamOf(value));
    }
    else if (name == "sky")
    {
        TakeSky(options, value);
    }
    else if (name == "param" || name == "model" || name == "texture")
    {
        // A level's tints, which are tools/tint_level.py's to read.
    }
    else if (name == "glow")
    {
        options.glows.push_back(GlowOf(value));
    }
    else if (name == "cubeview")
    {
        options.cubeview = value;
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

// The command itself.
// @return 0 for a level lit again, 1 for one that was not, 2 for a command not understood
int Run(int argc, char **argv)
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

} // namespace

int main(int argc, char **argv)
{
    // What cannot be done may be said by an exception, memory that runs out
    // above all: it is said here in a line, where it was the runtime's to
    // end the program with none (0xC0000409). By the C library, which throws
    // nothing: the log makes its line in memory it asks for.
    try
    {
        return Run(argc, argv);
    }
    catch (const std::exception &error)
    {
        std::fputs("elgarashaders_relight: ", stderr);
        std::fputs(error.what(), stderr);
        std::fputs("\n", stderr);
        return 3;
    }
}
