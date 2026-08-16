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
    │   ├── GTRD                        Terrain heightmap + splatmap paint (§7)
    │   ├── GROL                        Road pool -> GROA records (§9)
    │   ├── GDCL                        Decal pool -> GDEC records (§9)
    │   └── GRVL                        River pool -> GRVR records
    ├── BLCK                            Block grid: u16 flag + u8 type plane (§8)
    ├── PATH                            Pathfinding data (slot pool, §4.9)
    ├── CAMS                            Camera definitions (slot pool, §4.9)
    ├── WTHR                            Lighting/weather presets (slot pool, §4.9 + §11)
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

### 4.9 PATH, CAMS, WTHR — slot pools

All three share a **slot-pool** container: a version, a live count, two
doubly-linked lists (a free list and a used list) over a fixed-capacity slot
array, then the slots themselves.

```
0x00    4     tag     "PATH" / "CAMS" / "WTHR"
0x04    4     uint32  Content size
0x08    4     uint32  Version
0x0C    4     uint32  Live entry count
0x10    4     int32   freeHead      (-1 when the free list is empty)
0x14    4     int32   freeTail
0x18    4     int32   listHead      (first entry in presentation order)
0x1C    4     int32   listTail
0x20    4     int32   capacity      (number of slots that follow)
0x24    ...           capacity x slot:
                        4   int32   next      (index, -1 = end)
                        4   int32   prev
                        1   uint8   isFree
                        ..          the record, present only when isFree == 0
```

An empty pool ships as `capacity 0` with `(-1,-1,-1,-1)` — several MPMission maps
carry `CAMS` and `PATH` exactly that way, and that is valid, not a failure.

**Iterate `listHead -> next`, not the slot array.** The used list is not in slot
order: M_17's weather chain is `7 -> 0 -> 1 -> 4`, which is the authored order
(`M_17_1_Clouds`, `_2_Rain_1`, `_2_Rain_2`, `_3_After_Rain`).

`WTHR`'s record is decoded in §11 below. `PATH` and `CAMS` record bodies are not
decoded (`CAMS` nests `GCAC` sub-chunks with their own sizes).

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
0x0C    ...           count x element
```

Elements are **not** always `OBJT`. The element type comes from the field's own
composite type id (§6.1): `0x898A` is an array of objects, `0x148A` an array of
4-byte entrefs, `0x1790` an array of bytes, and so on. Deriving an element width
from `(size - 4) / count` instead gives non-integral results (18.00, 23.27,
32.40, 24.50 on four real trigger folders) — use the schema, not the arithmetic.

### 5.6 HEAP — object slot pool — **SOLVED**

`HEAP` is the container the whole scenario layer hangs off: locations,
objectives, trigger variables, groups and camera paths all live in one. It is
the same slot pool the engine uses for `WTHR`/`PATH`/`CAMS` and
`GROL`/`GDCL`/`GRVL` (§4.9, §9), but **its six header dwords are in a different
order** — `slotCount` leads and there is no trailing capacity:

```
0x00    4     tag     "HEAP"
0x04    4     uint32  Content size
0x08    4     uint32  slotCount     -- physical slots that follow
0x0C    4     uint32  usedCount     -- live records
0x10    4     int32   freeHead      -- -1 when nothing is free
0x14    4     int32   freeTail
0x18    4     int32   listHead      -- head of the USED chain
0x1C    4     int32   listTail
0x20    ...           slotCount x slot
```

Each slot is a 9-byte header, followed by an `OBJT` **only when the slot is
live**:

```
+0x00   4     int32   prev          -- -1 at the head of its chain
+0x04   4     int32   next          -- -1 at the tail
+0x08   1     uint8   isFree        -- 0 = live, 1 = free
+0x09   ...           OBJT record   -- present only when isFree == 0
```

An empty pool ships `slotCount = 0` and all four indices `-1`; 765 of the 924
shipped heaps are empty this way.

**Slot index is the stable key, not position in the chain.** `SLocation` carries
a field literally named `HeapIndex` — the engine's own name for it. The used
chain is non-monotonic in slot index on 58 of the 924 shipped heaps, so
iterating the slot array and walking `listHead → next` give different orders.

Verified across all 45 shipped maps by `mapeditor --heaptest`:

| Measure | Result |
|---|---|
| HEAPs walking to exactly their chunk end | 924 / 924 |
| Both chains valid (length, `prev`/`next` inverse, `-1` ends, disjoint) | 924 / 924 |
| Live slots | 1947 of 2616 |
| `SLocation` / `SGroup` / `SObjective` / `TriggerVar` / `SCameraPath` | 1265 / 260 / 167 / 139 / 116 |

Those five counts equal a brute-force scan of every `VOBJ` tag carrying the
matching per-map type id, so the walk reaches **all** of them — a tag-first scan,
which is what the tools did before, reached **none**.

### 5.7 HASH — string-keyed object map — **SOLVED**

```
0x00    4     tag     "HASH"
0x04    4     uint32  Content size
0x08    4     uint32  Entry count
0x0C    ...           count x { key, value }
```

Key and value types come from the field's own type id (§6), not from the chunk.
The only instance in a shipped map is `SLuaHandler.Triggers` (type `0x8904A6`,
one per map, 45 total): a `uint16`-length-prefixed ASCII key such as
`joinUs4_OnLocationEntered` mapping to an `OBJT`. This is the trigger table.

---

## 6. Field Type Reference

| Code     | Name      | Size    | Encoding |
|----------|-----------|---------|----------|
| `0x0001` | int32     | 4       | Signed 32-bit integer |
| `0x0002` | float     | 4       | IEEE 754 single-precision |
| `0x0003` | bool      | 1       | 0 = false, nonzero = true |
| `0x0004` | string    | var     | uint16 length + ASCII bytes |
| `0x0005` | **vec2f** | 8       | **2 floats, NOT a double** — see below |
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
| `0x002B` | locstr    | var     | **uint32 char count + 2 bytes/char UTF-16LE** |
| `0x0088` | object    | var     | Inline OBJT chunk |
| `0x0089` | object2   | var     | Inline OBJT or VOBJ chunk |
| `0x0165` | blob      | fixed   | Raw binary (size from schema) |

### 6.1 Composite type ids — the container encoding — **SOLVED**

Everything above `0x0165` is a **composite** id, not a distinct type. The **low
byte selects the container kind** and the bytes above it give the element type
(and for `HASH`, the key type as well):

| Low byte | Container | On-disk form |
|---|---|---|
| `0x8A` | ARRY | `'ARRY' u32 size u32 count` + elements |
| `0x90` | ARRY | identical on disk to `0x8A` |
| `0x9C` | ARRY | identical on disk to `0x8A` |
| `0xA5` | HEAP | §5.6 |
| `0xA6` | HASH | §5.7 |

So the "extra" codes decode mechanically:

| Id | Reads as | Example field |
|---|---|---|
| `0x018A` | ARRY of int32 | `SP2ScenarioSettings.ActiveTimeLimits` |
| `0x048A` | ARRY of string | `STriggerFolderEntry.Triggers` |
| `0x128A` | ARRY of ref | `SBuyableUnit.ForbiddenSkills` |
| `0x148A` | ARRY of entref | `SGroup.Members` |
| `0x898A` | ARRY of object | the common case |
| `0x0190` | ARRY of int32 | `SP2Player.PredefinedGroupIndex` |
| `0x0390` | ARRY of bool | `SGeneralSpeech.SpeechEnabled` |
| `0x1790` | ARRY of uint8 | `SAlliance.Table` (count 256) |
| `0x8990` | ARRY of object | `SP2TeamInfo.Players` |
| `0x039C` | ARRY of bool | `SP2ScenarioSettings.CanChangeSettings` |
| `0x89A5` | HEAP of object | `SP2World.LocationsHeap` |
| `0x8904A6` | HASH string → object | `SLuaHandler.Triggers` |

`0x??90` and `0x??8A` are the *same thing on disk* — both write a plain `ARRY`
chunk. The distinction is a runtime one (fixed-capacity vs growable) that leaves
no trace in the file. `0x039C`, which the tools called `flags`, is simply an
`ARRY` of bools, consistent with its high byte `0x03` = bool.

### 6.2 Three corrections these rules make to earlier readings

1. **`0x0011` GUID and `0x0012` ref are variable-length strings, not 4-byte
   handles.** Reading them as 4 bytes desynchronises 133 of the 225 `OBJS`
   sections.
2. **`0x002B` locstr is *not* `uint16` length + bytes.** It is a `uint32`
   *character* count followed by two bytes per character.
3. **`0x0005` is a pair of floats, not a double.** Both readings are 8 bytes, so
   no structural walk can tell them apart — only the values can.
   `SLocation.Size` (ellipse half-extents) reads `0..697 × 0..677` as a vec2f in
   a world that tops out near 512×672, and `7.5e9` / `2.3e20` as a double, over
   all 1265 records.

None of the three appears in an entity schema, which is why the entity path
never noticed: entity schemas use only
`{0x01,0x02,0x03,0x04,0x06,0x12,0x14,0x17,0x19,0x88,0x898A}`. Applying all three
fixes leaves `--selftest` byte-identical on every map.

### 6.3 Verification

With these rules, **all 225 `OBJS` sections in all 45 shipped maps parse to
exactly the byte where their own `SCHD` table begins** — 164,540 `OBJT` records,
924 `HEAP`s, 45 `HASH`es, zero bytes left over. A wrong width anywhere in any
schema desynchronises the remainder of the tree, so this is a statement about
every field of every record, not just about the containers.

Two independent implementations agree on every count: `mapeditor --heaptest`
(`mapeditor/src/heap.cpp`) and the Python probe used to derive the layout.

**Absolute offsets.** `OBJS.schema_offset` is the only absolute file offset in a
`.map`. Checked by enumerating all 285,272 integer-typed field values in the
scenario trees of all 45 maps and testing whether any lands on a chunk-tag
position: hits run at 1.22× the local-density chance rate, and no field exceeds a
~5% hit rate where a real pointer field would sit at 100%. This is what makes
variable-length string editing tractable — only the ancestor chunk sizes and that
one dword need patching.

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

### 7.3 Splatmap Data — SOLVED

The GTRD post-layer region is a stack of contiguous **per-vertex grids**, all
row-major at the same `(world_w+1) × (world_h+1)` resolution as the heightmap
(§7.4). The exact layout, verified across every map (world 384² to 769²):

```
GTRD post-layer region:
  [ small preamble, 7..2700 bytes ]
  [ heightmap:  (W×H) × float32 ]            (§7.4, world-unit elevation)
  [ layer 0 splat:  (W×H) × uint8 ]          per-vertex paint opacity, layer 0
  [ layer 1 splat:  (W×H) × uint8 ]                    "                layer 1
    ...                                        one grid per layer in §7.2 order,
  [ layer N-1 splat: (W×H) × uint8 ]           INCLUDING inactive slots
  [ +4 trailing grids: (W×H) × uint8 ]        dense, ~255 (baked normals/AO)
```

So `region_bytes ≈ (W×H) × (4  +  num_layers  +  4)`. This holds exactly on
every checked map (e.g. `region/need == 18` for a 10-layer map, `== 24` for a
16-layer map).

- **Per-layer opacity:** each splat grid is one uint8 per vertex, `0` = layer
  absent, `255` = full. Layer 0 is the base (≈255 everywhere); higher layers are
  painted overlays (mostly 0, non-zero where that texture shows).
- **Compositing:** alpha-composite the layers in file order ("over"), starting
  from layer 0: `c = c·(1−w_i) + layer_color_i·w_i`, `w_i = opacity_i/255`.
- The 4 trailing grids are dense (mostly 255) and are almost certainly a baked
  terrain normal (3 ch) + AO/brightness; not needed to reproduce the paint.
- Implemented in `map_format.py MapFile.get_splatmap()` → `(layers, weights,
  W, H)` and used by the Blender map add-on to tint the terrain per-vertex
  (real layer `.dds` averages resolved under the data root, else a by-type
  colour). Layer names may carry a map prefix (`M2_`, `M6_`, `Tutor_1_`) that
  must be stripped to resolve the shared tile texture.

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

`GROL`, `GDCL`, `GRVL` (siblings under GTRN) are much smaller — road ribbons /
decals / (vestigial) — and do not carry the elevation. See §9 (now decoded).

---

## 8. BLCK — Block Grid — **TWO PLANES, not 12 bytes per cell**

A per-**vertex** terrain property grid, stored as **two separate planes** at the
dimensions in BLCK's own header: a `uint16` plane followed by a `uint8` plane.

> **Corrected 2026-08-15.** This section previously described the payload as
> `world_w x world_h` cells of six `uint16`s. That is the same byte count
> (`(W/2)*(H/2)*12 == W*H*3`) read at the wrong stride, which is why it "worked".
> It is wrong on two counts, both measured over all 45 maps — see 8.3.

### 8.1 Header

```
Offset  Size  Type    Description
0x00    4     uint32  Version (observed: 3)
0x04    4     uint32  Grid width   (BLCK's own; usually but NOT always world_w*2)
0x08    4     uint32  Grid height  (BLCK's own; usually but NOT always world_h*2)
```

Total chunk content = `12 + hdr_w * hdr_h * 3` bytes. Verified on **45/45** maps.

**Always take the dimensions from this header, never from WRLD.** They are
`world * 2` on only 41 of 45 maps. The four exceptions:

| Map | WRLD | BLCK header | `world*2` would be |
|-----|------|-------------|--------------------|
| Domination/(2) Island Thunder   | 544x464 | 1088x960   | 1088x928 |
| Domination/(2) Urban Legend     | 528x448 | 1088x896   | 1056x896 |
| Domination/(4) Islands Of Hope  | 720x720 | 1472x1472  | 1440x1440 |
| Domination/(4) The Last Village | 576x400 | 1152x832   | 1152x800 |

Any code deriving the size from WRLD reads past or short of the payload on
exactly those four.

### 8.2 Payload

```
0x0C                      flags[hdr_h][hdr_w]   uint16, row-major (Y outer, X inner)
0x0C + w*h*2              types[hdr_h][hdr_w]   uint8,  row-major, same dims
```

### 8.3 Why two planes (the discriminating measurement)

Both readings consume identical bytes, so they were separated on **spatial
coherence and value cardinality** — a terrain property map is smooth, a
misaligned read of one is not. On M_01 (BLCK 1024x1344, 1,376,256 samples):

| Reading | horiz. neighbour equality | vert. | distinct values |
|---|---|---|---|
| `uint16` plane | 0.9956 | 0.9946 | **4** |
| `uint8` plane  | 0.9879 | 0.9854 | **6** |
| 6x`uint16` per cell, lanes 0..5 | 0.942 | 0.959 | ~30 each |

The plane reading is more coherent on **45/45** maps (narrowest margin
+0.0067, Domination/(4) Cold War). The cell reading's six lanes also have
near-identical statistics to each other, which is the signature of one field
being read at six offsets rather than six distinct channels — and it is where
the old "251 unique cell patterns" figure came from.

### 8.4 Values — semantics still UNKNOWN

M_01 counts (of 1,376,256): `uint16` plane `{256: 1146408, 33: 158397,
32: 61471, 2: 9980}`; `uint8` plane `{0: 802422, 5: 343986, 2: 158397,
7: 50930, 11: 10541, 3: 9980}`. The planes are correlated but **not** 1:1 —
Island Thunder maps both `36` and `256` to type `0`.

The old `(1,1,1,1,1,1)` = passable / `(0,0,0,0,0,0)` = blocked table was an
artifact of the wrong alignment; do not rely on it. Until the flag bits are
pinned against the engine, **BLCK is read-only**: render it, do not paint it.

---

## 9. GROL / GDCL / GRVL — Terrain overlays (roads, decals, rivers) — SOLVED

`GROL`, `GDCL` and `GRVL` are siblings of `GTRD` under `GTRN`. They hold the
**road/sidewalk, decal and river geometry** drawn on top of the terrain with
`Terrain/Road/*`, `Terrain/Decal/*` and `Terrain/River/*` materials — **not**
physics "ground role" zones (an earlier guess). All three are present in all 45
CPCW maps. Decoded by `mapeditor/src/overlays.cpp` (`parse_overlays`);
`cpcw_mapeditor --overlayscan <map|dir>` checks the containers and
`--overlaytest <map>` dumps render counts.

> **`GRVL` is not vestigial.** An earlier note called it "24 empty bytes in every
> map"; that measured only the 17 maps where the pool is empty. The other 28 carry
> **51 `GRVR` river records** (49 with more than one node).

### Container layout — a slot pool, shared by all three

Identical to §4.9's pool minus the leading version dword. The engine reads all
three with one templated routine instantiated three times — `FUN_004bdba0`
(roads), `FUN_004bdd80` (decals), `FUN_004bdf60` (rivers) — differing only in the
record size they allocate (0x120 / 0x184 / 0x11c).

```
0x00    4     tag       "GROL" / "GDCL" / "GRVL"
0x04    4     uint32    Content size
--- content ---
0x00    4     uint32    usedCount     (live records)
0x04    4     int32     freeHead      (-1 when empty)
0x08    4     int32     freeTail
0x0C    4     int32     usedHead
0x10    4     int32     usedTail
0x14    4     int32     slotCount     (= capacity; the array below)
0x18    ...             slotCount x:
                          4   int32   next     (slot index, -1 = end)
                          4   int32   prev
                          1   uint8   isFree
                          ..          "GROA"/"GDEC"/"GRVR" chunk, only if isFree == 0
```

This walks to the **exact** content end on all 135 shipped containers, with
`usedCount` matching the live slots every time. The earlier reader's "9 or 18
byte per-record prefix, so scan forward for the next tag" was this structure
misread: a free slot (9 bytes, no record) followed by a real slot header.

**Order.** The engine's loader iterates the **slot array**, i.e. file order, not
the used list. The used list is a different order on **71 of 90** road/decal
pools (`usedHead != 0` on 28), so if the *renderer* walks the list instead, the
list order is the Z-order. That has **not** been established — the loader does not
settle it — and the measured visual consequence is small (of the overlapping
decal pairs, the two orders disagree on 1 of 51 pairs in M_02, 24 of 302 in M_05,
0 of 16 in M_01). Emit in slot order until the draw side is read.

### GROA — road/sidewalk ribbon

```
0x00    4     uint32    Version (observed 11)
0x04    4     uint32    Node count N
0x08    N×36  nodes     Per node (36 bytes = 9 floats):
                          float x, y, z   world-space centreline point (y≈0)
                          float ×3        "in" Catmull-Rom handle
                          float ×3        "out" Catmull-Rom handle
0x08+N*36
        4     uint32    N2 (== N)
        N×16  params    Per node; float0 is a width-or-UV scale (see below)
        1     uint8     flag
        var   string    Material path (u16 len), first "Terrain/Road/..." string
        var   string    Shader (u16 len, e.g. ".../BumpDisplace", ".../AlphaBlend")
        22    bytes     tail
```
The N nodes trace the road centreline; extrude ±half-width along the segment
normal for a ribbon, projecting Y onto the heightmap (§7.4). Road width is **not**
in the record — it derives from the road texture's short dimension (§ memory
`cpcw-road-groa`). The handle magnitudes are the distances to the previous and
next node, so they are re-derivable when a node moves.

`params[i].float0` is 1.0 on 31517 of 35668 nodes with 137 distinct values in
(0,1]; it is a width scale **or** a UV scale and has not been separated, so
nothing multiplies the half-width by it yet.

### GDEC — decal quad

```
0x00    4     uint32    Field count (observed 6)
0x04    4     float     Center X    (world)
0x08    4     float     Center Z    (world)
0x0C    4     float     Size X
0x10    4     float     Size Y
0x14    4     float     Rotation    (radians, about the up axis)
        ...             flag + padding
        var   string    Material path (first "Terrain/Decal/..." string)
```
One rotated, terrain-projected textured quad (mud/water/grass-dry/sidewalk-piece).

### GRVR — river ribbon

The same record shape as `GROA`, at version 2:

```
0x00    4     uint32    Version (observed 2)
0x04    4     uint32    Node count N
0x08    N×36  nodes     float x, y, z + "in" handle (3f) + "out" handle (3f)
0x08+N*36
        4     uint32    N2 (== N)
        N×16  params    float0 = WIDTH in world units; float1..3 = 1,1,0
        1     uint8     flag
        var   string    Material path ("Terrain/River/...")
```

Two things differ from a road, and both matter:

- **`y` is the water surface**, constant along a record (−13.0 to +15.4 across the
  corpus), so a river must **not** be projected onto the heightmap the way roads
  and decals are — it cuts through the banks.
- **The width is real and per node**, in world units (1.0 … 37.0 observed), so a
  river needs none of the road's texture-dimension derivation.

51 records across 28 maps; 2 of them have a single node and cannot form a ribbon
(both in Domination/(8) Sole Survivor).

**Material → texture is NOT decoded.** River materials are per-map names —
`Terrain/River/M_03/M_03_rivers`, `Terrain/River/Water`,
`Terrain/River/M_12/Elbe_Kanal` — and no DDS of that stem ships; the actual water
textures are generic, under `CPCWPak/Rivers/` (`P2_diffuse_alfa_river_01` …).
A basename-stem resolver will silently borrow an unrelated terrain texture, so
resolve river materials against the `Rivers/` set instead.

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

## 11. WTHR — SWeather lighting presets — SOLVED

Each live slot of the `WTHR` pool (§4.9) is one named lighting/weather preset:

```
4     uint32   Record version — 13 on 219/219 shipped records
2+n   string   Name (uint16 length prefix)
194   bytes    The record body, laid out below
```

**Assert the record version.** `SWeather::Load` is a cascade of `if (N < version)`
guards, so a lower version is a *shorter* record and the fixed 194-byte stride
would silently desync. Likewise **refuse chunk version 2**: it is a flat
count+records list the engine routes through a different reader.

### 11.1 Body layout — STREAM order, not struct order

The offsets below are into the 194-byte body, in the order `SWeather::Load`
reads them. This is deliberately **not** the engine's reflection/struct order —
`FogColor` is read 8th and `SunSpecular` 12th. Both orders sum to 194, so a table
in the wrong order still walks every map cleanly while decoding every colour into
the wrong field; see 11.2 for how the order is actually pinned.

| Off | Field | Type | Engine | Notes |
|-----|-------|------|--------|-------|
| 0   | FogEnabled       | uint8    | +0x08 | |
| 1   | FogStart         | float    | +0x0c | |
| 5   | FogEnd           | float    | +0x10 | |
| 9   | SunDirection     | 3x float | +0x2c | unit vector; see 11.2 |
| 21  | SunColor         | 4x float | +0x38 | **not** clamped to 1 — reaches 2.78 |
| 37  | SunAmbient       | 4x float | +0x48 | |
| 53  | SunShadow        | 4x float | +0x58 | 4th component is **not** an alpha (0.25..2.58) |
| 69  | FogColor         | 4x float | +0x1c | |
| 85  | Night            | uint8    | +0x78 | |
| 86  | WindDirection    | 2x float | +0x7c | |
| 94  | CloudSpeed       | float    | +0x84 | |
| 98  | SunSpecular      | 4x float | +0x68 | |
| 114 | CloudCover       | float    | +0x88 | |
| 118 | CloudMovementDir | 2x float | +0x8c | usually `WindDirection * CloudSpeed` — see 11.3 |
| 126 | FogBottom        | float    | +0x14 | |
| 130 | FogTop           | float    | +0x18 | |
| 134 | EffectCount      | uint32   | —     | 4 on 219/219 |
| 138 | Effects          | 4x float | +0xc0 | slot meanings **unknown** — label "Effect 0..3" |
| 154 | Puddles          | float    | +0x94 | |
| 158 | BloomMul         | float    | +0xa0 | |
| 162 | BloomAdd         | float    | +0xa4 | 0.0 on every shipped record |
| 166 | SoftShadows      | float    | +0xb4 | |
| 170 | TimeOfTheDay     | float    | +0xb8 | hours, 4.0 .. 24.0 |
| 174 | Brightness       | float    | +0xa8 | exactly 1.0 on 219/219 |
| 178 | Contrast         | float    | +0xac | exactly 1.0 on 219/219 |
| 182 | Saturation       | float    | +0xb0 | 1.0 on 214/219, min 0.58 |
| 186 | unknown98        | float    | +0x98 | read at v13, **absent from the reflection table** |
| 190 | unknown9c        | float    | +0x9c | ditto — do not invent a name |

Total 194. Every field is fixed-width, so a preset edit is a size-preserving
in-place write and the save stays byte-faithful.

### 11.2 How the order is pinned (CONFIRMED)

"The walk consumed the chunk exactly" passes on 45/45 maps for a wrong
permutation, so the order is established by semantics that a permutation cannot
satisfy at once. Measured over all 45 maps / 219 records:

| Assertion | Result |
|---|---|
| record version == 13 | 219/219 |
| alpha of SunColor / SunAmbient / FogColor / SunSpecular == 1.0 | 876/876 |
| `SunShadow[3]` range (so it is *not* an alpha) | 0.2500 .. 2.5800 |
| `EffectCount` == 4 | 219/219 |
| `\|SunDirection\|` == 1 ± 0.02 | 219/219 |
| `SunDirection[1] < 0` | 219/219 |
| FogEnabled, Night ∈ {0,1} | 219/219 |
| TimeOfTheDay ∈ [0,24] | 219/219 |
| Brightness == Contrast == 1.0 | 219/219 |
| FogEnd > FogStart when fog is on | 186/186 (min FogEnd **162.0**) |

`SunDirection[1] < 0` on every record, with components 0 and 2 taking either
sign, means **index 1 is the vertical axis and the vector is the direction light
travels** (downward). A shader wants `L = -SunDirection`.

### 11.2a The horizontal swizzle — HALF resolved

Four candidates remain once the vertical axis is fixed. `--sunprobe` scores them
against the light that was hard-coded in the editor's renderer long before WTHR
was decoded, `(0.4, 0.8, 0.35)`:

| # | Candidate | On the stock `Default` |
|---|-----------|------------------------|
| 0 | `( D.x, -D.y,  D.z)` — engine→GL, i.e. negate X and Z as `loadModel` does | **3.2°** |
| 1 | `(-D.x, -D.y, -D.z)` — plain `-D`, no handedness change | 69.6° |
| 2 | `(-D.x, -D.y,  D.z)` — X only | 49.3° |
| 3 | `( D.z, -D.y,  D.x)` — horizontal swap | **3.2°** |

**Candidates 1 and 2 are eliminated.** Candidates 0 and 3 cannot be separated:
the stock `Default` is `(0.4156, -0.8090, 0.4156)`, where `D.x == D.z` makes a
swap a no-op. Candidate 0 is implemented because it follows the engine→GL
transform the model loader already uses.

Two caveats worth keeping:
- Only a map whose `Default` **is** that stock vector discriminates (M_01 and M_12
  both carry it). A map with its own sun direction — M_06's is
  `(-0.282, -0.500, -0.819)` — has no reason to match a hard-coded constant, and
  its distance to that constant carries no information. An earlier version of this
  probe averaged over every preset in a map and produced a different "winner" per
  map; that was measuring nothing.
- The swizzle is therefore **not confirmed**, only narrowed. The editor's default
  lighting mode stays neutral, and settling it properly means reading the engine's
  draw-side light setup.

### 11.3 NOT invariants — do not assert these hard

- `CloudMovementDir == WindDirection * CloudSpeed` fails on 8 of 219 records
  (6 above 1e-4, 2 above 1e-3, worst 0.00999999 in M_07 `02_cloudy`). Treat it as
  a derived default, never auto-rewrite it — that would destroy authored data.
- **Night presets are not uniformly dark.** M_06 `03_night` has SunColor
  (1.242, 1.502, 1.800). The genuinely black case is the multiplayer
  `Night_multi` presets, at exactly (0,0,0) with ambient (0.5, 0.7, 1.0).
- **Preset names are not unique.** Domination/(4) Ring Of Fire ships two live
  presets both named `Night_multi` with different bodies. Key any UI or CLI on
  the pool slot, not the name.
- **The active preset is the one literally named `"Default"`** — `LoadWeathers`
  builds that string and only assigns on a find-by-name hit. It is *not* slot 0
  and *not* the list head. M_17 has no `Default` at all; MPMission/(6)
  Breakthrough has both `Default` and `Default2`.

Implemented by `mapeditor/src/weather.{h,cpp}`; `cpcw_mapeditor --wthrtest <map|dir>`
re-checks every assertion in this section.

---

## 12. Cross-Map Observations

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

## 13. Relationship to ProtoDB.bin

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

## 14. File Inventory

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
