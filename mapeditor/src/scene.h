// Editor scene model, loaded via the JSON bridge (cpcw_map.py `scene`).
#pragma once
#include "heap.h"
#include <array>
#include <string>
#include <vector>

// One schema field of an entity, with the byte offset it lives at so the editor
// can overwrite it in place. Every field the schema declares is kept, not just the
// four the viewport needs — that is what makes the Properties panel schema-driven
// (SUnitDesc HP/Level/XP/Ammo/armour, SBuildingUnitDesc PlayerHQ/garrison, ...).
enum { FK_NONE = 0, FK_INT, FK_FLOAT, FK_VEC3, FK_STR };
struct EntityField {
    std::string name;
    unsigned ftype = 0;       // schema field type id (drives the write size)
    long off = -1;            // byte offset in Scene::raw
    int  kind = FK_NONE;
    long i = 0; double f = 0; float v3[3] = {0,0,0}; std::string s;
    bool mirrored = false;    // Pos/Dir/Player/Scale — edited through the Entity fields
    bool dirty = false;       // changed in the UI; only dirty fields are written back
};

struct Entity {
    std::string type, proto;
    float pos[3] = {0, 0, 0};   // world: x, y (horizontal plane), z (elevation)
    float dir = 0;
    float scale = 1.0f;   // SEntityDesc.Scale (uniform); 1.0 when the field is absent
    int player = 0;
    long id = 0;
    int kind = 0;         // 0 doodad, 1 building/unit, 2 effect/sound/deformer
    // byte offsets of editable fields in Scene::raw (for native in-place save)
    long posOff = -1;         // FT_VEC3 (12 bytes)
    long dirOff = -1;         // Dir yaw = first float at this offset
    long idOff = -1;          // ID (u32) offset
    long playerOff = -1;
    long scaleOff = -1;       // Scale (f32) offset, -1 if this schema has none
    unsigned playerFtype = 0; // schema field type of Player (write size)
    long protoOff = -1;       // Prototype GUID string (u16 len + bytes)
    long objtStart = -1, objtEnd = -1;   // byte range of this entity's OBJT in raw
    std::vector<EntityField> fields;     // every schema field, in declaration order
};

// One WRLD/WTHR lighting preset. `values` is parallel to kWeatherFields[]
// (weather.h): one row of up to 4 floats per field, so a colour, a direction and
// a bool all live in the same store. Every field is fixed-width, so an edit is a
// byte-faithful in-place write at `tailOff + kWeatherFields[i].tail` — the same
// mechanism EntityField::off gives entity fields.
struct WeatherPreset {
    std::string name;
    int  slot = -1;             // pool slot; stable, and what the UI keys on
                                // (names are NOT unique — one map ships two
                                // live presets both called "Night_multi")
    long nameOff = -1;          // u16 length prefix (name is not editable yet)
    long tailOff = -1;          // first byte of the 194-byte record body
    std::vector<std::array<float, 4>> values;
    std::vector<unsigned char> dirty;   // per field; only dirty fields are written
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
        int srcSlot = -1;                // pool slot it came from (-1 = fallback scan)
    };
    std::vector<OverlayMesh> roads, decals;
    // Centreline roads: extruded to a ribbon at render time using the road TEXTURE's
    // dimensions for width (engine derives width from tex height; see cpcw-road-groa).
    // Area-fill roads (aprons/plazas) stay in `roads` as pre-triangulated meshes.
    struct RoadSpline { std::string tex; std::vector<float> cx, cz; int srcSlot = -1; };
    std::vector<RoadSpline> roadSplines;
    // Rivers (GRVL -> GRVR). Same record shape as GROA — version + nodes +
    // per-node params + material — but the centreline sits at a CONSTANT y (the
    // water level) and `w` is a real per-node width in world units, so a river
    // needs none of the road's texture-dimension guessing.
    struct RiverSpline {
        std::string tex; float level = 0.0f;
        std::vector<float> cx, cz, w; int srcSlot = -1;
    };
    std::vector<RiverSpline> rivers;
    // GROL/GDCL/GRVL are slot pools (docs/MAP_FORMAT.md §4.9 + §9). Keeping the
    // byte offsets of the header and of every live record is what a write path
    // needs; the render products above are derived and carry `srcSlot` back.
    struct OverlaySlotRef {
        int slot = -1; long chunkOff = -1, bodyOff = -1, bodySize = 0;
        int next = -1, prev = -1;        // the slot's own list links
    };
    struct OverlayPool {
        long chunkOff = -1, hdrOff = -1, contentEnd = -1;
        int used = 0, cap = 0;
        int freeHead = -1, freeTail = -1, usedHead = -1, usedTail = -1;
        bool ok = false;                 // the slot walk landed exactly on the end
        std::vector<OverlaySlotRef> live;
        // Byte offset of EVERY slot's 9-byte header (free ones included), indexed
        // by slot. `live` only covers occupied slots, but relinking a free list
        // has to write through the current freeTail, which is by definition free.
        std::vector<long> slotOff;
    };
    OverlayPool roadPool, decalPool, riverPool;
    // Decoded GDEC transforms, parallel to `decals`. The render meshes are baked
    // geometry; these are the five editable floats and where they live.
    struct DecalRec {
        int slot = -1; long xformOff = -1;      // first of 5 floats (body + 4)
        float cx = 0, cz = 0, sx = 0, sy = 0, rot = 0;
        std::string tex;
    };
    std::vector<DecalRec> decalRecs;
    int decalEdited = 0;                 // how many decals differ from the file
    // Decoded GROA node arrays. Roads render as a baked ribbon (area fills as a
    // triangulated polygon), so as with decals the geometry cannot be edited
    // back into node positions — keep the byte offsets alongside it.
    // Each node is 36 bytes: 3f pos, 3f in-handle, 3f out-handle. The handles
    // are Catmull-Rom tangents and must be re-derived when a node moves.
    struct RoadRec {
        int slot = -1; long nodesOff = -1;  // first node (body + 8)
        int nodeCount = 0;
        bool isArea = false;                // area fill, not a centreline
        std::string tex;
    };
    std::vector<RoadRec> roadRecs;
    int roadEdited = 0;                  // how many road nodes differ from the file
    // BLCK block grid: TWO planes at BLCK's OWN header dims (docs §8). The dims
    // are world*2 on only 41 of 45 maps — the other four are padded by exactly 32
    // texels (16 world units) on one or both axes — so never derive them from
    // WRLD. Resolution is 0.5 world units per texel with the padding at the far
    // edge, so the plane covers blckW*0.5 x blckH*0.5 world units. READ-ONLY.
    int blckW = 0, blckH = 0;
    std::vector<unsigned short> blckFlags;
    std::vector<unsigned char>  blckTypes;
    // Scenario records (locations / objectives / trigger vars / groups / camera
    // paths) out of the HEAP slot pools. READ-ONLY — see docs/MAP_FORMAT.md §5.6.
    ScenarioData scen;
    // WRLD/WTHR lighting presets (weather.cpp). Empty when the chunk is absent,
    // is the older flat chunk version 2, or fails to walk exactly.
    std::vector<WeatherPreset> weather;
    long wthrOff = -1;                  // byte offset of the WTHR chunk tag
    int  weatherCap = 0, weatherFree = 0;
    int  weatherActive = -1;            // index into `weather` of the preset the
                                        // engine binds: the one literally NAMED
                                        // "Default", or -1 if the map has none
    bool weatherEdited = false;         // a preset field changed -> write on save
    std::vector<unsigned char> raw;     // original .map bytes (for native save), or empty
    std::string srcPath;                // original .map path, or empty (loaded from .json)
    long heightOff = -1;                // byte offset of the heightmap grid in raw
    long splatOff = -1;                 // byte offset of the per-layer uint8 weight
                                        // grids (layer i at splatOff + i*grid_w*grid_h)
    bool terrainEdited = false;         // heights changed -> write them on save
    bool splatEdited = false;           // layer opacities painted -> write them on save
    std::vector<unsigned char> heightDirty;  // per-cell: brush-touched (save only these)
    // per-layer, per-cell mask of painted splat weights (same shape as splatWeights)
    std::vector<std::vector<unsigned char>> splatDirty;
    // structural edits: size-field byte offsets of EVERY container that holds the
    // entities (SCEN, WRLD, ..., UNTS, OBJS), plus the OBJS absolute schema_offset.
    std::vector<long> containerSizeOffs;
    long objsSchemaOff = -1;
    long untsCountOff = -1;   // UNTS entity_count u32 (decrement on delete)
    bool loaded = false;
};
