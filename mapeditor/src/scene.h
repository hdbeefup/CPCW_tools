// Editor scene model, loaded via the JSON bridge (cpcw_map.py `scene`).
#pragma once
#include <string>
#include <vector>

struct Entity {
    std::string type, proto;
    float pos[3] = {0, 0, 0};   // world: x, y (horizontal plane), z (elevation)
    float dir = 0;
    int player = 0;
    long id = 0;
    int kind = 0;         // 0 doodad, 1 building/unit, 2 effect/sound/deformer
    // byte offsets of editable fields in Scene::raw (for native in-place save)
    long posOff = -1;         // FT_VEC3 (12 bytes)
    long dirOff = -1;         // Dir yaw = first float at this offset
    long playerOff = -1;
    unsigned playerFtype = 0; // schema field type of Player (write size)
};

struct Scene {
    std::string name = "(none)";
    int world_w = 0, world_h = 0;    // terrain extent in world units
    int grid_w = 0, grid_h = 0;      // heightmap vertex dims (world_*+1)
    std::vector<Entity> entities;
    std::vector<float> heights;         // grid_w*grid_h row-major elevations, or empty
    std::vector<unsigned char> colors;  // grid_w*grid_h*3 RGB splat colours, or empty
    std::vector<unsigned char> raw;     // original .map bytes (for native save), or empty
    std::string srcPath;                // original .map path, or empty (loaded from .json)
    bool loaded = false;
};
