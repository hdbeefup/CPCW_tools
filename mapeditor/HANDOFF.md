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

## RECENTLY FIXED (verify live, then delete from here)
- **SRM model fidelity (shared `viewer/src/srm_model.{cpp,h}` + `mapeditor/src/
  viewport3d.h` — fixes BOTH viewer and map editor).**
  - **Foliage trees loaded (36 `Objects/Tree/*.srm` incl. oak_1_autumn).** Their VERS
    chunk prepends a 32-byte vertex-declaration block before the vertex array; the
    parser read POSITION 32 B early -> NaN verts -> invisible. Fix: skip `avail-need`
    leading bytes in the VERS loop. UV/normal are interleaved INSIDE the stride-44
    POSITION stream (uv@+16), so `SrmStream` gained an `offset` field and parseMesh
    synthesizes a TEXCOORD view at +16 for the foliage class. Verified: `--srmcheck
    oak_1_autumn.srm` -> nonfinite=0, sane bbox, meshesWithUV=2/2.
  - **Alpha-test cutouts (foliage/fence/window).** Model shaders forced alpha=1. Now
    per-mesh `alphaTest` (RenderMesh->Part/GpuMesh): true only when diffuse came from a
    `DiffuseTexture`/`Diffuse` key (alpha=mask), NEVER `DiffuseSpec*` (alpha=spec — would
    perforate tanks). Shader `if(a<0.5) discard;` + drawn two-sided (cull off) for those
    parts. `pickDiffuse` reports the flag.
  - **FR_ARL-44 / merged-tank 'shadow' sheet removed.** `srm_build_render_w` drops tris
    skinned to a node named `shadow` (also US_M103, US_Maus). Verified: shadow mesh
    1438->376 tris.
  - **Texture quality.** `loadTexture`: mipmaps (`glGenerateMipmap`) + trilinear +
    anisotropy (added `glGenerateMipmap`/aniso enums to glcore.h). Model shader: gamma
    (pow 1/2.2, fixes muddy) + hemispheric ambient + key + fill light (was one flat
    directional). Dev harness: `--srmcheck <file>` (finite/bbox/uv/alpha report).
  - **DEFERRED — merged-tank "track explosion" (17 heavy tanks).** `srm_bone_node_list`
    (srm_model.cpp ~391-403) ALL-fallback (`unk4 != 4`) maps track verts onto rotated
    `dust/entrance` gameplay markers -> verts fling to X+-5.6. Needs the engine's real
    `model+0x180` bone-gather order (Ghidra). Affected: FR_ARL-44, SU_Object_279,
    US_Maus, US_A41_Centurion, SU_IS-10, SU_Product_416, SU_ISU_152M, US_M103,
    US_M53-GMC, US_M48_Patton, US_M59, US_M26-Pershing, su_tiger, US_tiger, SU_T-54,
    US_M41_Bulldog, SK_Skoda_E100. (`cpcw_srm.py`/writer have the same latent
    decl-prefix bug on foliage — oracle parity not yet applied.)
- **Camera zoom-to-cursor + no zoom-in slowdown.** Wheel dollies toward the terrain
  point under the cursor and lands the pivot on real ground (`updateCamera`,
  main.cpp). Min dist 5->1, near plane 1.0->0.5 (viewport3d.h). WASD/pan floors
  raised so movement doesn't crawl when zoomed in.
- **#2 Mirrored fuselage/decal text (SOLVED).** loadModel's single negate-Z (LH
  .srm -> RH GL) is a REFLECTION -> mirrors text. Fix (baked): negate BOTH X and Z
  (=180deg Y rotation, no mirror) + NORMAL winding + keep +180 yaw. Exterior is CCW,
  so **Back-cull (cullMode=1)** is default/correct (Front-cull shows the interior).
  `View>Model cull` / key **C** toggles cull; key **X** = debug flip (mirror is baked).
- **Roads invisible in live app (SOLVED).** `buildOverlays` was missing from the
  scene-dirty rebuild (only --shot called it). Added -> roads/decals/apron fills show.
- **Picking (improved).** `Viewport3D::pickModel` picks by projected model AABB (via
  `pickAny`) so big models are clickable anywhere + hover tracks the right model.
- **#3 Place-on-click.** With a browser prototype selected + the **Place** tool
  active (Object/Unit/Ambient), left-click on terrain drops a grounded copy
  (`activeToolIsPlace()` + `placeDuplicateAt()`, main.cpp; reuses `terrainHit` +
  `add_entity_native`). Panel button relabelled "Place at view center".
- **#4 Thick highlight lines.** New geometry-shader program `thickProg` expands
  GL_LINES into screen-space quads (`drawThickLines`, viewport3d.h) — real px width
  despite GL 3.3 core clamping glLineWidth to 1. Used by hover/selection boxes +
  brush ring. Added `GL_GEOMETRY_SHADER` to glcore.h + a 3-arg `glProgram`.
- **#1 Road width — Ghidra-RE'd + rewritten.** Roads are texture-projected STRIPS
  (engine `FUN_004d7a10`); width = road-texture SHORT dim (across-road px) x WPT.
  Rewrite: `overlays.cpp` stores centrelines in `Scene::roadSplines`; `viewport3d.h
  buildOverlays` extrudes them using `resolveTexDims()` — strip textures (1024x128
  narrow / 1024x256 wide / 1024x512 Dwide) give exact proportional widths
  (2.56/5.12/10.24 full), square textures (cobblestone/runway) fall back to name.
  WPT=0.02 (calibrated, not the engine's exact runtime const). `resolveTexKey` skips
  corner/cross/junc pieces so roads pick the tiling strip. Full RE writeup:
  `N:\gamePAKdata\re\ROADS_RESULT.md`, memory [[cpcw-road-groa]]. Overlay z-fighting
  fixed (depth-write off, roads-then-decals 2 passes); purple decals fixed (aux/normal
  maps excluded in fallback). Area fills (apron/plaza concrete) still centroid-fan
  triangulated in `overlays.cpp`. TUNE: WPT if widths look off; verify tiling/other maps.

## Verified-working (leave alone unless regressing)
Textured terrain, road/decal decode+render, THMB thumbnails, category browser,
byte-faithful save + structural add/delete (harnesses: `--overlaytest`, `--addtest`,
`--deltest`, `--heighttest`, `--protodbtest`, `--paktest`, `--thumbtest`), handedness
(text readable, solid models), raymarch cursor picking, smooth drag-follow, brush ring
+ size readout, hover/selection highlight.
