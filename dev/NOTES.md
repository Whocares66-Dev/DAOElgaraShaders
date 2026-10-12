# Notes

What was learned about the game's lighting while this was DAOLib's `examples/graphics` (issue 1 here, which was DAOLib's 65, until 2026-10-11). Addresses are GOG's 1.05; the IDs are the address library's.

## What lights a figure

- **Point lights.** A figure's shader is handed `g_vLightParams`, thirteen registers at c0: point lights as a place and reach and a colour, at [0]/[1], [5]/[6] and [7]/[8] (three "flagged" lights) and [9]/[11] (one "unflagged"); the sun's direction and colour at [2]/[3]. A colour's fourth component is passed to the shader and no shader of the game's reads it as a number, so the plugin carries a light's strength there.
- **Fills.** An area keeps three far-off lights (a reach of 1000 m in the Circle) for its figures alone, the same everywhere in it. They are what makes figures flat. `FillsLess` turns them down where the area has no sun; under a sun they stand for the sky.
- **Lamps.** A level's own lamps light its walls and never its figures in vanilla. `Lamps` offers a figure those of 2 to 100 m reach that are near it.
- **Ambient light.** A figure's ambient colour comes from a light probe's irradiance (`g_mLightParams`, three 4 by 4 matrices, Ramamoorthi and Hanrahan's over pi), made a colour per vertex. The engine chooses the one nearest probe of the figure's room, with no blending: it switches in a frame.
- **Portraits and the inventory's figure** are each a scene of their own with their own light manager and three lights of some 200 m. Only the current area's scene (`CCArea::Scene`, the owner at +8 of its light manager) has fills.

## The engine's light manager

`Renderer::LightManagerDefault` (vftable 0x00AE7BF0) has six lists. `AddLights` (2701) sorts a point light by kind into the level's pair (+0x24 flagged, +0xC unflagged) or the figures' pair (+0x6C, +0x84); light probes go to +0x3C and ambient lights to +0x54. `ChooseForMoving` (2694) chooses a figure's lights through `LightList::ChooseFor` (2691), each light scored by its virtual at +0x84: a point light's score (7628) is its strength times what is left of its reach, a probe's (7629) one over its distance, both nothing for a light of another room.

The chosen lights go into the figure's light state (groups at +0, +0x10, +0x20 for probes, +0x30). `Renderer::LightState::Fill` (2848) makes of them what the shaders are handed: the point lights at +0x40, the probe's cube map at +0x114, its three matrices at +0x130, +0x170, +0x1B0, copied from the probe's blueprint only when the probe is not the one kept at +0x110. The plugin writes its own mix over the matrices after that function.

## A level's files

- A level is a folder of `packages\core\env`. `<level>.rim` has each room's definition (`<level>_<room>.rml`); each room's own archives its layout (`<level>_<room>_ll.mmh`: its lamps, its ambient light, its light probe) and its lightmap atlases (DXT5, up to 1024 a side; the alpha is where the sun reaches).
- Models' trees are in `modelhierarchies.erf` and their meshes in `modelmeshdata.erf`. A mesh node's part id is what a room's lightmap list goes by.
- A level's shader lays the lightmap on at its face value: surface = texture times (lightmap + sun times the lightmap's alpha + ambient).
- An area's level is named in its `.are` (in `designerareas.erf`), not by the area's name. The mage origin's Circle is `lak303d`, the Arl of Denerim's dungeon `den011d`, the Dead Trenches `orz900d`, Orzammar's Commons `orz100d`.
- **The lamps of a layout are not what the lightmaps were baked from.** By the toolset's lightmapper's own formula (EclipseRay's `pointlight.cc`: colour times one less the distance over the radius, by the cosine) they give 6% of how a floor's lightmap varies. The lightmaps were baked from lights of the level editor's that did not ship.
- Of 158 levels, 662 rooms have a light probe of the level's own, and a five-metre grid over their floors is about 23,000 probes. Most rooms of a level made of terrain (Orzammar's Commons) have none.
- **A level is of rooms or of terrain** (106 and 52 of the 158). A terrain level is everything out of doors and Orzammar's halls: its rooms are tiles (`<level>_<x>_<y>_0.rml`, the last number the detail), its ground patches of two tiles by two, each a model one of its tiles places (`<level>_<n>_0.msh`, in that tile's own `.rim`; one set of texture coordinates, the lightmap's; its corners in the other order from a room's floor), and it has one probe for all of it, seldom two or three, in `<level>_ll.mmh`, which the level's own `<level>.rml` places. The engine gives a figure anywhere in the level that probe.
- **A probe's cube map is its photograph**, and its `.mtx` the nine coefficients of that picture: Direct3D's six faces in the level's own axes (z up), each with its smaller pictures after it, the values as they are stored. Redcliffe's reckoned so are the file's to 0.003.
- A lightmap out of doors is one dim colour over a whole village (41, 52, 41 of 255 in Redcliffe); the sun's shadows are in its alpha.

## The probes the plugin bakes

A lightmap is the light a surface receives, the lamps' own and the bounce both. So each side of a figure is lit as the level's surface behind it is: rays go out from a place a metre above the floor, the lightmap is read where each lands, and its colour is counted as light arriving from the ray's opposite side. A level's own probes hold the bounce alone (a camera's picture of the surfaces about, about 0.6 of the lightmaps seen), which is why vanilla needs the fills and why the look lifts a level's own probe (`Ambient`) and not a baked one.

A terrain level's probes are made another way, since its lightmaps say so little: as its own probe was made. From the level's own probe's photograph the baker takes the sky (wherever a ray from the probe's place lands on nothing) and what a lit surface of the level sends on (so much of its baked light, so much of the sun where the lightmap's alpha says it reaches, so much with neither, fitted to what the photograph shows of the surfaces about); a probe anywhere else is the photograph taken again by reckoning. At the level's own probe that gives the level's own back: 0.998 like it and 1.02 as bright in Redcliffe, 0.999 and 0.98 in Denerim, 0.98 and 0.92 in Orzammar's Commons. Such a probe holds what a level's own does, the bounce and the sky, and is lifted like one where there is no sun; a room's holds all the light and is not. Places are every floor with room to stand, eight metres apart: Redcliffe 3,936, the Commons 2,818.

The baker reads a level's archives on a thread in Windows' background mode, bakes every room that has a probe of the level's own (the engine's choice of that probe is how a figure's room is known, by the probe's place and its light, which tells a village by day from the same by night), and hands the level over whole. A table is kept in a file and read when the player next comes into the level; the nearest few probes are found through a grid, a metre of height counting for three. The mage origin's Circle is 269 probes in 2.3 s from the command line; the same in play, 4 to 5 s; the Dead Trenches' 4,573 in 5.5 s. A figure is given the nearest four, each by the square of how far one over its distance is above the fifth's.

## The picture

- The scene is drawn into a 16-bit target of the plugin's own and brought back to 8 bits through a shoulder (hue kept to 1.15 times white, burnt out by 1.8). The game's colour grade, which runs after, is still 0 to 1, and there is no HDR output: that wants a presentation that is not Direct3D 9's.
- Shaders are not loaded again when a save is: a shader change needs the game started again.
- The lights' registers are set on a render thread that replays recorded commands, so a stack walked there does not reach the game's code that chose them.

## Not done

- Rooms with no probe of the level's own, levels of an add-in, files a mod overrides loose.
- **What the probes are worth to the eye is little as the looks stand** (the user, 2026-10-11: "doesn't seem to make any significant visual difference"): a frame differs by a fraction of a level with them. A figure's ambient light is a small part of its light beside the fills and the sun, and out of doors the sky is most of it and the same everywhere. They are what a look without fills would stand on.
- Fewer probes where the light is even (a dense bake, then the probes taken one at a time where the mix is worst; written as a script, not in the baker).
- Materials: real gloss and metal from the textures the game has; a lamp's shadow on a figure; leaf cards' depth.
