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
  The left-handed→right-handed conversion is baked into geometry (models import
  upright and un-mirrored); the root Empty carries only the uniform scale.
  - **Assemble** modes:
    - **Auto** — *default*. Skins bone-local parts (wheels, turret, tracks,
      rotors, panels, hulls) into place and leaves already-posed model-space
      bodies (a civilian car body/speaker) where they are — one setting for
      tanks, cars, aircraft and buildings. (An inferred proxy for the engine's
      unstored inverse-bind, not a decoded flag; if a model looks wrong, try the
      overrides below.)
    - **Full (debug)** — skins *every* group by its bone. Same as Auto for
      fully-articulated models; shifts a civilian car body off-centre.
    - **Off (raw bind pose)** — no skinning; each mesh placed by its node matrix.
  - **Variant** — vehicles pack every upgrade loadout into one file; the part's
    variant is read from the bone it is skinned to (`_std` / `_upg` / `camo`).
    *Standard* (default) shows base + standard parts; *Upgraded* shows base +
    upgraded parts + camo net; *All* shows everything (they overlap at attach
    points). Non-variant models are unaffected.
- **Export:** *File > Export > CPCW Model (.srm)*. Select an imported model and
  export. The exporter re-writes from the pristine source file (byte-faithful —
  round-trip verified over all 2087 game models), so importing a game asset and
  exporting it never corrupts it.
  - *Write Back Node Transforms* applies moves/rotations/scales of root-level
    nodes back into the file.
  - *Write Back Geometry (reshape)* rewrites each mesh's vertex **positions** from
    the edited Blender mesh, so you can reshape existing geometry and export it.
    Requires the model to have been imported with **Assemble = Off (raw bind
    pose)** — that's the only mode whose Blender coordinates invert cleanly back
    to stored positions — and the vertex count unchanged (reshape, don't add or
    remove verts; topology-changed meshes are skipped, not corrupted). Indices,
    UVs, normals and skinning are preserved, so a no-edit export stays
    byte-identical. (Authoring brand-new meshes/topology from scratch is still
    future work — the writer's `replace_geometry` supports it, but a full
    Blender-mesh → VERS/INDS/BONE encoder with new bone weights is not wired up.)
- **Map:** *File > Import > CPCW Map (.map)*.
  - **Real Heightmap** (*default on*) — builds a subdivided terrain mesh
    displaced by the decoded GTRD elevation grid (the actual in-game hills;
    *Terrain Resolution* caps subdivisions per axis). Turn off for a flat plane
    with optional *Tint Passability* (green = passable, red = blocked, BLCK).
  - **Paint Terrain** (*default on*) — tints the terrain per-vertex from the
    GTRD paint layers (decoded splatmap), so roads / fields / river beds / grass
    read like the game. Uses the real layer `.dds` ground-texture averages when
    the *Data Root* is found, else a colour-by-ground-type fallback.
  - **Place Real Models** — resolves each entity's `Prototype` through
    `ProtoDB.bin` to its `.srm` and instances the actual model at the entity's
    position / yaw / scale, so the scene matches the game (needs the SRM add-on
    enabled and the extracted *Data Root*; auto-detected from the .map path).
  - **Place Entity Markers** — an Empty per entity, grouped into collections by
    type (Units / Buildings / Doodads / …), each carrying `cpcw_prototype`,
    `cpcw_id`, `cpcw_player` custom properties.

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
- **Left-handed → right-handed (un-mirror).** SRM is DirectX **left-handed**
  Y-up. The old importer converted to Blender Z-up with `Rx(90°)` — a pure
  rotation (determinant +1) that leaves every model **mirrored** (train
  lettering read reversed; the ka_15's one-sided canopy floated off). The fix
  bakes a **reflection** into geometry: swap `(x,y,z)→(x,z,y)` on positions and
  normals (det −1) and **reverse triangle winding**; node/bone matrices are
  conjugated `P·M·P` so the skeleton still assembles.
- **Rigid skinning + AUTO assembly.** Each vertex stores a **bone-palette index
  in byte 3 of the NORMAL stream** (`Usage == 3`; corpus-verified
  `max(byte3) <= bone_count-1`). The game renders `BoneWorld[b] @ InvBind[b] @ v`
  but stores no InvBind, so **Auto** mode uses a geometric proxy: skin bone-local
  parts (wheels/turret/tracks/rotors/panels/hulls) by their bone matrix, leave
  already-posed model-space bodies in place. It matches every tested model but is
  an inferred heuristic, not a decoded flag — `Full`/`Off` remain as overrides.
  Rotations compose `Rx @ Ry @ Rz`. Smooth-skinned meshes (266) instead carry
  explicit BLENDINDICES (`Usage == 2`) + BLENDWEIGHT (`Usage == 1`) streams.
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

- **Assembly is an inferred heuristic, not the engine's rule.** *Auto* skins
  bone-local parts and leaves model-space bodies via a geometric proxy for the
  unstored inverse-bind (see above). It matches every tested model, but the true
  per-bone rule lives in the D3D render path and hasn't been reversed yet; a few
  edge cases (e.g. the moskvitch `_speaker` body sits ~0.1 low) need it. `Full`/
  `Off` remain as overrides.
- **Map terrain has no tiled texture detail.** The splatmap paint is reproduced
  as a per-vertex tint (roads/fields/grass read correctly), but the layer `.dds`
  textures are not tiled/baked onto the surface — the terrain is coloured, not
  texture-mapped.
- **Single material per mesh.** Sub-mesh/multi-material splitting is not applied.
