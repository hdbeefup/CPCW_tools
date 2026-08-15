# CPCW Map Editor (`mapeditor/`)

A standalone map editor for **Codename: Panzers Cold War**, which shipped without
one. C++17 + GLFW + OpenGL 3.3 + Dear ImGui + ImGuizmo, all pulled by CMake
`FetchContent` — no system dependencies, 64-bit, no DX9. Drop the single exe into
the game folder and it reads maps, models, textures and ProtoDB straight out of
the `.pak` archives. **No Python at runtime.**

UX follows the S.W.I.N.E. editor (see `../docs/MAP_EDITOR.md`); the file format
work is documented in `../docs/MAP_FORMAT.md`.

## Build

```sh
cmake -S . -B build
cmake --build build --config Release      # MSVC; the exe locks itself while running
```
The first configure fetches GLFW, Dear ImGui (docking), ImGuizmo, nlohmann/json
and zlib (needs network once).

## Run

```sh
build/Release/cpcw_mapeditor.exe                 # auto-mounts every *.pak beside it
build/Release/cpcw_mapeditor.exe --load Some.map # or drag a .map onto the window
```
`File > Open` and `File > Open from .pak` both work; so does drag-and-drop.

## What it does

**View** — splat-textured terrain composited from the real ground-layer `.dds`,
`.srm` models with DDS textures and alpha-cut foliage, GROA road ribbons and GDEC
decals projected onto the ground, orbit/pan/zoom-to-cursor plus WASD.

**Select** — click, Ctrl-click to toggle, Shift-click to add, drag on empty ground
to rubber-band. Selection is resolved against a GPU colour-code buffer, so
overlapping models and alpha-cut leaves pick exactly. `Ctrl+A` / `Ctrl+Shift+A`
select all visible / none.

**Transform** — an ImGuizmo translate/rotate gizmo over the whole selection about
its centroid, world or local axes, grid and angle snapping (hold Shift or toggle
it), `G` drops the selection onto the terrain. Drag an entity to move it,
`[` / `]` to nudge its yaw.

**Edit entities** — place any of the ~2000 ProtoDB prototypes (searchable browser
with thumbnails, categories and favourites), cut/copy/paste including paste-in-
place, duplicate, delete. A schema-driven Properties panel exposes every
fixed-width field the record declares — HP, Level, XP, Ammo, armour, Scale,
Elevation, garrison, and so on.

**Edit terrain** — Raise / Lower / Smooth / SetPlane / Raise-to-plane /
Lower-to-plane / Grab brushes with a terrain-conforming cursor ring, and
Blend / TileFill painting of the splat layers.

**Save** — byte-faithful. Only the bytes you actually changed differ; everything
else in the file is preserved verbatim. `Ctrl+S` writes `<map>_edited.map`,
`Ctrl+Shift+S` is Save As, and `File > Overwrite original` replaces the source
after moving the previous bytes to `<map>.bak`. Writes are two-phase (`.tmp` then
rename), so a failure never leaves a truncated map. The **Changes** panel lists
everything that differs from the file on disk before you commit to it.

Undo/redo (`Ctrl+Z` / `Ctrl+Y`) covers field edits, gizmo and group moves, terrain
strokes, and structural add/delete — the structural commands carry the exact OBJT
bytes, so undo and redo are byte-identical.

**Edit lighting** — the `WTHR` pool is decoded, so **Light** mode lists a map's
named weather presets (`Default`, `Rain`, `Sundown`, `Desert_night`, …) in their
authored order and edits every field: sun direction as an azimuth/elevation
compass, HDR sun/ambient/shadow/specular colours, fog range and colour, clouds,
time of day. Edits are size-preserving in-place writes, so the save stays
byte-faithful, and each drag is one undo step.

**Rivers** — `GRVL`/`GRVR` river splines are decoded and drawn at their own water
level with the per-node width the record carries. River mode lists them with a
node table; `View > Rivers` toggles them.

## Not implemented

Roads, decals and rivers are decoded and rendered but read-only. The trigger
system is not decoded — it sits behind an undecoded `HEAP` container — and that
mode opens a raw chunk inspector (`View > Map chunks`) rather than offering
invented fields. Lake/Water is **retired**, not pending: no lake or water data
exists in any of the 45 shipped maps.

The lighting presets are not yet applied to the viewport: the vertical axis of
`SunDirection` is established but its horizontal swizzle is not, and guessing it
would light every map wrong in a way that looks plausible.

## Headless harnesses

Every write path has a no-window check that runs from the command line:

| Flag | Checks |
|---|---|
| `--selftest --load <map>` | parse summary (entities, grid, Scale field, splat offset) |
| `--shot <out.bmp>` | render one scene frame to a BMP — **no ImGui**, so it cannot show the selection overlay or any UI |
| `--uishot <out.bmp> [--uishot-frames N] [--uishot-select IDX]` | run the real frame loop and capture the whole window, UI overlays included |
| `--picktest [out.bmp]` | colour-code pick buffer resolves entities; optional dump |
| `--structtest <map> [out]` | a field edit survives a structural insert; undo/redo byte-exact |
| `--fieldtest <map> <out>` | one schema field written, nothing outside it touched |
| `--splattest <map> <out>` | one painted splat cell written, nothing else touched |
| `--protoplacetest <map> <ProtoDB.bin> <out>` | place a prototype absent from the map |
| `--addtest` `--deltest` `--heighttest` `--applytest` | structural insert/delete, height, field save |
| `--overlaytest <map>` | GROA/GDEC decode counts and material resolution |
| `--wthrtest <map\|dir> [out.map]` | WTHR pool walks exactly + the semantic assertions that pin the field order; with `out.map`, a write leg |
| `--chunktile <map\|dir>` | container sizes tile exactly + every OBJS `schema_offset` points at its own SCHD |
| `--overlayscan <map\|dir>` | GROL/GDCL/GRVL slot pools walk exactly, counts match, used-list invariants hold; river summary |
| `--no-roads` `--no-decals` `--no-rivers` | drop one overlay layer, so a `--shot` pair isolates its pixels |
| `--uishot-mode <n>` | which mode's panel a `--uishot` captures |
| `--paktest` `--protodbtest` `--thumbtest` `--srmcheck` | archive, prototype DB, thumbnails, model |

`cpcw_map.py` is no longer needed at runtime but remains an **oracle**: its
`roundtrip` and `entities` commands are part of what the native writer is
validated against after every change — but **`roundtrip` does not check container
sizes** (it re-emits each chunk's original span rather than recomputing one), so
`--chunktile` is the check that actually catches a missed ancestor bump.
