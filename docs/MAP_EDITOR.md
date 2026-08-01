# CPCW Map Editor — design & roadmap

Goal: a map editor for **Codename: Panzers Cold War** (the game shipped none),
modelled on the S.W.I.N.E. editor's UX but **modern and portable** — cross-platform,
cross-compilable, 64-bit, standalone, no DX9, with a UI toolkit reusable across
other projects.

## Constraints & stack
- **Format core:** Python `cpcw_map.py` (mature, schema-driven parser + now a
  byte-faithful writer). It is the *oracle* and the first place edit-ops land.
- **Editor front-end:** **C++17 + GLFW + OpenGL 3.3 + Dear ImGui** — portable,
  64-bit, cross-compilable; ImGui panels/gizmos are reusable elsewhere. The map
  parse/write is ported from the Python reference into this core (the way
  `viewer/src/srm_model.cpp` mirrors `cpcw_srm.py`), so the shipped editor has no
  Python dependency.
- The S.W.I.N.E. editor (`swinedecomp`, an MFC/DX9 app) and swinedecomp source are
  **feature/UX references only** — Gepard 1 vs 3, different `.map` binary.

## Interface (mirrors the S.W.I.N.E. editor, rebuilt in ImGui)
Mental model: a **mode switcher** that swaps a **(toolbar + dock panel)** pair over
a single **3D perspective viewport**. Exactly one mode active at a time.

**Modes** (each = a tool set + an asset/param panel):
| Mode | Focus | Tools | Panel |
|------|-------|-------|-------|
| Vertex / Terrain | terrain | grab, raise, lower, set-plane, raise/lower-to-plane, smooth, texture-blend paint, tile-fill, area-select | brush height / size / pressure + texture-layer list |
| Spline | terrain | new river/spline, node move, close loop, per-node altitude/width, per-node texture | sub-spline rings, sliders, texture picker |
| Object / Doodad | object | place, move, lift, rotate, tilt, align-to-terrain | folder+file prototype browser |
| Unit | object | place, move, rotate | team/category list + unit-type list; per-unit properties |
| Ambient | object | place, move, lift, distance | folder+file browser |
| Shader / Decals | terrain | place, move, rotate, z-order | decal-texture browser |
| Lake / Water | terrain | place, move, lift | lake-type list; flow dir + sparkle |
| Light | global | (settings panel) ambient/sun/fog/shadow + rain/snow/night toggles | HSV + sliders |
| Trigger | logic | named locations/zones; trigger list w/ conditions & actions | trigger list + cond/action lists |

**Menus:** File (New/Open/Save/Save-As) · Edit (Undo/Redo/Cut/Copy/Paste/Delete/
Select All/None, Unit & Map properties) · View (toolbar/statusbar/sidebar dock L/R/
off, wireframe/filled/filled+wire, blockmap, texture no/sketch/real) · Mode (the
above) · Tools (mod manager) · Test-in-game. *(Native New/Open/Save only — no
import/export.)*

**Viewport:** single 3D perspective; orbit = middle-drag, pan = right-drag /
arrows, zoom = wheel / PageUp-Down; toggles for wireframe / textures / blockmap /
grid; hover-highlight + click-select, right-click multi-select; a translucent
"ghost" preview follows the cursor when placing.

**Key shortcuts:** Ctrl+S save, Ctrl+Z/R undo/redo, Ctrl+C/V/X, Ctrl+Del delete,
Ctrl+A / Shift+Ctrl+A select all/none, Enter properties, B blockmap, W
filled+wireframe, arrows pan, Ctrl-invert on raise/lower.

**Entity/property model:** placed objects are prototype instances; the property
panel is schema-driven (units expose Player/Level/HP/Ammo/Fuel/Cargo/AI/upgrades).

## Editing model (how saves stay byte-faithful)
`cpcw_map.py` parses the file into a chunk tree over a **mutable** buffer.
- **Size-preserving edits** (entity Pos/Dir/Elevation/HP/…, terrain heights, splat
  cells): written **in place** via `set_field` / `set_entity_field` / `move_entity`;
  every non-edited byte is untouched, so `pack()` stays identity except the edit.
- **Structural edits** (add/delete entity, resize terrain — *future*): mutate the
  chunk tree's payloads and **recompute chunk sizes up the parent chain** in
  `pack()`. The `_serialize_chunk` writer already reconstructs from boundaries, so
  this is the natural extension point.

## Milestones
- [x] **M1 — byte-faithful writer** (`MapFile.pack()/write()`, `roundtrip` cmd).
      68/68 CPCW maps byte-exact.
- [x] **M2 — in-place field edits** (`set_field`, `set_entity_field`, `move_entity`;
      `get_entities(with_offsets=True)`). Verified: move + scalar edits persist,
      size-preserving, round-trip intact.
- [x] **M2b — terrain height edits** in place (`heightmap_info`, `set_height`,
      `set_heights`). Verified: brush-patch raise persists, size-preserving,
      round-trip intact. (Splat-layer painting landed natively in M10.)
- [x] **M3 — structural edits** (add/remove entity; size recompute up the chain).
      Done natively in C++ (`mapfile.cpp`), not in the Python oracle: the buffer
      ops patch every ancestor container size, the OBJS `schema_offset` and the
      UNTS `entity_count`. Verified by `--addtest` / `--deltest` / `--structtest`
      and cross-checked against `cpcw_map.py entities` + `roundtrip`.
- [x] **M4 — C++ editor scaffold** (`mapeditor/`): GLFW+OpenGL+ImGui, docking,
      File/Edit/View/Mode menus, mode switcher, swappable tool/param panel,
      Properties panel, dockable viewport. Builds (MSVC) and runs.
- [x] **M4b — scene data bridge**: `cpcw_map.py scene` exports terrain+entities
      as JSON; the editor loads it (direct `.json`, or a `.map` via python using
      `CPCW_MAP_PY`) and shows an entity list + top-down markers (player-coloured,
      click-select) + properties. Verified headlessly (`--selftest`): 779
      entities / 576x576 terrain load on both paths.
- [x] **M4c — save path**: `cpcw_map.py apply <map> <out> --edits e.json` applies
      an edit list (entity pos/player/hp/... by ID) through the in-place setters.
      Verified full round-trip: scene -> edit -> apply -> reload persists, only the
      edited fields' bytes change (8 bytes on an 8.9 MB map), byte-faithful.
- [x] **M5a — 3D viewport**: real OpenGL 3.3 terrain mesh from the heightmap
      (elevation-shaded + Lambert lighting), entity markers (player-coloured points,
      selection highlight), orbit/pan/zoom camera, rendered into the dockspace
      central node. Compact GL loader (`glcore.h`), no glad/glew. Heightmap via a
      raw-f32 sidecar from `cpcw_map.py scene`; NaN sentinel cells sanitised.
      Verified with `--shot` FBO render-to-BMP (Ring of Fire, Fields of Glory).
- [x] **M5b — splat-textured terrain**: `cpcw_map.py` gains `get_splatmap`
      (upstreamed from the Blender fork) + a ground-type palette; `scene` bakes a
      per-vertex RGB colormap sidecar; the editor colours the terrain by it.
- [x] **M5c — native C++ .map parser** (`mapfile.cpp`): a faithful port of
      `cpcw_map.py` (chunk tree, SCHD schemas, OBJT/VOBJ entities, heightmap
      locator, splatmap colormap). The editor loads `.map` **directly, no Python**.
      Verified vs the Python oracle: entity counts / grid dims / render match
      exactly (Ring of Fire 779, Fields of Glory 2116, Grinder 805, M_02 3152).
      `cpcw_map.py` still handles the byte-faithful **save** path.
- [x] **M5d** — real entity **models** (native ProtoDB resolve `Prototype`->`.srm` +
      the viewer's `.srm` loader) with **DDS textures**, placed at each entity's
      pos+yaw. Buildings/vehicles/aircraft render textured on the terrain.
- [x] **M5e** — native everything: `.map` load + save + ProtoDB all in C++ (no
      Python). Native Windows Open dialog + drag-and-drop. WASD/arrow camera with a
      speed floor. 3D click-select, drag-to-move, `[`/`]` rotate, editable Pos/Dir/
      Player in Properties, Ctrl+S native byte-faithful save.
- [x] **M6 — full interactive editing** (all native, byte-faithful):
      - Textured DDS models on the terrain; WASD/arrow + orbit/pan/zoom camera.
      - Entities: 3D click-select, drag-to-move, `[`/`]` rotate, editable Pos/Dir/
        Player; **delete** (Delete), **duplicate/place** (Ctrl+D + prototype browser).
        Structural add/delete patch every ancestor container size + OBJS
        `schema_offset` + UNTS `entity_count` (WRLD is a container to EOF holding
        UNTS — must shrink/grow too).
      - Terrain: Raise/Lower/Smooth brushes (camera-ray → grid, radial falloff),
        per-cell dirty mask so save changes only brushed cells.
      - **Undo/redo** (Ctrl+Z/Y); **Ctrl+S** native save (`<map>_edited.map`).
- [x] **M7 — PAK reading**: native `.pak` reader (decrypt + zlib) + VFS; the exe
      auto-mounts main1/main2/enUS.pak next to it and reads maps/models/textures/
      ProtoDB straight from them (single-exe in the game folder). File > "Open from
      .pak" browses maps. Verified: a full scene renders from the Steam paks alone.
- [x] **M8 — model thumbnails**: the **THMB codec is decoded** (RLE over 3-byte BGR
      pixels; `docs/FORMAT_SRM.md`). `cpcw_srm.py read_thumbnail()` / `thumb` CLI and
      native `mapeditor/src/thumb.cpp load_thmb()` (verified byte-identical, 2087/2087).
      The prototype browser now shows each model's baked preview as a thumbnail grid
      (lazy-decoded + GL-cached).
- [x] **M9 — game-accurate terrain + roads/decals + browser polish**:
      - **Real splat-textured terrain**: composites the actual ground-layer `.dds`
        (grass/dirt/gritty/cobblestone/…) blended by the per-vertex splatmap and
        tiled by uv_scale, replacing the flat keyword palette. `scene.h` stores
        `terrainLayers` + `splatWeights`; `viewport3d.h` `terrainTexProg` +
        `buildSplatTextures`/`resolveLayerTex` (map-prefix-stripping stem index).
        View > Terrain: Textured / Palette / Height.
      - **Roads & decals decoded and rendered** (`overlays.cpp`, `MAP_FORMAT.md`
        §9): GROL/GROA road ribbons + GDCL/GDEC decal quads, projected onto the
        terrain, textured + alpha-blended. Verified 45/45 CPCW maps (3932 roads +
        4109 decals, 0 unresolved materials). View > Roads / Decals; `--overlaytest`.
      - **Browser**: THMB thumbnails flipped upright, grouped by category
        (Vehicles/Buildings/Objects/Nature…), placement grounded on the terrain +
        auto-selected (view-preserving reload).
- [x] **M10 — trustworthy editing, real selection, full content pipeline**:
      - **Structural edits happen in memory** (`delete_entity_bytes` /
        `add_entity_bytes` / `insert_objt_at_index` / `erase_objt_bytes` +
        `reparse_entities`), not by writing a temp map and reloading it. That
        removed two silent data-loss defects: a place/delete used to discard every
        unsaved field edit, and the undo stack was keyed on entity *index*, which
        an insert renumbers. Undo is keyed on entity ID and structural commands
        carry the exact OBJT bytes, so undo/redo are byte-identical.
      - **GPU colour-code picking**: an offscreen pass draws terrain as code 0 and
        each model flat-shaded with its entity index in RGB. One pixel read =
        exact click select (occlusion- and alpha-cut-correct); a rectangle read =
        rubber-band multi-select. Runs on demand, not per frame.
      - **Selection set + ImGuizmo** translate/rotate about the centroid, world or
        local, grid/angle snapping, drop-to-ground, batched undo.
      - **Place any prototype**, not just ones already on the map. No OBJT
        construction needed: ProtoDB's `SP<X>` schema is the map's `S<X>Desc`, and
        every prototype GUID is exactly 36 chars, so it is "clone a record of the
        matching schema, splice the GUID" — size-preserving. Browser sources from
        ProtoDB with search, categories, thumbnails and favourites.
      - **Clipboard** (cut/copy/paste/paste-in-place over the selection) and a
        **schema-driven Properties panel** covering every fixed-width field.
      - **Changes since last save** panel derived from a per-entity saved-state copy.
      - **Terrain**: SetPlane / Raise>Plane / Lower>Plane / Grab wired to the Height
        slider, Ctrl-invert, and **splat layer painting** (Blend / TileFill) written
        back through `Scene::splatOff` — size-preserving, so still byte-faithful.
      - **Entity `Scale`** is read and applied (2884/3427 records on M_01 carry it).
      - Save As / overwrite-original with two-phase `.tmp` -> `.bak` -> rename.
      - Every write path has a headless harness: `--structtest`, `--fieldtest`,
        `--splattest`, `--protoplacetest`, `--picktest`.
- [ ] **Remaining**: roads/decals are decoded and rendered but view-only (no write
      path); splines/rivers, lake & water data, the WTHR weather/lighting block and
      the trigger system are not decoded — those modes open a raw chunk inspector
      instead of offering invented fields.

See also `docs/MAP_FORMAT.md`, `cpcw_map.py`, the `.srm` viewer under `viewer/`
(the renderer whose UX this echoes), and `CPCWMap_Blender/` (existing preview).
