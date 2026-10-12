# DAOElgaraShaders -- project context

A graphics plugin for Dragon Age: Origins built on DAOLib (the native plugin loader and library, `C:\project\DAOLib`): better lighting, shadows, colours and materials through the game's own Direct3D 9 pipeline. It began as DAOLib's `examples/graphics` on the branch `gh-65-post-effects`; Issue 1 here (it was DAOLib's 65, and the code's comments call it #65) is the record of everything found and tried until 2026-10-11, with pictures. `dev/NOTES.md` is what was learned that the code's comments do not say in one place.

Markdown here is not hard-wrapped: one line per paragraph or list item.

## The goal -- the user's words

Looking good is the goal: more realistic lighting, better materials, better shadows and colours. **Vanilla is the "before" picture, not the target** (the user, 2026-10-11). Judge a frame by what is wrong in it (a black figure, orange hair, white skin), not by how far it is from vanilla. Improve on vanilla without swinging an area's brightness to an extreme. The unmodified game is called "vanilla", never "the game's own".

The bar, since 2026-10-11: "something on the quality of a Minecraft visual overhaul" (the user, with a shader pack's before and after: a low sun, lit haze, soft shadows, bloom). The light probes were judged "good infra work" that "doesn't seem to make any significant visual difference"; what is wanted is "some kind of lighting overhaul to really make the scenes look realistic and punchy". `dev/NOTES.md`'s "Not done" has the steps in the order meant.

## Layout

```
src/Graphics.cpp     the plugin: looks, Direct3D detours, the engine's light chooser, the probes' mixing
src/ProbeBaker.*     light probes baked from a level's files on a background thread
src/BakeTool.cpp     the baker as a command (elgarashaders_bake.exe)
shaders/             what the game's shaders are given: *.hlsl, SMAA, and build_shaders.py, which rebuilds the game's shaders with them
data/graphics.ini    the looks
tools/               build.ps1, deploy.ps1, shader_hlsl.py (the game's compiled shaders back to HLSL) and its test
extern/DAOLib        the library, a submodule
dev/                 notes
```

The plugin is `elgarashaders`: `build\<preset>\daolib\plugins\elgarashaders\` is its whole folder (the DLL, `graphics.ini`, `override\` with the rebuilt shaders). Its log is `Documents\BioWare\Dragon Age\daolib\elgarashaders.log` (on this machine under OneDrive), and baked probes are kept beside it in `elgarashaders\probes\<level>.bin`.

## DAOLib

What is general belongs in DAOLib and is DAOLib's own session's (lib-agent's) to integrate: the pure, tested parts the baker stands on (`dao/core/Erf.h`, `Dds.h`, `Level.h`, `RayCast.h`, `LightProbes.h`, the GFF4 reader's floats and raw lists), `CCArea::Scene`, and the address rows. They are on DAOLib's master since its issue 76 (bbd057f, 2026-10-11), which the submodule is at. `DAOLIB_DIR` (the environment's, or CMake's) builds against another checkout of DAOLib instead.

A change DAOLib needs is asked of lib-agent, not made from here (one agent, one repo). An engine address is read in the GOG exe and named through the address library's session (addr-lib-agent), as DAOLib's `CLAUDE.md` says.

## Building and playing

```powershell
.\tools\build.ps1 -Preset release -Test
.\tools\deploy.ps1 -Preset release     # the game closed
.\tools\deploy.ps1 -Remove
```

Everything is 32-bit, as the game is. The shaders are rebuilt on every build from the installed game's `shaders.erf` (GOG's install, or `DAO_GAME_DIR`) with the Windows SDK's `fxc.exe`. Shaders are not reloaded while the game runs: a shader change needs a restart. `graphics.ini` is read again when it changes, so a look is tuned live.

But for the relighter as a tool: `.\tools\build-bake.ps1` builds it for 64 bits with Embree into `build\bake64`, held to the plugin's warnings. `-Lint` runs clang-tidy over its three sources, which is the only lint the Embree half of `Relighter.cpp` gets (`build.ps1 -Lint` reads the 32-bit build, where it is not compiled); `-Sanitize` builds it with AddressSanitizer into `build\bake64-asan`, to run a small bake under (`side=64`) after a change to the bake's code.

The game is driven through Eluvian (`.mcp.json`; `C:\project\DAOEluvianMCP\tools\eluvian_bridge.py --call game_launch ...`). One main game at a time, shared with other sessions: say so before launching, check for a running one, take the plugin out of `bin_ship\daolib\plugins` after. When turn-based-agent asks for a gap, close the game, take the plugin out, answer "gap", and start no game and no build until it says "done".

Our game runs as a copy of its own, the named instance `elgara` (Eluvian's `game_instance`; since 2026-10-11, when the main copy was wanted elsewhere): its profile is `%LOCALAPPDATA%\DAOLib\instances\elgara\BioWare\Dragon Age` (logs, baked probes, and the saves the tours load, copied there by hand: Lyna's slots 16, 35, 86, 111, 347, 397, 400, 423, 468, 471, 480, 489, 491, 500 and Lyna2's 4), and its port is 8930 or the next free. The scratch scripts take both from `. build/scratch/elgara-env.sh`. A start of one copy can freeze another for some seconds: say so to whoever is timing something.

A test deploys nothing: `game_launch` takes a `plugins` folder to load from in place of the game's, so the build's plugin and a copy of `eluvian\` are put in `build\stage\plugins` (`build\scratch\elgara-stage.sh`) and the game is started on that. Another session's copy of the game may be running beside ours (its own instance and port); only ours holds the staged files. Screenshots are taken with `source: window`: the game's own screenshot can crash it (an over-read in the engine, reported to DAOLib 2026-10-11). `build\scratch\elgara-tour.py` photographs a list of saves in several looks, `elgara-ab.py` the frame as it stands; a look is tried live as `Lighting only|Sun=2|SkyFogCap=0.4`.

## Checking the work

- **See it in the game before saying it works**, and measure: the mean colour over the figure on the same frame both ways, not a judgement by eye. A null result needs a control in the same run.
- **One launch per look.** Portraits are drawn once, at a load, and the inventory's figure and each portrait are small scenes of their own: a comparison made by changing the look in a running game misses them. Check the whole frame, not only the hero.
- **Several areas.** Indoors, underground, outdoors; a dark place and a lit one.
- Find which level a save is really in before testing a level's files (an area's level is named in its `.are`, not by the area's name).
- A change that touches the Vanilla look is checked against the game with the plugin removed.
- Findings worth keeping go on the issue with a before/after picture of the same frame.

## Style

DAOLib's: clang-format's Microsoft style, clang-tidy with `.clang-tidy`'s exclusions, comments that say why and what was read, with dates. Scripts with backslashes are written to files, never passed inline through a shell.
