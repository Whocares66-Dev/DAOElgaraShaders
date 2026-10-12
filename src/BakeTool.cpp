// A level's light probes baked from the command line, with no game running:
// what Elgara Shaders's baker (ProbeBaker.h) makes of a level and how
// long it takes.
//
//   elgarashaders_bake <game folder> <level> [<out .bin>]
//
// The level is a folder's name under the game's packages\core\env, such as
// lak303d. With an out file the table is written there, as the plugin keeps
// one; every room baked is listed either way.
#include "ProbeBaker.h"

#include "dao/core/LightProbes.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        spdlog::error("usage: elgarashaders_bake <game folder> <level> [<out .bin>]");
        return 2;
    }
    spdlog::set_level(spdlog::level::debug);
    const std::filesystem::path game(argv[1]);
    const std::filesystem::path level(argv[2]);
    const auto began = std::chrono::steady_clock::now();
    const std::vector<probebaker::Room> rooms =
        probebaker::BakeNow(game / "packages" / "core" / "env" / level / (level.string() + ".rim"), game);
    std::size_t probes = 0;
    for (const probebaker::Room &room : rooms)
    {
        probes += room.probes.size();
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    spdlog::info("{}: {} rooms, {} probes, {} ms", level.string(), rooms.size(), probes, took.count());
    if (argc > 3)
    {
        const std::vector<std::byte> file = probebaker::Write(rooms);
        std::ofstream out(argv[3], std::ios::binary);
        out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
    }
    return rooms.empty() ? 1 : 0;
}
