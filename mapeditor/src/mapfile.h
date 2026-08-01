// Native C++ .map loader — parses a CPCW map directly into a Scene (no Python
// bridge). A faithful port of cpcw_map.py (chunk tree, SCHD schemas, OBJT/VOBJ
// entity parser, heightmap locator, splatmap colormap). See mapfile.cpp.
#pragma once
#include "scene.h"
#include <string>

#include <vector>

// Load a .map into `out`. Returns false on error (out.loaded stays false).
bool load_map_native(const std::string& path, Scene& out);

// Overwrite `editedIds` entities' fields (and any brush-dirty terrain heights)
// into `b` in place — byte-faithful, only the edited fields' bytes change. `b` is
// normally a copy of Scene::raw, or Scene::raw itself when flushing pending edits
// before a structural change.
void apply_edits_inplace(const Scene& s, const std::vector<long>& editedIds,
                         std::vector<unsigned char>& b);

// apply_edits_inplace over a copy of Scene::raw, written to outPath. Requires the
// scene to have been loaded from a .map (Scene::raw non-empty).
bool save_map_native(const Scene& s, const std::vector<long>& editedIds,
                     const std::string& outPath);

// ---- structural edits, in memory -------------------------------------------
// These mutate Scene::raw and re-walk the entity table (Scene::entities and the
// container offsets are refreshed; terrain/splat/overlay data is preserved, so
// unsaved brush work survives). They do NOT touch the disk. Flush pending field
// edits with apply_edits_inplace(s, ids, s.raw) first, or they are lost.

// Remove entity `id`. Optionally returns the removed OBJT bytes and its index in
// the entity list, which is everything needed to undo the delete exactly.
bool delete_entity_bytes(Scene& s, long id,
                         std::vector<unsigned char>* outBytes = nullptr,
                         int* outIndex = nullptr);

// Clone entity `srcId`'s OBJT with a new ID + position and append it before SCHD.
bool add_entity_bytes(Scene& s, long srcId, const float pos[3], long newId,
                      std::vector<unsigned char>* outBytes = nullptr);

// Build a clone blob without inserting it (for batched paste).
bool build_entity_clone(const Scene& s, long srcId, const float pos[3], long newId,
                        std::vector<unsigned char>& out);

// Insert a ready-made OBJT blob so it becomes entity number `entIndex`
// (entIndex < 0 or past the end -> appended just before SCHD). Undo of a delete.
bool insert_objt_at_index(Scene& s, int entIndex, const std::vector<unsigned char>& objt);

// Erase `len` bytes of OBJT starting at `pos`. Undo of an add.
bool erase_objt_bytes(Scene& s, long pos, long len);

// ---- file wrappers (dev harnesses) -----------------------------------------
bool delete_entity_native(const Scene& s, long id, const std::string& outPath);
bool add_entity_native(const Scene& s, long srcId, const float pos[3], long newId,
                       const std::string& outPath);
