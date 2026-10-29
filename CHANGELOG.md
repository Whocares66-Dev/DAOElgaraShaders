# Changelog

## 0.4.1

- Faster indoors and underground: the plugin lights the level's fires, torches and lamps itself only in the areas Day and night runs in, and leaves them to the game elsewhere, as 0.3.0 did. Orzammar's Commons went from about 7.1 ms a frame to 4.3, as in 0.3.0 (#61).
- Faster sun and moon shadows when the light is low or the area is large: the soft shadows no longer search metres around each pixel for what shades it. Redcliffe at 21:00 went from about 9.4 ms a frame to 5.7, the Dalish camp at noon from 15.4 to 8.5, Haven from 13.9 to 12.5 (#61).
- Shadows are right from the moment an area loads: the shadow maps' depth was worked out before the level was measured, and left so until the sun moved or the camera did (#61).
- Vigil's Keep's overcast day is back: 0.4.0's download was missing its clouds (#61).
- Needs DAOLib 0.7.0 or newer.

## 0.4.0

- Day / night cycle in the outdoor areas: Lothering, Redcliffe's village and castle, the Brecilian Forest and the Dalish camp, Haven and its mountain top, the Frostback pass, Denerim's streets, the world map's encounters, the Circle's docks, Awakening's outdoors and the DLC's (Honnleath, Soldier's Peak, Return to Ostagar, Flemeth's hut). On by default. Places the story sets the hour for keep it: the prologue, the sieges, the climax, Leliana's Song, the Darkspawn Chronicles; Redcliffe stays day until the siege, Soldier's Peak and the Circle's docks night until their quests are done.
- Sunsets redder and sunrises paler; the sky and the air take the hour's light, and a place's fog stays its own at night, moonlit.
- The plugin's own haze is gone: the game's fog is drawn as the game draws it, with its color by the hour. The haze's teal and green patches, its white glow toward a low sun and its banding are gone with it, with the game's anti-aliasing on or off.
- Fires, torches and lamps light their surroundings by night, with shadows; lava glows.
- Water and mist take the hour's light: the docks' lake by day, a rainy keep's rain by night.
- Fixes: black walls and statues in the Ruined Temple at some angles (#55), white hair at some angles (#56), portraits lit by the area's lights (#51).
- Needs DAOLib 0.7.0 or newer.

## 0.3.0

- Day / night cycle: a new option on the Video tab, with a Time of day slider. The sun and moon cross the sky, a day an hour; at night the moon casts the shadows, fires are lit, and water and its spray are a dimmer white, mirroring the night sky. Redcliffe only for now; needs Improved lighting. Off by default.
- Trees, bushes and grass take the sun's shadow, distant trees too.
- Templar armor: the plate is metal and the robe cloth; the breastplate was matte beside the metal gauntlets (#32).
- Faster, most of all underground and with options off: in Orzammar the plugin cost about 2.5 ms a frame with every option on and now about 1, and with every option off about 2 and now under 0.5 (#33).
- Needs DAOLib 0.6.0 or newer.

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
