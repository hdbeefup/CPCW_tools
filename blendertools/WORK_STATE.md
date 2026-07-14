# CPCW Blender tools — work state (resume notes)

## LATEST SESSION (handedness + skinning + map preview) — all committed
Big deliverables since the round-trip writer:
- **Handedness fix (commit 37cd613):** .srm is DirectX LEFT-handed; old Rx(90)
  root left every model MIRRORED (train text reversed, ka_15 windows "floated").
  Now baked reflection swap (x,y,z)->(x,z,y) + reversed winding + node matrices
  conjugated P·M·P. Root Empty = scale only. See [[srm-handedness]] memory.
- **AUTO skinning (37cd613):** replaced FULL/PARTS/NONE default with per-group
  rule `_skin_decisions` (skin bone-local parts, leave posed model-space bodies).
  INFERRED heuristic (render path NOT recovered), FULL/NONE kept as overrides.
- **Named-prop attach (387e498):** props named after an unk4=8 attach node
  (moskvitch roof `speaker`, Studebaker `awning`, `helipad`) get pinned there
  (`_attach_override`, texture-basename == node-name). Fires on 3/2087 models.
  Speaker now on the roof (verified Z[1.34,1.82] above body 1.21).
- **Export handedness (5f5758d):** node writeback conjugates P·matrix_local·P;
  no-edit round-trip still byte-identical.
- **Map real models (b5ba6db):** Prototype GUID -> ProtoDB ModelName -> .srm,
  instanced at Pos/yaw/Scale (849/859 on a Domination map). protodb.py vendored.
- **Map real terrain (2c4c04c):** GTRD f32 (w+1)x(h+1) heightmap, entity-Z
  calibrated locate (R2 0.72-1.0). get_heightmap() in cpcw_map + addon.
- Docs updated (FORMAT_SRM handedness/assembly + MAP_FORMAT §7.4). Renders in
  N:\ProjectsCODE\CPCW_tools\renders\ (gitignored). Verified via headless Blender.

### SESSION 2 (continued) — DONE
- **Map splatmap terrain paint (commit 1547bdd):** decoded the GTRD splatmap —
  one W×H uint8 opacity grid per layer right after the heightmap, then ~4 dense
  trailing grids (baked normals/AO). `region == (W*H)*(4+num_layers+4)` on every
  map. `get_splatmap()`; import_map paints per-vertex (real .dds layer averages,
  sRGB→linear) into a FLOAT_COLOR attr. M_02 shows roads/river/fields. dds.py
  vendored into map add-on. MAP_FORMAT §7.3 rewritten (SOLVED).
- **SRM upgrade-variant filter (commit 0acd636):** vehicles pack all loadouts;
  variant read from the bone name (_std/_upg/camo). Import **Variant** option:
  STANDARD (default) / UPGRADED / ALL, per-vertex-by-bone. Single-variant models
  unaffected. Patton verified. FORMAT_SRM "Upgrade variants" section added.
- **Standalone glTF mirror fixed:** cpcw_srm.py `nodes_to_glb` now applies the
  LH->RH reflection for glTF's Y-up frame (M=diag(1,1,-1): negate Z on
  pos/normal/translation, quat (x,y,z,w)->(-x,-y,z,w), reverse winding). Verified:
  GLB round-trip vs native SRM import of v200 have identical signed volume
  (125.146, ratio 1.0) -> chirally identical, un-mirrored. [[srm-handedness]].
- **Variant safety fix (commit 4b01049):** bare-"camo" match hid whole models
  named "camo*" (camo_tent). Now classify by `_std`/`_upg` suffix only (camo-net
  always carries `_upg`). Corpus: 0 models hide-everything (was 1), 18 tagged.
- **Geometry write-back / reshape (commit 75abc9b):** exporter option rewrites
  each mesh's POSITION stream from the edited Blender mesh (surgical: positions
  only; indices/UVs/normals/bone kept). Gated on Assemble='NONE' + unchanged vert
  count. Verified: no-edit byte-identical (v200); vtx move -> exact stored delta.
  Modders can now RESHAPE existing geometry, not just re-serialize.
- **Texture sibling-folder fix (commit 2e192e6):** `_find_texture` now falls back
  to a recursive basename index of the search dirs, so models that reference a
  texture in a sibling folder (SU_T-54.dds under SovietAdditional) import textured.
- **Tiled-texture terrain (commits 609a916, c7ffd35, 237e2b4):** map add-on
  builds a real node material -- each GTRD layer's .dds tiled across the surface,
  alpha-composited by per-layer splat masks. Grass/dirt/road detail like the game.
  Refinements: pick the most-PAINTED layers when over the cap (not first-N);
  per-layer palette-colour fallback when a .dds name doesn't resolve (typo'd base
  still shows grass); skip aux bump/normal/wind layers (not diffuse). Verified
  M_02 / M_06 (14 layers) / T_01. Falls back to per-vertex tint if all textures
  missing. Resolves the "terrain not textured" gap.
- Multi-submesh materials: investigated, NON-ISSUE (every mesh has 1 submesh).
- Render gallery in renders/gallery/ (models + textured maps); AUTO confirmed
  correct vs FULL for tanks (FULL mangles tracks). Tank floaters = AUTO limit.

### Skinning RE — SOLVED this session (2 Ghidra passes)
`N:\gamePAKdata\re\SKINNING_RESULT.md` (§1-8). **The exact rule is
`v_world = boneWorld[BONE_palette[boneIdx]] · v_stored` — the node WORLD matrix,
NO inverse-bind.** HIGH confidence. Pass 1: proved shader-based VS matrix palette,
ruled out fixed-function. Pass 2: found the palette FILL loop `FUN_004c0160`
(`0x004c0330`) = pure gather `palette[i]=worldMatrix[BONE[i]]` (×64 into
`model+0x180`), uploaded as shader const `SkinMatrices`; NO matrix multiply / NO
inverse in the loop; no InvBind field/string anywhere in the binary.
- This is EXACT for non-animated bones (body0, turret, hull, panels). The only gap
  is ANIMATED bones (tank road wheels `rotate*`, suspension): the engine recomputes
  their world matrix per frame, so their settled rest pose isn't in the static file.
- FULL = the exact rule (now re-labelled "Full (game's exact rule)"). AUTO (default)
  = practical heuristic that LEAVES a body anchored to an animated node, hiding that
  gap on cars. Verified FULL-vs-AUTO renders (renders/gallery/fullauto/): base
  moskvitch401 FULL==AUTO; _speaker FULL lifts body (animated suspension0l); patton
  FULL floats road-wheels. Docs/memory/operator descriptions updated to SOLVED.

### New-topology authoring — DONE (commit 3d17dad)
Full modding round-trip now works: reshape AND add/remove geometry. Import "Bone
Vertex Groups" exposes each vertex's bone as a group; export "Allow Topology
Changes" rebuilds meshes (replace_geometry + set_bones), per-vertex bone from the
dominant group, palette extended for new bones. Requires Assemble='NONE'. Verified
structurally (authored cube bound to body0 re-imports correctly; no-edit still
byte-identical). Caveat: per-vertex UV can't split a vertex at a UV seam. NOT
verifiable in-game here (no game runtime), but structurally valid + re-imports.

### STILL TODO
- Nothing substantive. **AUTO stays default** (best-looking); FULL is the
  documented exact rule (proven). The moskvitch_speaker residual is inherent
  (animated-bone rest pose not in the file) — fully explained, not open.
- Possible future polish (all optional): in-game load test of authored models
  (needs the game); UV-seam vertex splitting on export; opt-in "hide gameplay
  markers" (target_*/crew) toggle; terrain splat tiling-scale option.

## Goal
Blender import (and eventually export) of Codename: Panzers Cold War `.srm` models
+ `.map` scenarios. User wants: (1) import faithful to how the GAME reads the file
(no guessing), (2) round-trip import/export for modding. Blender 5.0, win, Python.

## Delivered & committed (branch main, repo N:\ProjectsCODE\CPCW_tools)
- `blendertools/SRM_Blender/` — .srm import addon (__init__, srm_format.py parser,
  dds.py pure-Python DXT1/3/5 decoder, import_srm.py builder). Menu: File > Import >
  CPCW Model (.srm). Assemble enum: FULL / PARTS / NONE (see skinning below).
- `blendertools/CPCWMap_Blender/` — .map viewer (map_format.py, import_map.py):
  flat terrain plane + entity Empties by type. Wheels/models in maps deferred
  (needs ProtoDB prototype->.srm link, not done).
- Standalone `cpcw_srm.py` backported: multi-mesh fix, parent(unk3), bake_skinning
  with `convert --skin full|parts|none`.
- Docs updated: `blendertools/README.md`, `docs/FORMAT_SRM.md`. `.gitignore` added.
- Installed addon copy (Blender 5.0): `%APPDATA%\Blender Foundation\Blender\5.0\
  scripts\addons\SRM_Blender` (+CPCWMap_Blender). Re-sync after edits; the addon
  may also be installed as a Blender EXTENSION (which shadows the module import).
- Test data extracted: `N:\gamePAKdata\CPCWPak` (2087 .srm, 68 .map, 3072 .dds,
  ProtoDB.bin) via scratchpad/extract_selective.py from main1.pak+main2.pak.
- Commits: 56e6bdc (addons), 75d14bc (skinning modes).

## SRM format — CONFIRMED (data + Ghidra of the loader)
- Node header 56 bytes after name: unk3 = **parent index** (-1 root); pos(3f);
  rot(3f XYZ radians, compose Rx@Ry@Rz — NOT mathutils Euler 'XYZ' which is Rz@Ry@Rx);
  scale(4f use xyz); unk4; unk5 = **mesh index** into the MESH block; unk6.
- All node headers first, THEN a block of MESH chunks. node.mesh = meshes[unk5].
  (Original tool assumed inline MESH -> lost geometry on ~810/2087 multi-mesh models.)
- Mesh: BONE chunk = u16 node-index palette; INDS; StreamCount x VERS. Rigid skin:
  per-vertex **bone-palette index = byte 3 of the NORMAL stream** (the stride-4
  stream whose byte3 spans [0,bonecount-1]; tangent/binormal stride-4 have byte3=0).
- Ghidra confirms struct: mesh+0x24 bone palette, +0x20 index buf, +0x10 vtx streams;
  node[1]=parent, node[0x12]=mesh index. Parser funcs FUN_0051a550 (mesh),
  FUN_004aed60 (node/PMOD v007), FUN_0051b940 (VERS), FUN_0051d6a0 (INDS).

## THE OPEN PROBLEM: skinning / which transform the game applies
Meshes MIX conventions in ONE mesh:
- bone-local parts (wheels): geometry stored at ORIGIN, positioned by bone.
- model-space body: geometry spans the whole model, must NOT move.
Moskvitch mesh0: body(1631v, bone suspension0l) model-space; 4 wheels(91v each) at
origin. Skinning ALL (FULL) shifts body off-center by suspension0l (~-0.6,0,1.28).
Skinning only compact groups (PARTS, extent<0.85) fixes moskvitch but BREAKS patton
(patton has large bone-local groups that need skinning). No per-group geometric
rule separates them (moskvitch body vs bulldozer body are geometrically identical).
=> Currently a user toggle FULL/PARTS/NONE. NOT auto-detectable from geometry.
The true rule is in the GAME RENDER code (not the parser). Standard skinning
v=bone_world @ invBind @ v would need per-bone invBind (identity for wheels,
bone_world^-1 for body) — no bind-matrix chunk exists in the SRM (only BONE/INDS/VERS).
So invBind is computed by the engine at render/bind time — must be found in Ghidra.

User's ground-truth exports (great data, keep):
`C:\Users\swine\Documents\RawExport.glb` (FULL import, unedited) and
`C:\Users\swine\Documents\CorrectedByHandExport.glb` (user placed wheels around body,
speaker on roof). Parser: scratchpad/glb_parts.py. Findings: corrected wheels sit at
their bone spacing AROUND the centered body (== PARTS result); user noted model is
off-center in FULL (== body shifted by suspension0l). Speaker mesh (mesh2, bound to
suspension0r) belongs on the ROOF but no bone points there — model-specific quirk.

## Ghidra / RE setup (ALL under N:\gamePAKdata\re)
- Steamless (downloaded): `steamless\Steamless.CLI.exe`. Unpacked exe: `CPCWu.exe`
  (source game exe: "N:\SteamLibrary\steamapps\common\Codename Panzers Cold War\
  Home\Game\CPCW.exe", SteamStub v3.0).
- Ghidra 12.0.4 (scoop: C:\Users\swine\scoop\apps\ghidra\current). Java 21 scoop.
  Analyzed project: `N:\gamePAKdata\re\gp` name `u`, program `/CPCWu.exe`
  (24078 funcs, Analysis+Save succeeded).
- pyghidra 2.2.0 in scoop python (C:\Users\swine\scoop\apps\python\current\python.exe).
  Run scripts via that python with GHIDRA_INSTALL_DIR set. Use GhidraProject.openProject
  + openProgram("/","CPCWu.exe",False). GOTCHAS: analyzeHeadless.bat runs java DETACHED
  and returns fast (task "completes" early) — do NOT kill java matching *gamePAKdata*
  before analysis finishes; poll log for "Analysis succeeded". Batch has trailing
  pause -> run with `< /dev/null`. Clear stale `u.lock`/`u.lock~` before pyghidra open.
- Decompiled loader: `N:\gamePAKdata\re\srm_funcs.txt`. Finder script: `find4.py`
  (find tag immediates + DiffuseTexture refs, decompile). Full analyze+find: `find_all.py`.

## DELIVERED (round-trip session, commit 3911f33)
- **Byte-faithful writer** `cpcw_srm_writer.py` == `SRM_Blender/srm_writer.py`:
  `parse()->SrmFile->pack()` reproduces original bytes for **2087/2087** files
  (container+PMOD nodes+MESH/BONE/INDS/VERS+material trailers; untouched chunks
  kept raw). This is the user's #1 goal ("import then export shouldn't break").
- **Blender exporter** `SRM_Blender/export_srm.py` (File > Export > CPCW Model):
  re-writes from the pristine source stashed on import (`root["cpcw_srm_source"]`);
  no-edit export is byte-identical; root-level node transform edits are written
  back. Verified headless in Blender 5.0.1 (no-edit identical + moved body0 → +2.0
  written back, other nodes untouched).
- **Usage-based stream classification**: VERS 4th word = D3DDECLUSAGE is the real
  type (semantic can't tell normal/tangent/binormal apart). Fixed normals never
  resolving. Rigid bone idx = NORMAL(usage 3) byte3 (corpus-verified
  max<=nb-1); smooth-skin uses usage 2/1. Applied to srm_format.py + cpcw_srm.py.
- **Standalone** `cpcw_srm.py roundtrip <file|dir> [--batch]`.
- Docs updated (FORMAT_SRM VERS/usage table + round-trip note; README export).

## SKINNING: resolved as far as the static file allows
Concluded (data + Ghidra loader): the file carries **no per-bone inverse-bind**
(PBND is a bbox). A mesh mixes model-space body + origin-stored bone-local parts;
the exact rest pose is animation/bind-time driven and not statically recoverable.
The loader (FUN_004af8a0 -> node reader 004aed60 -> mesh reader 0051a550) only
deserializes; it does NOT bake transforms. So the Assemble toggle (Full/Parts/Off)
stays, documented. render_funcs.txt has the loader family; the D3D9 render/skin
transform was NOT pursued further (unsymbolized, low marginal value). If revisited:
trace the scene-graph world-matrix update + matrix-palette setup from the model
class draw method (callers of 004af8a0).

## NEXT STEPS (resume plan)
0. (optional) Blender-mesh -> VERS/INDS/BONE encoder for authoring NEW geometry
   (writer/format ready; only the Blender->stream packer is missing).
1. Ghidra: find the RENDER/skinning path. Anchors: who consumes mesh+0x24 (bone
   palette) at draw; node scene-graph WORLD-matrix update (parent_world@local);
   whether an inverse-bind is computed at bind time and where wheel vs body differ.
   D3D9 is via COM vtables (no named imports) — trace from model draw method.
   Goal: determine the exact per-vertex transform so import matches game with NO mode.
2. Build round-trip SRM EXPORTER (writer) preserving raw vtx/bone/node data ->
   byte-faithful round-trip for modding. Independent of #1.
3. Fold the discovered rule into import_srm.py (+ standalone) replacing the FULL/
   PARTS/NONE heuristic with the true rule; re-verify moskvitch+patton+bulldozer+
   barrack via `blender --background --python scratchpad/render_srm.py -- <srm> <out> <MODE>`.

## Verify commands
- Headless import test: scratchpad/test_srm_blender.py, batch_test.py.
- Render: scratchpad/render_srm.py -- <srm.path> <out.png> FULL|PARTS|NONE
  (uses addon_utils.enable after read_factory_settings; re-sync installed addon first).
- Parser parity (no Blender): import srm_format vs cpcw_srm on N:\gamePAKdata\CPCWPak.

## NEW: standalone D3D9 SRM viewer (viewer/) — ground-truth renderer
Built `viewer/` — a Win32 + Direct3D 9 fixed-function C++ viewer that renders a
`.srm` **the way the engine does**: native DirectX left-handed Y-up (NO LH->RH
reflection, NO winding reversal), rigid+smooth skinning by the PROVEN rule
`v_world = boneWorld[BONE_palette[boneIdx]] . v_stored` (no inverse-bind). It is
the ground-truth companion to the Blender importer (Blender converts to RH Z-up,
so it can only approximate the raster path). Modelled on swinedecomp's
swine_viewer (same engine family: SWINE -> Panzers -> CPCW), and a seed for a
future CPCW decompile.
- Self-contained: Windows SDK `d3d9` only (no legacy DirectX SDK). Ports of
  srm_format.py (parser) and dds.py (DXT1/3/5) to C++; own column-vector Mat4
  matching the importer's mathutils (T*R*S, R=Rx*Ry*Rz, world=parent*local).
- Files: viewer/src/{mathx.h, srm_model.{h,cpp}, dds.{h,cpp}, viewer.cpp},
  viewer/CMakeLists.txt, viewer/README.md.
- Build: `cmake -S viewer -B viewer/build -G "Visual Studio 18 2026" -A x64 &&
  cmake --build viewer/build --config Release` -> viewer/build/Release/cpcw_viewer.exe
  (builds clean on VS 18 2026 / MSVC 19.50, Windows 10 SDK 10.0.26100).
- Run: `cpcw_viewer <model.srm> [dataRoot] [--shot out.bmp] [--skin full|none]`.
  `--shot` renders one frame offscreen to a 24-bit BMP and exits (headless
  verification / gallery). Interactive: LMB orbit, RMB pan, wheel zoom, W wire,
  T tex, F skin FULL/NONE, C cull, R reset.
- VERIFIED headless (renders/viewer/*.png, gitignored): v200 train (lettering
  reads FORWARD -> handedness correct), M48 Patton FULL (hull/turret/gun/tracks
  assemble; road wheels+camo net float = documented animated-bone gap, matches
  Blender FULL) + NONE (raw bind pose), moskvitch (body+wheels, tiny front-wheel
  residual), ka_15 (canopy/star/rotor disc correct), british_barrack (unskinned
  by node matrix). All model classes render.
- Faithful limits (not bugs): animated bones float without animation eval
  (future phase); diffuse alpha is spec/team mask -> no alpha blend (rotor disc
  draws opaque); no variant filter yet.

### viewer follow-ups (drag-drop, variants, RDP robustness)
- **Drag-and-drop**: drop a `.srm` onto the window to load/reload (WM_DROPFILES +
  DragAcceptFiles); load path refactored into `loadModel()`. Can launch with no
  model (empty window) and drag one in.
- **Upgrade-variant filter** (matches import_srm.py): `V` cycles all/standard/
  upgraded; `--variant`. Drops faces whose bone-node name suffix (_std/_upg) is
  excluded. Verified headless via `--info`: Patton ALL 5215 tris -> STANDARD 4393
  (camo-net mesh dropped) / UPGRADED 4713; train identical across all (no _std/
  _upg bones -> no-op). Per-vertex tag from dominant bone; framing bounds now
  computed over referenced verts only.
- New keys: `V` variant, `P` screenshot (cpcw_shot_NNN.bmp). Title shows filename+variant.
- **`--info`**: headless CPU-only mode (parse + per-variant tri/vert counts, no
  D3D device) — works with NO display. Use to verify logic when rendering can't run.
- **RDP robustness**: prefer `Direct3DCreate9Ex` + match backbuffer to desktop
  format + REF/SW fallbacks. IMPORTANT ENV FACT: plain D3D9 (and Ex) return 0
  adapters / D3DERR_NOTAVAILABLE when the user's RDP session is DISCONNECTED
  (`query session` showed session 2 = swine = Disc). Live rendering needs the
  session CONNECTED; earlier in-session renders worked, then failed after the
  session disconnected. Not a bug — clear on-screen message added; `--info`
  is the display-free fallback.
