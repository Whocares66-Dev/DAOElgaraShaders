// Writes files of the game's out as the game itself reads them, for the tools
// that are run on a developer's machine: the add-ins' archives are enciphered
// with a key of each add-in's own (ERF V2.2, flags 0x21: read 2026-10-12), so
// the table's tool (elgarashaders_materials) cannot read their material
// objects and textures from disk as it reads the game's own, and the game can.
//
// Asked for by a file beside the DLL, `dump.txt`, a file's name a line
// (`h_cailanprca_0.mao`; `?name` only asks whether the game has it, and
// `dump.done` says; `*mao` writes every name of that type the game has to
// `names.mao.txt`: tools\dump_addins.py asks in that order). While the game runs, each is read through the
// game's resource manager (dao::resources::Read) and written into `dump\`
// beside the DLL; `dump.done` then lists what was written and what the game
// had none of, and `dump.txt` is taken away. Nothing is done without the
// file, and nothing here is for a player.
#pragma once

namespace resourcedump
{

// Looks for the list and writes what it names. For dao::OnFrame: it is the
// game's thread, which the game's resource manager is read on.
void Poll();

} // namespace resourcedump
