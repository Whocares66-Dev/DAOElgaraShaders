# Elgara Shaders

Better lighting, shadows, colours and materials for Dragon Age: Origins, through the game's own Direct3D 9 pipeline. A plugin for [DAOLib](https://github.com/Whocares66-Dev/DAOLib). Unreleased and in development.

It rebuilds the game's shaders from the installed game at build time, bakes light probes for each level in the background as you play, and offers named looks on a slider in the options screen; the Vanilla look is the game without it. [dev/NOTES.md](dev/NOTES.md) says how the game's lighting works and what the plugin does to it; the history until 2026-10-11 is [issue 1](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/1).

## Building

Windows, Visual Studio 2022, vcpkg, Python 3, the Windows SDK, and the game installed.

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Preset release -Test
.\tools\deploy.ps1 -Preset release      # with the game closed; -Remove takes it out
```

[CLAUDE.md](CLAUDE.md) has the layout, how it builds against DAOLib, and how the work is checked in play.

## Licence

GPLv3 ([LICENSE](LICENSE)). `shaders/smaa/` is SMAA, under its own MIT licence.

The bake tool (`elgarashaders_relight`, built for 64 bits by `tools/build-bake.ps1`) casts its rays with [Intel Embree](https://www.embree.org/), which runs its threads on [oneTBB](https://github.com/uxlfoundation/oneTBB); both are © Intel Corporation, under the Apache License 2.0. Neither is part of the plugin or is loaded into the game: they are used only to make a level's lightmaps and light probes, which are what the plugin ships. Their source is not in this repository (vcpkg fetches it at build time); a release that carried the bake tool itself would carry their licences and notices with it.
