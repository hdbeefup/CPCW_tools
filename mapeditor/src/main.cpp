// CPCW Map Editor — portable editor (GLFW + OpenGL 3.3 + Dear ImGui).
//
// A modern, cross-platform re-imagining of the S.W.I.N.E. editor (docs/
// MAP_EDITOR.md): a mode switcher driving a swappable tool/param panel over a
// central 3D viewport, with File/Edit/View/Mode menus. Map data comes through
// cpcw_map.py's verified core via a JSON scene bridge (+ a raw f32 heightmap
// sidecar). No game/DX9 dependency.
//
// Usage: cpcw_mapeditor [--load <map.map|scene.json>] [--selftest]
//   --load     open a scene (.json direct, or .map via python + CPCW_MAP_PY).
//   --selftest load, print a summary, exit (headless; no window).

#include "imgui.h"
#include "imgui_internal.h"        // DockBuilderGetCentralNode
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "glcore.h"
#include "scene.h"
#include "viewport3d.h"
#include "mapfile.h"
#include "protodb.h"
#include "pak.h"
#include "vfs.h"
#include "thumb.h"
#include <map>
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F   // GL 1.2; absent from the ancient Windows gl.h
#endif
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>
#include <iterator>
#include <algorithm>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

// --- editor modes (mirrored from the S.W.I.N.E. editor) ----------------------
struct Mode { const char* name; const char* focus; const char* tools; };
static const Mode kModes[] = {
    {"Vertex / Terrain", "terrain", "Grab Raise Lower SetPlane Raise>Plane Lower>Plane Smooth Blend TileFill Area"},
    {"Spline",           "terrain", "New-river Node-move Close-loop Altitude Width Texture"},
    {"Object / Doodad",  "object",  "Place Move Lift Rotate Tilt Align"},
    {"Unit",             "object",  "Place Move Rotate"},
    {"Ambient",          "object",  "Place Move Lift Distance"},
    {"Shader / Decals",  "terrain", "Place Move Rotate Z-order"},
    {"Lake / Water",     "terrain", "Place Move Lift"},
    {"Light",            "global",  "(settings)"},
    {"Trigger",          "logic",   "Locations Triggers Conditions Actions"},
};
static const int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));

static Scene      g_scene;
static Camera     g_cam;
static Viewport3D g_vp;
static bool  g_glReady = false, g_sceneDirty = false;
static int   g_mode = 0, g_activeTool = 0, g_selected = -1, g_hovered = -1;
static float g_brushSize = 2.0f, g_brushHeight = 0.0f, g_brushPress = 0.5f;
static bool  g_wireframe = false;
static bool  g_showModes = true, g_showPanel = true, g_showProps = true,
             g_showEntities = true;
static char  g_mapPath[512] = "(no map loaded)";
static char  g_openPath[512] = "";
static bool  g_openPopup = false;
static bool  g_pakBrowser = false;
static std::vector<std::string> g_pakMaps;
static char  g_pakFilter[128] = "";
static long  g_placeSrcId = -1;   // prototype-browser: entity to duplicate on place
static bool  g_orbiting = false, g_panning = false;
static GLFWwindow* g_win = nullptr;
static bool  g_showKind[3] = {true, true, true};   // doodad / building / effect
static bool  g_entDirty = false;
static bool  g_showModels = true, g_showDots = true;
static bool  g_draggingEnt = false, g_modelsDirty = false, g_terrainDirty = false;

// Before-and-after state of one entity, for batched undo (gizmo, group ops).
struct EntSnap { long id = 0; float pos0[3]{}, pos1[3]{}; float dir0 = 0, dir1 = 0; };

// --- selection ---------------------------------------------------------------
// g_selected is the PRIMARY selection (what Properties edits and what the gizmo
// pivots on when it is the only one); g_selection is the full set.
static std::set<int> g_selection;
// --- gizmo / snapping --------------------------------------------------------
static bool  g_gizmoOn = true;
static int   g_gizmoOp = 0;               // 0 translate, 1 rotate (yaw only)
static bool  g_gizmoLocal = false;        // world vs local axes
static bool  g_snapOn = false;            // held Shift also forces snapping on
static float g_snapGrid = 1.0f, g_snapAngle = 15.0f;
static bool  g_gizmoDragging = false;
// Set once per frame by drawGizmo. Don't call ImGuizmo::IsOver() directly from the
// input code: when no gizmo was drawn this frame it answers from stale state.
static bool  g_gizmoHot = false;
static float g_gizmoMat[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static float g_gizmoPivot0[3] = {0,0,0};  // pivot when the drag began (GL world)
static std::vector<EntSnap> g_gizmoStart; // per-entity state when the drag began
// --- rubber-band select ------------------------------------------------------
static bool   g_marquee = false;
static ImVec2 g_marqueeA{0,0};

// --- undo/redo -------------------------------------------------------------
// Commands are keyed by entity ID, never by index: a structural insert/delete
// renumbers every later index, so an index-keyed stack would undo onto the wrong
// entity. Structural commands carry the exact OBJT bytes, so both directions are
// byte-exact.
enum { CMD_ENTITY, CMD_TERRAIN, CMD_ADD, CMD_DELETE, CMD_BATCH };
struct EditCmd {
    int  kind = CMD_ENTITY;
    long entId = 0;
    float pos0[3]{}, pos1[3]{}; float dir0 = 0, dir1 = 0; int pl0 = 0, pl1 = 0;
    std::vector<int> cells; std::vector<float> h0, h1;   // CMD_TERRAIN
    std::vector<unsigned char> objt; int entIndex = -1;  // CMD_ADD / CMD_DELETE
    std::vector<EntSnap> ents;                           // CMD_BATCH (gizmo, group ops)
};
static std::vector<EditCmd> g_undo, g_redo;
static bool g_snapActive = false; static EditCmd g_snap;   // pending entity snapshot
static bool g_strokeActive = false; static std::vector<float> g_strokeH0;
static std::string g_dataRoot;                     // folder holding ProtoDB.bin + models
static std::string g_srcMap;                       // original .map (empty if .json)
static std::set<long> g_edited;                    // ids with pending field edits
static char  g_saveStatus[256] = "";

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static int entityIndexById(long id) {
    for (int i = 0; i < (int)g_scene.entities.size(); i++)
        if (g_scene.entities[i].id == id) return i;
    return -1;
}

// --- selection helpers -------------------------------------------------------
static void syncSelection() { g_vp.selectionSet = g_selection; }
static void selectNone() { g_selection.clear(); g_selected = -1; syncSelection(); }
static void selectOnly(int idx) {
    g_selection.clear();
    if (idx >= 0) g_selection.insert(idx);
    g_selected = idx; syncSelection();
}
static void selectToggle(int idx) {
    if (idx < 0) return;
    if (g_selection.count(idx)) {
        g_selection.erase(idx);
        if (g_selected == idx) g_selected = g_selection.empty() ? -1 : *g_selection.begin();
    } else {
        g_selection.insert(idx); g_selected = idx;
    }
    syncSelection();
}
static void selectAdd(const std::vector<int>& idxs, bool replace) {
    if (replace) g_selection.clear();
    for (int i : idxs)
        if (i >= 0 && i < (int)g_scene.entities.size()) g_selection.insert(i);
    if (g_selected < 0 || !g_selection.count(g_selected))
        g_selected = g_selection.empty() ? -1 : *g_selection.begin();
    syncSelection();
}
static void selectAllVisible() {
    g_selection.clear();
    for (int i = 0; i < (int)g_scene.entities.size(); i++) {
        const Entity& e = g_scene.entities[i];
        if (g_showKind[(e.kind >= 0 && e.kind < 3) ? e.kind : 2]) g_selection.insert(i);
    }
    g_selected = g_selection.empty() ? -1 : *g_selection.begin();
    syncSelection();
}
// Flush every pending in-memory field edit into Scene::raw. MUST run before any
// structural edit — those rewrite the byte buffer, and anything not yet in it is
// lost (this was a silent data-loss bug: place one object, lose ten moves).
static void flushEditsToRaw() {
    if (g_scene.raw.empty()) return;
    std::vector<long> ids(g_edited.begin(), g_edited.end());
    if (ids.empty() && !g_scene.terrainEdited) return;
    apply_edits_inplace(g_scene, ids, g_scene.raw);
}
// After a structural edit the entity list is rebuilt: re-anchor the selection by
// ID and mark only the entity/model buffers dirty (terrain and overlays are
// untouched by an entity insert/erase).
static void afterStructural(long selectId) {
    selectOnly(selectId >= 0 ? entityIndexById(selectId) : -1);
    g_hovered = -1;
    g_entDirty = true; g_modelsDirty = true;
}

static void applyCmd(const EditCmd& c, bool useNew) {
    if (c.kind == CMD_TERRAIN) {
        for (size_t i = 0; i < c.cells.size(); i++)
            if (c.cells[i] >= 0 && c.cells[i] < (int)g_scene.heights.size())
                g_scene.heights[c.cells[i]] = useNew ? c.h1[i] : c.h0[i];
        g_scene.terrainEdited = true; g_terrainDirty = true;
        if (g_scene.heightDirty.size() == g_scene.heights.size())
            for (int ci : c.cells) g_scene.heightDirty[ci] = 1;
        return;
    }
    if (c.kind == CMD_BATCH) {
        for (const EntSnap& es : c.ents) {
            int i = entityIndexById(es.id);
            if (i < 0) continue;
            Entity& e = g_scene.entities[i];
            for (int k = 0; k < 3; k++) e.pos[k] = useNew ? es.pos1[k] : es.pos0[k];
            e.dir = useNew ? es.dir1 : es.dir0;
            g_edited.insert(e.id);
        }
        g_entDirty = true; g_modelsDirty = true;
        return;
    }
    if (c.kind == CMD_ADD || c.kind == CMD_DELETE) {
        // CMD_ADD's "new" state is the entity present; CMD_DELETE's is absent.
        bool wantPresent = (c.kind == CMD_ADD) == useNew;
        flushEditsToRaw();
        if (wantPresent) {
            if (insert_objt_at_index(g_scene, c.entIndex, c.objt)) afterStructural(c.entId);
        } else {
            if (delete_entity_bytes(g_scene, c.entId, nullptr, nullptr)) afterStructural(-1);
        }
        return;
    }
    int idx = entityIndexById(c.entId);
    if (idx < 0) return;                       // entity no longer exists — skip
    Entity& e = g_scene.entities[idx];
    for (int k = 0; k < 3; k++) e.pos[k] = useNew ? c.pos1[k] : c.pos0[k];
    e.dir = useNew ? c.dir1 : c.dir0; e.player = useNew ? c.pl1 : c.pl0;
    g_edited.insert(e.id); g_entDirty = true; g_modelsDirty = true;
}
static void undoEdit() {
    if (g_undo.empty()) return;
    EditCmd c = g_undo.back(); g_undo.pop_back(); applyCmd(c, false); g_redo.push_back(std::move(c));
}
static void redoEdit() {
    if (g_redo.empty()) return;
    EditCmd c = g_redo.back(); g_redo.pop_back(); applyCmd(c, true); g_undo.push_back(std::move(c));
}
static void pushCmd(EditCmd c) { g_undo.push_back(std::move(c)); g_redo.clear(); }

// snapshot the selected entity's state before an edit begins
static void snapEntity(int idx) {
    if (idx < 0 || idx >= (int)g_scene.entities.size()) { g_snapActive = false; return; }
    const Entity& e = g_scene.entities[idx];
    g_snap = EditCmd{}; g_snap.kind = CMD_ENTITY; g_snap.entId = e.id;
    for (int k = 0; k < 3; k++) g_snap.pos0[k] = e.pos[k];
    g_snap.dir0 = e.dir; g_snap.pl0 = e.player; g_snapActive = true;
}
// commit the snapshot as an undo command if the entity actually changed
static void commitEntity() {
    if (!g_snapActive) { g_snapActive = false; return; }
    int idx = entityIndexById(g_snap.entId);
    if (idx < 0) { g_snapActive = false; return; }
    const Entity& e = g_scene.entities[idx];
    bool changed = e.dir != g_snap.dir0 || e.player != g_snap.pl0;
    for (int k = 0; k < 3; k++) if (e.pos[k] != g_snap.pos0[k]) changed = true;
    if (changed) {
        for (int k = 0; k < 3; k++) g_snap.pos1[k] = e.pos[k];
        g_snap.dir1 = e.dir; g_snap.pl1 = e.player;
        pushCmd(g_snap);
    }
    g_snapActive = false;
}

// --- structural edits (in memory; no temp file, no full reload) --------------
// Returns the new entity's ID, or -1.
static long placeEntityClone(long srcId, const float pos[3]) {
    if (g_scene.raw.empty() || g_srcMap.empty()) return -1;
    long newId = 1;
    for (const auto& en : g_scene.entities) if (en.id >= newId) newId = en.id + 1;
    flushEditsToRaw();
    std::vector<unsigned char> blob;
    if (!add_entity_bytes(g_scene, srcId, pos, newId, &blob)) return -1;
    afterStructural(newId);
    EditCmd c; c.kind = CMD_ADD; c.entId = newId; c.objt = std::move(blob);
    c.entIndex = entityIndexById(newId);
    pushCmd(std::move(c));
    return newId;
}
static bool deleteEntityById(long id) {
    if (g_scene.raw.empty() || g_srcMap.empty()) return false;
    flushEditsToRaw();
    std::vector<unsigned char> blob; int idx = -1;
    if (!delete_entity_bytes(g_scene, id, &blob, &idx)) return false;
    afterStructural(-1);
    g_edited.erase(id);
    EditCmd c; c.kind = CMD_DELETE; c.entId = id; c.objt = std::move(blob); c.entIndex = idx;
    pushCmd(std::move(c));
    return true;
}

// write a 24-bit BMP from an RGB framebuffer read (rows bottom-up, as GL gives)
static void writeBMP(const char* path, int w, int h, const unsigned char* rgb) {
    int rowsz = (w * 3 + 3) & ~3, imgsz = rowsz * h;
    unsigned char hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    int fsz = 54 + imgsz;
    hdr[2]=fsz; hdr[3]=fsz>>8; hdr[4]=fsz>>16; hdr[5]=fsz>>24;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=w; hdr[19]=w>>8; hdr[20]=w>>16; hdr[21]=w>>24;
    hdr[22]=h; hdr[23]=h>>8; hdr[24]=h>>16; hdr[25]=h>>24;
    hdr[26]=1; hdr[28]=24;
    FILE* f = fopen(path, "wb"); if (!f) return;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> row(rowsz, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const unsigned char* p = rgb + (y * w + x) * 3;
            row[x*3+0] = p[2]; row[x*3+1] = p[1]; row[x*3+2] = p[0]; // RGB->BGR
        }
        fwrite(row.data(), 1, rowsz, f);
    }
    fclose(f);
}

static bool endsWithI(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[s.size()-n+i]) != tolower((unsigned char)suf[i])) return false;
    return true;
}
static std::string dirOf(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}
// walk up from the map's folder to find the data root (contains ProtoDB.bin)
static std::string findDataRoot(const std::string& mapPath) {
    std::string d = dirOf(mapPath);
    for (int i = 0; i < 6; i++) {
        std::ifstream f(d + "/ProtoDB.bin", std::ios::binary);
        if (f.good()) return d;
        std::string up = dirOf(d);
        if (up == d) break;
        d = up;
    }
    return "";
}
static std::string runCapture(const std::string& cmd) {
    std::string out; char buf[4096]; size_t n;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return out;
    while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

static bool parseScene(const std::string& txt, const std::string& baseDir, Scene& s) {
    try {
        auto j = nlohmann::json::parse(txt);
        s = Scene{};
        s.name = j.value("name", std::string("(map)"));
        auto t = j.value("terrain", nlohmann::json::object());
        s.world_w = t.value("world_w", 0); s.world_h = t.value("world_h", 0);
        s.grid_w = t.value("grid_w", 0);   s.grid_h = t.value("grid_h", 0);
        std::string hm = t.value("heightmap", std::string());
        if (!hm.empty()) {
            std::ifstream hf(baseDir + "/" + hm, std::ios::binary);
            if (hf) {
                hf.seekg(0, std::ios::end); std::streamoff n = hf.tellg(); hf.seekg(0);
                s.heights.resize((size_t)n / sizeof(float));
                hf.read((char*)s.heights.data(), n);
                // some maps carry NaN/garbage sentinel cells (e.g. an edge row) —
                // replace non-finite so they don't poison the mesh / camera.
                for (float& v : s.heights)
                    if (!(v == v) || v > 1e30f || v < -1e30f) v = 0.0f;
            }
        }
        std::string cmName = t.value("colormap", std::string());
        if (!cmName.empty()) {
            std::ifstream cf(baseDir + "/" + cmName, std::ios::binary);
            if (cf) {
                cf.seekg(0, std::ios::end); std::streamoff n = cf.tellg(); cf.seekg(0);
                s.colors.resize((size_t)n);
                cf.read((char*)s.colors.data(), n);
            }
        }
        for (auto& e : j.value("entities", nlohmann::json::array())) {
            Entity en;
            en.type = e.value("type", std::string("?"));
            auto pr = e.value("proto", nlohmann::json());
            if (pr.is_string()) en.proto = pr.get<std::string>();
            auto p = e.value("pos", nlohmann::json::array());
            if (p.is_array() && p.size() >= 3)
                for (int k = 0; k < 3; k++) en.pos[k] = p[k].get<float>();
            auto d = e.value("dir", nlohmann::json(0.0));
            if (d.is_number()) en.dir = d.get<float>();
            else if (d.is_array() && !d.empty() && d[0].is_number()) en.dir = d[0].get<float>();
            en.player = e.value("player", 0);
            en.kind = e.value("kind", 0);
            auto idv = e.value("id", nlohmann::json(0));
            if (idv.is_number_integer()) en.id = idv.get<long>();
            s.entities.push_back(std::move(en));
        }
        s.loaded = true;
        return true;
    } catch (const std::exception& ex) {
        fprintf(stderr, "scene parse error: %s\n", ex.what());
        return false;
    }
}

static void resetThumbCache();   // defined below; clears the prototype-thumb GL cache
static void groundSelection();   // defined below; drops the selection onto the heightmap

// Load a scene. `preserveView` keeps the current camera + data root (used when
// reloading the temp work-map after a structural edit, so the view doesn't jump
// and model resolution stays anchored to the original data root). `selectId`, if
// >= 0, selects the entity with that ID after loading (feedback for place/dup).
static bool loadScene(const std::string& path, bool preserveView = false, long selectId = -1) {
    Scene s;
    if (endsWithI(path, ".json")) {
        // a pre-exported scene (cpcw_map.py scene) + its sidecars
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
        std::stringstream ss; ss << f.rdbuf();
        if (!parseScene(ss.str(), dirOf(path), s)) return false;
    } else {
        // a .map: parse natively (no Python) straight into a Scene
        if (!load_map_native(path, s)) {
            fprintf(stderr, "native .map parse failed for %s\n", path.c_str());
            return false;
        }
    }
    g_scene = std::move(s); g_hovered = -1; g_sceneDirty = true;
    g_selection.clear(); g_selected = -1; syncSelection();
    g_gizmoDragging = false; g_gizmoStart.clear(); g_marquee = false;
    g_srcMap = endsWithI(path, ".json") ? std::string() : path;  // Save needs the .map
    if (!preserveView)
        g_dataRoot = g_srcMap.empty() ? std::string() : findDataRoot(g_srcMap);
    g_edited.clear(); g_saveStatus[0] = '\0';
    // a different map means different entities and different assets: an undo
    // command from the old scene must never be replayed onto the new one, and the
    // model/texture GL caches would otherwise grow with every load.
    g_undo.clear(); g_redo.clear(); g_snapActive = false; g_strokeActive = false;
    if (g_glReady) { g_vp.clearModels(); g_vp.clearOverlays(); }
    resetThumbCache();
    if (!preserveView) snprintf(g_mapPath, sizeof(g_mapPath), "%s", path.c_str());
    if (selectId >= 0)
        for (int i = 0; i < (int)g_scene.entities.size(); i++)
            if (g_scene.entities[i].id == selectId) { selectOnly(i); break; }
    if (preserveView) return true;   // keep camera; skip the reframe below
    // frame the camera on the loaded terrain
    float W = g_scene.world_w > 0 ? (float)g_scene.world_w : 512.0f;
    float H = g_scene.world_h > 0 ? (float)g_scene.world_h : 512.0f;
    float mid = 0.0f;
    if (!g_scene.heights.empty()) {
        double sum = 0; for (float v : g_scene.heights) sum += v;
        mid = (float)(sum / g_scene.heights.size());
    }
    g_cam.target = {W * 0.5f, mid, H * 0.5f};
    g_cam.dist = (W > H ? W : H) * 1.1f;
    g_cam.yaw = 0.8f; g_cam.pitch = 0.85f;
    return true;
}

// Write `bytes` to `path` without ever leaving a truncated map behind: write a
// sibling .tmp first, and only once that has fully landed move any existing file
// aside to .bak and rename the temp into place. A failure at any point leaves the
// original untouched.
static bool writeMapAtomic(const std::string& path, const std::vector<unsigned char>& bytes,
                           bool keepBackup, std::string& err) {
    std::error_code ec;
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot create " + tmp; return false; }
        f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f) { f.close(); std::filesystem::remove(tmp, ec); err = "write failed (disk full?)"; return false; }
    }
    bool hadOriginal = std::filesystem::exists(path, ec);
    std::string bak = path + ".bak";
    if (hadOriginal) {
        if (keepBackup) {
            std::filesystem::remove(bak, ec);
            std::filesystem::rename(path, bak, ec);
            if (ec) { std::filesystem::remove(tmp, ec); err = "cannot back up the original"; return false; }
        } else {
            std::filesystem::remove(path, ec);
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (hadOriginal && keepBackup) std::filesystem::rename(bak, path, ec);   // roll back
        err = "cannot replace the target file";
        return false;
    }
    return true;
}

// Save every pending edit into a copy of the loaded bytes (byte-faithful: only the
// edited fields' bytes change) and write it out.
static void doSaveTo(const std::string& out, bool keepBackup) {
    if (g_scene.raw.empty() || g_srcMap.empty()) {
        snprintf(g_saveStatus, sizeof(g_saveStatus),
                 "Save needs a .map source (open a .map, not a .json)."); return;
    }
    std::vector<long> ids(g_edited.begin(), g_edited.end());
    std::vector<unsigned char> bytes = g_scene.raw;
    apply_edits_inplace(g_scene, ids, bytes);
    std::string err;
    if (writeMapAtomic(out, bytes, keepBackup, err)) {
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Saved -> %s%s", out.c_str(),
                 keepBackup ? "  (previous kept as .bak)" : "");
        g_edited.clear(); g_scene.terrainEdited = false;
        if (!g_scene.heightDirty.empty())
            std::fill(g_scene.heightDirty.begin(), g_scene.heightDirty.end(), (unsigned char)0);
    } else {
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Save failed: %s", err.c_str());
    }
}

// Default save: alongside the source as <map>_edited.map, never over the original.
static void doSave() {
    if (g_srcMap.empty()) {
        snprintf(g_saveStatus, sizeof(g_saveStatus),
                 "Save needs a .map source (open a .map, not a .json)."); return;
    }
    doSaveTo(g_srcMap.substr(0, g_srcMap.size() - 4) + "_edited.map", true);
}

// Save As: OS picker on Windows, <map>_edited.map elsewhere.
static void doSaveAs() {
    if (g_srcMap.empty()) {
        snprintf(g_saveStatus, sizeof(g_saveStatus),
                 "Save needs a .map source (open a .map, not a .json)."); return;
    }
#ifdef _WIN32
    char file[1024] = {0};
    snprintf(file, sizeof(file), "%s", (g_srcMap.substr(0, g_srcMap.size()-4) + "_edited.map").c_str());
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_win ? glfwGetWin32Window(g_win) : nullptr;
    ofn.lpstrFilter = "CPCW map\0*.map\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
    ofn.lpstrDefExt = "map";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn) && file[0]) doSaveTo(file, true);
#else
    doSave();
#endif
}

// Overwrite the map that was opened, keeping the previous bytes as <map>.bak.
static void doSaveOverOriginal() { if (!g_srcMap.empty()) doSaveTo(g_srcMap, true); }

// Open via the OS native file picker (Windows explorer); text-field fallback else.
static void doOpen() {
#ifdef _WIN32
    char file[1024] = {0};
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_win ? glfwGetWin32Window(g_win) : nullptr;
    ofn.lpstrFilter = "CPCW map or scene\0*.map;*.json\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) && file[0]) loadScene(file);
#else
    g_openPopup = true;
#endif
}

static void dropCallback(GLFWwindow*, int count, const char** paths) {
    if (count > 0 && paths && paths[0]) loadScene(paths[0]);
}

// Load a map that lives inside a mounted .pak: extract to a temp cache and load
// it; models/textures/ProtoDB then resolve from the paks via the VFS.
static void loadPakMap(const std::string& logical) {
    std::string tmp = vfs_resolve(logical, "");
    if (!tmp.empty()) loadScene(tmp);
}
// Pick a .pak, mount its folder, and list the maps inside it.
static void doOpenPak() {
#ifdef _WIN32
    char file[1024] = {0};
    OPENFILENAMEA ofn; memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_win ? glfwGetWin32Window(g_win) : nullptr;
    ofn.lpstrFilter = "CPCW pak\0*.pak\0All files\0*.*\0";
    ofn.lpstrFile = file; ofn.nMaxFile = sizeof(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn) && file[0]) {
        vfs_mount_dir(dirOf(file));
        g_pakMaps = vfs_list_suffix(".map");
        std::sort(g_pakMaps.begin(), g_pakMaps.end());
        g_pakBrowser = true;
    }
#endif
}
static void drawPakBrowser() {
    if (g_pakBrowser) { ImGui::OpenPopup("Open map from PAK"); g_pakBrowser = false; }
    if (ImGui::BeginPopupModal("Open map from PAK", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%d maps in the mounted pak(s):", (int)g_pakMaps.size());
        ImGui::SetNextItemWidth(500);
        ImGui::InputTextWithHint("##flt", "filter", g_pakFilter, sizeof(g_pakFilter));
        std::string flt = g_pakFilter; for (char& c : flt) c = (char)tolower((unsigned char)c);
        ImGui::BeginChild("maplist", ImVec2(520, 360), ImGuiChildFlags_None);
        for (const std::string& m : g_pakMaps) {
            if (!flt.empty() && m.find(flt) == std::string::npos) continue;
            if (ImGui::Selectable(m.c_str())) { loadPakMap(m); ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndChild();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Sample the terrain elevation at world (x, y) via bilinear interp of the height
// grid. Returns 0 if there is no heightmap.
static float terrainHeightAt(float x, float y) {
    const Scene& s = g_scene;
    if (s.heights.empty() || s.grid_w < 2 || s.grid_h < 2) return 0.0f;
    float fx = x, fy = y;
    if (fx < 0) fx = 0; if (fx > s.grid_w - 1) fx = (float)(s.grid_w - 1);
    if (fy < 0) fy = 0; if (fy > s.grid_h - 1) fy = (float)(s.grid_h - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < s.grid_w ? x0 + 1 : x0, y1 = y0 + 1 < s.grid_h ? y0 + 1 : y0;
    float tx = fx - x0, ty = fy - y0;
    auto H = [&](int i, int j){ return s.heights[(size_t)j * s.grid_w + i]; };
    float a = H(x0, y0) * (1 - tx) + H(x1, y0) * tx;
    float b = H(x0, y1) * (1 - tx) + H(x1, y1) * tx;
    return a * (1 - ty) + b * ty;
}

// Place a copy of entity srcId at world XZ (wx,wy), grounded on the terrain.
// In-memory structural insert: the view, the terrain and every pending edit stay
// exactly as they were, and the new entity is selected.
static void placeDuplicateAt(long srcId, float wx, float wy) {
    if (srcId < 0 || g_srcMap.empty()) return;
    float p[3] = { wx, wy, terrainHeightAt(wx, wy) };   // ground on the heightmap
    placeEntityClone(srcId, p);
}
// Convenience: place at the current camera target (used by the panel button).
static void placeDuplicate(long srcId) { placeDuplicateAt(srcId, g_cam.target.x, g_cam.target.z); }

// True when the active mode's selected tool is the "Place" tool (Object/Unit/
// Ambient modes) — i.e. a left-click on terrain should drop a prototype copy.
static bool activeToolIsPlace() {
    char buf[256]; snprintf(buf, sizeof(buf), "%s", kModes[g_mode].tools);
    int idx = 0;
    for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " "), idx++)
        if (idx == g_activeTool) return strcmp(tok, "Place") == 0;
    return false;
}

// --- prototype thumbnails (THMB chunk of each model .srm) ------------------
static std::map<std::string, std::string> g_protoIndex;   // guid -> model .srm
static bool  g_protoIndexBuilt = false;
static std::map<std::string, unsigned> g_thumbCache;      // guid -> GL tex (0=none)

// Lazily decode & upload the THMB thumbnail for a prototype GUID. Returns a GL
// texture id, or 0 if the model has no thumbnail / can't be resolved. Each guid
// is attempted once (0 is cached too) so the browser stays cheap.
static unsigned thumbForProto(const std::string& protoGuid) {
    std::string g; for (char c : protoGuid) g += (char)tolower((unsigned char)c);
    auto ci = g_thumbCache.find(g);
    if (ci != g_thumbCache.end()) return ci->second;
    if (!g_protoIndexBuilt) {
        g_protoIndex = protodb_model_index(vfs_resolve("ProtoDB.bin", g_dataRoot));
        g_protoIndexBuilt = true;
    }
    unsigned tex = 0;
    auto it = g_protoIndex.find(g);
    if (it != g_protoIndex.end()) {
        std::string mp = vfs_resolve(it->second, g_dataRoot);
        int w, h; std::vector<unsigned char> rgba;
        if (!mp.empty() && load_thmb(mp, w, h, rgba)) {
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    g_thumbCache[g] = tex;
    return tex;
}
// Drop cached thumbnails/index when the map (and thus data root) changes.
static void resetThumbCache() {
    for (auto& kv : g_thumbCache) if (kv.second) { unsigned t = kv.second; glDeleteTextures(1, &t); }
    g_thumbCache.clear(); g_protoIndex.clear(); g_protoIndexBuilt = false;
}
// Resolve a prototype's model .srm path (forward-slashed, original case), or "".
static std::string modelPathForProto(const std::string& protoGuid) {
    if (!g_protoIndexBuilt) {
        g_protoIndex = protodb_model_index(vfs_resolve("ProtoDB.bin", g_dataRoot));
        g_protoIndexBuilt = true;
    }
    std::string g; for (char c : protoGuid) g += (char)tolower((unsigned char)c);
    auto it = g_protoIndex.find(g);
    return it == g_protoIndex.end() ? std::string() : it->second;
}
// Bucket a prototype into a browser category from its model path (top folder),
// falling back to the entity schema kind.
static std::string categoryForProto(const std::string& protoGuid, const std::string& schemaType) {
    std::string mp = modelPathForProto(protoGuid);
    if (!mp.empty()) {
        std::string low; for (char c : mp) low += (char)tolower((unsigned char)c);
        size_t s1 = low.find('/');
        std::string top = s1 == std::string::npos ? low : low.substr(0, s1);
        std::string rest = s1 == std::string::npos ? std::string() : low.substr(s1 + 1);
        size_t s2 = rest.find('/');
        std::string sub = s2 == std::string::npos ? rest : rest.substr(0, s2);
        auto has = [](const std::string& s, const char* k){ return s.find(k) != std::string::npos; };
        if (top == "vehicles") return "Vehicles";
        if (top == "buildings") return "Buildings";
        if (top == "objects") {
            if (has(sub,"nature")||has(sub,"tree")||has(sub,"veget")||has(sub,"plant")||has(sub,"forest"))
                return "Nature / Trees";
            return "Objects";
        }
        if (!top.empty()) { std::string c = top; c[0] = (char)toupper((unsigned char)c[0]); return c; }
    }
    if (schemaType.find("Building") != std::string::npos) return "Buildings";
    if (schemaType.find("Vehicle") != std::string::npos || schemaType.find("Unit") != std::string::npos) return "Units";
    if (schemaType.find("Effect") != std::string::npos || schemaType.find("Sound") != std::string::npos) return "Effects";
    if (schemaType.find("Doodad") != std::string::npos) return "Doodads";
    return "Other";
}

static ImU32 playerColor(int p) {
    static const ImU32 c[] = {
        IM_COL32(200,200,200,255), IM_COL32(220,70,70,255), IM_COL32(70,120,220,255),
        IM_COL32(80,200,90,255), IM_COL32(220,200,70,255), IM_COL32(200,90,210,255),
        IM_COL32(80,210,210,255), IM_COL32(230,140,60,255) };
    return c[((p % 8) + 8) % 8];
}

static void drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) doOpen();
        if (ImGui::MenuItem("Open from .pak...")) doOpenPak();
        ImGui::Separator();
        if (ImGui::MenuItem("Save edits", "Ctrl+S", false, !g_srcMap.empty())) doSave();
        if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, !g_srcMap.empty())) doSaveAs();
        if (ImGui::MenuItem("Overwrite original", nullptr, false, !g_srcMap.empty()))
            doSaveOverOriginal();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes over the map you opened.\nThe previous bytes are kept as <map>.bak.");
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) glfwSetWindowShouldClose(g_win, 1);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !g_undo.empty())) undoEdit();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !g_redo.empty())) redoEdit();
        ImGui::Separator();
        bool hasSel = g_selected >= 0 && g_selected < (int)g_scene.entities.size();
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, g_scene.loaded)) selectAllVisible();
        if (ImGui::MenuItem("Select none", "Ctrl+Shift+A", false, !g_selection.empty())) selectNone();
        if (ImGui::MenuItem("Drop to ground", "G", false, !g_selection.empty())) groundSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) {
            const Entity& e = g_scene.entities[g_selected];
            float p[3] = { e.pos[0] + 8.0f, e.pos[1] + 8.0f, e.pos[2] };
            placeEntityClone(e.id, p);
        }
        if (ImGui::MenuItem("Delete", "Del", false, hasSel))
            deleteEntityById(g_scene.entities[g_selected].id);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Modes", nullptr, &g_showModes);
        ImGui::MenuItem("Mode tools", nullptr, &g_showPanel);
        ImGui::MenuItem("Entities", nullptr, &g_showEntities);
        ImGui::MenuItem("Properties", nullptr, &g_showProps);
        ImGui::Separator();
        ImGui::MenuItem("3D models", nullptr, &g_showModels);
        ImGui::MenuItem("Entity dots", nullptr, &g_showDots);
        ImGui::MenuItem("Wireframe", nullptr, &g_wireframe);
        ImGui::Separator();
        if (ImGui::BeginMenu("Terrain")) {
            if (ImGui::MenuItem("Textured", nullptr, g_vp.terrainMode==0)) g_vp.terrainMode=0;
            if (ImGui::MenuItem("Palette",  nullptr, g_vp.terrainMode==1)) g_vp.terrainMode=1;
            if (ImGui::MenuItem("Height ramp", nullptr, g_vp.terrainMode==2)) g_vp.terrainMode=2;
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Roads", nullptr, &g_vp.showRoads);
        ImGui::MenuItem("Decals", nullptr, &g_vp.showDecals);
        ImGui::Separator();
        if (ImGui::BeginMenu("Model cull")) {   // hides hull interior (pick what looks right)
            if (ImGui::MenuItem("Off (two-sided)", nullptr, g_vp.cullMode==0)) g_vp.cullMode=0;
            if (ImGui::MenuItem("Backface",        nullptr, g_vp.cullMode==1)) g_vp.cullMode=1;
            if (ImGui::MenuItem("Frontface",       nullptr, g_vp.cullMode==2)) g_vp.cullMode=2;
            ImGui::EndMenu();
        }
        ImGui::MenuItem("Flip model X (debug)", "X", &g_vp.flipModelX);   // mirror now baked in
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Transform")) {
        ImGui::MenuItem("Show gizmo", "T", &g_gizmoOn);
        if (ImGui::MenuItem("Translate", "R toggles", g_gizmoOp == 0)) g_gizmoOp = 0;
        if (ImGui::MenuItem("Rotate (yaw)", "R toggles", g_gizmoOp == 1)) g_gizmoOp = 1;
        ImGui::Separator();
        if (ImGui::MenuItem("World axes", nullptr, !g_gizmoLocal)) g_gizmoLocal = false;
        if (ImGui::MenuItem("Local axes", nullptr, g_gizmoLocal)) g_gizmoLocal = true;
        ImGui::Separator();
        ImGui::MenuItem("Snap (hold Shift)", nullptr, &g_snapOn);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Grid step", &g_snapGrid, 0.1f, 0.1f, 64.0f, "%.2f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Angle step", &g_snapAngle, 1.0f, 1.0f, 90.0f, "%.0f deg");
        ImGui::Separator();
        if (ImGui::MenuItem("Drop selection to ground", "G", false, !g_selection.empty()))
            groundSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Mode")) {
        for (int i = 0; i < kNumModes; i++)
            if (ImGui::MenuItem(kModes[i].name, nullptr, g_mode == i)) g_mode = i;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) { ImGui::MenuItem("Test in game"); ImGui::EndMenu(); }
    if (ImGui::BeginMenu("Help"))  { ImGui::MenuItem("About");        ImGui::EndMenu(); }
    ImGui::EndMainMenuBar();
}

static void drawOpenPopup() {
    if (g_openPopup) { ImGui::OpenPopup("Open map"); g_openPopup = false; }
    if (ImGui::BeginPopupModal("Open map", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Path to a .map (needs python + CPCW_MAP_PY) or a .json scene:");
        ImGui::SetNextItemWidth(560);
        ImGui::InputText("##path", g_openPath, sizeof(g_openPath));
        if (ImGui::Button("Load", ImVec2(120, 0)) && loadScene(g_openPath))
            ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void drawModesPanel() {
    if (!g_showModes) return;
    if (ImGui::Begin("Modes", &g_showModes))
        for (int i = 0; i < kNumModes; i++)
            if (ImGui::Selectable(kModes[i].name, g_mode == i)) { g_mode = i; g_activeTool = 0; }
    ImGui::End();
}
static void drawModePanel() {
    if (!g_showPanel) return;
    const Mode& m = kModes[g_mode];
    if (ImGui::Begin(m.name, &g_showPanel)) {
        ImGui::TextDisabled("%s mode", m.focus);
        ImGui::SeparatorText("Tools");
        char buf[256]; snprintf(buf, sizeof(buf), "%s", m.tools);
        int idx = 0;
        for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " "), idx++)
            if (ImGui::Selectable(tok, g_activeTool == idx)) g_activeTool = idx;
        ImGui::SeparatorText("Parameters");
        if (m.focus[0] == 't') {
            ImGui::SliderFloat("Size", &g_brushSize, 0.5f, 8.0f);
            float rad = g_brushSize * 4.0f;
            ImGui::TextDisabled("diameter %.0f world units  (radius %.1f)", rad * 2.0f, rad);
            ImGui::SliderFloat("Height", &g_brushHeight, -50.0f, 50.0f);
            ImGui::SliderFloat("Pressure", &g_brushPress, 0.0f, 1.0f);
        } else {
            // Prototype browser: distinct prototypes on the map, grouped by
            // category, each shown as its baked THMB preview. Click one, then Place.
            ImGui::TextWrapped("Prototypes on this map — click a thumbnail, then Place:");
            struct PInfo { long srcId; int count; std::string type; };
            std::map<std::string, PInfo> protos;   // proto guid -> info
            for (const Entity& e : g_scene.entities) {
                if (e.proto.empty()) continue;
                auto& pr = protos[e.proto];
                if (pr.count == 0) { pr.srcId = e.id; pr.type = e.type; }
                pr.count++;
            }
            // bucket into categories (sorted; each holds pointers into `protos`)
            std::map<std::string, std::vector<std::pair<const std::string, PInfo>*>> cats;
            for (auto& kv : protos)
                cats[categoryForProto(kv.first, kv.second.type)].push_back(&kv);

            ImGui::BeginChild("protos", ImVec2(0, 340), ImGuiChildFlags_None);
            const float img = 56.0f, cell = img + 18.0f;
            for (auto& c : cats) {
                char hdr[64]; snprintf(hdr, sizeof(hdr), "%s (%d)", c.first.c_str(), (int)c.second.size());
                if (!ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) continue;
                float availw = ImGui::GetContentRegionAvail().x;
                int perRow = (int)(availw / cell); if (perRow < 1) perRow = 1;
                int col = 0;
                for (auto* kvp : c.second) {
                    const std::string& proto = kvp->first;
                    const PInfo& pi = kvp->second;
                    unsigned tex = thumbForProto(proto);
                    bool sel = (g_placeSrcId == pi.srcId);
                    ImGui::PushID(proto.c_str());
                    ImGui::BeginGroup();
                    if (sel) ImGui::PushStyleColor(ImGuiCol_Button,
                                 ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    bool clicked;
                    if (tex)   // flip V: THMB rows are top-down, ImGui samples bottom-up
                        clicked = ImGui::ImageButton("t", (ImTextureID)tex, ImVec2(img, img),
                                                     ImVec2(0, 1), ImVec2(1, 0));
                    else
                        clicked = ImGui::Button("?##noimg", ImVec2(img + 8, img + 8));
                    if (sel) ImGui::PopStyleColor();
                    if (clicked) g_placeSrcId = pi.srcId;
                    char cap[32];
                    snprintf(cap, sizeof(cap), "%.6s x%d", pi.type.c_str(), pi.count);
                    ImGui::TextUnformatted(cap);
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered()) {
                        std::string mp = modelPathForProto(proto);
                        ImGui::SetTooltip("%s\n%s\n%s  (x%d)", pi.type.c_str(),
                                          mp.empty() ? "(no model)" : mp.c_str(), proto.c_str(), pi.count);
                    }
                    ImGui::PopID();
                    if (++col % perRow != 0) ImGui::SameLine();
                }
                ImGui::TreePop();
            }
            ImGui::EndChild();
            if (ImGui::Button("Place at view center") && g_placeSrcId >= 0)
                placeDuplicate(g_placeSrcId);
            ImGui::TextDisabled("Or: with the Place tool, left-click on terrain to drop a copy.");
            ImGui::TextDisabled("(grounded on the heightmap; Ctrl+D dup, Delete remove)");
        }
    }
    ImGui::End();
}
static void drawEntities() {
    if (!g_showEntities) return;
    char title[64];
    snprintf(title, sizeof(title), "Entities (%d)###ents", (int)g_scene.entities.size());
    if (ImGui::Begin(title, &g_showEntities)) {
        if (!g_scene.loaded) ImGui::TextDisabled("No map loaded (File > Open).");
        bool ch = false;
        ch |= ImGui::Checkbox("Doodads", &g_showKind[0]); ImGui::SameLine();
        ch |= ImGui::Checkbox("Buildings", &g_showKind[1]); ImGui::SameLine();
        ch |= ImGui::Checkbox("Effects", &g_showKind[2]);
        if (ch) g_entDirty = true;
        ImGui::Separator();
        ImGuiListClipper clip; clip.Begin((int)g_scene.entities.size());
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                const Entity& e = g_scene.entities[i];
                char lbl[160];
                snprintf(lbl, sizeof(lbl), "%ld  %s  p%d##e%d", e.id, e.type.c_str(), e.player, i);
                ImGui::PushStyleColor(ImGuiCol_Text, playerColor(e.player));
                if (ImGui::Selectable(lbl, g_selection.count(i) != 0)) {
                    ImGuiIO& lio = ImGui::GetIO();
                    if (lio.KeyCtrl) selectToggle(i);
                    else if (lio.KeyShift && g_selected >= 0) {   // range-add
                        int a = std::min(g_selected, i), b = std::max(g_selected, i);
                        for (int k = a; k <= b; k++) g_selection.insert(k);
                        g_selected = i; syncSelection();
                    } else selectOnly(i);
                }
                ImGui::PopStyleColor();
            }
    }
    ImGui::End();
}
static void drawProperties() {
    if (!g_showProps) return;
    if (ImGui::Begin("Properties", &g_showProps)) {
        if (g_selected < 0 || g_selected >= (int)g_scene.entities.size())
            ImGui::TextDisabled("Nothing selected.");
        else {
            Entity& e = g_scene.entities[g_selected];
            ImGui::Text("Type:  %s", e.type.c_str());
            ImGui::Text("ID:    %ld", e.id);
            ImGui::TextWrapped("Proto: %s", e.proto.c_str());
            int player = e.player;
            if (ImGui::InputInt("Player", &player)) {
                e.player = player; g_edited.insert(e.id); g_entDirty = true;
            }
            if (ImGui::IsItemActivated()) snapEntity(g_selected);
            if (ImGui::IsItemDeactivatedAfterEdit()) commitEntity();
            float pos[3] = {e.pos[0], e.pos[1], e.pos[2]};
            if (ImGui::DragFloat3("Pos", pos, 0.5f)) {
                e.pos[0]=pos[0]; e.pos[1]=pos[1]; e.pos[2]=pos[2];
                g_edited.insert(e.id); g_entDirty = true; g_modelsDirty = true;
            }
            if (ImGui::IsItemActivated()) snapEntity(g_selected);
            if (ImGui::IsItemDeactivatedAfterEdit()) commitEntity();
            float dir = e.dir;
            if (ImGui::DragFloat("Dir (yaw)", &dir, 1.0f)) {
                e.dir = dir; g_edited.insert(e.id); g_modelsDirty = true;
            }
            if (ImGui::IsItemActivated()) snapEntity(g_selected);
            if (ImGui::IsItemDeactivatedAfterEdit()) commitEntity();
            if (g_edited.count(e.id)) ImGui::TextDisabled("(edited — File > Save edits)");
        }
        if (g_saveStatus[0]) { ImGui::Separator(); ImGui::TextWrapped("%s", g_saveStatus); }
    }
    ImGui::End();
}

// Ray from the camera through the mouse, intersected with the ACTUAL terrain by
// raymarching the heightmap (so the hit tracks the cursor over hills, not a flat
// plane). Falls back to a plane at the target height. Returns (gx,gy) = world XZ.
static bool terrainHit(const ImVec2& mp, const ImVec2& cmin, const ImVec2& cmax,
                       float& gx, float& gy) {
    float W = cmax.x - cmin.x, H = cmax.y - cmin.y;
    if (W < 1 || H < 1) return false;
    float ndcx = 2.0f * (mp.x - cmin.x) / W - 1.0f;
    float ndcy = 1.0f - 2.0f * (mp.y - cmin.y) / H;
    V3 eye = g_cam.eye(), fwd = norm(g_cam.target - eye);
    V3 right = norm(cross(fwd, V3{0,1,0})), up = cross(right, fwd);
    float th = std::tan(0.45f), aspect = W / H;
    V3 dir = norm(fwd + right * (ndcx * th * aspect) + up * (ndcy * th));
    auto rayAt = [&](float t){ return V3{ eye.x+dir.x*t, eye.y+dir.y*t, eye.z+dir.z*t }; };
    // march downward toward the terrain, then bisect the crossing
    if (dir.y < -1e-5f && g_scene.loaded && !g_scene.heights.empty()) {
        float tMax = g_cam.dist * 3.0f + 500.0f;
        float step = std::max(0.5f, g_cam.dist * 0.005f);
        float tPrev = 0.0f; bool above = (eye.y > terrainHeightAt(eye.x, eye.z));
        for (float t = step; t < tMax; t += step) {
            V3 p = rayAt(t);
            bool a = (p.y > terrainHeightAt(p.x, p.z));
            if (above && !a) {
                float t0 = tPrev, t1 = t;
                for (int b = 0; b < 24; b++) {
                    float tm = (t0+t1)*0.5f; V3 pm = rayAt(tm);
                    if (pm.y > terrainHeightAt(pm.x, pm.z)) t0 = tm; else t1 = tm;
                }
                V3 pf = rayAt((t0+t1)*0.5f); gx = pf.x; gy = pf.z; return true;
            }
            tPrev = t; above = a;
        }
    }
    if (std::fabs(dir.y) < 1e-5f) return false;
    float d = (g_cam.target.y - eye.y) / dir.y;
    if (d < 0) return false;
    gx = eye.x + dir.x * d; gy = eye.z + dir.z * d;
    return true;
}

// Raise/Lower/Smooth the heightmap around (cx,cy) with radial falloff.
static void applyTerrainBrush(float cx, float cy, int tool) {
    int W = g_scene.grid_w, H = g_scene.grid_h;
    if (W < 2 || H < 2 || (int)g_scene.heights.size() != W * H) return;
    float radius = g_brushSize * 4.0f;
    float strength = (g_brushPress * 1.8f + 0.2f);
    int i0 = std::max(0, (int)(cx - radius)), i1 = std::min(W - 1, (int)(cx + radius));
    int j0 = std::max(0, (int)(cy - radius)), j1 = std::min(H - 1, (int)(cy + radius));
    std::vector<float>& h = g_scene.heights;
    if (g_scene.heightDirty.size() != h.size()) g_scene.heightDirty.assign(h.size(), 0);
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float dx = i - cx, dy = j - cy, r = std::sqrt(dx*dx + dy*dy);
        if (r > radius) continue;
        float w = 1.0f - r / radius; w *= w;
        size_t gi = (size_t)j * W + i;
        g_scene.heightDirty[gi] = 1;
        if (tool == 1)      h[gi] += strength * w;              // Raise
        else if (tool == 2) h[gi] -= strength * w;              // Lower
        else {                                                  // Smooth
            float a = (h[(size_t)j*W + std::max(0,i-1)] + h[(size_t)j*W + std::min(W-1,i+1)] +
                       h[(size_t)std::max(0,j-1)*W + i] + h[(size_t)std::min(H-1,j+1)*W + i]) * 0.25f;
            h[gi] += (a - h[gi]) * w * 0.5f;
        }
    }
    g_scene.terrainEdited = true; g_terrainDirty = true;
}

// Nearest entity to a screen point (by projected origin, within a pixel radius),
// respecting the category show/hide toggles. -1 if none.
static int pickEntity(const ImVec2& mp, const ImVec2& cmin, const ImVec2& cmax) {
    float W = cmax.x - cmin.x, H = cmax.y - cmin.y;
    if (W <= 1 || H <= 1 || !g_scene.loaded) return -1;
    M4 vp = g_cam.viewProj(W / H);
    float best = 18.0f; int bi = -1;
    for (int i = 0; i < (int)g_scene.entities.size(); i++) {
        const Entity& e = g_scene.entities[i];
        if (g_showKind[(e.kind >= 0 && e.kind < 3) ? e.kind : 2] == false) continue;
        V3 wp{ e.pos[0], e.pos[2], e.pos[1] };
        float cx = vp.m[0]*wp.x + vp.m[4]*wp.y + vp.m[8]*wp.z + vp.m[12];
        float cy = vp.m[1]*wp.x + vp.m[5]*wp.y + vp.m[9]*wp.z + vp.m[13];
        float cw = vp.m[3]*wp.x + vp.m[7]*wp.y + vp.m[11]*wp.z + vp.m[15];
        if (cw <= 0.001f) continue;
        float sx = cmin.x + (cx/cw*0.5f + 0.5f) * W;
        float sy = cmin.y + (1.0f - (cy/cw*0.5f + 0.5f)) * H;
        float d = fabsf(sx - mp.x) + fabsf(sy - mp.y);
        if (d < best) { best = d; bi = i; }
    }
    return bi;
}

// Pick under the cursor: prefer a real model footprint (easy to click big models),
// fall back to the projected-origin dot pick for model-less entities (effects).
// Approximate but cheap — this is the per-frame hover path.
static int pickAny(const ImVec2& mp, const ImVec2& cmin, const ImVec2& cmax) {
    int mi = g_vp.pickModel(mp, cmin, cmax, g_cam);
    if (mi >= 0) return mi;
    return pickEntity(mp, cmin, cmax);
}

// ImGui screen point -> pick-buffer pixel (buffer origin is bottom-left).
static void screenToPickBuffer(const ImVec2& p, const ImVec2& cmin, const ImVec2& cmax,
                               int& bx, int& by) {
    ImGuiIO& io = ImGui::GetIO();
    float sx = io.DisplaySize.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
    float sy = io.DisplaySize.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
    if (sx <= 0) sx = 1.0f; if (sy <= 0) sy = 1.0f;
    bx = (int)((p.x - cmin.x) * sx);
    by = (int)((cmax.y - p.y) * sy);
}
static void pickBufferSize(const ImVec2& cmin, const ImVec2& cmax, int& w, int& h) {
    ImGuiIO& io = ImGui::GetIO();
    float sx = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.0f;
    float sy = io.DisplayFramebufferScale.y > 0 ? io.DisplayFramebufferScale.y : 1.0f;
    w = (int)((cmax.x - cmin.x) * sx);
    h = (int)((cmax.y - cmin.y) * sy);
}

// Exact pick under the cursor via the colour-code buffer (occlusion- and
// alpha-cut-correct). Falls back to the AABB test if the FBO is unavailable.
static int pickExact(const ImVec2& mp, const ImVec2& cmin, const ImVec2& cmax) {
    int w, h; pickBufferSize(cmin, cmax, w, h);
    if (!g_vp.pickPassReady() || w < 1 || h < 1) return pickAny(mp, cmin, cmax);
    g_vp.renderPickBuffer(g_cam, w, h, g_showModels, g_showDots);
    int bx, by; screenToPickBuffer(mp, cmin, cmax, bx, by);
    int id = g_vp.pickBufferAt(bx, by);
    if (id >= 0 && id < (int)g_scene.entities.size()) {
        const Entity& e = g_scene.entities[id];
        if (!g_showKind[(e.kind >= 0 && e.kind < 3) ? e.kind : 2]) return -1;
    }
    return id;
}

// Commit whatever the gizmo (or any group move) changed as ONE undo command.
static void commitBatch(std::vector<EntSnap>& start) {
    EditCmd c; c.kind = CMD_BATCH;
    for (EntSnap& s : start) {
        int i = entityIndexById(s.id);
        if (i < 0) continue;
        const Entity& e = g_scene.entities[i];
        bool moved = e.dir != s.dir0;
        for (int k = 0; k < 3; k++) { s.pos1[k] = e.pos[k]; if (e.pos[k] != s.pos0[k]) moved = true; }
        s.dir1 = e.dir;
        if (moved) c.ents.push_back(s);
    }
    if (!c.ents.empty()) pushCmd(std::move(c));
    start.clear();
}

// 3D translate/rotate handles over the whole selection (ImGuizmo). Runs BEFORE
// updateCamera each frame so IsOver()/IsUsing() can gate viewport input — a click
// on a handle must not also pick or orbit.
static void drawGizmo(const ImVec2& cmin, const ImVec2& cmax) {
    g_gizmoHot = false;
    if (!g_gizmoOn || !g_scene.loaded || g_selection.empty()) {
        if (g_gizmoDragging) { commitBatch(g_gizmoStart); g_gizmoDragging = false; }
        return;
    }
    float W = cmax.x - cmin.x, H = cmax.y - cmin.y;
    if (W <= 1 || H <= 1) return;
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
    ImGuizmo::SetRect(cmin.x, cmin.y, W, H);
    M4 view = g_cam.view(), proj = g_cam.proj(W / H);

    // pivot = centroid of the selection, in GL world space {x, elevation, mapY}
    float px = 0, py = 0, pz = 0; int n = 0;
    for (int i : g_selection) {
        if (i < 0 || i >= (int)g_scene.entities.size()) continue;
        const Entity& e = g_scene.entities[i];
        px += e.pos[0]; py += e.pos[2]; pz += e.pos[1]; n++;
    }
    if (n == 0) return;
    px /= n; py /= n; pz /= n;
    if (!g_gizmoDragging) {
        float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, px,py,pz,1};
        memcpy(g_gizmoMat, m, sizeof(m));
    }
    ImGuiIO& io = ImGui::GetIO();
    bool snap = g_snapOn || io.KeyShift;
    float snapT[3] = { g_snapGrid, g_snapGrid, g_snapGrid };
    float snapR[3] = { g_snapAngle, g_snapAngle, g_snapAngle };
    ImGuizmo::OPERATION op = (g_gizmoOp == 0) ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE_Y;
    ImGuizmo::MODE mode = g_gizmoLocal ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    ImGuizmo::Manipulate(view.m, proj.m, op, mode, g_gizmoMat, nullptr,
                         snap ? (g_gizmoOp == 0 ? snapT : snapR) : nullptr);
    g_gizmoHot = ImGuizmo::IsOver() || ImGuizmo::IsUsing();

    if (ImGuizmo::IsUsing()) {
        if (!g_gizmoDragging) {                       // drag just began: snapshot
            g_gizmoDragging = true;
            g_gizmoPivot0[0] = px; g_gizmoPivot0[1] = py; g_gizmoPivot0[2] = pz;
            g_gizmoStart.clear();
            for (int i : g_selection) {
                if (i < 0 || i >= (int)g_scene.entities.size()) continue;
                const Entity& e = g_scene.entities[i];
                EntSnap s; s.id = e.id; s.dir0 = e.dir;
                for (int k = 0; k < 3; k++) s.pos0[k] = e.pos[k];
                g_gizmoStart.push_back(s);
            }
        }
        if (g_gizmoOp == 0) {
            // translation delta in GL world -> entity space (x, mapY, elevation)
            float dx = g_gizmoMat[12] - g_gizmoPivot0[0];
            float dy = g_gizmoMat[13] - g_gizmoPivot0[1];
            float dz = g_gizmoMat[14] - g_gizmoPivot0[2];
            for (const EntSnap& s : g_gizmoStart) {
                int i = entityIndexById(s.id); if (i < 0) continue;
                Entity& e = g_scene.entities[i];
                e.pos[0] = s.pos0[0] + dx;
                e.pos[1] = s.pos0[1] + dz;
                e.pos[2] = s.pos0[2] + dy;
                g_edited.insert(e.id);
            }
        } else {
            // pure-Y rotation: read it straight off the basis (m[0]=cos, m[8]=sin)
            // rather than from an Euler decomposition, which is ambiguous past 90deg.
            float th = std::atan2(g_gizmoMat[8], g_gizmoMat[0]);
            float c = std::cos(th), sn = std::sin(th);
            float thDeg = th * 180.0f / 3.14159265f;
            for (const EntSnap& s : g_gizmoStart) {
                int i = entityIndexById(s.id); if (i < 0) continue;
                Entity& e = g_scene.entities[i];
                // rotY: x' = c*x + s*z,  z' = -s*x + c*z   (GL x = pos[0], GL z = pos[1])
                float rx = s.pos0[0] - g_gizmoPivot0[0], rz = s.pos0[1] - g_gizmoPivot0[2];
                e.pos[0] = g_gizmoPivot0[0] + (c * rx + sn * rz);
                e.pos[1] = g_gizmoPivot0[2] + (-sn * rx + c * rz);
                e.dir = s.dir0 + thDeg;
                g_edited.insert(e.id);
            }
        }
        g_entDirty = true; g_modelsDirty = true;
    } else if (g_gizmoDragging) {
        commitBatch(g_gizmoStart);
        g_gizmoDragging = false;
    }
}

// Drop every selected entity onto the heightmap (undoable, one command).
static void groundSelection() {
    if (g_selection.empty()) return;
    std::vector<EntSnap> start;
    for (int i : g_selection) {
        if (i < 0 || i >= (int)g_scene.entities.size()) continue;
        const Entity& e = g_scene.entities[i];
        EntSnap s; s.id = e.id; s.dir0 = e.dir;
        for (int k = 0; k < 3; k++) s.pos0[k] = e.pos[k];
        start.push_back(s);
    }
    for (const EntSnap& s : start) {
        int i = entityIndexById(s.id); if (i < 0) continue;
        Entity& e = g_scene.entities[i];
        e.pos[2] = terrainHeightAt(e.pos[0], e.pos[1]);
        g_edited.insert(e.id);
    }
    g_entDirty = true; g_modelsDirty = true;
    commitBatch(start);
}

// camera navigation over the central viewport region
static void updateCamera(const ImVec2& cmin, const ImVec2& cmax) {
    ImGuiIO& io = ImGui::GetIO();
    // The gizmo owns the mouse while a handle is hovered or dragged — otherwise a
    // click on an arrow would also pick an entity behind it, or start an orbit.
    bool over = ImGui::IsMouseHoveringRect(cmin, cmax, false) && !io.WantCaptureMouse
                && !g_gizmoHot;

    // WASD / arrow-key movement over the ground plane. Speed has a floor so it
    // does NOT crawl when zoomed in (was purely distance-scaled before).
    if (!io.WantCaptureKeyboard && !io.KeyCtrl) {
        float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 0.016f;
        float sp = (g_cam.dist * 0.4f + 50.0f) * dt * (io.KeyShift ? 3.0f : 1.0f);
        V3 fwd = norm(g_cam.target - g_cam.eye()); fwd.y = 0; fwd = norm(fwd);
        V3 right = norm(cross(fwd, V3{0, 1, 0}));
        if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow))    g_cam.target = g_cam.target + fwd * sp;
        if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow))  g_cam.target = g_cam.target - fwd * sp;
        if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow))  g_cam.target = g_cam.target - right * sp;
        if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) g_cam.target = g_cam.target + right * sp;
    }
    if (over && ImGui::IsMouseClicked(2)) g_orbiting = true;
    if (!ImGui::IsMouseDown(2)) g_orbiting = false;
    if (over && ImGui::IsMouseClicked(1)) g_panning = true;
    if (!ImGui::IsMouseDown(1)) g_panning = false;
    if (g_orbiting) {
        g_cam.yaw   -= io.MouseDelta.x * 0.01f;
        g_cam.pitch += io.MouseDelta.y * 0.01f;
        if (g_cam.pitch > 1.5f) g_cam.pitch = 1.5f;
        if (g_cam.pitch < -0.2f) g_cam.pitch = -0.2f;
    }
    if (g_panning) {
        V3 e = g_cam.eye(), fwd = norm(g_cam.target - e);
        V3 right = norm(cross(fwd, {0,1,0})), up = cross(right, fwd);
        float k = g_cam.dist * 0.0016f + 0.15f;   // floor so it pans when zoomed in
        g_cam.target = g_cam.target - right * (io.MouseDelta.x * k)
                                    + up    * (io.MouseDelta.y * k);
    }
    if (over && io.MouseWheel != 0.0f) {
        float gx, gy;
        bool hit = g_scene.loaded &&
                   terrainHit(io.MousePos, cmin, cmax, gx, gy);   // BEFORE changing dist
        float old = g_cam.dist;
        g_cam.dist *= std::pow(0.88f, io.MouseWheel);
        if (g_cam.dist < 1.0f)    g_cam.dist = 1.0f;              // was 5.0 — get close
        if (g_cam.dist > 6000.0f) g_cam.dist = 6000.0f;
        // On zoom-IN, glide the pivot toward the cursor's ground point by the same
        // fraction the distance shrank, so that point stays put and the pivot lands
        // on real terrain (fixes the mid-elevation floating pivot).
        if (hit && io.MouseWheel > 0.0f && old > 1e-4f) {
            float f = 1.0f - g_cam.dist / old;                    // (0,1)
            V3 gp{ gx, terrainHeightAt(gx, gy), gy };
            g_cam.target = g_cam.target + (gp - g_cam.target) * f;
        }
    }
    // Terrain mode + a brush tool (Raise/Lower/Smooth) => left-drag brushes the
    // heightmap instead of selecting entities.
    bool brushing = (g_mode == 0 && (g_activeTool == 1 || g_activeTool == 2 || g_activeTool == 6));
    // Brush cursor ring: show the terrain area the brush covers whenever hovering
    // in Vertex/Terrain mode (any tool), matching applyTerrainBrush's radius.
    {
        float gx, gy;
        if (over && g_mode == 0 && g_scene.loaded && terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            const int N = 48; float rad = g_brushSize * 4.0f;
            std::vector<float> ring; ring.reserve(N * 3);
            for (int k = 0; k < N; k++) {
                float a = 6.2831853f * k / N;
                float x = gx + rad * std::cos(a), z = gy + rad * std::sin(a);
                ring.push_back(x); ring.push_back(terrainHeightAt(x, z) + 0.3f); ring.push_back(z);
            }
            g_vp.setBrushRing(std::move(ring));
        } else {
            g_vp.setBrushRing({});
        }
    }
    if (over && brushing && ImGui::IsMouseDown(0)) {
        if (!g_strokeActive) { g_strokeH0 = g_scene.heights; g_strokeActive = true; }
        float gx, gy;
        if (terrainHit(io.MousePos, cmin, cmax, gx, gy)) applyTerrainBrush(gx, gy, g_activeTool);
        return;   // don't pick/move entities while brushing
    }
    // Place-on-click: with a browser prototype selected and the Place tool active
    // (Object/Unit/Ambient), left-click on terrain drops a grounded copy there.
    bool placing = g_scene.loaded && g_placeSrcId >= 0 && activeToolIsPlace();
    if (over && placing && ImGui::IsMouseClicked(0)) {
        float gx, gy;
        if (terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            placeDuplicateAt(g_placeSrcId, gx, gy);
            return;   // consumed the click; don't also pick/move an entity
        }
    }
    // Hover highlight: track the entity under the cursor each frame (not while
    // brushing/dragging/placing), so the viewport shows what a click would select.
    // Deliberately the cheap AABB path — a full colour-code pass every frame would
    // double the model draw cost on a 3400-entity map just to tint a box.
    g_hovered = (over && !brushing && !placing && !g_draggingEnt && !g_marquee)
              ? pickAny(io.MousePos, cmin, cmax) : -1;

    // Left-click selects. Ctrl toggles, Shift adds. Clicking empty space starts a
    // rubber-band; clicking an entity starts a drag-move.
    if (over && ImGui::IsMouseClicked(0) && g_scene.loaded) {
        int bi = pickExact(io.MousePos, cmin, cmax);
        bool additive = io.KeyCtrl || io.KeyShift;
        if (bi >= 0) {
            if (io.KeyCtrl) selectToggle(bi);
            else if (io.KeyShift) { g_selection.insert(bi); g_selected = bi; syncSelection(); }
            else if (!g_selection.count(bi)) selectOnly(bi);
            else g_selected = bi;                 // clicking inside a multi-selection keeps it
            if (g_selection.count(bi)) { g_draggingEnt = true; snapEntity(bi); }
        } else {
            g_marquee = true; g_marqueeA = io.MousePos;
            if (!additive) selectNone();
        }
    }
    // Rubber-band: draw the box live, resolve it against the colour-code buffer on
    // release so occluded entities behind terrain are never caught.
    if (g_marquee) {
        ImVec2 b = io.MousePos;
        ImVec2 lo(std::min(g_marqueeA.x, b.x), std::min(g_marqueeA.y, b.y));
        ImVec2 hi(std::max(g_marqueeA.x, b.x), std::max(g_marqueeA.y, b.y));
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(lo, hi, IM_COL32(90, 160, 255, 40));
        dl->AddRect(lo, hi, IM_COL32(120, 190, 255, 220));
        if (!ImGui::IsMouseDown(0)) {
            g_marquee = false;
            if (hi.x - lo.x >= 3 && hi.y - lo.y >= 3) {
                int w, h; pickBufferSize(cmin, cmax, w, h);
                if (g_vp.pickPassReady() && w > 0 && h > 0) {
                    g_vp.renderPickBuffer(g_cam, w, h, g_showModels, g_showDots);
                    int x0, y0, x1, y1;
                    screenToPickBuffer(lo, cmin, cmax, x0, y1);   // lo.y is the TOP edge
                    screenToPickBuffer(hi, cmin, cmax, x1, y0);   // -> becomes the HIGH buffer row
                    std::vector<int> hits;
                    g_vp.pickBufferRect(x0, y0, x1 - x0, y1 - y0, hits);
                    std::vector<int> keep;
                    for (int i : hits) {
                        if (i < 0 || i >= (int)g_scene.entities.size()) continue;
                        const Entity& e = g_scene.entities[i];
                        if (g_showKind[(e.kind >= 0 && e.kind < 3) ? e.kind : 2]) keep.push_back(i);
                    }
                    selectAdd(keep, !(io.KeyCtrl || io.KeyShift));
                }
            }
        }
    }
    // Left-drag: the selected entity follows the cursor on the terrain (grounded).
    // Only its own model instance is updated (no full rebuild), so it's smooth.
    if (g_draggingEnt && ImGui::IsMouseDown(0) && g_selected >= 0 &&
        g_selected < (int)g_scene.entities.size()) {
        float gx, gy;
        if (terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            Entity& e = g_scene.entities[g_selected];
            e.pos[0] = gx; e.pos[1] = gy; e.pos[2] = terrainHeightAt(gx, gy);
            g_edited.insert(e.id); g_entDirty = true;
            g_vp.moveInstance(g_selected, V3{ gx, e.pos[2], gy }, e.dir, e.scale);   // live model follow
        }
    }
    if (g_draggingEnt && !ImGui::IsMouseDown(0)) { g_draggingEnt = false; commitEntity(); }
    // finalize a terrain brush stroke into one undo command
    if (g_strokeActive && !ImGui::IsMouseDown(0)) {
        EditCmd c; c.kind = CMD_TERRAIN;
        for (size_t i = 0; i < g_scene.heights.size() && i < g_strokeH0.size(); i++)
            if (g_scene.heights[i] != g_strokeH0[i]) {
                c.cells.push_back((int)i); c.h0.push_back(g_strokeH0[i]); c.h1.push_back(g_scene.heights[i]);
            }
        if (!c.cells.empty()) pushCmd(std::move(c));
        g_strokeActive = false; g_strokeH0.clear();
    }
}

// dev: end-to-end check that a structural edit no longer eats pending field edits
// and that undo/redo of add+delete round-trips the byte buffer exactly.
static int structTest(const char* mapPath, const char* outPath) {
    Scene s;
    if (!load_map_native(mapPath, s)) { printf("structtest: load failed\n"); return 2; }
    if (s.entities.size() < 2) { printf("structtest: too few entities\n"); return 2; }
    const size_t n0 = s.entities.size();
    const std::vector<unsigned char> raw0 = s.raw;

    // 1. an unsaved field edit on entity[0]
    long editId = s.entities[0].id, srcId = s.entities[1].id;
    s.entities[0].pos[0] += 123.5f;
    float wantX = s.entities[0].pos[0];
    apply_edits_inplace(s, {editId}, s.raw);          // what the editor now flushes

    // 2. a structural insert on top of it
    long newId = 1; for (const auto& e : s.entities) if (e.id >= newId) newId = e.id + 1;
    float p[3] = { s.entities[1].pos[0] + 8.0f, s.entities[1].pos[1] + 8.0f, s.entities[1].pos[2] };
    std::vector<unsigned char> blob;
    if (!add_entity_bytes(s, srcId, p, newId, &blob)) { printf("structtest: add failed\n"); return 3; }
    size_t nAdd = s.entities.size();

    // 3. the field edit must have survived the insert
    float gotX = 0; bool found = false;
    for (const auto& e : s.entities) if (e.id == editId) { gotX = e.pos[0]; found = true; }
    bool editKept = found && std::fabs(gotX - wantX) < 0.01f;

    // 4. undo the add (erase by id) -> byte-identical to the pre-insert buffer
    std::vector<unsigned char> afterAdd = s.raw;
    if (!delete_entity_bytes(s, newId, nullptr, nullptr)) { printf("structtest: del failed\n"); return 3; }
    bool undoExact = (s.raw.size() == raw0.size()) && s.entities.size() == n0;

    // 5. redo the insert -> byte-identical to step 2
    if (!insert_objt_at_index(s, -1, blob)) { printf("structtest: reinsert failed\n"); return 3; }
    bool redoExact = (s.raw == afterAdd);

    bool ok = editKept && undoExact && redoExact && nAdd == n0 + 1;
    printf("structtest ents %zu->%zu  fieldEditKept=%d undoByteExact=%d redoByteExact=%d\n",
           n0, nAdd, (int)editKept, (int)undoExact, (int)redoExact);
    if (outPath) { std::ofstream f(outPath, std::ios::binary); f.write((const char*)s.raw.data(), (std::streamsize)s.raw.size()); }
    return ok ? 0 : 3;
}

int main(int argc, char** argv) {
    std::string loadPath, shotPath, pickDump; bool selftest = false, pickTest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--picktest")) {
            pickTest = true;
            if (i + 1 < argc && argv[i+1][0] != '-') pickDump = argv[++i];
        }
        else if (!strcmp(argv[i], "--pakmap") && i + 2 < argc) {   // mount dir, load a pak map
            vfs_mount_dir(argv[i+1]);
            std::string tmp = vfs_resolve(argv[i+2], "");
            if (!tmp.empty()) loadPath = tmp;
            i += 2;
        }
        else if (!strcmp(argv[i], "--structtest") && i + 1 < argc) {
            const char* out = (i + 2 < argc && argv[i+2][0] != '-') ? argv[i+2] : nullptr;
            return structTest(argv[i+1], out);
        }
        else if (!strcmp(argv[i], "--paktest") && i + 1 < argc) {
            PakArchive pak;
            if (!pak.open(argv[i+1])) { printf("pak open FAILED\n"); return 2; }
            printf("pak entries=%zu\n", pak.count());
            if (i + 3 < argc) {
                auto data = pak.read(argv[i+2]);
                std::ifstream df(argv[i+3], std::ios::binary);
                std::vector<unsigned char> disk((std::istreambuf_iterator<char>(df)),
                                                std::istreambuf_iterator<char>());
                printf("extract '%s': pak=%zu disk=%zu identical=%d\n",
                       argv[i+2], data.size(), disk.size(), (int)(data == disk));
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--srmcheck") && i + 1 < argc) {
            // dev: parse+build a .srm and report finite-ness/bbox/uv (foliage fix check)
            SrmModel m; std::string err;
            if (!srm_parse(argv[i+1], m, &err)) { printf("parse FAIL: %s\n", err.c_str()); return 2; }
            std::vector<RenderMesh> rms; srm_build_render(m, SKIN_FULL, VAR_ALL, rms);
            int tot=0, bad=0; float lo[3]={1e30f,1e30f,1e30f}, hi[3]={-1e30f,-1e30f,-1e30f};
            for (auto& rm : rms) for (auto& v : rm.verts) { tot++;
                if (!std::isfinite(v.x)||!std::isfinite(v.y)||!std::isfinite(v.z)) { bad++; continue; }
                lo[0]=std::min(lo[0],v.x); hi[0]=std::max(hi[0],v.x);
                lo[1]=std::min(lo[1],v.y); hi[1]=std::max(hi[1],v.y);
                lo[2]=std::min(lo[2],v.z); hi[2]=std::max(hi[2],v.z); }
            int uvMesh=0; for (auto& mesh : m.meshes) if (mesh.byUsage(USAGE_TEXCOORD)) uvMesh++;
            printf("srmcheck '%s': nodes=%zu meshes=%zu rmeshes=%zu verts=%d nonfinite=%d\n",
                   argv[i+1], m.nodes.size(), m.meshes.size(), rms.size(), tot, bad);
            printf("  bbox=[%.2f %.2f %.2f]..[%.2f %.2f %.2f]  meshesWithUV=%d/%zu\n",
                   lo[0],lo[1],lo[2], hi[0],hi[1],hi[2], uvMesh, m.meshes.size());
            for (auto& rm : rms) printf("  rmesh node=%d verts=%zu tris=%zu tex='%s' alphaTest=%d\n",
                   rm.nodeIndex, rm.verts.size(), rm.indices.size()/3, rm.diffuseTex.c_str(), (int)rm.alphaTest);
            return bad ? 3 : 0;
        }
        else if (!strcmp(argv[i], "--protodbtest") && i + 1 < argc) {
            auto idx = protodb_model_index(argv[i+1]);
            printf("protodb models=%zu\n", idx.size());
            int shown=0; for (auto& kv : idx) { if(shown++>=3) break; printf("  %s -> %s\n", kv.first.c_str(), kv.second.c_str()); }
            return 0;
        }
        else if (!strcmp(argv[i], "--thumbtest") && i + 1 < argc) {
            // dev: --thumbtest <model.srm> [out.ppm]  (decode THMB, print + dump)
            int w, h; std::vector<unsigned char> rgba;
            if (!load_thmb(argv[i+1], w, h, rgba)) { printf("thumb: no/invalid THMB\n"); return 2; }
            unsigned long sum = 0; for (unsigned char c : rgba) sum += c;
            printf("thumb %dx%d bytes=%zu checksum=%lu\n", w, h, rgba.size(), sum);
            if (i + 2 < argc) {
                std::ofstream o(argv[i+2], std::ios::binary);
                o << "P6\n" << w << " " << h << "\n255\n";
                for (int k = 0; k < w*h; k++) o.write((const char*)&rgba[k*4], 3);
                printf("wrote %s\n", argv[i+2]);
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--overlaytest") && i + 1 < argc) {
            // dev: --overlaytest <map>  (decode roads/decals, print counts + bounds)
            Scene s; if (!load_map_native(argv[i+1], s)) return 2;
            size_t rv=0, dv=0; int noTex=0;
            for (auto& m : s.roads)  { rv += m.verts.size()/5; if (m.tex.empty()) noTex++; }
            for (auto& m : s.decals) { dv += m.verts.size()/5; if (m.tex.empty()) noTex++; }
            printf("overlays: roadSplines=%zu areafills=%zu (%zu verts) decals=%zu (%zu verts) noTex=%d\n",
                   s.roadSplines.size(), s.roads.size(), rv, s.decals.size(), dv, noTex);
            for (size_t k=0;k<s.roadSplines.size() && k<4;k++)
                printf("  spline[%zu] nodes=%zu %s\n", k, s.roadSplines[k].cx.size(), s.roadSplines[k].tex.c_str());
            for (size_t k=0;k<s.decals.size() && k<3;k++) printf("  decal[%zu] %s\n", k, s.decals[k].tex.c_str());
            return 0;
        }
        else if (!strcmp(argv[i], "--addtest") && i + 4 < argc) {
            Scene s; if (!load_map_native(argv[i+1], s)) return 2;
            long srcId = atol(argv[i+2]), newId = atol(argv[i+3]);
            float pos[3] = {100.0f, 100.0f, 0.0f};
            bool ok = add_entity_native(s, srcId, pos, newId, argv[i+4]);
            Scene s2; bool rok = load_map_native(argv[i+4], s2);
            printf("addtest add=%d reparse=%d before=%zu after=%zu\n",
                   (int)ok, (int)rok, s.entities.size(), s2.entities.size());
            return ok && rok ? 0 : 3;
        }
        else if (!strcmp(argv[i], "--deltest") && i + 3 < argc) {
            Scene s; if (!load_map_native(argv[i+1], s)) return 2;
            long id = atol(argv[i+2]);
            bool ok = delete_entity_native(s, id, argv[i+3]);
            // re-parse the output to confirm it's valid + has one fewer entity
            Scene s2; bool rok = load_map_native(argv[i+3], s2);
            printf("deltest del=%d reparse=%d before=%zu after=%zu\n",
                   (int)ok, (int)rok, s.entities.size(), s2.entities.size());
            return ok && rok ? 0 : 3;
        }
        else if (!strcmp(argv[i], "--heighttest") && i + 3 < argc) {
            // dev: --heighttest <map> <cellIndex> <out>  (bump a height, save)
            Scene s; if (!load_map_native(argv[i+1], s)) return 2;
            int k = atoi(argv[i+2]);
            s.heightDirty.assign(s.heights.size(), 0);
            if (k >= 0 && k < (int)s.heights.size()) { s.heights[k] += 10.0f; s.heightDirty[k] = 1; }
            s.terrainEdited = true;
            bool ok = save_map_native(s, {}, argv[i+3]);
            printf("heighttest heightOff=%ld cell=%d %s\n", s.heightOff, k, ok?"ok":"fail");
            return ok ? 0 : 3;
        }
        else if (!strcmp(argv[i], "--applytest") && i + 7 < argc) {
            // dev: --applytest <map> <id> <px> <py> <pz> <player> <out>
            Scene s; if (!load_map_native(argv[i+1], s)) return 2;
            long id = atol(argv[i+2]);
            for (auto& e : s.entities) if (e.id == id) {
                e.pos[0]=(float)atof(argv[i+3]); e.pos[1]=(float)atof(argv[i+4]);
                e.pos[2]=(float)atof(argv[i+5]); e.player=atoi(argv[i+6]);
            }
            bool ok = save_map_native(s, {id}, argv[i+7]);
            printf("applytest %s\n", ok ? "ok" : "fail"); return ok ? 0 : 3;
        }
    }
    if (!loadPath.empty()) loadScene(loadPath);
    if (selftest) {
        int withScale = 0, nonUnit = 0;
        for (const auto& e : g_scene.entities) {
            if (e.scaleOff >= 0) withScale++;
            if (e.scale != 1.0f) nonUnit++;
        }
        printf("selftest: map=%s loaded=%d entities=%d terrain=%dx%d grid=%dx%d heights=%zu"
               " scaleField=%d scaled=%d splatOff=%ld\n",
               g_scene.name.c_str(), (int)g_scene.loaded, (int)g_scene.entities.size(),
               g_scene.world_w, g_scene.world_h, g_scene.grid_w, g_scene.grid_h,
               g_scene.heights.size(), withScale, nonUnit, g_scene.splatOff);
        return g_scene.loaded ? 0 : 2;
    }

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* win = glfwCreateWindow(1360, 850, "CPCW Map Editor", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    g_win = win;
#ifdef _WIN32
    { char exe[MAX_PATH]; if (GetModuleFileNameA(nullptr, exe, MAX_PATH))
        vfs_mount_dir(dirOf(exe)); }   // auto-mount paks next to the exe (game folder)
#endif
    glfwSetDropCallback(win, dropCallback);   // drag-and-drop a .map / .json
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!loadGLCore()) fprintf(stderr, "warning: some GL functions failed to load\n");
    g_glReady = true;
    g_vp.init();

    // headless render-to-BMP: one frame of the 3D scene, full framebuffer, exit.
    if (!shotPath.empty()) {
        g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene, g_showKind);
        g_vp.buildSplatTextures(g_scene, g_dataRoot);
        g_vp.buildOverlays(g_scene, g_dataRoot);
        g_vp.buildModels(g_scene, g_dataRoot);
        g_sceneDirty = false;
        int fbw = 1360, fbh = 850;
        // render into an offscreen FBO (reliable regardless of window visibility)
        GLuint fbo, tex, rbo;
        glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fbw, fbh, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glGenRenderbuffers(1, &rbo); glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fbw, fbh);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            fprintf(stderr, "warning: FBO incomplete\n");
        glViewport(0, 0, fbw, fbh);
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        g_vp.render(g_cam, (float)fbw / (float)fbh, g_wireframe, g_selected,
                    g_showModels, g_showDots);
        glFinish();
        std::vector<unsigned char> px((size_t)fbw * fbh * 3);
        glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
        writeBMP(shotPath.c_str(), fbw, fbh, px.data());
        printf("wrote %s (%dx%d)\n", shotPath.c_str(), fbw, fbh);
        glfwDestroyWindow(win); glfwTerminate();
        return 0;
    }

    // headless check of the colour-code pick buffer (no mouse needed): render it,
    // then for every model instance project its AABB centre and read the code back.
    // A high self-hit rate means click-select resolves to the model you clicked.
    if (pickTest) {
        g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene, g_showKind);
        g_vp.buildSplatTextures(g_scene, g_dataRoot);
        g_vp.buildModels(g_scene, g_dataRoot);
        const int W = 1360, H = 850;
        if (!g_vp.pickPassReady()) { printf("picktest: pick program failed to build\n"); return 3; }
        Camera wideCam = g_cam;
        g_vp.renderPickBuffer(g_cam, W, H, true, true);
        std::vector<Viewport3D::InstProbe> centers;
        g_vp.instanceProbes(centers);
        M4 vp = g_cam.viewProj((float)W / (float)H);
        int tested = 0, self = 0, other = 0, empty = 0, offscreen = 0;
        for (auto& c : centers) {
            const V3& w = c.center;
            float cx = vp.m[0]*w.x + vp.m[4]*w.y + vp.m[8]*w.z + vp.m[12];
            float cy = vp.m[1]*w.x + vp.m[5]*w.y + vp.m[9]*w.z + vp.m[13];
            float cw = vp.m[3]*w.x + vp.m[7]*w.y + vp.m[11]*w.z + vp.m[15];
            if (cw <= 0.001f) { offscreen++; continue; }
            int bx = (int)((cx/cw*0.5f + 0.5f) * W);
            int by = (int)((cy/cw*0.5f + 0.5f) * H);   // buffer origin is bottom-left
            if (bx < 0 || by < 0 || bx >= W || by >= H) { offscreen++; continue; }
            tested++;
            int got = g_vp.pickBufferAt(bx, by);
            if (got == c.entIdx) self++;
            else if (got >= 0) other++;
            else empty++;
        }
        // Every non-self hit is a legitimate occlusion (a nearer model covering this
        // one's centre in a top-down view of 3000+ models), so the wide pass is
        // judged on "resolved to SOME entity".
        int resolved = self + other;
        printf("picktest instances=%zu tested=%d self=%d occluded=%d empty=%d offscreen=%d"
               "  selfRate=%.1f%% resolvedRate=%.1f%%\n",
               centers.size(), tested, self, other, empty, offscreen,
               tested ? 100.0 * self / tested : 0.0,
               tested ? 100.0 * resolved / tested : 0.0);

        // Decisive check: frame individual models close up, at a distance scaled to
        // each model's own size, and require the centre pixel to name that exact
        // entity. No occlusion excuse here. Alpha-cut foliage is skipped — its
        // centre pixel legitimately falls in a gap between leaves.
        int closeTested = 0, closeHit = 0, closeMiss = 0;
        for (size_t k = 0; k < centers.size() && closeTested < 24; k += centers.size()/24 + 1) {
            const auto& pr = centers[k];
            if (pr.radius < 0.5f) continue;
            g_cam.target = pr.center;
            g_cam.dist = pr.radius * 3.0f;      // always outside the model
            g_cam.yaw = 0.7f; g_cam.pitch = 0.35f;
            g_vp.renderPickBuffer(g_cam, W, H, true, true);
            closeTested++;
            // Sample the middle third of the frame rather than the single centre
            // pixel: at 3x radius the model fills roughly that area, and an exact
            // centre pixel can legitimately fall in a gap between alpha-cut leaves
            // or through a hollow model. This asks the real question — "is this
            // model pickable where it is drawn?"
            std::vector<int> hits;
            g_vp.pickBufferRect(W/3, H/3, W/3, H/3, hits);
            bool found = false;
            for (int id : hits) if (id == pr.entIdx) { found = true; break; }
            if (found) closeHit++;
            else {
                closeMiss++;
                printf("  close miss: ent %d r=%.1f  codes in frame=%d\n",
                       pr.entIdx, pr.radius, (int)hits.size());
            }
        }
        printf("picktest close-up %d/%d models pickable where drawn\n",
               closeHit, closeTested);
        if (!pickDump.empty()) {   // dump the wide-view pick buffer for eyeballing
            g_cam = wideCam;
            g_vp.renderPickBuffer(g_cam, W, H, true, true);
            std::vector<unsigned char> rgb; int dw, dh;
            if (g_vp.readPickBufferRGB(rgb, dw, dh)) {
                // codes are near-black at low indices; amplify so shapes are visible
                for (auto& b : rgb) b = (unsigned char)std::min(255, b * 3);
                writeBMP(pickDump.c_str(), dw, dh, rgb.data());
                printf("picktest wrote pick buffer -> %s\n", pickDump.c_str());
            }
        }
        glfwDestroyWindow(win); glfwTerminate();
        // Thresholds are deliberately loose on the close-up pass: the sample includes
        // alpha-cut foliage whose silhouette is mostly holes, so a model can be
        // correctly rendered and still not cover the middle third of the frame.
        bool ok = tested > 0 && resolved * 10 >= tested * 9        // >=90% resolved
                  && closeTested > 0 && closeHit * 3 >= closeTested * 2;   // >=66% close-up
        return ok ? 0 : 3;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuizmo::BeginFrame();
        // Alt-Tabbing away mid-drag otherwise leaves a modifier or a drag latched.
        if (!glfwGetWindowAttrib(win, GLFW_FOCUSED)) {
            if (g_gizmoDragging) { commitBatch(g_gizmoStart); g_gizmoDragging = false; }
            g_marquee = false; g_orbiting = false; g_panning = false;
            if (g_draggingEnt) { g_draggingEnt = false; commitEntity(); }
        }

        ImGuiID dsid = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
                            ImGuiDockNodeFlags_PassthruCentralNode);
        drawMenuBar();
        drawOpenPopup();
        drawPakBrowser();
        drawModesPanel();
        drawModePanel();
        drawEntities();
        drawProperties();

        // central viewport region (empty dock node -> 3D shows through)
        ImVec2 cmin(0,0), cmax(0,0);
        if (ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dsid)) {
            cmin = central->Pos;
            cmax = ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y);
        }
        drawGizmo(cmin, cmax);   // before updateCamera: sets IsOver()/IsUsing()
        updateCamera(cmin, cmax);

        ImGuiIO& kio = ImGui::GetIO();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) { if (kio.KeyShift) doSaveAs(); else doSave(); }
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) doOpen();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undoEdit();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redoEdit();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            if (kio.KeyShift) selectNone(); else selectAllVisible();
        }
        // C cycles model cull (Off -> Back -> Front); X flips model local-X (mirror text)
        if (!kio.WantCaptureKeyboard && !kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
            g_vp.cullMode = (g_vp.cullMode + 1) % 3;
        if (!kio.WantCaptureKeyboard && !kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X))
            g_vp.flipModelX = !g_vp.flipModelX;
        // gizmo: T toggles, R switches translate/rotate, G drops to ground.
        // (W/E/A/S/D are the camera; don't steal them.)
        if (!kio.WantCaptureKeyboard && !kio.KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_T)) g_gizmoOn = !g_gizmoOn;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) g_gizmoOp = 1 - g_gizmoOp;
            if (ImGui::IsKeyPressed(ImGuiKey_G)) groundSelection();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selectNone();
        }
        // [ / ] rotate the selected entity 5 degrees (yaw)
        if (!kio.WantCaptureKeyboard && g_selected >= 0 &&
            g_selected < (int)g_scene.entities.size()) {
            Entity& e = g_scene.entities[g_selected];
            const long selId = e.id;
            const float selPos[3] = { e.pos[0], e.pos[1], e.pos[2] };
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { snapEntity(g_selected); e.dir -= 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { snapEntity(g_selected); e.dir += 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
            // Delete / Ctrl+D: structural, in memory, undoable. Both rebuild the
            // entity list, so `e` must not be touched afterwards.
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                deleteEntityById(selId);
            } else if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
                float p[3] = { selPos[0] + 8.0f, selPos[1] + 8.0f, selPos[2] };
                placeEntityClone(selId, p);
            }
        }

        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav))
            ImGui::Text("Mode: %s | Map: %s | %d entities | %d selected%s | "
                        "WASD move  MMB orbit  RMB pan  wheel zoom  drag-empty marquee  "
                        "T gizmo  R rot/move  G ground",
                        kModes[g_mode].name, g_mapPath, (int)g_scene.entities.size(),
                        (int)g_selection.size(),
                        (g_snapOn || ImGui::GetIO().KeyShift) ? "  [SNAP]" : "");
        ImGui::End();

        if (g_sceneDirty && g_glReady) {
            g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene, g_showKind);
            g_vp.buildSplatTextures(g_scene, g_dataRoot);
            g_vp.buildOverlays(g_scene, g_dataRoot);   // roads + decals (was missing -> no roads)
            g_vp.buildModels(g_scene, g_dataRoot);
            g_sceneDirty = false;
        }
        if (g_entDirty && g_glReady) {
            g_vp.buildEntities(g_scene, g_showKind); g_entDirty = false;
        }
        if (g_modelsDirty && g_glReady) {   // after a drag: refresh model instances
            g_vp.buildModels(g_scene, g_dataRoot); g_modelsDirty = false;
        }
        if (g_terrainDirty && g_glReady) {  // after a terrain brush stroke
            g_vp.buildTerrain(g_scene); g_terrainDirty = false;
        }

        ImGui::Render();
        int fbw, fbh; glfwGetFramebufferSize(win, &fbw, &fbh);
        ImGuiIO& io = ImGui::GetIO();
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // render 3D into the central node rect
        float sx = io.DisplaySize.x > 0 ? fbw / io.DisplaySize.x : 1.0f;
        float sy = io.DisplaySize.y > 0 ? fbh / io.DisplaySize.y : 1.0f;
        int vx = (int)(cmin.x * sx);
        int vy = (int)((io.DisplaySize.y - cmax.y) * sy);
        int vw = (int)((cmax.x - cmin.x) * sx);
        int vh = (int)((cmax.y - cmin.y) * sy);
        if (vw > 8 && vh > 8) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(vx, vy, vw, vh);
            glViewport(vx, vy, vw, vh);
            glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            g_vp.render(g_cam, (float)vw / (float)vh, g_wireframe, g_selected,
                        g_showModels, g_showDots, g_hovered);
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, fbw, fbh);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
