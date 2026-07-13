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

## THMB Chunk — Thumbnail

```
Version:  4 bytes "v001"
Width:    u32 LE (typically 64)
Height:   u32 LE (typically 64)
Unknown:  u32 LE
Data:     remaining bytes (encoded thumbnail image, appears to be raw BGRA or compressed)
```

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
Unknown4:   u32 LE
MeshIndex:  i32 LE (index into the mesh block; -1 = transform-only node)  [was "Unknown5"]
Unknown6:   i32 LE (always -1)
```

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

### What the render path actually does (Ghidra, partial)

The D3D9 draw path was traced far enough to establish the **mechanism** (hard
decompiled evidence; see `N:\gamePAKdata\re\SKINNING_RESULT.md`):

- SRM meshes are drawn through the **programmable (vertex-shader) pipeline**. The
  SRM geometry class's bind method `FUN_0051b7a0` calls
  `dev->SetVertexShader(geom+0x18)` (vtable +0x15c) then `SetStreamSource` per
  stream — a custom VS is bound, so the per-vertex transform runs in that shader.
- **Fixed-function indexed vertex blending is ruled out:** `SetTransform` is never
  called with a `D3DTS_WORLDMATRIX(256+i)` state, and `D3DRS_VERTEXBLEND(151)` /
  `INDEXEDVERTEXBLENDENABLE(167)` appear only in device-init defaults, never in a
  per-draw setup. So skinning is a **vertex-shader matrix palette indexed by the
  rigid bone index** (NORMAL byte 3 → BONE palette → node).
- **Not recovered:** whether the palette matrix for a bone is `boneWorld[node]`
  (simple) or `boneWorld[node]·invBind[node]`. The `SetVertexShaderConstantF`
  upload site could not be isolated (vtable-offset collisions + a cached device
  pointer). The **simple** rule `v_world = boneWorld[palette[idx]]·v_stored` is the
  most likely and is consistent with every empirical result here (it un-mirrored
  the train and re-attached the ka_15 windows), but is not *proven* by the code.

The key consequence stands: **no inverse-bind / bind matrix is stored in the SRM**
(BONE is a u16 palette, PBND a bbox). The exact per-bone matrix is computed by the
engine's skeleton/animation system at bind time — the wheels/suspension sit at an
**animation-driven rest pose** that differs from the static node hierarchy, so a
model's exact rest cannot be reconstructed from the file alone.

Accordingly the importer's **AUTO** mode uses a geometric proxy to pick
skin-vs-leave per group (a group is left only when it spans most of the mesh AND
skinning it would shove it off the x=0 centre line). This matches every tested
model (car, tank, helicopter, buildings) but is a static approximation, not a
decoded flag; `FULL` (skin all) and `NONE` (skin none) remain as overrides.

## Upgrade variants (node-name convention)

Vehicles pack **every upgrade loadout into one .srm** (the shipped file has a few
pre-merged meshes named `Merged mesh N`, each skinned to a mixed bone palette).
A part's variant is read from the **name of the bone it is skinned to**:

| Bone-name token | Variant part |
|-----------------|--------------|
| `…_std`         | standard loadout (e.g. `gun0m0_std`, `door0_std`) |
| `…_upg`         | upgraded loadout (`gun2h0_upg`, `camo_body0_upg`) |
| `camo…`         | the upgrade's camouflage net (these also carry `_upg`) |
| (untagged)      | always-present base hull / tracks |

The engine shows the loadout matching the unit's in-game upgrade state; the file
carries all of them, overlapping at their attach points. The Blender importer
reproduces this with a **Variant** option that keeps faces per the selected set —
`STANDARD` = {base, `_std`}, `UPGRADED` = {base, `_upg`, camo}, `ALL` = every
part — read per-vertex from each vertex's bone tag. Single-variant models are
entirely untagged (base) so the filter is a no-op for them.
