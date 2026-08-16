// Decode CPCW road (GROL/GROA) & decal (GDCL/GDEC) overlay geometry from a .map's
// raw bytes into Scene::roads / Scene::decals, projected onto the terrain height
// grid. See overlays.cpp for the decoded record layout. Call after the heightmap
// is loaded (needs Scene::heights + grid dims + world dims).
#pragma once
#include "scene.h"
#include <vector>

// Parse GROL (roads), GDCL (decals) and GRVL (rivers) from `raw` and fill
// s.roads / s.decals / s.rivers plus the three pools' byte offsets.
// Safe to call when the chunks are absent (leaves the vectors empty).
void parse_overlays(const std::vector<unsigned char>& raw, Scene& s);

// Move / resize / rotate one decal, writing the five floats straight into
// Scene::raw. Exactly 20 bytes change and nothing is regenerated: the GDEC record
// carries no value derived from the transform — checked on the corpus, where
// decals at genuinely different positions ship byte-identical trailing data
// (Domination/(4) Islands Of Hope has one trailing body shared by four
// transforms, x=230 and x=490 among them). Size-preserving, so the save stays
// byte-faithful with no ancestor-size patching at all.
//
// `slot` is the POOL SLOT, not an index into `decals`: slots are stable, indices
// are not. Returns false if the map has no writable decal pool or that slot is
// not live. Re-run parse_overlays afterwards or the edit is correct and invisible.
bool overlay_set_decal(Scene& s, int slot, float cx, float cz,
                       float sx, float sy, float rot);
