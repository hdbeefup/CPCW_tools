# CPCW Map Editor — handoff / open issues

A native C++ (GLFW + OpenGL 3.3 + Dear ImGui + ImGuizmo) editor for Codename:
Panzers Cold War `.map` files. It loads maps/models/textures directly from the game
`.pak` archives, renders textured terrain + real `.srm` models + decoded road/decal
overlays, and edits entities and terrain with a byte-faithful save. This file lists
the FACTS not to re-derive and the OPEN issues, with enough context to fix them
without re-investigating. `README.md` describes what the editor does; this file is
for whoever picks the work up next.

## Build & run
```
cd mapeditor && cmake --build build --config Release      # MSVC; exe locks while running
build/Release/cpcw_mapeditor.exe                          # auto-mounts *.pak next to it
```
Headless verification (no window) — every write path has one:
```
E=build/Release/cpcw_mapeditor.exe
M=N:/gamePAKdata/CPCWPak/Maps/M_01.map
D=N:/gamePAKdata/CPCWPak/ProtoDB.bin
GD="N:/SteamLibrary/steamapps/common/Codename Panzers Cold War"

$E --load $M --selftest                    # parse summary
$E --structtest $M                         # field edit survives a structural insert;
                                           #   undo/redo of the insert byte-identical
$E --fieldtest      $M out.map             # one schema field written, nothing else
$E --splattest      $M out.map             # one painted splat cell, nothing else
$E --protoplacetest $M $D out.map          # place a prototype absent from the map
$E --addtest --deltest --heighttest --applytest --overlaytest
$E --pakmap "$GD" "Maps/M_01.map" --picktest [dump.bmp]   # colour-code pick buffer
$E --pakmap "$GD" "Maps/M_01.map" --shot out.bmp          # one rendered frame
```
`--shot` renders into an offscreen FBO. It CANNOT show mouse-driven UI (brush ring,
hover box, gizmo, marquee) — those need the live app; ask the user to verify.

Always cross-check a written map against the Python oracle:
`python cpcw_map.py roundtrip <out.map>` must say IDENTICAL, and
`python cpcw_map.py entities <out.map>` must agree with the native count.

Test maps on disk: `N:/gamePAKdata/CPCWPak/Maps/*.map` (45 are real CPCW `SCEN`
maps; the `add_ge_*`/`fr_*`/`ger_*` ones are an older Gepard format and are
correctly rejected).

## Key files
- `src/main.cpp` — app, menus, modes, camera, selection, gizmo, clipboard, brushes,
  browser, undo, save. `terrainHit()` (raymarch), `terrainHeightAt()`, `pickExact()`
  (colour-code), `pickAny()` (cheap AABB, hover only), `drawGizmo()`,
  `placePrototypeAt()`, `applyTerrainBrush()`, `applySplatBrush()`.
- `src/viewport3d.h` — all rendering. `renderPickBuffer()`/`pickBufferAt()`/
  `pickBufferRect()`, `loadModel()` (handedness), `buildModels()`,
  `buildSplatTextures()` + `refreshSplatWeights()`, `buildOverlays()`, `render()`.
- `src/mapfile.cpp` — native `.map` parser + writer. `apply_edits_inplace()`,
  the in-memory structural ops, `reparse_entities()`, `map_chunk_outline()`.
- `src/overlays.cpp` — decode roads (GROL/GROA) + decals (GDCL/GDEC).
- `src/protodb.cpp` — `protodb_full_index()` (guid -> model/name/schema),
  `protodb_map_schema()`. `src/thumb.cpp` (THMB), `src/pak.cpp`/`vfs.cpp`.
- `docs/MAP_FORMAT.md` §7 (GTRD terrain/splat), §9 (GROA/GDEC). `docs/FORMAT_SRM.md`.
  `cpcw_map.py` is the Python oracle.

## Facts already established (don't re-derive)
- **Handedness**: `.srm` is DirectX LH. `loadModel()` negates BOTH X and Z (a 180°
  Y rotation, det +1, **not** a mirror — a single negate mirrors decal text) and
  keeps normal winding. Exterior is CCW, so **Back-cull (`cullMode=1`) is correct**.
  Entity world pos = `{pos.x, pos.z(elev), pos.y}`; **model yaw = `e.dir + 180`**.
  `View > Model cull` / key **C** cycles cull; key **X** is a debug flip.
- **Entity `Scale`** (SEntityDesc) is real and must be applied — 2884/3427 records
  on M_01 carry it, 708 non-unit, values ~0.5–1.34. Verified against the oracle.
- **Prototype GUIDs are always exactly 36 characters** (2017/2017 in ProtoDB,
  3427/3427 on M_01). This is what makes "place a prototype the map has never used"
  a size-preserving byte splice instead of OBJT construction.
- **ProtoDB schema ↔ map schema**: `SP<X>` ↔ `S<X>Desc`. SPDoodad→SDoodadDesc,
  SPVehicleUnit→SVehicleUnitDesc, SPBuildingUnit→SBuildingUnitDesc, SPSquad→
  SSquadDesc, SPEffectEntity→SEffectEntityDesc. Derivable, not guesswork.
- **Structural edits are in-memory.** Mutate `Scene::raw`, then `reparse_entities()`
  re-walks only the entity table; terrain/splat/overlay data is preserved and its
  offsets shift only if they sit at or after the edit point. **Always
  `flushEditsToRaw()` first** — anything not yet in the buffer is lost otherwise.
  Undo is keyed on entity **ID**, never index: an insert renumbers every later one.
- **Terrain**: splat-blended real layer `.dds`, base layer opaque + overlays by
  per-vertex weight, tiled by `uv_scale * terrainTile` (0.125). Layer path e.g.
  `Terrain/Layer/Tiles/Gritty ground/Gritty_ground_08c` -> resolve `.dds` by basename
  stem index with map-prefix (`M1_`,`Tutor_1_`) stripping + longest-prefix fallback.
  The per-layer uint8 opacity grids sit back to back at `Scene::splatOff`; painting a
  layer must also clear the layers composited ON TOP of it or the paint is invisible.
  M_01 has NO concrete layer; the airfield base is `Gritty_ground_08c` (grey gravel),
  so the apron reads grey but not as smooth as the game's concrete asset.
- **Roads (GROA)**: container = 24-byte header (u32 record count + 5 dwords) then
  records = variable prefix (9 or 18 bytes) + `GROA` chunk. GROA body = `u32 type(11)`
  + `u32 nodeCount` + `nodeCount × 36-byte nodes` + trailer(4x4 matrix/bbox) + u16
  material path (first `Terrain/...` string) + u16 shader. Each 36-byte node = 9
  floats: `x,y,z` (world centreline, y≈0 -> project onto heightmap) + 6 aux (aux[0]=
  segment length, aux[3..5]/[6..8]=tangent). **No per-node width field** — width is
  texture-derived (below). Walk records by SCANNING for the next `GROA` tag.
  Decals (GDEC) = `u32(6)` + float `cx,cz,sizeX,sizeY,rot` + material -> a rotated
  terrain-projected quad. Verified: M_01 320 roads + 126 decals, all materials resolve.
- **Road width — Ghidra-RE'd.** Roads are texture-projected STRIPS (engine
  `FUN_004d7a10`); width = road-texture SHORT dim (across-road px) × WPT.
  `overlays.cpp` stores centrelines in `Scene::roadSplines`; `viewport3d.h
  buildOverlays` extrudes them using `resolveTexDims()` — strip textures (1024x128
  narrow / 1024x256 wide / 1024x512 Dwide) give exact proportional widths
  (2.56/5.12/10.24 full), square textures (cobblestone/runway) fall back to name.
  `resolveTexKey` skips corner/cross/junc pieces so roads pick the tiling strip.
  Full RE writeup: `N:\gamePAKdata\re\ROADS_RESULT.md`, memory [[cpcw-road-groa]].

## OPEN issues
1. **Merged-tank "track explosion" (17 heavy tanks) — DEFERRED, the one genuinely
   unsolved rendering bug.** `srm_bone_node_list` (`viewer/src/srm_model.cpp`
   ~391-403) ALL-fallback (`unk4 != 4`) maps track verts onto rotated
   `dust`/`entrance` gameplay markers -> verts fling to X±5.6. Needs the engine's
   real `model+0x180` bone-gather order (Ghidra). Affected: FR_ARL-44,
   SU_Object_279, US_Maus, US_A41_Centurion, SU_IS-10, SU_Product_416, SU_ISU_152M,
   US_M103, US_M53-GMC, US_M48_Patton, US_M59, US_M26-Pershing, su_tiger, US_tiger,
   SU_T-54, US_M41_Bulldog, SK_Skoda_E100.
2. **Oracle parity not applied**: `cpcw_srm.py` / `cpcw_srm_writer.py` still have the
   latent vertex-declaration-prefix bug on foliage that the C++ loader fixed.
3. **Road width `WPT = 0.02`** (`viewport3d.h`) is a calibration, not the engine's
   runtime constant. TUNE if widths look off; verify tiling on maps other than M_01.
4. **Area fills** (aprons/plazas) are still centroid-fan triangulated in
   `overlays.cpp` (a convexity assumption), and detected by a name+bbox heuristic.
5. **Roads and decals are read-only.** They decode and render; there is no write
   path, so the Shader/Decals mode says so and opens the chunk inspector.
6. **Not decoded at all**: splines/rivers, lake & water data, the `WTHR`
   weather/lighting block, the trigger system (Lua bodies). `View > Map chunks` is
   the read-only starting point for any of them — inspect the bytes, don't invent
   fields, and land the layout in `docs/MAP_FORMAT.md` before building UI.
7. **Strings are read-only in Properties** — editing one would resize the record.
   Doing it properly means patching the enclosing VOBJ/OBJT sizes as well as the
   container chain; the machinery for the container chain already exists.
8. **No settings persistence.** Panel visibility, snap steps, brush parameters,
   favourites and recent maps are all lost on exit. `imgui.ini` keeps only the dock
   layout. A versioned `key value` settings file with one-time migrations is the
   obvious next ops task.

## Verified-working (leave alone unless regressing)
Textured terrain, road/decal decode+render, THMB thumbnails, ProtoDB browser,
byte-faithful save + structural add/delete, colour-code picking, gizmo + snapping,
clipboard, schema Properties, splat painting, handedness, raymarch cursor picking,
brush ring, hover/selection highlight.
