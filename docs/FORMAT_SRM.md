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

- Y-up coordinate system
- Euler rotations in radians, applied as XYZ order
- Scale includes a 4th component (W), typically 1.0
