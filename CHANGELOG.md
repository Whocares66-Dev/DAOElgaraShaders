# Changelog

## 0.2.2

- The game no longer crashes as it closes with Improved lighting on, as 0.2.1 did.
- Needs DAOLib 0.3.0 or newer.

## 0.2.1

- Faster Improved lighting: in Denerim's market it cost about 8 to 9 ms a frame and now about 3, with the same picture (#30).
- Softer sun shadows: sharp where an object meets the ground, blurred the farther away it is, as the sun's disc makes them.
- The haze glows toward the sun only where the sun reaches it, not inside shade.
- Fire, spells and flaming weapons light everything they reach, the ground and walls too, and cast shadows of characters and the level.
- Props and furniture cast the sun's shadow.
- Bright light, such as a Fireball's flash, keeps its color instead of turning a room flat white.
- The sky below the horizon is no longer darkened in areas whose sky has no fog, such as the Fade (#29).
- Needs DAOLib 0.3.0 or newer.

## 0.2.0

- Improved lighting replaces Improved shadows. Out of doors one sun lights the level and the characters alike: the level casts its shadows as the sun falls (the plugin's own soft-edged shadow maps), on characters too, and a character walking into shade goes dark with the ground around it. Characters take their light from probes baked from the level's own lighting, and the haze glows toward the sun. Shade is lifted a little, so it reads as shade rather than black.
- Exposure: a new slider on the Video tab, -0.5 to +0.5 in quarter stops.
- Hair no longer darkens the face under it with ambient occlusion (#28).
- Needs DAOLib 0.3.0 or newer.

## 0.1.2

- Water is drawn as the game draws it: lakes and rivers were a white or electric-blue glare with rainbow fringes in places, worst by night (Lake Calenhad's docks) and also by day (Redcliffe's lake), whatever the options were (#25).

## 0.1.1

- A mod's armor is metal where the game's item data says so: the kind of item (a massive helm, a heavy breastplate) decides a piece's substance, not how its steel was painted. The Grey Warden Runic helm's steel now reflects (#24).
- Weapons, shields, staves, bows and quivers: each part (blade, grip, guard, face, straps, fittings) is metal or not whole, from the model's own meshes, reviewed piece by piece. Blades are no longer flecked by painted wear, grips and hafts are no longer metal, and the metal fittings of crossbows and staves are no longer missed (#24).

## 0.1.0

- Five options on the Video tab, each switching as the game runs: Improved materials, Improved shadows, SMAA, ambient occlusion (Off to High) and anisotropic filtering (Off to 16x).
- Improved materials: metal reflects its surroundings, leather, cloth and stone stay matte; the game's, its DLC's and Awakening's materials come with the plugin.
- Materials from mods worked out as the game starts, in the main menu, and given in the same session.
- Restore Defaults on the Video tab gives the five options back (DAOLib #140).
- Needs DAOLib 0.3.0 or newer.
