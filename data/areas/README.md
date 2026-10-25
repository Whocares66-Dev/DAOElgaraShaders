# The areas the day and night cycle runs in

One file an area, named by its tag (`lot100ar_lothering.ini`), read as the plugin starts (`DayNight.cpp`); `defaults.ini` gives what an area's file does not say. An area with no file keeps its own light. Lines are `key = value`; `;` starts a comment, which says why a value is what it is.

| key | what |
|---|---|
| `level` | the area's level (its day level), as its `.are` names it |
| `night` | the level its night is taken from: its moon, its sky and its sky's light |
| `day` | out of doors, in place of `night`, for an area whose own level is a night (made so, its bake the makers' night): the level its day is taken from, its sun, its sky and its sky's light; the area's own level is then its night, its moon the level's own sun. Not both `night` and `day` |
| `latitude` | degrees, or a level: the latitude at which that level's sun is an equinox's noon (90 less its height) |
| `moon_height` | degrees up at midnight, or `own` for the night level's own moon's; defaults.ini's 60 |
| `moon_turn` | degrees about the upright from the night level's own moon; default 0 |
| `declination` | the sun's, in degrees: how far north of the sky's equator it is, the season |
| `noon` | the hour the sun is highest |
| `night_fill` | how much of the night level's sky light the night keeps beside the moon, what lights the moon's shadows: set so that shadows are about as dark in every area as in Redcliffe's |
| `night_haze` | how hazy the night sky is, 0 to 1: 0 the night level's clear air (Redcliffe's deep blue), 1 the area's own haze and clouds as by day, between them a mix; the sky is as dark at any value, and the moon and the night's light are the night level's (outdoors.tune's `NightHaze` tries one live) |
| `hour` | the hour the area's light is held at, the story's, in place of the clock's; `own` for the level's own hour, its makers' light |
| `hour_until` | `<plot> <flag>`, several by commas: with `hour`, the clock runs there once the party has any of these flags (a plot by its resource name, `arl100pt_siege`) |
| `spray_moon` | how much of the moon's light the area's spray and mist (effects.ini's `lit`) take by night, 0 to 1, beside the night sky's: 0 for spray under trees, 1 for spray in the open moon |
| `move_lamp` | `x y z > x y z`: a lamp named as a fire that the level puts at the first place (to a tenth of a metre) lit at the second, where its lantern or fire is; a flame there is then lit by the lamp alone. Several keys may start `move_lamp` |
| `outside` | an interior's: the area out of doors (its file's name) whose clock and sky its windows see. Its own level keeps its makers' light whole by day and `fill` of it by night, going between as the light out of doors does, in its bake's colour at the end its makers lit and the colour its `day` or `night` says at the other |
| `day`, `night` | (an interior's) where its makers lit it as a night, `day`: the level its makers lit by day whose rooms' fill is the colour its own takes by day; where as a day, `night`: the same for the night. Not both. The sun's or the moon's colour comes in by its windows alone |
| `windows` | an interior's: the materials of its windows' models, each window a light of the hour aimed into the room |
| `fill` | an interior's: how much of its makers' light it keeps at night, whole by day (its bake, ambient colour, probes, layouts' lamps that are not fires, figures' fills and key light), 0 to 1; defaults.ini's 0.5 |
| `window` | an interior's: the light of a square metre of window, a share of the light out of doors at the hour; defaults.ini's |
| `window_reach` | an interior's: how far a window's light reaches, metres; defaults.ini's |
| `fire_strength` | (`defaults.ini` only) how bright the plugin lights fires and lamps, a share of their own light |
| `fires` | (`defaults.ini` only) the starts of the names of layouts' lights that are fires, which keep their light by night; every other layout light goes with the sun |

The sun and the moon go round one pole on the latitude's path, the moon highest at midnight. The day level's sun stands where the level has it at the afternoon hour it is that high, and the level's own light, its bake's, is that hour's (the user, 2026-10-22: "perhaps the default isn't noon, but something more like 10:AM or 3:PM"). Where the night is another level's, the night's reflections are the area's own day cube taken to the night sky's light (`NightCubes.cpp`).

A level is the ground of several areas, and the story has its own times in the others: `lak100d` is the village and the climax's and the epilogue's Redcliffe (`cli100ar`, `epi200ar`), `lak200d` the castle's courtyard and the climax's (`cli150ar`) (ghilana, 2026-10-22; the user, 2026-10-22: "certain areas / times should not change for story reasons"). Areas whose day or night is the story's have no file: Castle Cousland, Ostagar, the party camp, the sieges and the climax (issue 37 has the accounting).

Flames that the level gives no light are given one by type, not by area, in `data/effects.ini`: the same kind of torch or fire has the same light everywhere.

Ground that glows, lava, glows by its terrain palette's layer, not by area, in `data/glowing.ini`: drawn at its own color whatever the hour, and lighting the ground about it by lights the plugin places over it from the level's masks as the level opens.
