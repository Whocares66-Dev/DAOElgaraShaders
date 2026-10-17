// The sun's shadow maps fitted to the camera's view (cascaded shadow maps):
// each a slice of the view, from one distance in front of the camera to
// another, seen along the sun's light. The engine fits its two maps to its
// figures, some metres about the party (read in the Brecilian forest,
// 2026-10-16), which leaves the level beyond them with no map; LevelShadows
// fits them here instead and draws figures and level by the same matrices.
//
// Matrices are 4 by 4, row by row, and take a column (x, y, z, 1): what the
// game's shaders' `dot(M[i], p)` takes.
#pragma once

#include <array>
#include <optional>

namespace cascades
{

using Matrix = std::array<float, 16>;
using Vector = std::array<float, 3>;

// One map's matrices: to the map's clip space, as a shadow pass draws by
// (x, y in -1..1, z in 0..1 from the sun's side); and to the map's texture,
// as a surface looks itself up (u, v in 0..1 to texel centres, z as the clip's).
struct Fitted
{
    Matrix clip{};
    Matrix texture{};
};

// The inverse of a matrix, or none where it has none.
std::optional<Matrix> Inverse(const Matrix &m);

// A point taken by a matrix, divided by its w.
Vector Transformed(const Matrix &m, const Vector &p);

// The map for the slice of the view from `nearest` to `farthest` metres in
// front of the camera.
// @param toScreen the camera's view and projection, from the world
// @param sunAxes the sun's frame: across the map, up it, and the way the light
// goes; each a unit vector, at right angles
// @param texels the map's width and height in texels
// @return none where the camera's matrix has no inverse
std::optional<Fitted> Fit(const Matrix &toScreen, const std::array<Vector, 3> &sunAxes, float nearest, float farthest,
                          int texels);

// The camera's place: the point its view and projection takes to w = 0 with
// x and y, where every way through the screen meets. None for no camera.
std::optional<Vector> Eye(const Matrix &toScreen);

// Returns a map of `radius` metres about `centre`, square to the sun: moved in
// steps of about `stepShare` of its radius, each a whole number of its texels,
// so it stays put while the centre moves within a step, and when it moves what
// is in it falls on the same texels as before. Its depth spans `along`, the
// least and most of the sun's way it is to hold (widened to whole steps), or
// as much about the centre as its radius where none is given.
std::optional<Fitted> FitAround(const Vector &centre, const std::array<Vector, 3> &sunAxes, float radius, int texels,
                                float stepShare, const std::optional<std::array<float, 2>> &along = std::nullopt);

// The sun's frame from a shadow map's matrix to clip space (the engine's own,
// whose rows are scaled axes): its first three rows' directions, made exactly
// at right angles.
std::array<Vector, 3> AxesOf(const Matrix &clip);

// Returns a map's frame for sunlight travelling `going` (any length): across
// level with the ground (z up), up, and along the light, so that across times
// up is the light's way; nothing for a way of length 0.
std::optional<std::array<Vector, 3>> AxesToward(const Vector &going);

} // namespace cascades
