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
      round-trip intact. (Splat-layer painting still TODO.)
- [ ] **M3 — structural edits** (add/remove entity; size recompute up the chain).
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
- [ ] **M5b — port parse/write to C++** (drop the Python bridge) using the Python
      oracle; render real entity **models** (reuse the `.srm` loader) not just point
      markers; splat/texture the terrain.
- [ ] **M6 — GUI editing**: brushes/gizmos drive the edit-ops in the viewport and
      emit the edit list / call the save path; undo/redo; structural edits
      (add/remove entity — needs chunk-size recompute + schema-offset fixups).

See also `docs/MAP_FORMAT.md`, `cpcw_map.py`, the `.srm` viewer under `viewer/`
(the renderer whose UX this echoes), and `CPCWMap_Blender/` (existing preview).
