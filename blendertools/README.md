# CPCW Blender Add-ons

Blender import add-ons for *Codename: Panzers Cold War* (Gepard engine) assets,
built on the parsers in `../cpcw_srm.py` and `../cpcw_map.py`. Target: **Blender
4.2+ / 5.0** (uses only Blender's bundled Python — no numpy/Pillow).

Two independent, self-contained add-ons:

| Folder | Add-on | Imports |
|--------|--------|---------|
| `SRM_Blender/` | **CPCW Model (.srm)** | A single model: geometry, UVs, normals, DDS-textured materials |
| `CPCWMap_Blender/` | **CPCW Map (.map)** | A scenario: flat terrain extent (passability-tinted) + a labelled Empty per placed entity |

## Install

For each add-on: zip the **folder** (so the zip contains `SRM_Blender/__init__.py`,
etc.) and in Blender use *Edit > Preferences > Add-ons > Install…*, pick the zip,
and enable it. Or drop the folders into your Blender `scripts/addons/` directory.

## Getting the data

The importers read loose files. Extract them from the game paks first with
`../cpcw_pak.py` (or the selective helper used during development). Models and
textures live in `main2.pak`; maps in `main1.pak`:

```
Vehicles/**/**.srm   Buildings/**/**.srm   ...   # models
**/*.dds                                          # textures (same tree)
Maps/*.map                                        # scenarios
ProtoDB.bin                                       # prototype DB (for future model resolution)
```

## Usage

- **Model:** *File > Import > CPCW Model (.srm)*. Options: uniform *Scale*,
  *Assemble (Skinning)*, *Show Skeleton*, *Import Textures*, and an *Extra
  Texture Dir* (point at the extracted data root so cross-referenced `.dds`
  files resolve). The model is parented under one Empty that converts SRM's
  Y-up space to Blender's Z-up.
  - **Assemble (Skinning)** (default on) transforms each vertex by its bone so
    articulated models come together — wheels/tracks/turret/limbs in place.
    Turn it **off** for static models (most **buildings** and some props), which
    are authored already-posed in model space and will look scattered if skinned.
- **Map:** *File > Import > CPCW Map (.map)*. Options: *Build Terrain Plane*,
  *Tint Passability* (green = passable, red = blocked, from the BLCK grid),
  *Place Entities*, *Max Entities*. Entities become Empties grouped into
  collections by type (Units / Buildings / Doodads / …), each carrying
  `cpcw_prototype`, `cpcw_id`, `cpcw_player` custom properties.

## SRM format notes (reverse-engineered here)

The importer corrects several misreadings of the `.srm` format that the original
converter got wrong:

- **Node hierarchy.** A node's `unk3` field is its **parent index** (-1 = root),
  not "always -1". Bone world transforms are composed up this chain.
- **Multi-mesh.** `unk5` on a node is a **mesh index** into a block of MESH
  chunks stored after *all* node headers — not an inline "has-mesh" flag. Only
  the last node header abuts the block, so the old inline scan captured just one
  mesh. ~810 of 2087 models (most buildings, many props) silently lost geometry;
  they now import in full.
- **Rigid skinning.** Each vertex stores a **bone-palette index in byte 3 of the
  normal stream** (the stride-4 stream whose byte 3 spans `[0, bone_count-1]`).
  Vertices are in bone-local space; transforming each by its bone's world matrix
  assembles the model. Rotations compose `Rx @ Ry @ Rz`.
- **DDS.** `SRM_Blender/dds.py` decodes DXT1/DXT3/DXT5 (and simple uncompressed
  DDS) in pure Python, validated pixel-exact against Pillow.
- **Textures & alpha.** Base colour comes from `DiffuseTexture` or
  `DiffuseSpecTexture`; `NormalTexture` drives a Normal Map node. The diffuse
  alpha channel holds specular/team-mask data (not opacity) and is intentionally
  left unwired, so surfaces render solid.

## Known limitations / future work

- **Static vs skinned is not auto-detected.** Skinning assembles articulated
  models (vehicles, characters) but scatters static ones (buildings), which are
  authored already-posed. No reliable in-file flag distinguishing the two was
  found, so skinning is a user toggle (default on). Untick it for buildings.
- **Map terrain is flat.** No per-vertex height array has been reverse-engineered
  (`GTRD` is splat paint, `BLCK` is passability); the plane sits at Z=0.
- **No real models in maps yet.** A map entity's `Prototype` (a GUID) is not yet
  resolved to its `.srm` via `ProtoDB.bin`. The entity loop in `import_map.py` is
  structured so a resolver can later replace an Empty with a model instance.
- **Single material per mesh.** Sub-mesh/multi-material splitting is not applied.
