# CPCW Map Editor — handoff / open issues

A native C++ (GLFW + OpenGL 3.3 + Dear ImGui) editor for Codename: Panzers Cold War
`.map` files. It loads maps/models/textures directly from the game `.pak` archives,
renders textured terrain + real `.srm` models + decoded road/decal overlays, and
edits entities + terrain with a byte-faithful save. This file lists what's DONE and
the OPEN issues, with enough context to fix them without re-investigating.

## Build & run
```
cd mapeditor && cmake --build build --config Release      # MSVC; exe locks while running
build/Release/cpcw_mapeditor.exe                          # auto-mounts main*.pak next to it
```
Headless verification (no window):
```
# render one frame to BMP (fixed orbit camera — cannot show hover/cursor UI):
build/Release/cpcw_mapeditor.exe --pakmap "<GAME_DIR>" "Maps/M_01.map" --shot out.bmp
build/Release/cpcw_mapeditor.exe --overlaytest N:/gamePAKdata/CPCWPak/Maps/M_01.map
```
`--shot` renders into an offscreen FBO. It CANNOT show mouse-driven UI (brush ring,
hover box) — those need the live app; ask the user to verify, or add a temporary
forced-state hack. Test maps on disk: `N:/gamePAKdata/CPCWPak/Maps/*.map` (45 are
real CPCW `SCEN` maps; the `add_ge_*`/`fr_*`/`ger_*` ones are an older Gepard format
and are correctly rejected). Steam paks: `N:/SteamLibrary/steamapps/common/Codename
Panzers Cold War/*.pak`.

## Key files
- `src/main.cpp` — app, menus, modes, camera, picking, drag, brushes, undo, save.
  `terrainHit()` (raymarch), `terrainHeightAt()`, `pickEntity()`, `updateCamera()`,
  the browser (category tree + THMB thumbnails), `placeDuplicate()`.
- `src/viewport3d.h` — all rendering. Terrain (splat-textured), models, overlays,
  entity dots, highlight boxes, brush ring. `loadModel()` (handedness), `buildModels()`,
  `buildSplatTextures()` + `resolveLayerTex()`, `buildOverlays()`, `render()`.
- `src/overlays.cpp` — decode roads (GROL/GROA) + decals (GDCL/GDEC). **Road width is
  the open problem here.**
- `src/mapfile.cpp` — native `.map` parser (chunk tree, GTRD terrain+splat, entities,
  structural add/delete, byte-faithful save). `src/scene.h` — the `Scene` struct.
- `src/thumb.cpp` (THMB), `src/protodb.cpp` (guid->model), `src/pak.cpp`/`vfs.cpp`.
- `docs/MAP_FORMAT.md` §7 (GTRD terrain/splat), §9 (GROA/GDEC — decoded).
  `docs/FORMAT_SRM.md` (model + THMB). `cpcw_map.py` is the Python oracle.

## Facts already established (don't re-derive)
- **Handedness**: `.srm` is DirectX LH. `loadModel()` converts to GL RH by negating
  vertex/normal Z + reversing triangle winding (a depth flip — text stays readable).
  Culling is OFF; the model shader is two-sided via `gl_FrontFacing`. Entity world pos
  = `{pos.x, pos.z(elev), pos.y}`. **Model yaw = `(e.dir + 180)` degrees** (user
  confirmed a 180° offset). Roads/decals/positions match the game layout.
- **Terrain**: splat-blended real layer `.dds`, base layer opaque + overlays by
  per-vertex weight, tiled by `uv_scale * terrainTile` (0.125). Layer path e.g.
  `Terrain/Layer/Tiles/Gritty ground/Gritty_ground_08c` -> resolve `.dds` by basename
  stem index with map-prefix (`M1_`,`Tutor_1_`) stripping + longest-prefix fallback.
  M_01 has NO concrete layer; the airfield base is `Gritty_ground_08c` (a grey-ish
  gravel), so the apron reads grey but not as smooth as the game's concrete asset.
- **Roads (GROA)**: container = 24-byte header (u32 record count + 5 dwords) then
  records = variable prefix (9 or 18 bytes) + `GROA` chunk. GROA body = `u32 type(11)`
  + `u32 nodeCount` + `nodeCount × 36-byte nodes` + trailer(4x4 matrix/bbox) + u16
  material path (first `Terrain/...` string) + u16 shader. Each 36-byte node = 9
  floats: `x,y,z` (world centreline, y≈0 -> project onto heightmap) + 6 aux (aux[0]=
  segment length, aux[3..5]/[6..8]=tangent). **No obvious per-node width field.**
  Currently rendered as a centreline ribbon extruded by a material-name half-width
  heuristic (`wide`/`narrow`/`road`). Walk records by SCANNING for the next `GROA`
  tag (prefix length varies). Decals (GDEC) = `u32(6)` + float `cx,cz,sizeX,sizeY,rot`
  + material -> a rotated terrain-projected quad. Verified: M_01 320 roads + 126
  decals, all materials resolve.

## OPEN ISSUES (priority order)
1. **Road/pavement/apron width (biggest visual gap).** Wide roads, sidewalk pavements,
   and the airfield apron render as thin ribbons. Find the true width: check the GROA
   *trailer* (the ~149-byte block after the nodes: a 4x4 matrix + bbox — width may be
   derivable from the bbox vs centreline length), or the GROL per-record 9/18-byte
   prefix, or whether some records are area polygons (nodes forming a closed loop to
   be triangulated, not a centreline). Cross-check by rendering M_01 vs an in-game
   screenshot of the same street. Do NOT reinterpret nodes as left/right edge pairs —
   that was tried and breaks the streets (nodes are a centreline).
2. **Yaw / mirroring confirmation.** `+180` is applied. Confirm it's right for all
   objects. Check whether fuselage text is ever mirrored (user's image 11 looked
   reversed, but "USAF" read correctly — may be far-face bleed-through from two-sided
   no-cull rendering; if real, the depth-flip axis or winding needs revisiting).
3. **Place-on-click.** Replace the "Place copy at view center" button: with a browser
   prototype selected, left-click on terrain (use `terrainHit`) to place a copy there.
   `add_entity_native` already does the structural insert.
4. **Thick highlight lines.** Selection/hover boxes + brush ring use `glLineWidth(2.5)`,
   which GL 3.3 CORE clamps to 1.0. If lines look thin, render them as screen-space
   quads instead of `GL_LINES`.

## Verified-working (leave alone unless regressing)
Textured terrain, road/decal decode+render, THMB thumbnails, category browser,
byte-faithful save + structural add/delete (harnesses: `--overlaytest`, `--addtest`,
`--deltest`, `--heighttest`, `--protodbtest`, `--paktest`, `--thumbtest`), handedness
(text readable, solid models), raymarch cursor picking, smooth drag-follow, brush ring
+ size readout, hover/selection highlight.
