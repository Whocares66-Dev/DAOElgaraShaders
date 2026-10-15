// The materials' metalness, normal-map spread and clear solids' numbers
// (shaders/materials.hlsl's mml_fDaoMetal, mml_tDaoMetal, mml_vDaoBumps,
// mml_vDaoClear) added to each material object as the game reads it, from
// the table's answers beside the DLL (materials.answers; issue 19). The game
// resolves its own copy of a material, the main campaign's, Awakening's, an
// add-in's or a mod's, and that copy gets its own answer, known by its bytes'
// hash, or none: no file of ours replaces the game's.
#pragma once

#include <filesystem>

namespace materialpatch
{

// Reads the answers in `folder` and asks DAOLib to hand every material
// object the game reads through them (dao::resources::OnLoad). For
// DAOLib_Load: before the game reads anything.
void Start(const std::filesystem::path &folder);

// Logs how many material objects were given an answer and how many had
// none, when that has changed. For dao::OnFrame.
void LogCounts();

} // namespace materialpatch
