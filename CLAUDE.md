# DAOElgaraShaders -- project context

A graphics plugin for Dragon Age: Origins built on DAOLib (the native plugin loader and library, `C:\project\DAOLib`): better lighting, shadows, colours and materials through the game's own Direct3D 9 pipeline. It began as DAOLib's `examples/graphics` on the branch `gh-65-post-effects`; Issue 1 here (it was DAOLib's 65, and the code's comments call it #65) is the record of everything found and tried until 2026-10-11, with pictures. `dev/NOTES.md` is what was learned that the code's comments do not say in one place.

Markdown here is not hard-wrapped: one line per paragraph or list item.

## The goal -- the user's words

Looking good is the goal: more realistic lighting, better materials, better shadows and colours. **Vanilla is the "before" picture, not the target** (the user, 2026-10-11). Judge a frame by what is wrong in it (a black figure, orange hair, white skin), not by how far it is from vanilla. Improve on vanilla without swinging an area's brightness to an extreme. The unmodified game is called "vanilla", never "the game's own".

The bar, since 2026-10-11: "something on the quality of a Minecraft visual overhaul" (the user, with a shader pack's before and after: a low sun, lit haze, soft shadows, bloom). The light probes were judged "good infra work" that "doesn't seem to make any significant visual difference"; what is wanted is "some kind of lighting overhaul to really make the scenes look realistic and punchy". `dev/NOTES.md`'s "Not done" has the steps in the order meant.

**Released (0.1.x from issue 23, 2026-10-14; 0.2.0 from 2026-10-17)**, on master: Improved materials (each material lit as what it is made of, by `data/materials.ini`; normal maps shaping baked and ambient light; highlights rolled off in the 16-bit scene), Improved lighting (from 0.2.0, in place of Improved shadows: out of doors one sun, the level casting into the plugin's own shadow maps, figures lit by baked probes and that sun, the shade lifted a little; the plugin's own haze was taken out 2026-10-27, the game's fog kept and coloured by the hour), SMAA, Exposure, ambient occlusion and anisotropic filtering, each an option on the Video tab. The rest of the lighting work (the relighter, the engine's light chooser, sky, shafts, glow, fire, the looks and the per-level tuning) is on the `dev` branch, out of the release.

## Layout

```
src/Graphics.cpp     the plugin: its options, the Direct3D detours, the 16-bit scene, the shadow maps' size
src/MaterialPatch.*  the table's answers added to each material object as the game reads it (DAOLib's resources::OnLoad)
src/MaterialLearn.*  materials the table does not know (a mod's), worked out in play and kept in the player's profile
src/materials/       the metal rule, the names, the lines a material is given, one material worked out (Learn), the table and its tool (elgarashaders_materials)
src/ResourceDump.*   the add-ins' files read through the game, for tools/dump_addins.py
shaders/             what the game's shaders are given: *.hlsl, SMAA, and build_shaders.py, which rebuilds the game's shaders with them
data/materials.ini   what each material is made of (lines `; by hand` are kept by every run of the tool)
data/material_names.txt  the tables a material is classed by its names with, edited by hand
data/glowing.ini     the terrain palettes' layers that glow (lava), read by src/Glowing.cpp
tests/               Catch2 over src/materials; its [game] cases read the install
tools/               build.ps1, deploy.ps1, package.ps1 and release.ps1 (the zip and a release, dev/RELEASING.md), dump_addins.py, shader_hlsl.py (the game's compiled shaders back to HLSL) and its test
extern/DAOLib        the library, a submodule
dev/                 notes, RELEASING.md, TESTING.md (what is seen in the game)
```

The plugin is `elgarashaders`: `build\<preset>\daolib\plugins\elgarashaders\` is its whole folder (the DLL, `override\` with the rebuilt shaders, and `graphics.chosen`, the options as the player last set them). Its log is `Documents\BioWare\Dragon Age\daolib\elgarashaders.log` (on this machine under OneDrive).

## DAOLib

What is general belongs in DAOLib and is DAOLib's own session's (lib-agent's) to integrate: the pure, tested parts the baker stands on (`dao/core/Erf.h`, `Dds.h`, `Level.h`, `RayCast.h`, `LightProbes.h`, the GFF4 reader's floats and raw lists), `CCArea::Scene`, and the address rows. They are on DAOLib's master since its issue 76 (bbd057f, 2026-10-11), and the baker (on `dev`) reads archives through DAOLib's `dao::core::erf::Archive` since this repo's #22. `DAOLIB_DIR` (the environment's, or CMake's) builds against another checkout of DAOLib instead.

A change DAOLib needs is asked of lib-agent, not made from here (one agent, one repo). An engine address is read in the GOG exe and named through the address library's session (addr-lib-agent), as DAOLib's `CLAUDE.md` says.

## Building and playing

```powershell
.\tools\build.ps1 -Preset release -Test
.\tools\deploy.ps1 -Preset release     # the game closed
.\tools\deploy.ps1 -Remove
```

Everything is 32-bit, as the game is. The shaders are rebuilt on every build from the installed game's `shaders.erf` (GOG's install, or `DAO_GAME_DIR`) with the Windows SDK's `fxc.exe`. Shaders are not reloaded while the game runs: a shader change needs a restart.

The game is driven through Eluvian (`.mcp.json`; `C:\project\DAOEluvianMCP\tools\eluvian_bridge.py --call game_launch ...`). Several copies of the game may run at once (the user, 2026-10-18), each its own instance, plugins folder and Eluvian port, so a subagent can test in a copy of its own (another instance name, its profile given the saves); say so to the other sessions before a timing or picture-comparison run. The main copy (`bin_ship`) is shared: say so before launching it, check for a running one, take the plugin out of `bin_ship\daolib\plugins` after. When turn-based-agent asks for a gap, close the game, take the plugin out, answer "gap", and start no game and no build until it says "done".

Our game runs as a copy of its own, the named instance `elgara` (Eluvian's `game_instance`; since 2026-10-11, when the main copy was wanted elsewhere): its profile is `%LOCALAPPDATA%\DAOLib\instances\elgara\BioWare\Dragon Age` (logs, baked probes, and the saves the tours load, copied there by hand: Lyna's slots 16, 35, 86, 111, 347, 397, 400, 423, 468, 471, 480, 489, 491, 500 and Lyna2's 4), and its port is 8930 or the next free. The scratch scripts take both from `. build/scratch/elgara-env.sh`. A start of one copy can freeze another for some seconds: say so to whoever is timing something.

A test deploys nothing: `game_launch` takes a `plugins` folder to load from in place of the game's, so the build's plugin and a copy of `eluvian\` are put in `build\stage\plugins` (`build\scratch\elgara-stage.sh`) and the game is started on that. Another session's copy of the game may be running beside ours (its own instance and port); only ours holds the staged files. Screenshots are taken with `source: window`: the game's own screenshot can crash it (an over-read in the engine, reported to DAOLib 2026-10-11). `build\scratch\elgara-tour.py` photographs a list of saves in several looks, `elgara-ab.py` the frame as it stands; a look is tried live as `Lighting only|Sun=2|SkyFogCap=0.4`.

## Checking the work

- **See it in the game before saying it works**, and measure: the mean colour over the figure on the same frame both ways, not a judgement by eye. A null result needs a control in the same run.
- **One launch per look.** Portraits are drawn once, at a load, and the inventory's figure and each portrait are small scenes of their own: a comparison made by changing the look in a running game misses them. Check the whole frame, not only the hero.
- **Several areas.** Indoors, underground, outdoors; a dark place and a lit one.
- Find which level a save is really in before testing a level's files (an area's level is named in its `.are`, not by the area's name).
- A change that touches the Vanilla look is checked against the game with the plugin removed.
- A setting the options screen holds is switched through the options screen in a test, as a player switches it: `python -I tools\set_option.py <instance> "Draw distance=2x"` opens the screen from the Escape menu, shows the tab by its button, sets each control as a click does and presses OK (`--cancel` for Cancel), through Eluvian's `options` (#68). Never through a file, and never the screen's movie (`SceneManager.PlayScene` showed a tab's page under the Game tab's highlight, with no panel). Improved shadows' finer maps are made only as the game starts. And the game's own anti-aliasing changes how a frame is drawn (the distance in a pass of its own, no second target): say which it was, and try both for anything that reads or writes a target.
- Findings worth keeping go on the issue with a before/after picture of the same frame.

## Style

DAOLib's: clang-format's Microsoft style, clang-tidy with `.clang-tidy`'s exclusions, comments that say why and what was read, with dates. Scripts with backslashes are written to files, never passed inline through a shell.
