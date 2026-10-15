// Materials the table does not know, worked out in play and kept (issue 19;
// the user, 2026-10-15: "shouldn't we automatically detect & apply
// metallicity"). A mod's armour has no answer in materials.answers, so it was
// drawn as no metal (the Grey Warden runic plate).
//
// Eagerly, so that a player never restarts to see a material right (the
// user: "how shit would it be to have to constantly reload just to see new
// materials?"): in the main menu and after a load into another campaign,
// every material object the game has is read once through the game, so the load patch meets the
// unknown ones before they are drawn; their textures are read through the
// game too, within a budget a frame, and the table's own rule
// (materials/Learn.h) works each out on a thread of its own. An answer is
// given to every later read in the same session (one with a map once the
// game finds it, in a folder it watches), and kept in the player's
// profile, <Documents>\BioWare\Dragon Age\daolib\elgarashaders\materials,
// for the next.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace materiallearn
{

// Reads the answers learned in earlier sessions and adds their maps' folder
// to the places the game finds files in. Call it from DAOLib_Load.
// @param plugin the plugin's folder, which has the names' tables
void Start(const std::filesystem::path &plugin);

// A copy's bytes with the answer learned for it, or nullopt where none is
// learned or it was learned to need nothing. Any thread.
std::optional<std::vector<std::byte>> Patched(std::span<const std::byte> bytes);

// Whether a copy has been learned, whatever its answer. Any thread.
bool Knows(std::span<const std::byte> bytes);

// Asks for a material object the game has read and no answer is known for to
// be worked out. Any thread, as the game reads; it only keeps the bytes.
void Notice(std::wstring_view name, std::span<const std::byte> bytes);

// The game thread's part, each frame: finds the material objects not yet
// read, reads them, and reads the textures of those noticed, within a budget.
void Poll();

} // namespace materiallearn
