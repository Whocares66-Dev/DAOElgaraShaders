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
