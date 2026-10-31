# Elgara Shaders

Better shaders, shadows and anti-aliasing for Dragon Age: Origins, through the game's own Direct3D 9 pipeline. A plugin for [DAOLib](https://github.com/Whocares66-Dev/DAOLib).

Download it from [Nexus Mods](https://www.nexusmods.com/dragonage/mods/7061) or the [releases](https://github.com/Whocares66-Dev/DAOElgaraShaders/releases); the guide is at <https://whocares66-dev.github.io/DAOElgaraShaders/>. It needs [DAOLib](https://www.nexusmods.com/dragonage/mods/7052) 0.8.0 or later.

Nine options on the Video tab of the game's options screen:

- **Improved materials**: metal, gilt and polished stone mirror and scatter light as what they are made of, normal maps shape the light in shade and indoors, and bright highlights are rolled off rather than clipped. The game's own materials, its DLC's and Awakening's come with the plugin; a mod's are worked out from the game's own item data as the game starts.
- **Improved lighting**: one sun out of doors: the level casts its shadows as the sun falls, on characters too (the plugin's own cascaded shadow maps, soft-edged), characters take the sun's light and what the ground gives back, and the darkest shade is lifted a little.
- **SMAA**: smooths the edges the game's own anti-aliasing misses; it works with that or without it.
- **Exposure** (-0.5 to +0.5, 0 by default): how bright the picture is, in stops.
- **Ambient occlusion** (Off, Low, Medium, High): shade in corners, creases and under things.
- **Anisotropic filtering** (Off to 16x): textures stay sharp where they are seen at a slant.
- **Draw distance** (1x, 2x, 4x, Max; 1x by default): how far the land, trees, creatures and effects are drawn in full detail, as a multiple of the game's own distances.
- **Day / night cycle** (on by default) and **Time of day**: the sun crosses the sky and sets, the moon lights the night, a day an hour of play, in the outdoor areas, the DLC's and Awakening's included, and in the interiors with windows, where the candles and lamps light the night (not where the story sets the hour). It needs Improved lighting.

It rebuilds the game's shaders from the installed game at build time. [dev/NOTES.md](dev/NOTES.md) says how the game's lighting works; the history until 2026-10-11 is [issue 1](https://github.com/Whocares66-Dev/DAOElgaraShaders/issues/1).

## Building

Windows, Visual Studio 2022, vcpkg, Python 3, the Windows SDK, and the game installed.

```powershell
git submodule update --init --recursive
.\tools\build.ps1 -Preset release -Test
.\tools\deploy.ps1 -Preset release      # with the game closed; -Remove takes it out
```

To release: `.\tools\release.ps1` ([dev/RELEASING.md](dev/RELEASING.md)).

[CLAUDE.md](CLAUDE.md) has the layout, how it builds against DAOLib, and how the work is checked in play.

## Licence

GPLv3 ([LICENSE](LICENSE)). `shaders/smaa/` is SMAA, under its own MIT licence.
