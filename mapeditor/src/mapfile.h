// Native C++ .map loader — parses a CPCW map directly into a Scene (no Python
// bridge). A faithful port of cpcw_map.py (chunk tree, SCHD schemas, OBJT/VOBJ
// entity parser, heightmap locator, splatmap colormap). See mapfile.cpp.
#pragma once
#include "scene.h"
#include <string>

// Load a .map into `out`. Returns false on error (out.loaded stays false).
bool load_map_native(const std::string& path, Scene& out);
