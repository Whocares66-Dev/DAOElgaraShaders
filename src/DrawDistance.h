#pragma once

// How far the world is drawn in detail (issue 54; the user, 2026-10-29: "a
// slider for draw distance ... we can try 2x, 4x, 8x ... we should scale every
// distance appropriately"). Vanilla draws a terrain tile's far detail (its
// `_1.rml`, coarse and shaped otherwise) past some 80 m, so trees that stand on
// the near detail's ground float over the far's ridges (the issue's picture,
// a mountain pass), and hides creatures past 50 m.
//
// Every distance the engine chooses a detail or a showing by is scaled by one
// factor, read in the GOG exe 2026-10-29:
//
// - A terrain tile's change from its near detail to its far (0x005CCFE0, each
//   tile each frame from the area's update): at its +0x2A8, the tile's radius
//   over 0.777 and 80 m at least (0x005CBD00, as the tile is made; the larger
//   of 1.1 times a layout's +0xB0 and that plus 70 m for a model flagged
//   0x400), with +0x2B0 of hysteresis. Nothing else reads +0x2A8. The tile
//   loads the other detail as it changes, through the game's streaming.
// - Creatures' and placeables' showing and their models' levels of detail,
//   all by LodManager::BandDistance (0x006170F0): the bands at 10 and 29 m
//   (19 and 39 in a cutscene), a creature shown under the detail's 30 to 60
//   m and any other object under 60 (DAOLib's PARTY.md and Addresses.h). It
//   also sets how often a creature's AI is updated, which follows its model's
//   level. Its 1000 for an object of another area or with no model is kept.
// - The trees' change from their full shape to a picture (0x0052E8F0, each
//   frame: the square of 0x00BF22D8 times the tree's own distances, which are
//   15 to 20 m near and 35 to 120 m far by its size), and the bounds the
//   trees of a tile are given by the same factor (0x00532120), made again
//   through 0x005D6C20 as the detail setting does (0x00458EB0).
// - The effects' fade by distance (0x00467380): 0x00BF22CC, 0x00BF22D0 and
//   0x00BF22D4, an emitter's for its levels 0, 1 and 2.
//
// The game's own values of the globals are those the detail setting writes
// (0x00458EB0: 0.5 to 1.5 for the trees, 100 or 1000, 25 to 80 and 5 to 30 m
// for the effects); a value found other than the one last written is the
// game's anew. Not scaled: the camera's far plane (4000 m in play) and the
// area's fog, which is what a far view is lost in.
//
// Seen in my copy (2026-10-29, GraphicsDetailLevel 3, AA 8, vsync off, the
// photo camera 30 to 40 m over the leader): Redcliffe's trees at some 100 m
// are flat pictures at 1x and whole at 2x; the militia 72 m off is hidden at
// 1x and shown at 2x. What it costs, a frame (each part alone, through
// outdoors.tune): Redcliffe over the lake 4.6 ms at 1x, 5.9 at 2x and 9.9 at
// 8x, the trees 3.0 ms of 8x's 5.3 more, the objects 1.1, the tiles 0.7, the
// effects nothing; Ostagar 6.6, 8.2, 13.2 at 4x and 15.3 at 8x, the trees 7.0
// of it. Ostagar's process held 80 MB more at 8x. Max against 8x: the same
// three views of Redcliffe differed by no more than 8x against itself a
// minute later (clouds, wind, the windmill), and Ostagar ran at 62 fps for
// 64 (8x already drew all 1673 tiles near).
namespace drawdistance
{

// The factor that is no limit at all (the slider's Max): a level is a few
// kilometres across at most, so every tile draws its near detail (80 m at
// the least becomes 80 km), every tree its whole shape and every creature
// and object is shown at its finest model (a band's cost cap of 10 still
// pushes the rest outward). BandDistance's 1000 for nowhere is kept as it is.
// The trees' squared factor, (1.5 x 1000)^2, is far from a float's limit.
inline constexpr float kMax = 1000.0F;

// Puts the detours in. Call once, as the plugin starts.
void Install();

// The factor every distance is scaled by, 1 for vanilla's. The game thread's.
void Want(float factor);

// Which parts the factor scales: the terrain's tiles, the creatures and
// objects, the trees and the effects. All by default; a developer's, to
// measure what each costs (outdoors.tune's DrawTiles, DrawObjects,
// DrawTrees and DrawEffects).
void WantParts(bool tiles, bool objects, bool trees, bool effects);

// Keeps the globals at the game's values times the factor. For dao::OnFrame.
void Keep();

} // namespace drawdistance
