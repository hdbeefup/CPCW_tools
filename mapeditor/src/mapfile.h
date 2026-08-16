// Native C++ .map loader — parses a CPCW map directly into a Scene (no Python
// bridge). A faithful port of cpcw_map.py (chunk tree, SCHD schemas, OBJT/VOBJ
// entity parser, heightmap locator, splatmap colormap). See mapfile.cpp.
#pragma once
#include "scene.h"
#include <string>

#include <vector>

// Load a .map into `out`. Returns false on error (out.loaded stays false).
bool load_map_native(const std::string& path, Scene& out);

// Can this schema field type be overwritten in place (fixed width)? Strings and
// containers cannot — they would resize the record.
bool field_is_writable(unsigned ftype);

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
// `protoGuid`, when given, replaces the clone's Prototype — that is how a model
// the map has never used gets placed, by borrowing an existing record of the right
// entity schema as a byte template. GUIDs are a fixed 36 chars, so it is
// size-preserving; a differently-sized GUID makes the call fail rather than
// corrupt the record.
bool add_entity_bytes(Scene& s, long srcId, const float pos[3], long newId,
                      std::vector<unsigned char>* outBytes = nullptr,
                      const std::string& protoGuid = std::string());

// Build a clone blob without inserting it (for batched paste).
bool build_entity_clone(const Scene& s, long srcId, const float pos[3], long newId,
                        std::vector<unsigned char>& out,
                        const std::string& protoGuid = std::string());

// Insert a ready-made OBJT blob so it becomes entity number `entIndex`
// (entIndex < 0 or past the end -> appended just before SCHD). Undo of a delete.
bool insert_objt_at_index(Scene& s, int entIndex, const std::vector<unsigned char>& objt);

// Overwrite one string field of an entity, RESIZING the record when the new
// value is a different length. This is the one edit that is not size-preserving:
// it splices bytes and then grows/shrinks the enclosing VOBJ, the enclosing OBJT,
// every ancestor container size, and the OBJS `schema_offset` — which is the only
// absolute file offset a .map contains, checked over all 285,272 integer-typed
// field values in the scenario trees of all 45 shipped maps.
//
// Same-length values take the ordinary in-place path and change exactly those
// bytes. Verify any output with --chunktile, NOT just the Python oracle: the
// oracle re-emits each chunk's original span and cannot see a wrong size.
bool set_entity_string(Scene& s, long entId, const std::string& fieldName,
                       const std::string& value);

// ---- generic structural splice support -------------------------------------
// The entity path has its ancestor size-field offsets pre-recorded
// (Scene::containerSizeOffs), but an overlay pool lives under GTRN and is not on
// that chain. These two walk the chunk tree for ANY edit position instead.
//
// Collect BEFORE the splice (the tree must still be self-consistent); apply
// AFTER, passing the same editPos/delta. A container's size field is in its
// header and so lies before `editPos` — those offsets survive the splice. An
// OBJS `schema_offset` FIELD does not: it can sit on a different branch, after
// the edit, in which case the splice moved the field itself, so `map_apply_
// ancestors` relocates those by `delta`. `absOffs` holds the schema_offset
// fields whose VALUE points at or after the edit and therefore needs bumping.
bool map_collect_ancestors(const std::vector<unsigned char>& b, long editPos,
                           std::vector<long>& sizeOffs, std::vector<long>& absOffs);
void map_apply_ancestors(std::vector<unsigned char>& b, const std::vector<long>& sizeOffs,
                         const std::vector<long>& absOffs, long editPos, long delta);

// Re-walk the entity table and shift every recorded byte offset after a splice at
// `editPos` of `delta` bytes. Callers that changed overlay/weather CONTENT (not
// just position) must also re-run parse_overlays / parse_weather afterwards.
bool map_reparse_after_splice(Scene& s, long editPos, long delta);

// Erase `len` bytes of OBJT starting at `pos`. Undo of an add.
bool erase_objt_bytes(Scene& s, long pos, long len);

// ---- chunk outline (read-only inspector) ------------------------------------
// A flat, depth-tagged walk of the SCEN chunk tree. Lets the editor show what a
// map actually contains — including the chunks nobody has decoded yet (WTHR
// weather/lighting, CAMS, PATH, water) — without pretending to interpret them.
struct ChunkNode { int depth; std::string tag; long offset; long size; };
bool map_chunk_outline(const std::vector<unsigned char>& raw, std::vector<ChunkNode>& out);

// ---- structural-integrity check (--chunktile) -------------------------------
// `cpcw_map.py roundtrip` does NOT validate container sizes: its writer re-emits
// each chunk's original span and never recomputes one, so an edit that inserts
// bytes without bumping every ancestor still round-trips IDENTICAL. This is the
// check that catches it — children must tile their parent's content exactly, and
// every OBJS `schema_offset` must still point at its own SCHD child. Run it on
// any map a write path produced, alongside the oracle.
struct ChunkTileReport {
    long fileSize = 0;
    int  chunks = 0, containers = 0;
    int  objs = 0, objsSchemaOk = 0;
    long gapBytes = 0, overlapBytes = 0;
    long trailerBytes = 0;      // bytes past the root SCEN chunk (allowed)
    bool ok = false;
    std::vector<std::string> issues;
};
bool map_chunk_tile(const std::vector<unsigned char>& raw, ChunkTileReport& out);

// ---- file wrappers (dev harnesses) -----------------------------------------
bool delete_entity_native(const Scene& s, long id, const std::string& outPath);
bool add_entity_native(const Scene& s, long srcId, const float pos[3], long newId,
                       const std::string& outPath);
