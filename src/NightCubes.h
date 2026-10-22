// The day level's reflection cube by the hour: what every shiny surface and
// water mirror, the level's probe's picture, goes from the day level's to the
// night level's own as the night comes (the user, 2026-10-22, of the lake at
// night: "shouldn't it use a nighttime reflection cube?").
#pragma once

namespace nightcubes
{

// Notes each day level's probe cube as the engine makes it. Call once, as
// the plugin loads.
void Install();

// Writes the hour's cube into the engine's texture once one is made, and
// starts the next where the night's share has moved on. Call on the drawing
// thread, at a frame's end.
void Presented();

} // namespace nightcubes
