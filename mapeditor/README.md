# CPCW Map Editor (`mapeditor/`)

A portable, standalone map editor for **Codename: Panzers Cold War** — C++17 +
GLFW + OpenGL 3.3 + Dear ImGui, all pulled by CMake `FetchContent` (no system
deps). Cross-platform (Win/Linux/macOS), 64-bit, no DX9. UX mirrors the S.W.I.N.E.
editor (see `../docs/MAP_EDITOR.md`).

Status: **scaffold + data bridge** (milestones M4/M4b). It stands up the full
interface skeleton — File/Edit/View/Mode menus, a mode switcher (Vertex/Terrain,
Spline, Object, Unit, Ambient, Shader, Lake, Light, Trigger), a swappable per-mode
tool/param panel, an entity list, a properties panel, and a dockable viewport that
draws a **top-down preview** of the loaded map (terrain grid + player-coloured
entity markers, click to select). Real 3D rendering and GUI-driven editing are the
next milestones.

## Build
```sh
cmake -S . -B build
cmake --build build --config Release
```
First configure fetches GLFW, Dear ImGui (docking), and nlohmann/json (needs
network once).

## Run
```sh
# open a .map directly — parsed natively in C++, NO Python needed:
cpcw_mapeditor --load "Some Map.map"

# or a pre-exported scene JSON (from cpcw_map.py scene):
cpcw_mapeditor --load scene.json

# headless check (no window): prints a summary and exits
cpcw_mapeditor --load "Some Map.map" --selftest
```
Or just launch it and use **File > Open** (or drag none — type a path in the popup).

## Data flow
`.map` files are parsed **natively** by `mapfile.cpp` (a faithful C++ port of
`../cpcw_map.py`: chunk tree, SCHD schemas, OBJT/VOBJ entities, heightmap locator,
splatmap colormap) straight into the in-memory scene — the editor is
self-contained, no Python at runtime. `cpcw_map.py` remains the *oracle* the port
is verified against (native entity counts / grid dims / render match it exactly),
and it still handles the byte-faithful **save** (`apply`) path used by File > Save.
