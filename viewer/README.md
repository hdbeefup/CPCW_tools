# CPCW SRM Viewer

A standalone **Win32 + Direct3D 9** viewer for *Codename: Panzers Cold War*
(Gepard engine) `.srm` models. It renders a model **the way the game does** —
native DirectX **left-handed** Y-up space, fixed-function pipeline, rigid/smooth
skinning by the rule proven in the disassembly — so it's the ground-truth
companion to the Blender importer (which converts to Blender's right-handed
Z-up and so can only *approximate* the engine's raster path).

Inspired by `swinedecomp`'s `swine_viewer` (same author, same engine family:
S.W.I.N.E. → Panzers → CPCW). Long-term this is also a seed for a proper CPCW
decompilation: the loader/skin/render code here is written to match the engine's
own conventions, which is exactly what a `reccmp`-style effort matches against
`CPCWu.exe`.

## What it does

- Parses `.srm` directly (port of `blendertools/SRM_Blender/srm_format.py`):
  MAIN → PMOD node headers + MESH block; BONE palette, INDS indices, VERS
  streams keyed by **D3DDECLUSAGE**.
- **Skinning** — two cases, distinguished by how the vertices are stored:
  - *Rigid* (vehicles): no `BLENDINDICES` stream; each vertex's bone-palette
    index is **byte 3 of the NORMAL stream** and its position is in that bone's
    **local** space, so the proven rule `v_world = boneWorld[palette[idx]] · v`
    assembles it (verified in Ghidra: no inverse-bind).
  - *Smooth* (characters): has `BLENDINDICES` + `BLENDWEIGHT` streams and the
    vertices are in **model** space. The engine's per-bone skin matrix is
    `boneWorld · inverseBind`, which at the static rest pose is **identity** —
    so a static viewer must render the **bind pose** (applying `boneWorld`
    alone would double-transform and mangle the mesh). We therefore leave
    smooth meshes in bind pose; they'd only move once animation is evaluated.
  - *Unskinned* meshes (most buildings/props): placed by the owning node's
    world matrix (`T·R·S`, `R = Rx·Ry·Rz`, composed up the parent chain).
- Native **left-handed** rendering: no LH→RH reflection, no winding reversal —
  the mirror-sensitive test (train lettering "DEUTSCHE BUNDESBAHN" reads
  forward; the ka_15's one-sided canopy sits correctly) passes natively.
- DDS textures (DXT1/3/5 + uncompressed), decoded in-process (port of
  `dds.py`), resolved recursively from a data root (handles sibling-folder
  textures like the T-54's).
- Fixed-function lighting (camera headlight + ambient), orbit camera.

## Build

Needs MSVC + CMake. `d3d9.h`/`d3d9.lib` ship in the Windows SDK — **no legacy
DirectX SDK required**.

```sh
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
# -> build/Release/cpcw_viewer.exe
```

## Usage

```sh
cpcw_viewer [model.srm] [dataRoot] [--shot out.bmp] [--skin full|none]
            [--variant all|standard|upgraded] [--info]
```

- `model.srm` — optional; omit it to open with a file dialog (or an empty
  window you can **drag-and-drop** a `.srm` onto). Dropping a new file reloads.
- `dataRoot` — folder scanned recursively for `.dds` textures (default: the
  model's own directory). Point it at the extracted `CPCWPak` (or a subtree
  like `Vehicles`) for full texture coverage.
- `--shot out.bmp` — render one frame offscreen, write a 24-bit BMP, exit
  (headless; used for the render gallery / CI-style checks).
- `--skin full` (default) = the exact game rule; `--skin none` = raw bind pose.
- `--variant standard|upgraded` — show only that upgrade loadout (default
  `all` shows every variant merged; see below).
- `--info` — **no display needed**: parse the model and print node/mesh counts
  and per-variant triangle/vertex totals, then exit. Works even with no GPU /
  a disconnected Remote Desktop session.

Interactive: **LMB** orbit · **RMB** pan · **wheel** zoom · **drag-drop** a
`.srm` to load · **W** wireframe · **T** textures · **F** skin FULL/NONE ·
**V** variant cycle · **C** cull cycle · **P** screenshot · **R** reset · **Esc**.

## Upgrade variants

Vehicles pack every loadout into one `.srm`; a part's variant is read from the
**suffix of the bone it's skinned to** (`_std` / `_upg`; camo-net parts always
carry `_upg`) — matching `import_srm.py`. `V` cycles **all → standard →
upgraded**, filtering out the other loadout's geometry (e.g. a Patton's camo
net and upgraded gun only appear under *upgraded*). Non-variant models
(the train, buildings) are unaffected.

## Remote Desktop note

Direct3D 9 needs a live display. Inside an RDP session the session must be
**connected** (not disconnected) or `Direct3DCreate9`/`Ex` reports zero adapters
and device creation fails with a clear message. The viewer already prefers
**Direct3D9Ex** and matches the backbuffer to the desktop format for the
connected-RDP case. For inspection without any display, use `--info`.

## Known limits (faithful, not bugs)

- **Animated bones float.** A static file has no settled pose for bones the
  engine animates per frame (tank road wheels, suspension, rotor). Under the
  exact rule (`--skin full`) these parts render at their *un-animated* bone
  transform — the road wheels/camo net hover above a Patton, the moskvitch's
  front wheels sit slightly forward. This is inherent to a static viewer;
  settling them needs the animation data evaluated (a future phase). Blender's
  `AUTO` heuristic hides it by leaving such parts in model space, but that's a
  guess, not the engine rule — this viewer stays honest and shows the rule.
- **Diffuse alpha is a specular/team mask, not opacity** (per the format notes),
  so it's intentionally left unwired — no alpha blending. The ka_15 rotor-blur
  disc therefore draws opaque/dark rather than as a translucent disc.
- Under `--variant all` an upgraded tank shows base + upgrade parts merged
  (overlapping at attach points); cycle to `standard`/`upgraded` to separate.

## Files

- `src/mathx.h` — column-vector `Vec3`/`Mat4` (matches the importer's mathutils).
- `src/srm_model.{h,cpp}` — `.srm` parser + world-matrix build + skinning.
- `src/dds.{h,cpp}` — DDS decoder.
- `src/viewer.cpp` — Win32 window, D3D9 device, camera, render loop, BMP capture.
