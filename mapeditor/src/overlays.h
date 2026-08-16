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

// Move one GROA road node, re-deriving the Catmull-Rom handles of that node and
// both neighbours. Size-preserving like overlay_set_decal — nothing outside the
// three 36-byte node records changes, so the save stays byte-faithful with no
// ancestor-size patching.
//
// The handle identity `T = normalize(P[i+1]-P[i-1]); in = -T*|P[i]-P[i-1]|;
// out = T*|P[i+1]-P[i]|` reproduces the shipped bytes on 27790 of 27792 interior
// nodes (median error 5e-07; the 2 exceptions are both in Domination/(4) The Last
// Village). It is therefore a re-derivable DEFAULT a designer can perturb, not an
// invariant — see --roadauxtest, which asserts no NEW mismatch rather than zero.
//
// Endpoints are NOT reproducible: |in| == |out| holds on 7876/7876, but the
// stored direction departs from the chord by up to 83 degrees, so an endpoint
// keeps its direction bit-for-bit and only its magnitude is scaled.
//
// `slot` is the POOL SLOT, not an index into `roadSplines`. Returns false if the
// map has no writable road pool or that slot/node is not live. Re-run
// parse_overlays afterwards or the edit is correct and invisible (the ribbon is
// baked geometry).
bool overlay_set_road_node(Scene& s, int slot, int node, float x, float z);

// Delete one decal: free its pool slot and remove its GDEC record's bytes. This
// is a STRUCTURAL edit — the container shrinks — so it patches every ancestor
// chunk size and re-decodes the overlays. `slot` is the pool slot.
//
// The freed slot is appended at the free-list tail, which is a CHOICE rather than
// a discovered convention: the shipped free lists are in arbitrary slot order (18
// of the 28 pools with two or more free slots are neither ascending nor
// descending), so the engine follows the links and any self-consistent list is as
// valid as the ones on disk. The INVARIANTS are what matter — see --overlayscan.
bool overlay_delete_decal(Scene& s, int slot);

// Byte span of the node records a move of `node` can touch — that node and both
// neighbours. Undo snapshots these bytes verbatim rather than re-running the
// handle derivation backwards: an endpoint's magnitude is scaled by a float
// ratio, and scaling by newLen/oldLen then oldLen/newLen is not guaranteed to
// land on the original bits.
bool overlay_road_node_span(const Scene& s, int slot, int node, long& off, long& len);
