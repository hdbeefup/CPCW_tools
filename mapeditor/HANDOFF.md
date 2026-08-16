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
$E --chunktile  <map|dir>                  # container sizes tile exactly (see below)
$E --overlayscan <map|dir>                 # GROL/GDCL/GRVL pools + used-list invariants
$E --wthrtest   <map|dir> [out.map]        # WTHR layout + field-order semantics
$E --heaptest   <map|dir>                  # scenario trees: every OBJS section walks to its SCHD
$E --roadauxtest <map|dir>                 # GROA Catmull-Rom handle identity (no NEW mismatch)
$E --roadwritetest <map> out.map [slot] [node]   # move a road node + re-derive handles
$E --overlaypicktest <map>                 # world-space core of decal / road-node picking
$E --scentest   <map|dir>                  # scenario record VALUES (semantics, not just the walk)
$E --blcktest   <map|dir>                  # BLCK two-plane decode + world coverage
$E --strtest    <map> out.map              # variable-length string resize (+ a .grown.map copy)
$E --sunprobe   $M                         # sun-swizzle evidence (reports, no verdict)
$E --settingstest tmp.ini                  # settings round-trip + unknown-key survival
$E --crashtest                             # fault on purpose; report must name the frame
$E --pakmap "$GD" "Maps/M_01.map" --picktest [dump.bmp]   # colour-code pick buffer
$E --pakmap "$GD" "Maps/M_01.map" --shot out.bmp          # one rendered frame
```
Headless runs (`--shot`/`--uishot`/`--picktest`) deliberately ignore the settings
file, so a harness render never depends on the operator's saved view toggles.
`--shot` renders into an offscreen FBO. It CANNOT show mouse-driven UI (brush ring,
hover box, gizmo, marquee) — those need the live app; ask the user to verify.

Always cross-check a written map **three** ways:
```
$E --chunktile out.map                     # container sizes tile exactly
python cpcw_map.py roundtrip out.map       # must say IDENTICAL
python cpcw_map.py entities out.map        # must agree with the native count
```
**`roundtrip` alone does NOT validate container sizes, and never did.**
`cpcw_map.py:_serialize_chunk` re-emits each chunk's *original* byte span and
never recomputes a size, and `pack()` echoes any trailer verbatim — so a
structural edit that forgets to bump an ancestor still round-trips IDENTICAL.
Demonstrated: insert 100 bytes at M_01's `GROL` content end, bump only `SCEN`,
and the oracle reports "re-write IDENTICAL" on a corrupt map while `--chunktile`
fails it naming `SCEN`, `WRLD` and `GTRN` with exact byte deltas.

`--chunktile <map|dir>` is the real detector: every container's children must
tile its content exactly (no gap, no overlap, no slack) after that container's
fixed sub-header, and every `OBJS.meta_schema_off` must still point at its own
`SCHD` child. Baseline on the shipped corpus: **45/45 OK, 225/225 OBJS,
gap=0 overlap=0 trailer=0**.

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
- `src/overlays.cpp` — decode the three overlay slot pools: roads (GROL/GROA),
  decals (GDCL/GDEC) and rivers (GRVL/GRVR), keeping each record's byte offsets.
- `src/weather.cpp` — WTHR lighting presets; `kWeatherFields[]` is the single
  source of truth for the record layout (decoder, writer, UI and harness all
  iterate it).
- `src/settings.cpp` — versioned `key value` persistence. `src/crashdump.cpp` —
  symbolized fault report (needs the Release PDB; see OPEN 8).
- `src/protodb.cpp` — `protodb_full_index()` (guid -> model/name/schema),
  `protodb_map_schema()`. `src/thumb.cpp` (THMB), `src/pak.cpp`/`vfs.cpp`.
- `docs/MAP_FORMAT.md` §4.9 + §11 (slot pools, WTHR/SWeather), §7 (GTRD terrain/
  splat), §8 (BLCK), §9 (GROA/GDEC/GRVR). `docs/FORMAT_SRM.md`.
  `cpcw_map.py` is the Python oracle — but see the three-way cross-check above.

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
- **Roads (GROA)**: body = `u32 version(11)` + `u32 N` + `N × 36-byte nodes` +
  `u32 N2` + `N × 16-byte params` + u8 + u16 material path (first `Terrain/...`
  string) + u16 shader + 22-byte tail. Each node = 9 floats: `x,y,z` (world
  centreline, y≈0 -> project onto heightmap) + an `in` and an `out` Catmull-Rom
  handle. **No per-node width field** — width is texture-derived (below).
  Decals (GDEC) = `u32(6)` + float `cx,cz,sizeX,sizeY,rot` + material -> a rotated
  terrain-projected quad. Verified: M_01 320 roads + 126 decals, all materials resolve.
  For the CONTAINER these sit in — a slot pool, not a header-then-scan — and for
  the river pool, see the GROL/GDCL/GRVL entry below; it supersedes the earlier
  "24-byte header + 9-or-18-byte per-record prefix, scan for the next tag" reading.
- **Road width — Ghidra-RE'd.** Roads are texture-projected STRIPS (engine
  `FUN_004d7a10`); width = road-texture SHORT dim (across-road px) × WPT.
  `overlays.cpp` stores centrelines in `Scene::roadSplines`; `viewport3d.h
  buildOverlays` extrudes them using `resolveTexDims()` — strip textures (1024x128
  narrow / 1024x256 wide / 1024x512 Dwide) give exact proportional widths
  (2.56/5.12/10.24 full), square textures (cobblestone/runway) fall back to name.
  `resolveTexKey` skips corner/cross/junc pieces so roads pick the tiling strip.
  Full RE writeup: `N:\gamePAKdata\re\ROADS_RESULT.md`, memory [[cpcw-road-groa]].

- **WTHR is decoded** (`src/weather.{h,cpp}`, `docs/MAP_FORMAT.md` §4.9 + §11).
  `PATH`/`CAMS`/`WTHR` share a **slot-pool** container: version, live count,
  `freeHead/freeTail/listHead/listTail/capacity`, then `capacity ×
  {i32 next, i32 prev, u8 isFree[, record]}`. A live WTHR slot = `u32
  recordVersion(13)` + `u16`-prefixed name + **194 fixed bytes**. Walks to the
  exact chunk end on **45/45 maps, 219 records**.
  - **Iterate `listHead → next`, never the slot array.** M_17's chain is
    7→0→1→4, the authored order (`M_17_1_Clouds`, `_2_Rain_1`, `_2_Rain_2`,
    `_3_After_Rain`); slot order would present them scrambled.
  - The record body is read in **stream order, which is not the struct order**
    (`FogColor` 8th, `SunSpecular` 12th). Both orders sum to 194, so "the walk
    consumed the chunk exactly" passes on all 45 maps for a wrong permutation —
    `--wthrtest` pins the order by semantics instead (alphas ≡ 1.0 on 876/876,
    `SunShadow.w` ∈ 0.25..2.58 so it is *not* an alpha, `EffectCount` ≡ 4,
    `|SunDirection|` ≡ 1, `TimeOfTheDay` ∈ [0,24], `Brightness`/`Contrast` ≡ 1.0,
    `FogEnd > FogStart`). Verified by reintroducing a `SunShadow`↔`FogColor`
    swap: alphas fall to 811/876 and `SunShadow.w` collapses to 1.0..1.0.
  - Refuse **chunk version 2** (a flat count+records list) and assert
    **recordVersion == 13** — the loader is an `if (N < version)` cascade, so a
    lower version is a shorter record and the fixed stride would desync.
  - **The engine-active preset is the one literally NAMED `"Default"`** — never
    slot 0, never the list head. M_17 has none; MPMission/(6) Breakthrough has
    `Default` *and* `Default2`; Domination/(4) Ring Of Fire ships two live
    presets both named `Night_multi` with different bodies. **Key UI on the slot.**
  - NOT invariants: `CloudMovementDir == WindDirection*CloudSpeed` fails on
    8/219 (worst 0.00999999, M_07 `02_cloudy`) — never auto-rewrite it. Night
    presets are not uniformly dark (M_06 `03_night` SunColor is 1.24/1.50/1.80;
    the black ones are the MP `Night_multi` at exactly 0,0,0). `SunColor` is not
    clamped to 1 — it reaches 2.78. `unknown98`/`unknown9c` are read at v13 but
    absent from the reflection table: do not name them.
  - The **sun axis is half resolved, and the half that is open is documented**.
    `SunDirection[1] < 0` on 219/219, so index 1 is vertical and the vector is the
    direction light *travels* (`L = -SunDirection`). Of the four horizontal
    candidates, `--sunprobe` **eliminates two** (`-D` entirely, and X-only: 69.6°
    and 49.3° from the legacy hard-coded light on the stock `Default`). The
    surviving two — engine→GL negate-X-and-Z, and a horizontal swap — both land at
    3.2° and **cannot be separated**, because the stock `Default` is
    `(0.4156, -0.8090, 0.4156)` where `D.x == D.z` makes a swap a no-op. The
    negate-X-and-Z form is implemented, because it is the transform `loadModel`
    already applies. **Not confirmed — narrowed.**
    - Only a map whose `Default` IS that stock vector discriminates (M_01, M_12).
      M_06's `Default` is `(-0.282, -0.500, -0.819)` and has no reason to match a
      hard-coded constant; its distance to one means nothing. **Eliminated
      hypothesis:** a first version of `--sunprobe` averaged the angle over every
      preset in a map and named a different winner per map (2, 1, 2) — it was
      averaging sunset against noon against night. Do not re-derive that.
    - Corroboration for the implemented choice: under it, the `Default` preset
      renders within ~7 mean levels of neutral shading, which is what a 3.2°
      direction difference predicts; a 49–70° error would light the map from a
      visibly different quadrant.
- **GROL / GDCL / GRVL are SLOT POOLS**, not "a 24-byte header then records with a
  9-or-18-byte prefix" (that prefix was a free slot plus a real slot header):
  `u32 usedCount, freeHead, freeTail, usedHead, usedTail, slotCount`, then
  `slotCount × {i32 next, i32 prev, u8 isFree[, chunk]}`. Walks to the exact
  content end on **135/135** shipped containers with usedCount matching every
  time — 3944 GROA, 4114 GDEC, 51 GRVR. `overlays.cpp` now keeps every record's
  byte offsets (`Scene::roadPool/decalPool/riverPool`), which is what a write path
  needs. `--overlayscan` also asserts the used chain visits every live slot once
  with `prev` the exact inverse.
  - **Order — measured, and deliberately NOT acted on.** The engine's *loader*
    (FUN_004bdba0/dd80/df60, one templated routine, allocating 0x120/0x184/0x11c)
    iterates the **slot array**, i.e. file order. The used list is a *different*
    order on **71 of 90** road/decal pools (`usedHead != 0` on 28). If the
    *renderer* walks the list instead, list order is the Z-order — but the loader
    does not settle that, and nothing has been read on the draw side. The measured
    visual stake is small: of overlapping decal pairs, the two orders disagree on
    1/51 in M_02, 24/302 in M_05, 0/16 in M_01. **Emission stays in slot order**
    (unchanged rendering) until someone reads the draw loop. Do not reorder on the
    strength of the container layout alone.
- **CPCW HAS RIVERS** — `GRVL` is empty in 17/45 maps, and earlier notes measured
  only those. The other 28 carry **51 `GRVR` records** (49 with >1 node; the two
  single-node ones are both in Domination/(8) Sole Survivor and cannot form a
  ribbon). Same record shape as GROA at version 2, and two things differ that
  matter: `y` is a CONSTANT water level per record (−13.0 … +15.4), so a river must
  **not** be projected onto the heightmap; and `params[i].float0` is a REAL
  per-node width in world units (1.0 … 37.0), so rivers need none of the road's
  texture-dimension derivation. Mode 1 (Spline/River) is live and read-only;
  `View > Rivers` toggles them.
  - **River material -> texture is unsolved.** Materials are per-map names
    (`Terrain/River/M_03/M_03_rivers`, `Terrain/River/Water`, `.../Elbe_Kanal`)
    and no DDS of that stem ships; the real textures are generic under
    `CPCWPak/Rivers/`. `resolveTexKey`'s longest-prefix fallback silently borrowed
    an unrelated terrain texture (rivers drew as dark ground), so
    `resolveRiverTex()` matches the `Rivers/` set instead. That is a heuristic,
    not the engine's mapping.
- **GROA node aux floats are Catmull-Rom handles**, not "segment length + tangent":
  each node carries an `in` and an `out` 3-float handle whose magnitudes are the
  distances to the previous and next node. They are therefore re-derivable when a
  node moves. Full record: `u32 version(11) + u32 N + N×36 nodes + u32 N2 + N×16
  params + u8 + str16 material + str16 shader + 22-byte tail`.
- **BLCK is two planes, not 12 bytes per cell**: a `uint16` flag plane then a
  `uint8` type plane, both at BLCK's **own** header dims (`payload == w*h*3` on
  45/45). Those dims are `world*2` on only **41/45** — Island Thunder 544x464 →
  1088x960, Urban Legend 528x448 → 1088x896, Islands Of Hope 720x720 →
  1472x1472, The Last Village 576x400 → 1152x832 — so never derive them from
  WRLD. Separated from the old reading by spatial coherence (M_01: the plane
  reading is 0.9956/0.9879 neighbour-equality with 4 and 6 distinct values; the
  6×uint16 reading is 0.942 with ~30 per lane and near-identical stats across all
  six lanes, the signature of one field read at six offsets). The old
  `(1,1,1,1,1,1)`=passable table and the "251 cell patterns" figure were both
  artifacts of that misalignment. Values are still undecoded: **read-only**.
  **Now RENDERED** as `View > Terrain > Blockmap (BLCK)` (terrainMode 3), a
  categorical unlabelled palette over the type plane, brightened where the flag
  plane is non-zero, sampled NEAREST (interpolating category ids invents a third).
  The mapping is **0.5 world units per texel with the padding at the far edge**,
  so the plane spans `blckW*0.5 x blckH*0.5` world units — NOT the world rect.
  That was measured, not assumed: on Domination/(2) Urban Legend (padded 16 units
  in width) road nodes concentrate on one block type **81.7%** of the time under
  that mapping vs **57.9%** under stretch-to-world. Two other padded maps are
  ties, and M_01 confirms the two readings coincide when dims are exactly 2x.
  A hypothesis that dims are `2*ceil(world/32)*32` was **falsified** — 37/45,
  worse than the plain 2x rule's 41/45. There is no derivation: read the header.
- **A schema field's type id is COMPOSITE: the low byte is the container kind.**
  `0x8A`/`0x90`/`0x9C` -> `ARRY`, `0xA5` -> `HEAP`, `0xA6` -> `HASH`; the bytes
  above it are the element (and for HASH the key) type. So `0x148A` is an array
  of entrefs, `0x1790` an array of bytes, `0x039C` an array of *bools* (not the
  "flags string" the tools assumed), `0x89A5` a heap of objects and `0x8904A6` a
  string->object hash. `0x??90` and `0x??8A` are indistinguishable on disk — both
  write a plain `ARRY`. Do NOT derive an element width from `(size-4)/count`: it
  gives 18.00/23.27/32.40/24.50 on four real trigger folders.
- **`HEAP`'s six header dwords are in a DIFFERENT ORDER from the other slot
  pools**: `slotCount, usedCount, freeHead, freeTail, listHead, listTail` — count
  first, no trailing capacity — then `slotCount × {i32 prev, i32 next, u8 isFree}`
  with an `OBJT` only when `isFree == 0`. Note `prev` precedes `next`. `SLocation`
  has a field literally named `HeapIndex`: the **slot index is the stable key**,
  and the used chain is non-monotonic in slot index on 58 of 924 heaps.
- **Three field-type readings were wrong, and all three are invisible on
  entities.** `0x0011` GUID and `0x0012` ref are u16-length-prefixed *strings*,
  not 4-byte handles (fixing them at 4 desynchronises 133/225 OBJS sections);
  `0x002B` locstr is a **u32 character count + 2 bytes/char UTF-16LE**, not a
  u16-prefixed string; `0x0005` is a **pair of floats, not a double**. The last
  one cannot be settled by any structural walk — both readings are 8 bytes — only
  by values: `SLocation.Size` reads 0..697 x 0..677 as vec2f (ellipse
  half-extents, world tops out near 512x672) and 7.5e9/2.3e20 as a double.
  Entity records reach exactly `{0x01,0x02,0x03,0x04,0x06,0x12,0x14,0x17,0x19,
  0x88,0x898A}` across all 45 maps, which contains none of the corrected types —
  so the fixes are inert on entities **by construction**, and `--selftest` is
  byte-identical before and after.

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
5. **Decals and road nodes have write paths; rivers are read-only, and roads have
   no UI yet.** `overlay_set_road_node()` moves a GROA node and re-derives the
   Catmull-Rom handles of that node and both neighbours — size-preserving, only
   the three 36-byte node records change (`--roadwritetest`). Measured facts you
   should not re-derive: the handle identity `T = normalize(P[i+1]-P[i-1]);
   in = -T*|P[i]-P[i-1]|; out = T*|P[i+1]-P[i]|` holds on **27790 of 27792**
   shipped interior nodes (median error 5e-07; the 2 exceptions are both in
   Domination/(4) The Last Village, worst 0.187) — it is a re-derivable DEFAULT a
   designer can perturb, so `--roadauxtest` asserts **no NEW mismatch**, never
   zero. **Endpoints are NOT reproducible**: `|in| == |out|` holds 7876/7876 but
   the stored direction departs from the chord by up to **83 degrees** (median
   0.89, only 3226/6206 within 1 degree), so an endpoint keeps its direction
   bit-for-bit and only its magnitude is scaled by the segment-length ratio.
   `pos.y` is exactly 0.0 on all 35668 nodes — roads are 2D and projected onto
   the heightmap — so a drag must not write an elevation.
   The **Shader/Decals panel has Decals and Roads tabs**, and overlays are also
   **clickable and draggable in the viewport** in that mode. Undo (`CMD_ROADNODE`)
   snapshots the exact bytes of the affected span rather than re-running the move
   backwards — an endpoint's magnitude is scaled by a float ratio and inverting it
   is not guaranteed to land on the original bits.
   **Overlay selection is a SECOND domain** (`g_ovlSel` +
   `g_decalSel`/`g_roadSel`/`g_roadNodeSel`), deliberately kept out of
   `g_selection`: that `std::set<int>` holds ENTITY indices and is read by eight
   call sites, so tagged indices would corrupt every one. The two are mutually
   exclusive. Picking is a **CPU test against the terrain hit point**, not a
   colour-code pass — decals are flat on the terrain and nodes are points, so the
   ray already resolves both, and the entity pick buffer is untouched
   (`--picktest` unchanged: 3191 instances, 44.3% self, 92.0% resolved). It
   ignores occlusion on purpose, so a node under a tree stays grabbable.
   **Pick priority was measured, not chosen**: nodes of the ALREADY-selected road
   first, then decals, then any road node. Giving all road nodes priority — the
   obvious reading of "nodes are precise handles" — made **112 of M_01's 126
   decals unpickable**, because sidewalk and asphalt decals are road-adjacent by
   nature so a node is almost always nearer. `--overlaypicktest` reports that
   shadowing count; it must stay 0. Still missing: a gizmo for overlays (drag is
   direct-manipulation only) and river editing.
   **1670 of the 7876 endpoints ship all-zero handles**, which scaling leaves
   untouched — that made `--roadwritetest`'s first undo-span check VACUOUS (a
   deliberately narrowed span still passed) until the default test node moved from
   1 to 2, where both neighbours are interior and always change. If you add a
   check here, verify it against a node that can actually fail it.
   `overlay_set_decal()` writes the five GDEC transform floats straight into
   `Scene::raw` — 20 bytes, size-preserving, so `save_map_native` stays
   byte-faithful with NO ancestor-size patching. Verified safe on the corpus:
   decals at genuinely different positions ship byte-identical trailing data
   (Domination/(4) Islands Of Hope has one trailing body shared by four
   transforms), so nothing after the transform derives from it. `--decalwritetest`.
   Still open beyond the road UI: river editing, and create/delete for any of
   them (structural — free-list surgery, the one unproven inference).
   NOTE `g_overlayDirty` — decal and road meshes are BAKED geometry, so an edit
   without a re-decode + `buildOverlays` is correct and completely invisible.
6. **The scenario object trees are DECODED (read-only); only the Lua trigger
   bodies remain.** The `HEAP` (type `0x89A5`) that used to block this is solved
   and documented in `docs/MAP_FORMAT.md` §5.6/§5.7/§6.1. `--heaptest` walks all
   45 maps: **225/225 OBJS sections consume to exactly their own SCHD offset**
   (164,540 OBJT records, 924 HEAPs, 45 HASHes, 0 bytes left over), and the walk
   reaches all 1265 SLocation / 260 SGroup / 167 SObjective / 139 TriggerVar /
   116 SCameraPath — the counts a brute-force VOBJ tag scan finds. A tag-first
   walk reached **none** of them. Still open: what a trigger's Lua body *means*,
   **Trigger mode is now LIVE and read-only**: locations
   (drawn as ellipses in their own `Color`, `Size` being HALF-extents), objectives,
   trigger variables, groups and camera paths, all off `Scene::scen`
   (`map_scenario_read`). `--scentest` asserts SEMANTICS, not just the walk: every
   one of 1265 locations is named, sized sanely, and has its centre inside its own
   map's rect — none of which holds if a field is read at the wrong offset or
   width. Largest half-extent in the corpus is **697.3, a location literally named
   `AllOnMap`**, so whole-map zones are real data, not a decode bug; M_01's largest
   is 192.6 (`newObjHq`). Sorting locations by AREA hides these — sort by max
   extent. Still open: what a trigger's Lua body *means*, and any write path.
   - `WTHR` is **decoded and editable** (Light mode). Still open: the horizontal
     sun swizzle, so nothing lights the viewport yet.
   - `GRVL`/`GRVR` rivers are **decoded and drawn** (River mode, read-only). Still
     open: the material -> texture mapping, and no write path.
   - **Lake/water is not "undecoded", it is absent.** No lake, water, river or
     spline *schema* exists in any of the 45 maps. Mode 6 is marked `MK_RETIRED`.
     Note this is only true of *water*: `GRVL` is **not** vestigial — earlier
     notes calling it "24 empty bytes in every map" measured only the 17 maps
     where it is empty; the other 28 carry real `GRVR` river records.
7. ~~Strings are read-only in Properties.~~ **CLOSED.** `set_entity_string()`
   resizes the record: it splices the bytes, then bumps the enclosing VOBJ, the
   enclosing OBJT, every ancestor container size, and the `OBJS schema_offset` —
   the only absolute file offset a `.map` contains (checked over 285,272
   integer-typed field values across all 45 maps; chunk-tag hits run at 1.22x the
   LOCAL-density chance rate and no field exceeds ~5% where a pointer would be
   100%). Same-length values still take the ordinary in-place path.
   `Prototype` stays read-only on purpose: it is a fixed 36-char GUID the model
   loader resolves through, so an arbitrary-length value there is a different and
   much riskier edit — change the prototype through the browser instead.
   **This is the edit that proves why `--chunktile` exists.** Drop the OBJT size
   bump and `--strtest` fails with a **606,182-byte gap** and a broken entity
   list — while `cpcw_map.py roundtrip` calls the same corrupt file
   "re-write IDENTICAL". That is not a hypothetical any more; it is reproducible
   by commenting out one line.
   `--strtest` grows, shrinks and restores a field, checking tiling, entity count
   and readback at every step, and requires the round trip back to the original
   length to be **byte-identical to the input file**. It also leaves a
   `.grown.map` copy, because the restored file cannot prove a RESIZED map is
   well-formed; that copy passes `--chunktile` (5/5 OBJS), `--heaptest`,
   `--scentest`, `--blcktest`, `--overlayscan`, `--roadauxtest` and the oracle.
8. ~~No settings persistence.~~ **CLOSED.** `src/settings.{h,cpp}` — a versioned
   `key value` file beside the exe, unknown keys preserved, one `if (v < N)`
   migration block per bump. Two ordering constraints that are easy to get wrong:
   it must load **before `glfwCreateWindow`** (window size is restored) *and*
   **before the initial `loadScene`** (`Settings::load` clears the map, so a load
   afterwards wipes the recent-map entry that scene just pushed). Headless runs
   (`--shot`/`--uishot`/`--picktest`) neither read nor write it, so a harness
   render can never depend on the operator's saved toggles. `--settingstest`.
   `src/crashdump.cpp` writes a symbolized `.txt` + `.dmp` on an unhandled fault;
   it needs the Release PDB, so `CMakeLists.txt` adds `/Zi` and `/DEBUG` — without
   those every frame resolves to `??` and the report is worthless. `--crashtest`.

## Verified-working (leave alone unless regressing)
Textured terrain, road/decal decode+render, THMB thumbnails, ProtoDB browser,
byte-faithful save + structural add/delete, colour-code picking, gizmo + snapping,
clipboard, schema Properties, splat painting, handedness, raymarch cursor picking,
brush ring, hover/selection highlight, WTHR decode + read-only Light mode.

## Modes
`kModes[]` carries a `ModeKind`, and every predicate asks for the kind rather
than comparing a mode index — the indices are 0..8 and must stay that way
(settings and `View > Mode` write them). `MK_RETIRED` means *measured absent*,
not undecoded. Note `activeToolIsPlace()` is gated on `MK_OBJECT`: Shader/Decals
also names its first tool "Place", and without the gate a click in that mode
dropped an entity on the terrain.
