// Bakes a level's light probes from the level's own files while the game is
// played, on a thread of its own (DAOLib #65). Graphics.cpp says which level
// the player has come into; the baker reads that level's rooms, models and
// lightmaps from the archives on disk, bakes a room at a time, and hands
// each room's probes over as they are done. A level baked once is kept in a
// file and read at the next start.
//
// Nothing of the engine is called from the baker's thread: it reads files.
#pragma once

#include "dao/core/LightProbes.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace probebaker
{

// Starts the baker, with the tables kept in `kept` read first.
// @param kept the folder a level's table is kept in once baked, as <level>.bin
// @param game the game's folder, whose packages\core\data has the models
void Start(const std::filesystem::path &kept, const std::filesystem::path &game);

// Says the player has come into a level: the path of its .rim, as the engine
// opens it. The level is baked unless a table for it is already had. Call it
// from any thread; it returns at once.
void Want(const std::filesystem::path &levelRim);

// Returns a level's rooms baked here and now, on the calling thread: what
// the baker's thread does for a level, for a tool that bakes with no game.
// @return no rooms if the level's archives cannot be read
std::vector<dao::core::probes::Room> BakeNow(const std::filesystem::path &levelRim, const std::filesystem::path &game);

// Returns every room there are probes for now. The answer is whole and
// never changes; ask again for the rooms baked since.
std::shared_ptr<const std::vector<dao::core::probes::Room>> Rooms();

} // namespace probebaker
