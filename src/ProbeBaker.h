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
#include <span>
#include <string>
#include <vector>

namespace levelfiles
{
struct Geometry;
}

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
    // How bright the baked light is that the room's probes look out on: the
    // mean, over every ray of theirs that landed, of the lightmap there. And
    // the same over the whole level the room is of, which is the middle the
    // look's LightContrast turns about (Graphics.cpp).
    float seen = 0;
    float levelSeen = 0;
    // Whether the level has lightmaps of our own (Relighter.h), which its
    // probes were then baked from: such a level wants none of its ambient
    // colour, and its lamps are in its lightmaps (Graphics.cpp).
    bool relit = false;
    // The level the room is of, by its folder's name (lak303d). Not kept in a
    // table: said as a level's rooms are read or baked.
    std::string level;
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
// @param relit a folder to look in for lightmaps of our own before a level's
void Start(const std::filesystem::path &kept, const std::filesystem::path &game, const std::filesystem::path &relit);

// Says the player has come into a level: the path of its .rim, as the engine
// opens it. Its table is read if one is kept, and baked if not. Call it from
// any thread; it returns at once.
void Want(const std::filesystem::path &levelRim);

// Returns a level's rooms baked here and now, on the calling thread: what
// the baker's thread does for a level, for a tool that bakes with no game.
// @return no rooms if the level's archives cannot be read
std::vector<Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game,
                          const std::filesystem::path &relit = {});

// Returns the places to stand on a tile of a level of terrain, a metre above
// every surface of it that faces up and has free air over it, over a grid
// that is the whole level's.
// @param about the tile's own geometry first, then that of the tiles about it
// @param spacing metres between the grid's points
std::vector<dao::core::Point> PlacesOn(std::span<const levelfiles::Geometry *const> about, float spacing);

// Returns a table's file, as a level's is kept.
// @param stamp what says which lightmaps of our own it was baked from, 0 for the level's
std::vector<std::byte> Write(const std::vector<Room> &rooms, std::uint64_t stamp = 0);

// Returns every room there are probes for now. The answer is whole and
// never changes; ask again for the rooms baked since.
std::shared_ptr<const Rooms> AllRooms();

// Returns where the lamps of the levels lit again are, every such level's that
// has been come into, as their bakes said them (<level>.lamps): three floats
// a lamp. The answer is whole and never changes; ask again for more.
std::shared_ptr<const std::vector<float>> BakedLamps();

// Adds to `out` those of a room's probes that can be among the nearest few
// to a place, each with one over its distance for its score: every probe
// nearer than the nearest few's furthest is among them.
// @param place three floats
void Nearest(const Room &room, const float *place, std::vector<dao::core::probes::Near> &out);

} // namespace probebaker
