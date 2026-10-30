// An interior's window glass by the hour (build_shaders.py's with_window_glass):
// a window's pane is drawn by its emissive mask, the daylight behind it
// painted in, as bright at midnight as at noon (the user, 2026-10-30: "At
// night the window is still look the same as during day. We should make those
// a pale blue as well"). xyz the light out of doors at the hour against the
// day's (DayNight.h's Glass), 1 where the clock does not run indoors;
// mml_fDaoWindow 1 on a material the interiors' files name as a pane's
// (`glass`, MaterialPatch.cpp), 0 on every other, a lantern's glass among them.
#ifndef DAO_GLASS_HLSL
#define DAO_GLASS_HLSL

float4 daoGlass : register(c158);
float mml_fDaoWindow;

#endif
