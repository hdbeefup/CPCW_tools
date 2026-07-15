// Decode CPCW road (GROL/GROA) & decal (GDCL/GDEC) overlay geometry from a .map's
// raw bytes into Scene::roads / Scene::decals, projected onto the terrain height
// grid. See overlays.cpp for the decoded record layout. Call after the heightmap
// is loaded (needs Scene::heights + grid dims + world dims).
#pragma once
#include "scene.h"
#include <vector>

// Parse GROL (roads) and GDCL (decals) from `raw` and fill s.roads / s.decals.
// Safe to call when the chunks are absent (leaves the vectors empty).
void parse_overlays(const std::vector<unsigned char>& raw, Scene& s);
