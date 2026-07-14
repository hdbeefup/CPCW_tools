// Native C++ .map loader — parses a CPCW map directly into a Scene (no Python
// bridge). A faithful port of cpcw_map.py (chunk tree, SCHD schemas, OBJT/VOBJ
// entity parser, heightmap locator, splatmap colormap). See mapfile.cpp.
#pragma once
#include "scene.h"
#include <string>

#include <vector>

// Load a .map into `out`. Returns false on error (out.loaded stays false).
bool load_map_native(const std::string& path, Scene& out);

// Save `editedIds` entities' Pos/Player back into a copy of Scene::raw and write
// it to outPath (byte-faithful: only the edited fields' bytes change). Requires
// the scene to have been loaded from a .map (Scene::raw non-empty).
bool save_map_native(const Scene& s, const std::vector<long>& editedIds,
                     const std::string& outPath);

// Delete the entity with the given ID: remove its OBJT bytes and fix up the
// SCEN/UNTS/OBJS chunk sizes + the OBJS schema_offset. Writes to outPath.
bool delete_entity_native(const Scene& s, long id, const std::string& outPath);

// Duplicate the entity `srcId` (a valid OBJT) with a new ID + position, inserting
// it before the SCHD and growing every container. Writes to outPath.
bool add_entity_native(const Scene& s, long srcId, const float pos[3], long newId,
                       const std::string& outPath);
