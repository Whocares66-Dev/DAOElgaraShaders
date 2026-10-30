// A figure's light probes baked from the level's own files (ProbeBaker.h) in
// place of the level's one: out of doors, with Improved lighting on, a figure
// takes the light of the baked probes nearest it, mixed, where vanilla gives
// every figure of a level the one probe the level has. A level's own probe is
// a photograph taken at one place; a baked probe is that photograph reckoned
// again where the figure stands, darker under trees and beside a shaded wall,
// lighter beside a sunlit one.
//
// Ported from the dev branch (its Graphics.cpp, 2026-10-11), without what it
// did for levels lit again and the look's sliders.
#pragma once

#include <array>
#include <cstdint>

namespace probes
{

// Places the detours and starts the baker. False where a detour could not be
// placed: the plugin goes on, with the level's own probes.
bool Install();

// Whether figures take the baked probes now: the game thread's word, out of
// doors with Improved lighting on (Outdoors.cpp). Without, a figure has the
// engine's own probe's light back at its next choice of lights.
void Want(bool on);

// The sun's colour on the ground, for the bounce from the level's sunlit
// patches (Reflected.h), each giving back its own colour's share of it; 0 for
// none. The game thread's word (Outdoors.cpp).
void SunBounce(const std::array<float, 3> &sent);

// Whether a light state is of a figure in another scene than the area's: a
// portrait's, the record's or the inventory's, which have light managers and
// lights of their own and are lit as made at every hour (the user, 2026-10-30,
// of the portraits and the record dark and blue at night indoors: "should also
// avoid the lighting change on portraits & char page"). False for one not yet
// seen chosen. Any thread.
bool OfOtherScene(std::uintptr_t state);

} // namespace probes
