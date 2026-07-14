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
# open a pre-exported scene:
cpcw_mapeditor --load scene.json

# open a .map directly (shells out to the Python core for the scene bridge):
export CPCW_MAP_PY=/path/to/cpcw_map.py      # Windows: set CPCW_MAP_PY=...\cpcw_map.py
cpcw_mapeditor --load "Some Map.map"

# headless check (no window): prints a summary and exits
cpcw_mapeditor --load scene.json --selftest
```
Produce a scene JSON with the Python core:
```sh
python ../cpcw_map.py scene "Some Map.map" scene.json
```

## Data flow (interim)
The map is parsed/written by the verified Python core (`../cpcw_map.py`), which
exports an editor scene as JSON. The editor loads that JSON. This bridge lets the
UI come up against real data now; **M5** ports the parser to C++ to drop the
Python dependency and make the editor fully self-contained.
