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
    long idOff = -1;          // ID (u32) offset
    long playerOff = -1;
    unsigned playerFtype = 0; // schema field type of Player (write size)
    long objtStart = -1, objtEnd = -1;   // byte range of this entity's OBJT in raw
};

struct Scene {
    std::string name = "(none)";
    int world_w = 0, world_h = 0;    // terrain extent in world units
    int grid_w = 0, grid_h = 0;      // heightmap vertex dims (world_*+1)
    std::vector<Entity> entities;
    std::vector<float> heights;         // grid_w*grid_h row-major elevations, or empty
    std::vector<unsigned char> colors;  // grid_w*grid_h*3 RGB splat colours (fallback), or empty
    // real terrain paint: per-layer texture + per-vertex opacity, for textured
    // rendering (falls back to `colors` when a layer texture can't be resolved).
    struct TerrainLayer { std::string path; float uvScale = 1.0f; bool active = false; };
    std::vector<TerrainLayer> terrainLayers;              // file order (incl. inactive)
    std::vector<std::vector<unsigned char>> splatWeights; // per-layer grid_w*grid_h uint8
    // road (GROA) & decal (GDEC) overlay geometry, projected onto the terrain.
    struct OverlayMesh {
        std::string tex;                 // material path (e.g. Terrain/Road/.../M1_Cobblestone_01b)
        std::vector<float> verts;        // interleaved x,y,z,u,v
        std::vector<unsigned> idx;       // triangle indices
    };
    std::vector<OverlayMesh> roads, decals;
    // Centreline roads: extruded to a ribbon at render time using the road TEXTURE's
    // dimensions for width (engine derives width from tex height; see cpcw-road-groa).
    // Area-fill roads (aprons/plazas) stay in `roads` as pre-triangulated meshes.
    struct RoadSpline { std::string tex; std::vector<float> cx, cz; };
    std::vector<RoadSpline> roadSplines;
    std::vector<unsigned char> raw;     // original .map bytes (for native save), or empty
    std::string srcPath;                // original .map path, or empty (loaded from .json)
    long heightOff = -1;                // byte offset of the heightmap grid in raw
    bool terrainEdited = false;         // heights changed -> write them on save
    std::vector<unsigned char> heightDirty;  // per-cell: brush-touched (save only these)
    // structural edits: size-field byte offsets of EVERY container that holds the
    // entities (SCEN, WRLD, ..., UNTS, OBJS), plus the OBJS absolute schema_offset.
    std::vector<long> containerSizeOffs;
    long objsSchemaOff = -1;
    long untsCountOff = -1;   // UNTS entity_count u32 (decrement on delete)
    bool loaded = false;
};
