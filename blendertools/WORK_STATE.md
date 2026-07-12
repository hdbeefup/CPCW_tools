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

### STILL TODO (user: "still work to be had, will continue next day")
- **moskvitch body ~0.1-0.2 too low** (wheels clip the body/arches). Its body
  binds to `suspension0l` (base moskvitch401 binds body->`body0`); AUTO leaves it
  model-space (correct) but the exact ground-clearance offset isn't in the file;
  skinning by body0 DISTORTS it (180deg yaw). Needs the real inverse-bind rule.
- **AUTO skinning is a heuristic, not the game's true rule.** The Ghidra
  render/skinning path was NOT recovered (the workflow's ghidra agent failed on a
  structured-output cap; my find_attach.py showed the only name-based node assoc
  is GAMEPLAY mounts, not mesh render-attach). To make skinning exact, still need
  to reverse the D3D draw path / the load-time inverse-bind computation. Ghidra
  project ready at N:\gamePAKdata\re\gp; dumps: srm_funcs.txt, render_funcs.txt,
  map_funcs.txt, attach_funcs.txt.
- **Patton etc. show all upgrade variants** (camo-net/gun std+upgraded parts all
  present, some "floating" at attach points). Expected (all variants in one file);
  a variant filter could be added later.
- **Standalone cpcw_srm.py glTF export still has the latent mirror** (writes LH
  coords into RH glTF). Deferred; documented in [[srm-handedness]].
- **Map terrain has no splat textures** (grey). Terrain is geometrically correct.

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
