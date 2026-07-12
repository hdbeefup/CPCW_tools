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
  *Assemble*, *Show Skeleton*, *Import Textures*, and an *Extra Texture Dir*
  (point at the extracted data root so cross-referenced `.dds` files resolve).
  The model is parented under one Empty that converts SRM's Y-up space to
  Blender's Z-up.
  - **Assemble** has three modes:
    - **Full (articulated)** — *default*. Skins every part by its bone. Correct
      for fully-articulated models: tanks and tracked vehicles (Patton,
      bulldozer) where the whole model is bone-local.
    - **Parts only (static body)** — skins only the small parts (wheels…) and
      leaves the one large body group in place. Use for cars/models whose body
      shifts or whose wheels float in Full mode (e.g. the Moskvitch).
    - **Off (raw bind pose)** — no skinning. Correct for static models
      (**buildings**), which are authored already-posed in model space.
- **Export:** *File > Export > CPCW Model (.srm)*. Select an imported model and
  export. The exporter re-writes from the pristine source file (byte-faithful —
  round-trip verified over all 2087 game models), so importing a game asset and
  exporting it never corrupts it. *Write Back Node Transforms* also applies
  moves/rotations/scales of root-level nodes back into the file. (Authoring new
  geometry from scratch is future work — the writer/format supports it; the
  Blender-mesh → VERS/INDS/BONE encoder is not built yet.)
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
- **Vertex stream type is the `Usage` word (D3DDECLUSAGE), not `Semantic`.**
  Normal/tangent/binormal all share `Semantic == 4`; the importer keys off
  `Usage` (0=pos 1=blendweight 2=blendindices 3=normal 4=uv 5=tangent
  6=binormal). This fixed normals never resolving (the old code looked for
  `Semantic == 3`, which no stream has).
- **Rigid skinning.** Each vertex stores a **bone-palette index in byte 3 of the
  NORMAL stream** (`Usage == 3`). Corpus-verified: `max(byte3) <= bone_count-1`
  for every skinned mesh. Vertices are in bone-local space; transforming each by
  its bone's world matrix assembles the model. Rotations compose `Rx @ Ry @ Rz`.
  Smooth-skinned meshes (266 of them) instead carry explicit BLENDINDICES
  (`Usage == 2`) + BLENDWEIGHT (`Usage == 1`) streams.
- **Round-trip writer.** `srm_writer.py` parses to an editable model and
  re-serializes byte-for-byte (all 2087 files). This backs the SRM exporter and
  guarantees import→export fidelity.
- **DDS.** `SRM_Blender/dds.py` decodes DXT1/DXT3/DXT5 (and simple uncompressed
  DDS) in pure Python, validated pixel-exact against Pillow.
- **Textures & alpha.** Base colour comes from `DiffuseTexture` or
  `DiffuseSpecTexture`; `NormalTexture` drives a Normal Map node. The diffuse
  alpha channel holds specular/team-mask data (not opacity) and is intentionally
  left unwired, so surfaces render solid.

## Known limitations / future work

- **Assembly mode is not auto-detected.** A model may be fully bone-local (skin
  everything — "Full"), have a static model-space body mixed with bone-local
  parts (skin parts only — "Parts only"), or be entirely model-space (skin
  nothing — "Off", buildings). These cases are geometrically indistinguishable
  per-group and no in-file flag separating them was found, so the mode is a
  user choice (default "Full"). If a model looks wrong, try the other modes.
- **Map terrain is flat.** No per-vertex height array has been reverse-engineered
  (`GTRD` is splat paint, `BLCK` is passability); the plane sits at Z=0.
- **No real models in maps yet.** A map entity's `Prototype` (a GUID) is not yet
  resolved to its `.srm` via `ProtoDB.bin`. The entity loop in `import_map.py` is
  structured so a resolver can later replace an Empty with a model instance.
- **Single material per mesh.** Sub-mesh/multi-material splitting is not applied.
