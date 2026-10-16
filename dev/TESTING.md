# Testing in the game

What is seen in the game, newest last. How a test is run (the named instances, the staged plugins folder, photo cam for comparisons, one launch per look) is `CLAUDE.md`'s "Building and playing" and "Checking the work". Before this file, findings went on the issues: issue 1 until 2026-10-11, then each feature's.

## 2026-10-16: 0.1.1, materials from the item data (#24)

`e8a779a`, release, DAOLib v0.3.0, GOG's game, `elgara-agent`, with the Grey Warden Runic armor mod in its profile. Vanilla is the same launch with no plugin.

- The plugin reads the install's items at start in about 6 s ("the items say what 608 material objects are made of (239 metal)") and learns the Runic helm's material as metal. Its steel reflects at the Dalish camp and in the Dead Trenches; on master it drew as vanilla's.
- Massive plate (Heavy Plate), Scale Armor and Chainmail at the Dalish camp and in Redcliffe's tavern: plates, scales and mail metal, leather and straps not, as before the change.
- Dwyn's Sword and the Tevinter Shield, Sailor's Crossbow: the shield's painted face paint, its rim, straps and studs metal; the blade steel; the crossbow's stock wood and its fittings metal. Valor's Staff showed no clear difference.
- 40 area transitions between Redcliffe and its tavern with every option on: no crash (the render thread's crash on a released token, `crash-2026-10-16-00-57-09.log`, seen twice in the user's preview, was not reproduced).
- The comparison video and stills: `build\video`, `build\comparisons`.
