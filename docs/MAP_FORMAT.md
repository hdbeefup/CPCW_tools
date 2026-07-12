# Codename: Panzers Cold War — .MAP File Format Specification

**Game:** Codename: Panzers Cold War (Stormregion / Gepard Engine)  
**Extension:** `.map`  
**Byte order:** Little-endian throughout  
**Compression:** None (raw binary)  
**String encoding:** ASCII, length-prefixed (uint16 length + payload)

---

## 1. Overview

A `.map` file is a self-describing hierarchical chunk-based container that stores an
entire scenario (single-player mission, multiplayer map, or cinematic scene). It uses
the same OBJT/VOBJ/ARRY/SCHM serialization system as `ProtoDB.bin`, with additional
chunk types for terrain, pathfinding, cameras, weather, and placed entities.

Each OBJS section embeds its own SCHD (schema directory) making the format
self-describing — a parser does not need external schema files.

### File size breakdown (typical)

| Section | Typical % | Contents |
|---------|-----------|----------|
| GTRN    | 50–60%    | Terrain splatmap paint data |
| BLCK    | 30–40%    | Per-cell terrain property grid |
| UNTS    | 2–6%     | Placed entities (units, buildings, doodads) |
| OBJS    | 1–3%     | Game state (triggers, groups, players) |
| Other   | <2%      | PATH, CAMS, WTHR, PREC, STOR |

---

## 2. Chunk Format

Every chunk follows this layout:

```
Offset  Size  Type     Description
0x00    4     char[4]  Tag (ASCII identifier, e.g. "SCEN", "OBJT")
0x04    4     uint32   Content size in bytes (excludes the 8-byte header)
0x08    ...   bytes    Content ([size] bytes)
```

Total chunk size on disk = 8 + content_size.

**Container chunks** nest other chunks inside their content region.  
**Leaf chunks** contain only data fields.

---

## 3. Top-Level Structure

```
SCEN                                    Root container
├── PREC                                Preconditions
│   ├── SETS                            Settings
│   │   ├── OBJS                        SP2ScenarioSettings object tree
│   │   └── SCHD                        Schemas for settings
│   └── OJTS                            Objectives
│       ├── OBJS                        SP2Objectives object tree
│       └── SCHD                        Schemas for objectives
├── OBJS                                Game state (SP2Scenario)
│   └── SCHD                            Schemas for game state
├── STOR                                Entity ID store
└── WRLD                                World data (bulk of file)
    ├── OBJS                            World objects (locations, camera paths)
    │   └── SCHD                        Schemas for world objects
    ├── GTRN                            Ground Terrain
    │   ├── GTRD                        Terrain paint/splatmap data
    │   └── GROL                        Ground Roles (physics zones)
    │       └── GROA                    Ground Roles Array
    ├── BLCK                            Block grid (per-cell properties)
    ├── PATH                            Pathfinding data
    ├── CAMS                            Camera definitions
    ├── WTHR                            Weather settings
    └── UNTS                            Placed entities
        ├── OBJS                        Entity descriptor tree
        └── SCHD                        Entity schemas
```

---

## 4. Container Chunk Headers

### 4.1 SCEN — Scene Root

```
Offset  Size  Type    Description
0x00    4     tag     "SCEN"
0x04    4     uint32  Content size (= file_size - 8)
0x08    4     uint32  Version (observed: 5)
0x0C    ...           Child chunks (PREC, OBJS, STOR, WRLD)
```

### 4.2 PREC — Preconditions

```
0x00    4     tag     "PREC"
0x04    4     uint32  Content size
0x08    4     uint32  Version (observed: 2)
0x0C    ...           Children: SETS, OJTS
```

### 4.3 SETS — Settings

```
0x00    4     tag     "SETS"
0x04    4     uint32  Content size
0x08    4     uint32  Version (observed: 1)
0x0C    ...           Children: OBJS, SCHD
```

### 4.4 OJTS — Objectives

```
0x00    4     tag     "OJTS"
0x04    4     uint32  Content size
0x08    4     uint32  Version (observed: 1)
0x0C    ...           Children: OBJS, SCHD
```

### 4.5 OBJS — Object Store

```
0x00    4     tag     "OBJS"
0x04    4     uint32  Content size
0x08    4     uint32  Schema offset (relative to content start; points to SCHD)
0x0C    ...           OBJT tree, then SCHD at the declared offset
```

The schema_offset field tells the parser where the SCHD chunk sits within this
OBJS block, allowing forward-reference resolution.

### 4.6 STOR — Entity ID Store

```
0x00    4     tag     "STOR"
0x04    4     uint32  Content size (8)
0x08    4     uint32  Count
0x0C    4     uint32  Next available entity ID
```

### 4.7 WRLD — World

```
0x00    4     tag     "WRLD"
0x04    4     uint32  Content size
0x08    4     uint32  Version (observed: 3)
0x0C    4     uint32  World width (map units, e.g. 384, 512)
0x10    4     uint32  World height (map units, e.g. 384, 448, 672)
0x14    ...           Children: OBJS, GTRN, BLCK, PATH, CAMS, WTHR, UNTS
```

World dimensions define the playable area in engine units. Observed values:

| Map | Width | Height |
|-----|-------|--------|
| T_01 (Tutorial 1) | 384 | 384 |
| T_02 (Tutorial 2) | 512 | 448 |
| M_01 (Mission 1)  | 512 | 672 |

### 4.8 UNTS — Units / Entities

```
0x00    4     tag     "UNTS"
0x04    4     uint32  Content size
0x08    4     uint32  Version (observed: 2)
0x0C    4     uint32  Entity count
0x10    ...           Children: OBJS (entity descriptors) + SCHD
```

### 4.9 PATH, CAMS, WTHR — Simple versioned data

All three follow the same header pattern:

```
0x00    4     tag     "PATH" / "CAMS" / "WTHR"
0x04    4     uint32  Content size
0x08    4     uint32  Version
0x0C    4     uint32  Entry count
0x10    ...           Data entries (format TBD, contains HEAP structures)
```

---

## 5. Object Serialization System

The Gepard engine serializes game objects using a schema-driven system. Each OBJS
section contains both the data (OBJT/VOBJ trees) and the schema (SCHD/SCHM) needed
to decode it.

### 5.1 SCHD — Schema Directory

```
0x00    4     tag     "SCHD"
0x04    4     uint32  Content size
0x08    2     uint16  Schema count
0x0A    2     uint16  Unknown (increments per SCHD section in the file)
0x0C    ...           SCHM chunks (one per schema)
```

### 5.2 SCHM — Schema Definition

```
0x00    4     tag     "SCHM"
0x04    4     uint32  Content size
--- content ---
0x00    2     uint16  Name length
0x02    N     char[]  Schema name (ASCII)
N+2     2     uint16  Type ID (unique within file; used by OBJT/VOBJ)
N+4     2     uint16  Version
N+6     2     uint16  Field count
N+8     ...           Field definitions (repeated)
```

**Field definition:**

```
0x00    2     uint16  Field name length
0x02    N     char[]  Field name (ASCII)
N+2     4     uint32  Field type code
N+6     4     uint32  Field size (fixed byte count, or 0xFFFF for variable)
```

### 5.3 OBJT — Object

```
0x00    4     tag     "OBJT"
0x04    4     uint32  Content size
0x08    2     uint16  Type ID (references SCHM type_id)
0x0A    ...           One or more VOBJ chunks
```

### 5.4 VOBJ — Versioned Object Data

```
0x00    4     tag     "VOBJ"
0x04    4     uint32  Content size
0x08    2     uint16  Type ID
0x0A    2     uint16  Data version
0x0C    ...           Field values (sequential, ordered per schema)
```

An OBJT may contain multiple VOBJ chunks. The first carries the base fields;
subsequent VOBJs extend the object with additional versioned fields (merged by
the parser).

### 5.5 ARRY — Array

```
0x00    4     tag     "ARRY"
0x04    4     uint32  Content size
0x08    4     uint32  Element count
0x0C    ...           OBJT elements (repeated [count] times)
```

### 5.6 HEAP — Object Heap / Index

```
0x00    4     tag     "HEAP"
0x04    4     uint32  Content size
0x08    4     uint32  Capacity
0x0C    4     uint32  Count (active entries)
0x10    ...           Index entries + inline OBJT data
```

Used for indexed collections (e.g. cursor hotkey bindings, location heaps).
Entries contain sentinel values (0xFFFFFFFF) for unused slots.

---

## 6. Field Type Reference

| Code     | Name      | Size    | Encoding |
|----------|-----------|---------|----------|
| `0x0001` | int32     | 4       | Signed 32-bit integer |
| `0x0002` | float     | 4       | IEEE 754 single-precision |
| `0x0003` | bool      | 1       | 0 = false, nonzero = true |
| `0x0004` | string    | var     | uint16 length + ASCII bytes |
| `0x0005` | float64   | 8       | 64-bit double or 2D double-vector |
| `0x0006` | vec3      | 12      | 3 consecutive floats (x, y, z) |
| `0x0011` | GUID      | var     | uint16 length + ASCII GUID string |
| `0x0012` | ref       | var     | uint16 length + reference path |
| `0x0013` | IID       | 4       | Internal instance ID (uint32) |
| `0x0014` | entref    | 4       | Entity reference ID (uint32) |
| `0x0015` | vec2f     | 8       | 2 consecutive floats |
| `0x0016` | vec2i     | 8       | 2 consecutive int32 values |
| `0x0017` | uint8     | 1       | Unsigned byte |
| `0x0018` | color     | 4       | RGBA packed uint32 |
| `0x0019` | int16     | 2       | Signed 16-bit integer |
| `0x002B` | locstr    | var     | Localized string (uint16 len + data) |
| `0x0088` | object    | var     | Inline OBJT chunk |
| `0x0089` | object2   | var     | Inline OBJT or VOBJ chunk |
| `0x0165` | blob      | fixed   | Raw binary (size from schema) |
| `0x018A` | int_arr   | var     | Packed integer array |
| `0x039C` | flags     | var     | uint16 length + flags string |
| `0x898A` | array     | var     | ARRY chunk |

Additional type codes seen in specific schemas (0x0390, 0x0490, 0x1290, 0x1490,
0x1790, 0x1990, 0x8990, 0x89A5, 0x8904A6, etc.) are variants of array/inline
types used by specific subsystems. They follow the ARRY or OBJT encoding.

---

## 7. GTRD — Ground Terrain Data

The largest data section. Stores terrain texture layer definitions and per-vertex
splatmap paint weights.

### 7.1 Header

```
Offset  Size  Type    Description
0x00    1     uint8   Version (observed: 5)
0x01    4     uint32  Grid width (tile subdivision count, observed: 32)
0x05    4     uint32  Grid height (observed: matches world height)
0x09    4     float   World size X (same as WRLD width, in engine units)
0x0D    4     float   World size Y (same as WRLD height, in engine units)
0x11    4     uint32  Layer count (observed: 8–10)
```

### 7.2 Layer Definitions

Repeated `layer_count` times immediately after the header:

```
0x00    2     uint16  Name length
0x02    N     char[]  Layer path (e.g. "Terrain/Layer/M_Tutor1/Tutor_1_Grass1")
N+2     4     uint32  Layer type (3 = active, 0 = unused/empty slot)
N+6     4     float   UV scale (texture tiling factor, e.g. 1.0, 1.1905, 2.0)
N+10    2     uint16  Detail name length
N+12    M     char[]  Detail layer path (e.g. "Terrain/Layers/Default/Simple")
M+12    1     uint8   Active flag (1 = enabled, 0 = disabled)
```

Layer paths reference material definitions in the game's data files (typically
under `Tiles/` in the extracted pak archives).

### 7.3 Splatmap Data

After layer definitions, the remainder of GTRD contains **sparse terrain paint
weights** as IEEE 754 floats. The data represents per-vertex blend weights for
each terrain layer.

- Large zero regions indicate areas covered by the default layer (layer 0)
- Non-zero floats are very small (typically 0.000004 – 0.01+) representing
  paint brush opacity/weight
- Data is organized sequentially but sparsely — not a fixed grid. The exact
  vertex mapping requires further reverse engineering.
- For T_01.map: ~1 MB non-zero data within 2.6 MB total splatmap region

### 7.4 Terrain Heightmap (elevation) — SOLVED

Embedded within the GTRD post-layer region is the **terrain elevation grid**:

- A contiguous block of **IEEE-754 float32 values**, row-major, sized
  **`(world_w + 1) × (world_h + 1)` vertices** (one vertex per engine unit, plus
  the closing edge), giving heights directly in **world units**.
- Loaded by the engine class `STerrainTiled` (`STerrainTiled::Load` /
  `::LoadData`, which dispatches the GTRD/GROL/GDCL/GRVL sub-chunks). The terrain
  is conceptually tiled (32-unit tiles), but the elevation itself is stored as
  the single row-major grid above.
- **Byte alignment is not guaranteed to be 4** — the grid can start at any byte
  offset inside GTRD (a small per-map preamble precedes it), so a reader must
  scan all four byte phases.
- **Locating it** (implemented in `cpcw_map.py` `MapFile.get_heightmap()`): find
  the longest run of "height-like" float32 values (finite, `|v| < 500`) across
  all four byte phases; the grid start is then pinned by correlating candidate
  offsets against the map's own **entity elevations** (units/doodads sit on the
  terrain, so `entity.Pos.z ≈ height(entity.x, entity.y)`). Verified at
  **R² ≈ 0.72–1.00** vs entity Z over maps from 545×513 to 769×769.
- Heights observed roughly in `[-25, +55]` world units; map borders are flat
  (0.0), matching the in-game look. Reconstructing the terrain mesh from this
  grid reproduces the game's hills/valleys exactly.

`GROL`, `GDCL`, `GRVL` (siblings under GTRN) are much smaller — ground roles /
decals / (reveal?) — and do not carry the elevation.

---

## 8. BLCK — Block Grid

A fixed-size per-cell terrain property grid. The grid dimensions match the
world dimensions (WRLD width x height), with exactly 12 bytes per cell.

### 8.1 Header

```
Offset  Size  Type    Description
0x00    4     uint32  Version (observed: 3)
0x04    4     uint32  Vertex width  (= world_width * 2)
0x08    4     uint32  Vertex height (= world_height * 2)
```

Total data size = 12 + (world_width * world_height * 12) bytes.

### 8.2 Grid Data

```
grid[world_height][world_width]:
  uint16[6]    Six 16-bit values per cell
```

Cells are stored in row-major order (Y outer, X inner).

### 8.3 Cell Values

| Pattern | Meaning | Frequency (T_01) |
|---------|---------|-------------------|
| `(1,1,1,1,1,1)` | Default passable terrain | ~70% |
| `(0,0,0,0,0,0)` | Blocked/impassable | ~29% |
| Mixed values | Terrain properties | ~1% |

Non-trivial cell values (512, 1280, 1285, 2816, 8448, 8449, etc.) likely encode:
- Terrain movement type / ground material
- Passability flags per movement class (infantry, vehicle, etc.)
- Cover values
- Visual material index

251 unique cell patterns were observed in T_01.map.

---

## 9. GROL / GROA — Ground Roles

Defines gameplay property zones overlaid on the terrain.

### GROL Header

```
0x00    4     tag     "GROL"
0x04    4     uint32  Content size
0x08    ...           Header values (counts, dimensions as uint32)
                      Then GROA sub-chunk
```

### GROA

Contains an array of role entries with float3 bounding boxes (world-space
coordinates) linking terrain areas to physics/gameplay properties such as
cover, concealment, and movement speed modifiers.

---

## 10. Key Object Schemas

### SP2ScenarioSettings (0x0306)

Root settings object in PREC/SETS/OBJS.

| Field | Type | Description |
|-------|------|-------------|
| Name | string | Map display name |
| Description | string | Map description |
| MusicFileName | string | Background music path |
| SkyboxFileName | string | Skybox asset path |
| UnitLimit | int32 | Max units per player |
| PrestigeLimit | int32 | Prestige cap |
| PrestigeGainAmount | int32 | Prestige per tick |
| PrestigeGainTime | int32 | Tick interval |
| StartingPrestige | int32 | Initial prestige |
| CampaignPrestige | int32 | Campaign prestige pool |
| ScenarioPrestige | int32 | Scenario prestige pool |
| AllowedPrestigeAtStart | int32 | Army purchase budget |
| KilledUnitGivePrestige | bool | Award prestige for kills |
| WorldInfos | array | SWorldInfo entries (one per world/map layer) |
| BuyableUnitsAndSkills | array | Available unit roster |
| QuickNATOArmy | object | Preset NATO army |
| QuickUSSRArmy | object | Preset USSR army |

### SWorldInfo (0x030C)

| Field | Type | Description |
|-------|------|-------------|
| WorldIndex | int32 | World layer index |
| WorldSize | vec2i | Width, height in engine units |
| MinimapGenerated | bool | Has minimap thumbnail |
| TacticalMapGenerated | bool | Has tactical overlay |
| StartLocations | array | SStartLocation entries |

### SP2Scenario (0x0305)

Root game state object in the main OBJS section.

| Field | Type | Description |
|-------|------|-------------|
| MessageStore | object | In-game messages |
| TeamInfo | object | SP2TeamInfo (players, alliances) |
| TriggerSystem | object | STriggerSystem (scripting) |
| GroupPool | object | SGroupPool (unit groups) |

### SPlayer (0x00EB)

| Field | Type | Description |
|-------|------|-------------|
| Name | string | Player name |
| Type | int32 | Player type (human/AI) |
| AILevel | int32 | AI difficulty |
| Side | int32 | Faction (NATO/USSR) |
| StartLocationID | uint8 | Spawn point index |
| ColorIdx | uint8 | Player color |
| Team | uint8 | Team assignment |

### SEntityDesc (base for all entities)

| Field | Type | Description |
|-------|------|-------------|
| Prototype | string | **GUID** reference to a ProtoDB object |
| Pos | vec3 | World position (x, y, z) — z is the placed elevation |
| Dir | vec3 | Facing; `Dir[0]` is a **yaw angle in degrees** about the up (z) axis |
| Elevation | float | Ground elevation |
| Scale | float | Uniform scale (doodads) |
| ID | int32 | Unique entity ID |

**Resolving an entity to its 3D model — SOLVED.** `Prototype` is a GUID that
indexes `ProtoDB.bin`. The matching ProtoDB object carries a **`ModelName`**
string field naming the `.srm` model (backslash paths, e.g.
`Vehicles\Civilian\moskvitch401.srm`; also `WreckModelName`, `MarketModelName`).
So the full chain is: `entity.Prototype (GUID)` → ProtoDB object → `ModelName`
→ `.srm`. Build a `{guid → ModelName}` index by walking every object in ProtoDB
(see `protodb.build_model_index`). On a Domination map this resolved **859/859**
entities (849 with a model: trees, bushes, buildings). Placing each entity's
actual model at its `Pos` with yaw `Dir[0]` and `Scale` reproduces the in-game
scene layout. `Pos.z` already holds the correct elevation, so models sit on the
decoded terrain heightmap (§7.4).

### SUnitDesc (extends SEntityDesc)

| Field | Type | Description |
|-------|------|-------------|
| Player | uint8 | Owning player index |
| HP | int32 | Hit points |
| Level | int32 | Veterancy level |
| XP | int32 | Experience points |
| Ammo | int32 | Ammunition |
| TopArmor | int32 | Top armor value |
| LeftArmor | int32 | Left armor value |
| RightArmor | int32 | Right armor value |
| FrontArmor | int32 | Front armor value |
| BackArmor | int32 | Back armor value |

### SBuildingUnitDesc (extends SUnitDesc)

| Field | Type | Description |
|-------|------|-------------|
| Severity | int32 | Damage severity |
| MaxStoredEntity | int32 | Garrison capacity |
| PlayerHQ | bool | Is headquarters |
| StartLocationID | uint8 | Associated spawn point |
| PrestigeGainAmmount | int32 | POI prestige yield |
| PrestigeGainFreqInSec | int32 | Yield frequency |

### SGroupAI (0x0323)

| Field | Type | Description |
|-------|------|-------------|
| Enabled | bool | AI active |
| Patrol | bool | Patrol mode |
| PanicFactor | float | Fear threshold |
| AgroFactor | float | Aggression |
| Rope | float | Leash distance |
| BraveryBonus | float | Bravery modifier |
| Fear | float | Current fear level |
| ActionMode | uint8 | Current AI mode |
| HomePosition | vec3 | Home/anchor point |

### TriggerInfo (0x00DD)

| Field | Type | Description |
|-------|------|-------------|
| TriggerBody | string | Lua script body |
| EventDataMask | int32 | Event filter bitmask |
| IsActive | bool | Trigger enabled |
| EditorFolder | string | Editor organization folder |

---

## 11. Cross-Map Observations

Verified across T_01, T_02, and M_01:

- All maps use SCEN version 5
- WRLD version is always 3
- GTRD version is always 5 with grid_width always 32
- BLCK version is always 3 with vertex dims = 2x world dims
- Layer count varies (8–10) but format is consistent
- Entity schemas are consistent across maps

World sizes vary:

| Map | World W | World H | BLCK cells | Entities |
|-----|---------|---------|------------|----------|
| T_01 | 384 | 384 | 147,456 | 1,221 |
| T_02 | 512 | 448 | 229,376 | ~1,500 |
| M_01 | 512 | 672 | 344,064 | ~3,000 |

---

## 12. Relationship to ProtoDB.bin

The `.map` format shares the same serialization system as `ProtoDB.bin`:

| Feature | ProtoDB.bin | .map |
|---------|-------------|------|
| Root tag | OBJS | SCEN |
| Schema storage | Single SCHD | Multiple SCHD (per OBJS section) |
| Object system | OBJT/VOBJ/ARRY | Same |
| Field types | ~12 types | ~20+ types (extended) |
| Terrain data | N/A | GTRN/GTRD/BLCK |
| Entity placement | N/A | UNTS |

A parser that handles ProtoDB can be extended to handle .map files by:
1. Accepting SCEN as the root container
2. Walking nested container chunks (PREC, SETS, OJTS, WRLD, UNTS, etc.)
3. Collecting schemas from all SCHD sections before parsing objects
4. Supporting additional field types (vec3, color, entity references, etc.)

---

## 13. File Inventory

68 map files across the game data (`CPCWData/main1/Maps/`):

- **Tutorial:** T_01, T_02
- **Campaign:** M_01 through M_18 (18 missions)
- **Domination MP:** 15 maps (2/4/6/8 player variants)
- **Team Match MP:** 7 maps
- **MP Mission:** 4 maps (Beachhead, Cold War, Uphill, Breakthrough)
- **Cinematics:** 23 maps (in `main2/cinematic/Ingamemovies/`)

Each map has accompanying `.tga` thumbnails (minimap, tactical view, POI overlay).

---

## Appendix A: Known Schema Type IDs

| ID | Name | Section |
|----|------|---------|
| 0x0002 | FloatCurveKey | World |
| 0x0004 | SFloatCurve | World |
| 0x0006 | SEffectFloat | World |
| 0x0009 | SGuidList | Game State |
| 0x000E | SObject | Settings |
| 0x0038 | SCameraPath | World |
| 0x00D8 | SWireServer | World |
| 0x00D9 | SEntryType | Game State |
| 0x00DA | SMessageStore | Game State |
| 0x00DC | TriggerVar | Game State |
| 0x00DD | TriggerInfo | Game State |
| 0x00DF | SLuaHandler | Game State |
| 0x00E0 | SAlliance | Game State |
| 0x00E2 | SCameraEventManager | Game State |
| 0x00E8 | SP2Player | Game State |
| 0x00EA | SP2Statistics | Game State |
| 0x00EB | SPlayer | Game State |
| 0x00EF | STunnelPool | Game State |
| 0x0104 | HandlerEntryType | Game State |
| 0x025B | SGroup | Game State |
| 0x025C | SGroupPool | Game State |
| 0x02CD | SLocation | World |
| 0x02CF | STriggerSystem | Game State |
| 0x02D0 | STriggerFolderEntry | Game State |
| 0x02D1 | SArmy | Settings |
| 0x02D4 | SExternalHelp | Game State |
| 0x02ED | SP2World | World |
| 0x0302 | SObjective | Objectives |
| 0x0303 | STeamObjectives | Objectives |
| 0x0304 | SP2Objectives | Objectives |
| 0x0305 | SP2Scenario | Game State |
| 0x0306 | SP2ScenarioSettings | Settings |
| 0x0308 | SP2StatisticsInfoSlot | Game State |
| 0x0309 | SP2StatisticsInfo | Game State |
| 0x030A | SP2TeamInfo | Game State |
| 0x030B | SStartLocation | Settings |
| 0x030C | SWorldInfo | Settings |
| 0x0323 | SGroupAI | Game State |
| 0x0326 | SGeneralSpeech | Game State |

Entity schemas (UNTS section):

| ID | Name |
|----|------|
| varies | SEntityDesc |
| varies | SUnitDesc |
| varies | SVehicleUnitDesc |
| varies | SBuildingUnitDesc |
| varies | SSquadDesc |
| varies | SDoodadDesc |
| varies | SEffectEntityDesc |
| varies | SDeformerHoleEntityDesc |
| varies | SDeformerBombEntityDesc |
| varies | SDsgnrSoundEntityDesc |
| varies | SObjectAnimDesc |
| varies | SSkillDescStruct |
| varies | SIIDRefObjectTeleport |
