// Bakes a level's light probes from the level's own files while the game is
// played, on a thread of its own (DAOLib #65). Graphics.cpp says which level
// the player has come into; the baker reads that level's rooms, models and
// lightmaps from the archives on disk, bakes a room at a time, and hands the
// level's probes over when all are done. A level baked once is kept in a
// file, which is read when the player next comes into the level.
//
// Nothing of the engine is called from the baker's thread: it reads files.
#pragma once

#include "dao/core/LightProbes.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace probebaker
{

// The probes baked for one of a level's own: a room's, or in a level made of
// terrain those of its tiles that are nearest that probe.
struct Room
{
    dao::core::Point key{}; // where the level's own probe is
    // That probe's light as the level's files have it. Two levels have a
    // probe at one place, a village by day and by night, and this tells
    // their rooms apart.
    dao::core::probes::Irradiance own{};
    // Whether a probe here holds all the light of its place, the lamps' own
    // with the rest, or what a level's own probe holds: what the surfaces
    // about send on, and the sky.
    bool whole = false;
    std::vector<dao::core::probes::Probe> probes;

    // A grid over the probes, seen from above, so that the nearest few are
    // found without every probe looked at: `order` is the probes by cell,
    // row by row, and a cell's are those from its number in `starts` to the
    // next one's.
    float lowX = 0;
    float lowY = 0;
    std::uint32_t across = 0;
    std::uint32_t down = 0;
    std::vector<std::uint32_t> starts;
    std::vector<std::uint32_t> order;
};

using Rooms = std::vector<std::shared_ptr<const Room>>;

// Starts the baker.
// @param kept the folder a level's table is kept in once baked, as <level>.bin
// @param game the game's folder, whose packages\core\data has the models
void Start(const std::filesystem::path &kept, const std::filesystem::path &game);

// Says the player has come into a level: the path of its .rim, as the engine
// opens it. Its table is read if one is kept, and baked if not. Call it from
// any thread; it returns at once.
void Want(const std::filesystem::path &levelRim);

// Returns a level's rooms baked here and now, on the calling thread: what
// the baker's thread does for a level, for a tool that bakes with no game.
// @return no rooms if the level's archives cannot be read
std::vector<Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game);

// Returns a table's file, as a level's is kept.
std::vector<std::byte> Write(const std::vector<Room> &rooms);

// Returns every room there are probes for now. The answer is whole and
// never changes; ask again for the rooms baked since.
std::shared_ptr<const Rooms> AllRooms();

// Adds to `out` those of a room's probes that can be among the nearest few
// to a place, each with one over its distance for its score: every probe
// nearer than the nearest few's furthest is among them.
// @param place three floats
void Nearest(const Room &room, const float *place, std::vector<dao::core::probes::Near> &out);

} // namespace probebaker
