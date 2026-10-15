# Elgara Shaders

Better shaders, shadows and anti-aliasing for Dragon Age: Origins, through the game's own Direct3D 9 pipeline. A plugin for [DAOLib](https://github.com/Whocares66-Dev/DAOLib).

Five options on the Video tab of the game's options screen, each one on by default:

- **Improved materials**: metal, gilt and polished stone mirror and scatter light as what they are made of, normal maps shape the light in shade and indoors, and bright highlights are rolled off rather than clipped. The game's own materials, its DLC's and Awakening's come with the plugin; a mod's armour and items are worked out from their textures as the game starts, in the background, and kept for the next time.
- **Improved shadows**: soft-edged, finer shadows (sixteen filtered taps on maps twice the game's size), and the sun's shadows fall on characters too.
- **SMAA**: smooths the edges the game's own anti-aliasing misses; it works with that or without it.
- **Ambient occlusion** (Off, Low, Medium, High): shade in corners, creases and under things.
- **Anisotropic filtering** (Off to 16x): textures stay sharp where they are seen at a slant.

It rebuilds the game's shaders from the installed game at build time. [dev/NOTES.md](dev/NOTES.md) says how the game's lighting works; the history until 2026-10-11 is [issue 1](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/1). The lighting overhaul is on the `dev` branch.

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
