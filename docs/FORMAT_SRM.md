# CPCW .srm Model Format

3D model format for Codename: Panzers Cold War (Gepard engine). Contains geometry, materials, textures references, bounding data, and optional collision meshes.

> **Round-trip / writing.** `cpcw_srm_writer.py` (and the identical
> `blendertools/SRM_Blender/srm_writer.py`) parse a `.srm` into a fully editable
> model and re-serialize it **byte-for-byte** — verified over all 2087 game
> files (`cpcw_srm.py roundtrip <dir> --batch`). Chunks a mod doesn't touch keep
> their raw bytes, so importing a game asset and exporting it never corrupts it.
> The Blender add-on exposes this as **File > Export > CPCW Model (.srm)**.

## File Layout

```
MAIN (root container)
  THMB (thumbnail image)
  PMOD (model data, one or more)
    [per node: transform + optional MESH]
  PBND (bounding box, top-level)
  BMSK (collision mask, optional)
  BMES (collision mesh, optional)
  BREC (block record, optional)
  PACT (physics action, optional)
  PMEST (physics mesh transform, optional)
```

## Chunk Header

All chunks use the same header:

```
Tag:   4 bytes ASCII (e.g. "MAIN", "PMOD", "MESH")
Size:  u32 LE (content size in bytes, NOT including tag+size)
```

Content starts immediately after the 8-byte header and spans `Size` bytes.

## MAIN Chunk

Root container. Content = all other chunks concatenated.

## THMB Chunk — Thumbnail (decoded)

The preview image the game's authoring tool bakes into every `.srm` (shown in the
map editor's prototype browser). **RLE over 3-byte BGR pixels**, decoded 2087/2087.

```
Version:   4 bytes "v001"
Width:     u32 LE (always 64)
Height:    u32 LE (always 64)
Unknown:   u32 LE (0x1801 on every shipped file)
Flag:      1 byte  (0x01)
EncLen:    u32 LE  (length of the RLE stream that follows)
Stream:    EncLen bytes — RLE of 3-byte BGR pixels, row-major top-to-bottom
```

RLE control byte `C`:
- **high bit set** (`C & 0x80`): a *run* — `(C & 0x7F) + 1` copies of the next one
  BGR pixel (so `0xBF` = a run of 64 = one full row).
- **high bit clear**: *literals* — `C + 1` BGR pixels follow verbatim.

Pixels are stored **B, G, R**; background is white (`FF FF FF`). Decoder:
`cpcw_srm.py read_thumbnail()` / CLI `cpcw_srm.py thumb <file> [-o out.png]`;
native `mapeditor/src/thumb.cpp load_thmb()` (returns RGBA), verified
byte-identical to the Python reference.

## PMOD Chunk — Model Data

```
Version:      4 bytes "v007"
NodeCount:    u32 LE
Unknown1:     u32 LE (often NodeCount-1 or mesh count)
RootParent:   u32 LE (always 0)
RootUnknown:  u32 LE (always 0)
Nodes:        [NodeCount node entries]
```

### Node Entry

All node headers are stored first, one after another. There is **no** inline
MESH chunk per node — the mesh data forms a single block *after* the last node
header (see "Mesh block" below).

```
NameLen:    u16 LE
Name:       ASCII string (e.g. "body0", "gun0h0", "wheel_fl")

ParentIdx:  i32 LE (parent node index, -1 = root)   [was mislabelled "Unknown3, always -1"]
Position:   3x f32 LE (X, Y, Z translation)
Rotation:   3x f32 LE (X, Y, Z Euler angles in radians; compose Rx @ Ry @ Rz)
Scale:      4x f32 LE (X, Y, Z scale + W, W typically 1.0)
NodeRole:   u32 LE  (role bitfield; drives the bone-palette compaction)  [was "Unknown4"]
MeshIndex:  i32 LE (index into the mesh block; -1 = transform-only node)  [was "Unknown5"]
Unknown6:   i32 LE (always -1)
```

**NodeRole (`unk4`) bitfield** — identifies each node's role and, crucially,
which nodes make up the compact "bone" array a BONE palette indexes (see
"Rigid skinning / assembly"):

| bit  | value | meaning |
|------|-------|---------|
| 0x01 | 1     | node owns its own MESH chunk (modifier flag) |
| 0x02 | 2     | wheel deform-bone (`wheel*`, `bodybone`) |
| 0x04 | 4     | "Merged mesh" **container** node — never a skin bone |
| 0x08 | 8     | generic deform-bone (body, wheels, gun/turret, antennas) |
| 0x10 | 16    | scroll bone (track-scroll animation) |
| 0x20 | 32    | rotate bone (road-wheel spin animation) |
| 0x00 | 0     | plain transform / gameplay marker (lamp/man/target/entrance/suspension/shadow) |

**Per-node fixed data after name: 56 bytes** (4 + 12 + 12 + 16 + 4 + 4 + 4)

Nodes form a skeleton via `ParentIdx`. A node's world transform is the product
of local transforms from the root down. Meshes are rigidly skinned to this
skeleton (see "Vertex Semantics").

### Mesh block

Immediately after the final node header, one MESH chunk per distinct mesh is
stored back-to-back. A node with `MeshIndex == k` uses the k-th MESH chunk.
(The original tooling assumed each MESH followed its node inline; because only
the last node header abuts the block, that captured a single mesh and dropped
the rest — breaking most multi-mesh models such as buildings.)

## MESH Chunk

```
StreamCount:   u32 LE (number of vertex attribute streams)
SubmeshCount:  u32 LE (number of material groups)

[Optional BONE chunk]
INDS chunk
VERS chunks (StreamCount times)
[Submesh/material data (SubmeshCount times)]
[PBND chunk]
```

### BONE Chunk (optional)

```
BoneCount:  u32 LE
BoneIDs:    u16 LE * BoneCount (indices into node array)
```

### INDS Chunk — Index Buffer

```
IndexCount:  u32 LE (number of indices, triangles = IndexCount / 3)
IndexStride: u32 LE (2 = u16 indices, 4 = u32 indices)
Data:        IndexCount * IndexStride bytes
```

Triangle winding order: counter-clockwise.

### VERS Chunk — Vertex Stream

```
Unknown1:     u32 LE (stream index or flags, typically 1)
VertexCount:  u32 LE
Stride:       u32 LE (bytes per vertex)
Usage:        u32 LE (D3DDECLUSAGE — the real stream type; see table)
Semantic:     u32 LE (coarse category: 1=uv 2=pos 4=vector; NOT unique)
Data:         VertexCount * Stride bytes
```

**Header: 20 bytes**, then vertex data.

### Vertex stream type = `Usage` (D3DDECLUSAGE)

The 4th header word (`Usage`) is the authoritative discriminator. The 5th word
(`Semantic`) is only a coarse category — normal/tangent/binormal **all** share
`Semantic == 4`, so you must key off `Usage`. Verified across all 2087 files.

| Usage | Meaning | Typical Stride | Data Format |
|-------|---------|---------------|-------------|
| 0 | POSITION | 12 (rarely 44) | 3x f32 (X, Y, Z) |
| 1 | BLENDWEIGHT | 4 | 4x u8 / 255 (smooth skin) |
| 2 | BLENDINDICES | 4 | 4x u8 bone-palette indices (smooth skin) |
| 3 | NORMAL | 4 | 3x u8 packed + **1x u8 bone index** (byte 3) |
| 4 | TEXCOORD | 8 | 2x f32 (U, V); a mesh may have 2-3 UV sets |
| 5 | TANGENT | 4 | 4x u8 (packed same as normal) |
| 6 | BINORMAL | 4 | 4x u8 (packed same as normal) |
| 7 | (extra set) | 4 | 4x u8 |

**Packed normal decoding:** `component = (byte / 127.5) - 1.0`

**Rigid skinning / bone index (2603 meshes):** byte 3 of the **NORMAL** stream
(`Usage == 3`) is the per-vertex **bone-palette index**. Corpus-verified: for
every skinned mesh `max(byte3) <= bone_count - 1`. That index selects a node
from the mesh's BONE palette. Do *not* identify the stream by "byte 3 spans
`[0, bone_count-1]`" — on multi-bone meshes several stride-4 streams have a
non-trivial byte 3; key off `Usage == 3` instead.

**Smooth skinning (266 meshes):** use the explicit `Usage == 2` (BLENDINDICES)
and `Usage == 1` (BLENDWEIGHT) streams (up to 4 influences per vertex).

**Assembly note:** meshes mix conventions — a static body may be stored already
in model space while wheels/parts are stored bone-local at the origin. The
static file does not carry per-bone inverse-bind matrices (PBND is only a
bounding box), so the exact rest pose the engine shows is animation-driven and
not fully recoverable from geometry alone. The importer therefore offers an
Assemble mode (Full / Parts / Off); see FORMAT notes and import_srm.py.

### Submesh / Material Section

Per submesh, after all VERS chunks:

```
Flag:          u8
Field1:        u32 LE (typically 1)
Field2:        u32 LE (typically 0)
VertexCount:   u32 LE (vertices in this submesh)
Field4:        u32 LE (typically 0)
TriangleCount: u32 LE (triangles in this submesh)
Field6:        u32 LE

MaterialNameLen: u16 LE
MaterialName:    ASCII string (e.g. "Default\", trailing backslash common)

PropertyCountH:  u16 LE
Unknown:         u16 LE
PropertyCount:   u32 LE

[For each property:]
    NameLen:   u16 LE
    Name:      ASCII string (e.g. "DiffuseTexture", "SpecularPower")
    Type:      u32 LE
    Size:      u32 LE

    [If Type == 0 (scalar):]
        Value: Size bytes (typically 4 bytes = f32 LE)

    [If Type == 6 (texture reference):]
        TextureNameLen: u16 LE
        TextureName:    ASCII string (filename without extension, e.g. "road_fence")
```

### Material Property Types

| Type | Meaning | Value Format |
|------|---------|-------------|
| 0 | Scalar (float) | f32 LE, size=4 |
| 6 | Texture reference | u16 len + ASCII filename |

### Common Material Properties

| Property | Type | Description |
|----------|------|-------------|
| DiffuseTexture | 6 | Base color/albedo texture (.dds) |
| NormalTexture | 6 | Normal/bump map texture (.dds) |
| SpecularIntensity | 0 | Specular highlight strength |
| SpecularPower | 0 | Specular exponent/shininess |
| BumpAmount | 0 | Normal map intensity multiplier |

## PBND Chunk — Bounding Box

```
Version:  4 bytes "v001"
Unknown:  u32 LE (always 0)
MinX:     f32 LE
MinY:     f32 LE
MinZ:     f32 LE
MaxX:     f32 LE
MaxY:     f32 LE
MaxZ:     f32 LE
Extra1:   f32 LE (bounding sphere radius?)
Extra2:   f32 LE
```

## Texture Resolution

Texture names in material properties are **relative filenames without extension**. The actual texture files are `.dds` (DirectDraw Surface) located in the same directory as the `.srm` file.

Common DDS formats used: DXT1, DXT3, DXT5.

## Statistics (main2.pak)

- Total .srm files: 2,087
- Size range: 435 bytes to ~570 KB
- Associated files: .sra (animations), .dds (textures), .mat (materials)
- Nodes per model: 1 to 60+ (vehicles with wheels, turrets, crew positions)
- Typical vertex counts: 50 to 5,000+ per mesh

## Coordinate System

- **Left-handed, Y-up** (DirectX convention). This matters: converting to a
  right-handed target (Blender Z-up, or glTF's right-handed Y-up) requires a
  **reflection (determinant −1)**, not merely a rotation. Using a pure rotation
  (e.g. `Rx(90°)` for Y-up→Z-up) leaves the model **mirrored** — reversed text,
  one-sided parts landing on the wrong side. The Blender importer bakes the
  reflection into geometry: swap each position/normal `(x,y,z)→(x,z,y)` **and
  reverse triangle winding** so faces stay outward; node/bone matrices are
  conjugated `P·M·P`. See `blendertools/SRM_Blender/import_srm.py`.
- Euler rotations in radians; the node matrix composes **`Rx @ Ry @ Rz`** (NOT
  the same as a `Euler('XYZ')` which multiplies `Rz @ Ry @ Rx`).
- Scale includes a 4th component (W), typically 1.0.

## Rigid skinning / assembly

Each vertex names one bone (palette index = byte 3 of the NORMAL stream). The
engine renders `world_v = BoneWorld[bone] @ InvBind[bone] @ v`, but the SRM
stores **no InvBind** (BONE is only a u16 node palette; PBND is a bounding box).
Empirically the geometry is authored in a mix:

- **bone-local** parts (wheels, gun barrels, turret, tracks, rotor blades,
  building window panels, whole tank hulls) are stored at their bone's origin →
  `InvBind = I` → transform by `BoneWorld[bone]`.
- **model-space** parts (a civilian car body/speaker authored already-posed and
  merely anchored to a side node) are stored at their final position →
  `InvBind = BoneWorld[bone]⁻¹` → leave in place.

### The exact render transform — SOLVED (Ghidra)

The D3D9 draw and the CPU-side matrix-palette fill were both traced (hard
decompiled evidence; full write-up in `N:\gamePAKdata\re\SKINNING_RESULT.md`).
The per-vertex transform is:

> **`v_world = boneWorld[ node ] · v_stored`** — the bone's node **world matrix**,
> with **NO inverse-bind**. `boneIdx` = NORMAL-stream byte 3 selects a BONE-palette
> value `V = BONE[boneIdx]`, and `node = bone_node_list(nodes)[V]` (see next).

**The palette compaction — `V` is NOT a direct node index (RESOLVED).**
The Ghidra gather (below) reads `SkinMatrices[i] = model+0x180[ BONE[i] ]`. The
population of that `model+0x180` array was the one undecoded residual gap in
`SKINNING_RESULT.md` §5/§8 — and it is **not** the raw file-order node array: it
is a **compact, file-order subset of nodes** selected by the `NodeRole` (`unk4`)
bitfield. Indexing the full node array directly (as the old tooling did) binds
wheels to lamp/man marker nodes → wheels collapse to the model centre or float
off. The correct rule (`bone_node_list`, verified corpus-wide, Vehicles/
Buildings/Objects 100%):

```
c3a = [i for i,n in enumerate(nodes) if (n.unk4 & 0x3A)]   # bone bits 0x02|0x08|0x10|0x20
if max(all BONE palette values) < len(c3a):
    bone_node_list = c3a                                    # "skinned" export (common)
else:
    bone_node_list = [i for i,n in enumerate(nodes) if n.unk4 != 4]   # "merged" export
node = bone_node_list[V]
```

Two exporter regimes (the same tank ships both ways); both are strictly
file-order subsets, so `bone_node_list[V]` is monotonic. Implemented identically
in `cpcw_srm.py` (`bone_node_list`), `blendertools/SRM_Blender/srm_format.py`,
and `viewer/src/srm_model.cpp` (`srm_bone_node_list`).

Evidence:
- SRM meshes draw through the **programmable (vertex-shader) pipeline** — the SRM
  geometry Bind `FUN_0051b7a0` sets a custom vertex shader (`SetVertexShader`,
  device vtable +0x15c) + stream sources. Fixed-function indexed vertex blending
  is ruled out (no `D3DTS_WORLDMATRIX(256+i)`; VERTEXBLEND/INDEXEDVERTEXBLEND only
  in device-init defaults).
- The bone-matrix palette is filled by `FUN_004c0160` (loop `0x004c0330`–`…3ea`)
  and uploaded as the named shader constant `SkinMatrices` (string @ `0x00f79bac`).
  The loop is a **pure gather**: `palette[i] = worldMatrix[ BONE[i] ]` — read the
  u16 index, `×64` (matrix stride), index the **compact** world-matrix array at
  `model+0x180`, append the pointer. **No matrix multiply, no inverse in the loop.**
  (`model+0x180` is the compacted bone-node array decoded above, *not* every
  node — see "The palette compaction".)
- **No inverse-bind exists to fold in:** the node struct exposes only *forward*
  matrices (`LocalMatrix`, `WorldMatrix` @ `node+0x168`, `Transform`); there is no
  `InvBind`/`BindPose`/`RestPose` field in the struct and **no such string anywhere
  in the binary**. Confirms: BONE is a u16 palette, PBND a bbox — the file stores
  no bind matrix because the engine never uses one.

**Consequence for reconstruction.** The rule is *exact* for **non-animated** bones
(car body on `body0`, turret, hull, building panels): `boneWorld` from the static
node hierarchy = the render matrix, so skinning by it is pixel-correct. The only
gap is **animated** bones (tank road wheels `rotate*`, suspension travel): the
engine computes their world matrix each frame, so their settled at-rest pose is
**not in the static file** — skinning by the static hierarchy value can leave those
parts slightly off (e.g. a wheel/suspension-bound body floats).

Import/viewer modes:
- **`FULL`** (default) applies the game's exact rule to every vertex — with the
  palette compaction, non-animated parts (car bodies/wheels, gun, turret, window
  frames, building panels) are pixel-correct. Genuinely animated bones (tank road
  wheels `rotate*`, suspension travel) render in their static (un-settled) pose,
  since that settled pose is computed at runtime and is not in the file.
- **`NONE`** leaves the raw bind pose (each mesh placed by its node matrix only).

*(An earlier `AUTO` "fly" heuristic and a name-matching `_attach_override` were
removed: both existed only to paper over the missing palette compaction — with
`bone_node_list` correct, every part binds to the right node, so props like the
moskvitch roof speaker and the ka_15 window seat natively.)*

## Upgrade variants (node-name convention)

Vehicles pack **every upgrade loadout into one .srm**. The shipped file has a few
pre-merged meshes named `Merged mesh N` — these are produced by the **game's own
authoring/export tool**, which combines many parts (body + wheels + window + …)
into a few vertex buffers, each skinned to a *mixed* bone palette (that is why a
single `Merged mesh` node can carry a whole vehicle). They are **not** an artefact
of any tool in this repo, and there is no merge/join code here — the importer's
job is to *un-merge* them by skinning each vertex to its real bone via the palette
compaction above.

A part's variant is read from the **`_std` / `_upg` suffix on the name of the
bone it is skinned to** (resolved through `bone_node_list`, not the raw palette
value):

| Bone-name suffix | Variant part |
|------------------|--------------|
| `…_std`          | standard loadout (e.g. `gun0m0_std`, `door0_std`) |
| `…_upg`          | upgraded loadout (`gun2h0_upg`, `radar0_upg`, and camo-net `camo_body0_upg` — camo parts always carry `_upg`) |
| (neither)        | always-present base hull / tracks / base weapon |

The engine shows the loadout matching the unit's in-game upgrade state; the file
carries all of them, overlapping at their attach points. The Blender importer
reproduces this with a **Variant** option that keeps faces per the selected set —
`STANDARD` = {base, `_std`}, `UPGRADED` = {base, `_upg`}, `ALL` = every part —
read per-vertex from each vertex's bone tag. Only 18 of 2087 models carry these
tags; the rest are entirely untagged (base) so the filter is a no-op. Note the
match is on the `_std`/`_upg` **suffix**, not a bare `camo` substring — otherwise
a model whose own name contains "camo" (e.g. `camo_tent`) would be wrongly hidden.

## MOTS Chunk — Node Animation

A top-level chunk that follows `PMOD` in **95 of the 2087** models. It stores
keyframed **node** animation — the ambient/scripted motions the engine plays:
machinery loops (lighthouse beam, radar dish, windmill sails, oil derrick,
helicopter rotor), construction rise, open/close, and destruction. Reader:
`cpcw_mots.py` (`parse_mots(path) -> [Motion]`).

**This is not the runtime "settle" of animated bones** (tank road wheels /
suspension). That pose is terrain-contact IK computed at runtime and is *not*
stored anywhere in the file — MOTS does not contain it.

**How the animation reaches geometry — scene graph, not skinning.** A MOTS
channel animates a *node's local transform*. Child **mesh nodes** inherit it
through the parent chain (`world = parent · local`), the ordinary node
hierarchy. Example — `SU_Mi-4_M15.srm`: node `anim_rotorcsucs1` (the rotor hub)
is animated, and mesh node `anim_rotor1` is its child, so animating the hub spins
the rotor. The animated nodes are **not** entries in any mesh bone palette, so the
rigid-skinning path is unrelated. (A few models — lighthouse, windmill — animate
nodes that carry only a light *effect*, not mesh, so nothing visibly moves in the
model file itself.) Animated nodes are conventionally named `anim_*`.

### Layout (little-endian; offsets are chunk-body-relative)

```
MOTS body:
  "v004"                                   version
  MOTI chunk ...                           one per motion

MOTI (tag + u32 size):
  u16 name_len, name                       e.g. "object_meshloopplaying_idx1"
  i32 target_obj                           object/LOD index (0 = base)
  f32 weight                               1.0 in every shipped file
  i32 0
  i32 node_count                           == the model's node count
  i32 node_channel[node_count]             per-node channel index, -1 = static
  ANIM chunk

ANIM (tag + u32 size):
  "v002"                                   version
  f32 duration                             seconds
  i32 0, i32 0, i32 1
  i32 channel_count
  i32 28 (header_size)  + pad to 32
  channel_record[channel_count]            68 bytes each
  keyframe pool                            68-byte records, packed per channel

channel_record (68 bytes):
  f32 base[10]                             rest/summary floats (echo first key)
  i32 key_count
  i32 unk
  i32 key_offset                           byte offset (rel. to byte after
                                           ANIM's "v002") of this channel's keys
  i32 pad[4]

keyframe (68 bytes) — VERIFIED fields carry the animated value:
  i32 link_in                              chains prev key's link_out
  i32 0, i32 0
  i32 4, i32 2, i32 0                      (provisional: type/count tags)
  f32 t_or_dur                             == ANIM.duration in every key (NOT
                                           a per-key time)
  f32 value0                               the animated scalar
  f32 value1                               == value0 for interpolated keys
  f32 0[4]
  i32 201, i32 2                           (provisional)
  i32 pool_offset                          cumulative, +~402 per key
  i32 link_out                             chains next key's link_in
```

### Motion name vocabulary (`object_<kind>_idx<n>`)

| kind                | count | meaning |
|---------------------|-------|---------|
| `dest`              | 158   | destruction (parts fly / collapse) — value pairs look physics-like, not sampled poses |
| `animon`            | 48    | start/continuous machinery motion (rotor, dish, sails) |
| `const`             | 22    | construction (structure rises into place) |
| `animoff`           | 17    | stop / reverse motion |
| `meshloopplaying`   | 12    | looping motion (lighthouse beam, factory belt) |
| `pawup` / `pawdown` | 1 ea. | animal foot |

### What is proven vs. provisional

**Proven (self-consistent across the corpus, `cpcw_mots.py` parses all 95):** the
container/MOTI/ANIM/channel/keyframe structure; the `node_channel` map (it lands
exactly on the `anim_*` nodes); and that **`value0` is a rotation angle in
radians** — machinery loops read clean multiples of `2π` (lighthouse lamp
`-6.283 = -2π`, one turn; Mi-4 rotor `-62.832 = -20π`, ten turns; windmill/derrick
multi-turn).

**Provisional (wants a Ghidra pass on the engine's motion evaluator before
playback is treated as ground-truth):** the per-key **time** (the `t_or_dur`
field is constant `= duration`, so keys currently must be assumed uniformly
spaced), the **interpolation** curve, and exactly **which transform component /
axis** `value0` drives per node (rotation is clear for the loop cases; position
and scale channels — e.g. construction rise — are not yet disambiguated). The
`link_in/out` chain and `pool_offset` are decoded positionally but their runtime
role is unconfirmed.
