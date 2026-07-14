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
