# CPCW Blender tools — work state (resume notes)

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

## NEXT STEPS (resume plan)
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
