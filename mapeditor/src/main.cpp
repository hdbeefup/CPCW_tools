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
#include "weather.h"
#include "overlays.h"
#include "settings.h"
#include "crashdump.h"
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
// Each mode carries a `kind` so the rest of the file can ask what a mode IS
// rather than compare its index. The indices are still 0..8 and must stay that
// way: they are what `settings` persists and what `View > Mode` writes, and
// several modes share a kind (Object/Unit/Ambient are all MK_OBJECT).
enum ModeKind {
    MK_TERRAIN,   // heightmap + splat brushes
    MK_OBJECT,    // entity placement / transform
    MK_OVERLAY,   // GROA roads + GDEC decals
    MK_RIVER,     // GRVL/GRVR river splines
    MK_LIGHT,     // WTHR weather / lighting presets
    MK_TRIGGER,   // scenario logic
    MK_RETIRED,   // the format has no such data in ANY CPCW map
};
struct Mode { const char* name; const char* focus; const char* tools; ModeKind kind; };
static const Mode kModes[] = {
    {"Vertex / Terrain", "terrain", "Grab Raise Lower SetPlane Raise>Plane Lower>Plane Smooth Blend TileFill Area", MK_TERRAIN},
    {"Spline / River",   "terrain", "Select Node-inspect", MK_RIVER},
    {"Object / Doodad",  "object",  "Place Move Lift Rotate Tilt Align", MK_OBJECT},
    {"Unit",             "object",  "Place Move Rotate", MK_OBJECT},
    {"Ambient",          "object",  "Place Move Lift Distance", MK_OBJECT},
    {"Shader / Decals",  "terrain", "Place Move Rotate Z-order", MK_OVERLAY},
    {"Lake / Water",     "terrain", "(retired)", MK_RETIRED},
    {"Light",            "global",  "(settings)", MK_LIGHT},
    {"Trigger",          "logic",   "Locations Triggers Conditions Actions", MK_TRIGGER},
};
static const int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));

static Scene      g_scene;
static Camera     g_cam;
static Viewport3D g_vp;
static bool  g_glReady = false, g_sceneDirty = false;
static int   g_mode = 0, g_activeTool = 0, g_selected = -1, g_hovered = -1;
static ModeKind modeKind() { return kModes[g_mode].kind; }
static bool  modeIs(ModeKind k) { return kModes[g_mode].kind == k; }
static int   g_lightMode = 0;      // 0 neutral, 1 map preset, 2 preset + fog
static int   g_lightPreset = -1;   // Light panel selection (index into Scene::weather)
static Settings    g_settings;
static std::string g_settingsPath;
static int   g_winW = 1360, g_winH = 850;
static std::string g_presetArg;    // --preset <name|#slot>, for headless shots
static float g_brushSize = 2.0f, g_brushHeight = 0.0f, g_brushPress = 0.5f;
static bool  g_wireframe = false;
static bool  g_showModes = true, g_showPanel = true, g_showProps = true,
             g_showEntities = true, g_showChanges = false, g_showChunks = false;
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
static bool  g_splatTexDirty = false;   // painted layers -> re-upload the weight textures
static bool  g_overlayDirty = false;    // decal edited -> re-decode + rebuild batches
static std::set<int> g_decalTouched;    // pool slots edited since the last save
static int   g_decalSel = -1;           // Decals panel selection

// Vertex/Terrain tool indices, matching kModes[0].tools word for word.
enum { TT_GRAB=0, TT_RAISE, TT_LOWER, TT_SETPLANE, TT_RAISE_TO_PLANE,
       TT_LOWER_TO_PLANE, TT_SMOOTH, TT_BLEND, TT_TILEFILL, TT_AREA };
static bool toolIsHeight(int t) {
    return t==TT_GRAB || t==TT_RAISE || t==TT_LOWER || t==TT_SETPLANE ||
           t==TT_RAISE_TO_PLANE || t==TT_LOWER_TO_PLANE || t==TT_SMOOTH;
}
static bool toolIsPaint(int t) { return t==TT_BLEND || t==TT_TILEFILL; }

static int g_paintLayer = -1;   // Scene::terrainLayers index that Blend/TileFill paints

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
// CMD_WEATHER is keyed on (pool slot, field index), not an entity id: a preset is
// not an entity. Both keys are stable because nothing creates or deletes a preset
// yet, and an entity edit never touches the WTHR pool.
// CMD_DECAL is keyed on the pool SLOT, which is stable; a decal's index in
// Scene::decalRecs is not (a re-decode reorders nothing today, but nothing
// guarantees that once insert/delete land).
enum { CMD_ENTITY, CMD_TERRAIN, CMD_ADD, CMD_DELETE, CMD_BATCH, CMD_WEATHER, CMD_DECAL };
struct EditCmd {
    int  kind = CMD_ENTITY;
    long entId = 0;
    float pos0[3]{}, pos1[3]{}; float dir0 = 0, dir1 = 0; int pl0 = 0, pl1 = 0;
    std::vector<int> cells; std::vector<float> h0, h1;   // CMD_TERRAIN
    std::vector<unsigned char> objt; int entIndex = -1;  // CMD_ADD / CMD_DELETE
    std::vector<EntSnap> ents;                           // CMD_BATCH (gizmo, group ops)
    int wSlot = -1, wField = -1;                         // CMD_WEATHER
    std::array<float,4> w0{}, w1{};
    int dSlot = -1;                                      // CMD_DECAL
    std::array<float,5> d0{}, d1{};                      // cx, cz, sx, sy, rot
};
static std::vector<EditCmd> g_undo, g_redo;
static bool g_snapActive = false; static EditCmd g_snap;   // pending entity snapshot
static bool g_strokeActive = false; static std::vector<float> g_strokeH0;
static std::string g_dataRoot;                     // folder holding ProtoDB.bin + models
static std::string g_srcMap;                       // original .map (empty if .json)
static std::set<long> g_edited;                    // ids with pending field edits
static char  g_saveStatus[256] = "";

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static float terrainHeightAt(float x, float y);   // defined below; bilinear heightmap

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
    // Every edit kind that lives outside Scene::raw must be named here. The
    // early-out used to test only `terrainEdited`, so painting a splat layer (or
    // now editing a weather preset) and THEN placing an object silently threw the
    // paint away — apply_edits_inplace never ran, and the structural op rebuilt
    // raw from bytes that had never seen it.
    if (ids.empty() && !g_scene.terrainEdited && !g_scene.splatEdited &&
        !g_scene.weatherEdited) return;
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
    if (c.kind == CMD_WEATHER) {
        for (WeatherPreset& w : g_scene.weather) {
            if (w.slot != c.wSlot) continue;
            if (c.wField < 0 || c.wField >= (int)w.values.size()) break;
            w.values[(size_t)c.wField] = useNew ? c.w1 : c.w0;
            // The dirty flag is NOT cleared on undo: the field still differs from
            // the bytes on disk, so it must still be written on save or
            // Scene::weather and Scene::raw diverge.
            w.dirty[(size_t)c.wField] = 1;
            g_scene.weatherEdited = true;
            break;
        }
        return;
    }
    if (c.kind == CMD_DECAL) {
        const std::array<float,5>& v = useNew ? c.d1 : c.d0;
        if (overlay_set_decal(g_scene, c.dSlot, v[0], v[1], v[2], v[3], v[4])) {
            g_overlayDirty = true;
            // decalEdited is a count of decals differing from the file; recompute
            // it from the pending set rather than incrementing, so undo lowers it.
            g_decalTouched.insert(c.dSlot);
            g_scene.decalEdited = (int)g_decalTouched.size();
        }
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
// --- changes since last save -------------------------------------------------
// A per-entity copy of the state that is actually on disk. Everything the Changes
// panel reports is derived from it on demand, so nothing has to be maintained as
// edits happen — the same shape Ariane uses (m_savedTranslation + diff flags).
struct SavedEnt { float pos[3]; float dir; };
static std::map<long, SavedEnt> g_savedState;
enum { DIFF_ADDED = 1, DIFF_DELETED = 2, DIFF_MOVED = 4, DIFF_ROTATED = 8, DIFF_FIELD = 16 };

static void snapshotSavedState() {
    g_savedState.clear();
    for (const Entity& e : g_scene.entities) {
        SavedEnt s; for (int k = 0; k < 3; k++) s.pos[k] = e.pos[k]; s.dir = e.dir;
        g_savedState[e.id] = s;
    }
}
static int diffFlags(const Entity& e) {
    auto it = g_savedState.find(e.id);
    if (it == g_savedState.end()) return DIFF_ADDED;
    int f = 0;
    for (int k = 0; k < 3; k++)
        if (std::fabs(e.pos[k] - it->second.pos[k]) > 0.001f) f |= DIFF_MOVED;
    if (std::fabs(e.dir - it->second.dir) > 0.01f) f |= DIFF_ROTATED;
    for (const EntityField& fl : e.fields) if (fl.dirty) { f |= DIFF_FIELD; break; }
    return f;
}

// --- clipboard ---------------------------------------------------------------
// Entries hold the source entity's OBJT bytes plus its world position, so a paste
// can reproduce the group's shape around a new anchor (or land it back in place).
struct ClipEntry { std::vector<unsigned char> objt; float pos[3]; };
static std::vector<ClipEntry> g_clipboard;
static float g_clipAnchor[3] = {0,0,0};

static void copySelection() {
    g_clipboard.clear();
    if (g_selection.empty() || g_scene.raw.empty()) return;
    flushEditsToRaw();       // clipboard must carry the edited bytes, not the stale ones
    double ax = 0, ay = 0, az = 0; int n = 0;
    for (int i : g_selection) {
        if (i < 0 || i >= (int)g_scene.entities.size()) continue;
        const Entity& e = g_scene.entities[i];
        if (e.objtStart < 0 || e.objtEnd <= e.objtStart || e.objtEnd > (long)g_scene.raw.size()) continue;
        ClipEntry c;
        c.objt.assign(g_scene.raw.begin() + e.objtStart, g_scene.raw.begin() + e.objtEnd);
        for (int k = 0; k < 3; k++) c.pos[k] = e.pos[k];
        ax += e.pos[0]; ay += e.pos[1]; az += e.pos[2]; n++;
        g_clipboard.push_back(std::move(c));
    }
    if (n) { g_clipAnchor[0]=(float)(ax/n); g_clipAnchor[1]=(float)(ay/n); g_clipAnchor[2]=(float)(az/n); }
    snprintf(g_saveStatus, sizeof(g_saveStatus), "Copied %d entit%s.",
             (int)g_clipboard.size(), g_clipboard.size()==1?"y":"ies");
}

// Paste the clipboard, keeping the group's relative layout. `atX/atY` is the new
// anchor; inPlace pastes back at the original coordinates. One undo command covers
// the whole paste, and the pasted entities become the selection.
static void pasteClipboard(bool inPlace, float atX, float atY) {
    if (g_clipboard.empty() || g_scene.raw.empty() || g_srcMap.empty()) return;
    flushEditsToRaw();
    long nextId = 1;
    for (const auto& en : g_scene.entities) if (en.id >= nextId) nextId = en.id + 1;
    std::vector<long> newIds;
    EditCmd batch; batch.kind = CMD_BATCH;
    for (const ClipEntry& c : g_clipboard) {
        std::vector<unsigned char> blob = c.objt;
        long id = nextId++;
        // patch ID + Pos inside the blob using the offsets of the *pasted* record,
        // which we only know after it is parsed — so insert first, then edit fields.
        if (!insert_objt_at_index(g_scene, -1, blob)) continue;
        int idx = (int)g_scene.entities.size() - 1;
        if (idx < 0) continue;
        Entity& e = g_scene.entities[idx];
        e.id = id;
        if (!inPlace) {
            e.pos[0] = atX + (c.pos[0] - g_clipAnchor[0]);
            e.pos[1] = atY + (c.pos[1] - g_clipAnchor[1]);
            e.pos[2] = terrainHeightAt(e.pos[0], e.pos[1]);
        }
        // write the new ID straight into raw (Pos goes through the normal edit path)
        if (e.idOff >= 0 && e.idOff + 4 <= (long)g_scene.raw.size()) {
            unsigned v = (unsigned)id;
            g_scene.raw[e.idOff]   = v & 0xff;      g_scene.raw[e.idOff+1] = (v>>8) & 0xff;
            g_scene.raw[e.idOff+2] = (v>>16) & 0xff; g_scene.raw[e.idOff+3] = (v>>24) & 0xff;
        }
        g_edited.insert(id);
        newIds.push_back(id);
        EditCmd add; add.kind = CMD_ADD; add.entId = id; add.objt = blob; add.entIndex = idx;
        g_undo.push_back(std::move(add));     // one command per inserted record
    }
    g_redo.clear();
    flushEditsToRaw();                        // bake the moved positions into raw
    g_selection.clear();
    for (long id : newIds) { int i = entityIndexById(id); if (i >= 0) g_selection.insert(i); }
    g_selected = g_selection.empty() ? -1 : *g_selection.begin();
    syncSelection();
    g_entDirty = true; g_modelsDirty = true;
    snprintf(g_saveStatus, sizeof(g_saveStatus), "Pasted %d entit%s.",
             (int)newIds.size(), newIds.size()==1?"y":"ies");
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

// Delete every selected entity. Collect the IDs first: each removal renumbers the
// indices, so iterating the index set while mutating would delete the wrong rows.
static int deleteSelection() {
    if (g_selection.empty()) return 0;
    std::vector<long> ids;
    for (int i : g_selection)
        if (i >= 0 && i < (int)g_scene.entities.size()) ids.push_back(g_scene.entities[i].id);
    int n = 0;
    for (long id : ids) if (deleteEntityById(id)) n++;
    selectNone();
    if (n) snprintf(g_saveStatus, sizeof(g_saveStatus), "Deleted %d entit%s.", n, n==1?"y":"ies");
    return n;
}
static void cutSelection() { copySelection(); deleteSelection(); }

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
    g_clipboard.clear();
    snapshotSavedState();     // baseline for the Changes panel = the file as opened
    if (g_glReady) { g_vp.clearModels(); g_vp.clearOverlays(); }
    resetThumbCache();
    if (!preserveView) snprintf(g_mapPath, sizeof(g_mapPath), "%s", path.c_str());
    // Only a real user-facing open goes in the recent list — a structural edit
    // reloads with preserveView, and those must not churn it.
    if (!preserveView && !g_srcMap.empty()) g_settings.pushRecentMap(g_srcMap);
    g_lightPreset = -1;              // the new map's presets are different ones
    g_decalSel = -1; g_decalTouched.clear();
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
        // Clear EVERY dirty flag, not just the entity/terrain ones: the file on
        // disk now matches memory, so leaving a flag set makes the next save
        // rewrite cells the user never touched again (and keeps the Changes
        // panel and the title claiming unsaved work).
        g_edited.clear();
        g_scene.terrainEdited = false;
        g_scene.splatEdited = false;
        g_scene.weatherEdited = false;
        g_decalTouched.clear(); g_scene.decalEdited = 0;
        for (Entity& e : g_scene.entities) for (EntityField& f : e.fields) f.dirty = false;
        for (WeatherPreset& w : g_scene.weather)
            std::fill(w.dirty.begin(), w.dirty.end(), (unsigned char)0);
        snapshotSavedState();       // the Changes panel now diffs against this file
        if (!g_scene.heightDirty.empty())
            std::fill(g_scene.heightDirty.begin(), g_scene.heightDirty.end(), (unsigned char)0);
        for (auto& layer : g_scene.splatDirty)
            std::fill(layer.begin(), layer.end(), (unsigned char)0);
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

// True when the active mode's selected tool is the "Place" tool (Object/Unit/
// Ambient modes) — i.e. a left-click on terrain should drop a prototype copy.
// The MK_OBJECT gate matters: Shader/Decals also names its first tool "Place",
// so without it a click in that mode dropped an *entity* on the terrain.
static bool activeToolIsPlace() {
    if (!modeIs(MK_OBJECT)) return false;
    char buf[256]; snprintf(buf, sizeof(buf), "%s", kModes[g_mode].tools);
    int idx = 0;
    for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " "), idx++)
        if (idx == g_activeTool) return strcmp(tok, "Place") == 0;
    return false;
}

// --- prototype thumbnails (THMB chunk of each model .srm) ------------------
static std::map<std::string, std::string> g_protoIndex;   // guid -> model .srm
static std::map<std::string, ProtoInfo>   g_protoFull;    // guid -> model/name/schema
static bool  g_protoIndexBuilt = false;
static std::map<std::string, unsigned> g_thumbCache;      // guid -> GL tex (0=none)

static void ensureProtoIndex() {
    if (g_protoIndexBuilt) return;
    std::string p = vfs_resolve("ProtoDB.bin", g_dataRoot);
    g_protoFull = protodb_full_index(p);
    g_protoIndex.clear();
    for (const auto& kv : g_protoFull)
        if (!kv.second.model.empty()) g_protoIndex[kv.first] = kv.second.model;
    g_protoIndexBuilt = true;
}

// Lazily decode & upload the THMB thumbnail for a prototype GUID. Returns a GL
// texture id, or 0 if the model has no thumbnail / can't be resolved. Each guid
// is attempted once (0 is cached too) so the browser stays cheap.
static unsigned thumbForProto(const std::string& protoGuid) {
    std::string g; for (char c : protoGuid) g += (char)tolower((unsigned char)c);
    auto ci = g_thumbCache.find(g);
    if (ci != g_thumbCache.end()) return ci->second;
    ensureProtoIndex();
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
    g_thumbCache.clear(); g_protoIndex.clear(); g_protoFull.clear(); g_protoIndexBuilt = false;
}
// Resolve a prototype's model .srm path (forward-slashed, original case), or "".
static std::string modelPathForProto(const std::string& protoGuid) {
    ensureProtoIndex();
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

// --- placing prototypes the map has never used -------------------------------
// There is no need to construct an OBJT from the schema: clone an existing record
// of the RIGHT entity schema and point its Prototype at the new GUID. The schema
// to match is derivable — ProtoDB's "SP<X>" is the .map's "S<X>Desc" — and GUIDs
// are a fixed 36 chars, so the swap is size-preserving.
//
// Returns the entity index of a usable byte template, or -1 with `why` set.
static int templateForProto(const std::string& guid, std::string& why) {
    ensureProtoIndex();
    std::string g; for (char c : guid) g += (char)tolower((unsigned char)c);
    auto it = g_protoFull.find(g);
    if (it == g_protoFull.end()) { why = "prototype not in ProtoDB"; return -1; }
    std::string want = protodb_map_schema(it->second.schema);
    if (want.empty()) { why = "unrecognised prototype schema '" + it->second.schema + "'"; return -1; }
    for (int i = 0; i < (int)g_scene.entities.size(); i++) {
        const Entity& e = g_scene.entities[i];
        if (e.type == want && e.objtStart >= 0 && e.protoOff >= 0 && e.idOff >= 0 && e.posOff >= 0)
            return i;
    }
    why = "this map has no " + want + " to use as a template";
    return -1;
}

// Place `guid` at world XZ, grounded. Returns the new entity ID or -1.
static long placePrototypeAt(const std::string& guid, float wx, float wy, float yaw = 0.0f) {
    if (g_scene.raw.empty() || g_srcMap.empty()) return -1;
    std::string why;
    int ti = templateForProto(guid, why);
    if (ti < 0) {
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Cannot place: %s.", why.c_str());
        return -1;
    }
    long srcId = g_scene.entities[ti].id;
    long newId = 1;
    for (const auto& en : g_scene.entities) if (en.id >= newId) newId = en.id + 1;
    float p[3] = { wx, wy, terrainHeightAt(wx, wy) };
    flushEditsToRaw();
    std::vector<unsigned char> blob;
    if (!add_entity_bytes(g_scene, srcId, p, newId, &blob, guid)) {
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Cannot place: GUID splice refused.");
        return -1;
    }
    afterStructural(newId);
    // Apply the ghost's yaw to the placed copy, so what you previewed is what you
    // get. Done after the insert because the entity does not exist until then;
    // the CMD_ADD below still carries the original OBJT bytes, so undo removes
    // the whole record regardless.
    if (yaw != 0.0f) {
        int ni = entityIndexById(newId);
        if (ni >= 0) { g_scene.entities[(size_t)ni].dir = yaw; g_edited.insert(newId); }
    }
    EditCmd c; c.kind = CMD_ADD; c.entId = newId; c.objt = std::move(blob);
    c.entIndex = entityIndexById(newId);
    pushCmd(std::move(c));
    return newId;
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
        {
            std::vector<std::string> recent = g_settings.recentMaps();
            if (ImGui::BeginMenu("Open recent", !recent.empty())) {
                for (size_t r = 0; r < recent.size(); r++) {
                    const std::string& p = recent[r];
                    const char* leaf = p.c_str();
                    if (const char* sl = strrchr(leaf, '/')) leaf = sl + 1;
                    if (const char* bs = strrchr(leaf, '\\')) leaf = bs + 1;
                    char lbl[320]; snprintf(lbl, sizeof(lbl), "%s##rc%d", leaf, (int)r);
                    if (ImGui::MenuItem(lbl)) loadScene(p);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.c_str());
                }
                ImGui::EndMenu();
            }
        }
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
        bool hasSel = !g_selection.empty();
        if (ImGui::MenuItem("Select all", "Ctrl+A", false, g_scene.loaded)) selectAllVisible();
        if (ImGui::MenuItem("Select none", "Ctrl+Shift+A", false, hasSel)) selectNone();
        if (ImGui::MenuItem("Drop to ground", "G", false, hasSel)) groundSelection();
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, hasSel)) cutSelection();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, hasSel)) copySelection();
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !g_clipboard.empty()))
            pasteClipboard(false, g_cam.target.x, g_cam.target.z);
        if (ImGui::MenuItem("Paste in place", "Ctrl+Shift+V", false, !g_clipboard.empty()))
            pasteClipboard(true, 0, 0);
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, hasSel)) {
            copySelection();
            pasteClipboard(false, g_clipAnchor[0] + 8.0f, g_clipAnchor[1] + 8.0f);
        }
        if (ImGui::MenuItem("Delete", "Del", false, hasSel)) deleteSelection();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Modes", nullptr, &g_showModes);
        ImGui::MenuItem("Mode tools", nullptr, &g_showPanel);
        ImGui::MenuItem("Entities", nullptr, &g_showEntities);
        ImGui::MenuItem("Changes since save", nullptr, &g_showChanges);
        ImGui::MenuItem("Map chunks (raw)", nullptr, &g_showChunks);
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
        ImGui::MenuItem("Rivers", nullptr, &g_vp.showRivers);
        if (ImGui::BeginMenu("Lighting")) {
            // Neutral is the DEFAULT: the 30 multiplayer Night_multi presets carry
            // SunColor exactly (0,0,0), which would render a map flat blue and
            // unusable to edit. Mode 7 switches to Preset while it is open.
            if (ImGui::MenuItem("Editor (neutral)", "L", g_lightMode==0)) g_lightMode=0;
            if (ImGui::MenuItem("Map preset",       nullptr, g_lightMode==1)) g_lightMode=1;
            if (ImGui::MenuItem("Map preset + fog", nullptr, g_lightMode==2)) g_lightMode=2;
            ImGui::EndMenu();
        }
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
// --- prototype browser -------------------------------------------------------
// Sources from ProtoDB, not from what the map happens to contain, so anything in
// the game can be placed. Entries the current map cannot host (no entity of the
// matching schema to use as a byte template) are shown greyed with the reason.
static char  g_browseFilter[128] = "";
static bool  g_browseGrid = true;
static bool  g_browseFavOnly = false;
static bool  g_browseOnMapOnly = false;
static std::set<std::string> g_favourites;      // proto guids
static std::string g_placeProto;                // guid selected for placement
static float g_placeYaw = 0.0f;                 // ghost heading; [ / ] aim it
static std::string g_ghostArg;                  // --ghost <guid|#N> <wx> <wy>
static std::string g_dataRootArg;               // --dataroot <path> override
static float g_ghostAt[2] = {0, 0};

// Arm the ghost from --ghost, for headless checks. Returns false when not asked.
static bool applyGhostArg() {
    if (g_ghostArg.empty() || !g_scene.loaded) return false;
    std::string guid = g_ghostArg;
    if (guid[0] == '#') {                        // Nth distinct prototype on the map
        int want = atoi(guid.c_str() + 1), seen = 0;
        std::set<std::string> uniq;
        guid.clear();
        for (const Entity& e : g_scene.entities) {
            if (e.proto.empty() || !uniq.insert(e.proto).second) continue;
            if (seen++ == want) { guid = e.proto; break; }
        }
        if (guid.empty()) return false;
    }
    std::string mp = modelPathForProto(guid);
    V3 wp{ g_ghostAt[0], terrainHeightAt(g_ghostAt[0], g_ghostAt[1]), g_ghostAt[1] };
    if (!mp.empty()) g_vp.setGhost(mp, g_dataRoot, wp, g_placeYaw, 1.0f);
    printf("ghost: guid=%s model=%s at (%.1f,%.1f,%.1f) armed=%d\n",
           guid.c_str(), mp.empty() ? "(unresolved)" : mp.c_str(),
           wp.x, wp.y, wp.z, (int)g_vp.ghostOn);
    return g_vp.ghostOn;
}

static void drawPrototypeBrowser() {
    ensureProtoIndex();
    if (g_protoFull.empty()) {
        ImGui::TextWrapped("ProtoDB.bin not found — open a map from the game folder "
                           "or a .pak so prototypes can be listed.");
        return;
    }
    // how many of each prototype the map already uses (shown as "xN")
    static std::map<std::string, int> counts;
    static size_t countsFor = (size_t)-1;
    if (countsFor != g_scene.entities.size()) {
        counts.clear();
        for (const Entity& e : g_scene.entities) {
            if (e.proto.empty()) continue;
            std::string g; for (char c : e.proto) g += (char)tolower((unsigned char)c);
            counts[g]++;
        }
        countsFor = g_scene.entities.size();
    }
    ImGui::SetNextItemWidth(-70);
    ImGui::InputTextWithHint("##filter", "search name / model / category", g_browseFilter, sizeof(g_browseFilter));
    ImGui::SameLine(); ImGui::Checkbox("Grid", &g_browseGrid);
    ImGui::Checkbox("Favourites only", &g_browseFavOnly); ImGui::SameLine();
    ImGui::Checkbox("On this map only", &g_browseOnMapOnly);

    std::string needle; for (char* c = g_browseFilter; *c; c++) needle += (char)tolower((unsigned char)*c);
    struct Row { const std::string* guid; const ProtoInfo* info; int count; };
    std::map<std::string, std::vector<Row>> cats;
    int shown = 0;
    for (const auto& kv : g_protoFull) {
        const ProtoInfo& pi = kv.second;
        if (pi.model.empty()) continue;                       // nothing to show or place
        int cnt = counts.count(kv.first) ? counts[kv.first] : 0;
        if (g_browseOnMapOnly && cnt == 0) continue;
        if (g_browseFavOnly && !g_favourites.count(kv.first)) continue;
        std::string cat = categoryForProto(kv.first, pi.schema);
        if (!needle.empty()) {
            std::string hay = pi.name + " " + pi.model + " " + cat + " " + pi.schema;
            for (auto& c : hay) c = (char)tolower((unsigned char)c);
            if (hay.find(needle) == std::string::npos) continue;
        }
        cats[cat].push_back({ &kv.first, &pi, cnt });
        shown++;
    }
    ImGui::TextDisabled("%d of %d prototypes", shown, (int)g_protoFull.size());

    ImGui::BeginChild("protos", ImVec2(0, 340), ImGuiChildFlags_None);
    const float img = 56.0f, cell = img + 18.0f;
    for (auto& c : cats) {
        char hdr[80]; snprintf(hdr, sizeof(hdr), "%s (%d)", c.first.c_str(), (int)c.second.size());
        if (!ImGui::TreeNodeEx(hdr, needle.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) continue;
        int perRow = 1;
        if (g_browseGrid) {
            float availw = ImGui::GetContentRegionAvail().x;
            perRow = (int)(availw / cell); if (perRow < 1) perRow = 1;
        }
        int col = 0;
        for (const Row& r : c.second) {
            const std::string& guid = *r.guid;
            bool sel = (g_placeProto == guid);
            bool fav = g_favourites.count(guid) != 0;
            ImGui::PushID(guid.c_str());
            if (g_browseGrid) {
                unsigned tex = thumbForProto(guid);
                ImGui::BeginGroup();
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                bool clicked;
                if (tex)   // flip V: THMB rows are top-down, ImGui samples bottom-up
                    clicked = ImGui::ImageButton("t", (ImTextureID)tex, ImVec2(img, img),
                                                 ImVec2(0, 1), ImVec2(1, 0));
                else
                    clicked = ImGui::Button("?##noimg", ImVec2(img + 8, img + 8));
                if (sel) ImGui::PopStyleColor();
                if (clicked) g_placeProto = guid;
                char cap[40];
                if (r.count) snprintf(cap, sizeof(cap), "%s%.5s x%d", fav?"*":"", r.info->name.c_str(), r.count);
                else         snprintf(cap, sizeof(cap), "%s%.6s", fav?"*":"", r.info->name.c_str());
                ImGui::TextUnformatted(cap);
                ImGui::EndGroup();
            } else {
                char lbl[192];
                snprintf(lbl, sizeof(lbl), "%s%s  (%s)%s", fav?"* ":"",
                         r.info->name.empty() ? guid.c_str() : r.info->name.c_str(),
                         r.info->schema.c_str(), r.count ? "  [on map]" : "");
                if (ImGui::Selectable(lbl, sel)) g_placeProto = guid;
            }
            if (ImGui::IsItemHovered()) {
                std::string why; int ti = templateForProto(guid, why);
                ImGui::SetTooltip("%s\n%s\n%s\n%s\nused on this map: %d\n%s",
                                  r.info->name.c_str(), r.info->schema.c_str(),
                                  r.info->model.c_str(), guid.c_str(), r.count,
                                  ti >= 0 ? "placeable" : ("NOT placeable: " + why).c_str());
            }
            if (ImGui::IsItemClicked(1)) {           // right-click toggles favourite
                if (fav) g_favourites.erase(guid); else g_favourites.insert(guid);
            }
            ImGui::PopID();
            if (g_browseGrid && ++col % perRow != 0) ImGui::SameLine();
        }
        ImGui::TreePop();
    }
    ImGui::EndChild();

    if (!g_placeProto.empty()) {
        std::string why; int ti = templateForProto(g_placeProto, why);
        auto pi = g_protoFull.find(g_placeProto);
        ImGui::TextWrapped("Selected: %s", pi == g_protoFull.end() ? g_placeProto.c_str()
                                                                  : pi->second.name.c_str());
        if (ti < 0) ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Not placeable here: %s", why.c_str());
        else if (ImGui::Button("Place at view center"))
            placePrototypeAt(g_placeProto, g_cam.target.x, g_cam.target.z);
    }
    ImGui::TextDisabled("With the Place tool, left-click the terrain to drop one.");
    ImGui::TextDisabled("Right-click a tile to favourite it. Ctrl+D dup, Del remove.");
}

// --- Light mode (WTHR) -------------------------------------------------------
// Read-only for now: the format is decoded and every field is fixed-width, so
// editing is a size-preserving in-place write, but the write path + undo land
// with the editable panel. Showing the real values now is what makes the decode
// checkable by eye rather than only by --wthrtest.

// Resolve --preset into an index of g_scene.weather (-1 = leave as-is).
static int presetIndexFromArg() {
    if (g_presetArg.empty()) return -1;
    if (g_presetArg[0] == '#') {
        int slot = atoi(g_presetArg.c_str() + 1);
        for (size_t k = 0; k < g_scene.weather.size(); k++)
            if (g_scene.weather[k].slot == slot) return (int)k;
        return -1;
    }
    for (size_t k = 0; k < g_scene.weather.size(); k++)
        if (g_scene.weather[k].name == g_presetArg) return (int)k;
    return -1;
}

// Push the environment the viewport should shade with. While the Light panel is
// open the SELECTED preset drives the preview -- otherwise editing "Sundown"
// while the viewport renders "Default" makes every colour drag look broken.
// Everywhere else the engine-active preset drives it.
static void syncEnvironment() {
    if (!g_scene.loaded || g_scene.weather.empty() || g_lightMode == 0) {
        g_vp.setEnvironment(nullptr, 0);
        return;
    }
    int forced = presetIndexFromArg();
    int idx = forced >= 0 ? forced
            : (modeIs(MK_LIGHT) && g_lightPreset >= 0 &&
               g_lightPreset < (int)g_scene.weather.size())
              ? g_lightPreset : g_scene.weatherActive;
    if (idx < 0 || idx >= (int)g_scene.weather.size()) { g_vp.setEnvironment(nullptr, 0); return; }
    g_vp.setEnvironment(&g_scene.weather[(size_t)idx], g_lightMode);
}

static void drawLightPanel() {
    if (!g_scene.loaded) { ImGui::TextDisabled("Load a map."); return; }
    if (g_scene.weather.empty()) {
        ImGui::TextWrapped("This map has no readable WTHR pool. Either the chunk is "
                           "absent, or it is the older flat chunk version 2, which "
                           "this decoder refuses rather than mis-strides.");
        if (ImGui::Button("Open the raw chunk inspector")) g_showChunks = true;
        return;
    }
    if (g_lightPreset < 0 || g_lightPreset >= (int)g_scene.weather.size())
        g_lightPreset = g_scene.weatherActive >= 0 ? g_scene.weatherActive : 0;

    ImGui::Text("%d preset%s, %d free of %d slots", (int)g_scene.weather.size(),
                g_scene.weather.size() == 1 ? "" : "s", g_scene.weatherFree, g_scene.weatherCap);
    if (g_scene.weatherActive >= 0)
        ImGui::TextDisabled("engine-active: \"%s\" (slot %d)",
                            g_scene.weather[g_scene.weatherActive].name.c_str(),
                            g_scene.weather[g_scene.weatherActive].slot);
    else
        // Not a parse failure: the engine binds by NAME, and this map has none.
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
                           "No preset is named \"Default\" - the engine binds that name, "
                           "so this map has no active preset.");
    ImGui::SeparatorText("Presets");
    // Listed in list-chain order, which is the authored order, not slot order.
    for (int k = 0; k < (int)g_scene.weather.size(); k++) {
        const WeatherPreset& w = g_scene.weather[(size_t)k];
        char lbl[160];
        snprintf(lbl, sizeof(lbl), "%s%s##wp%d", w.name.c_str(),
                 k == g_scene.weatherActive ? "  *" : "", k);
        if (ImGui::Selectable(lbl, k == g_lightPreset)) g_lightPreset = k;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("pool slot %d", w.slot);
    }

    WeatherPreset& w = g_scene.weather[(size_t)g_lightPreset];
    ImGui::SeparatorText(w.name.c_str());
    ImGui::TextDisabled("edits are in-place and byte-faithful; Ctrl+S writes them");

    // One undo command per interaction, not per frame: snapshot when a widget is
    // grabbed, commit when it is released, matching snapEntity/commitEntity.
    static int   sSlot = -1, sField = -1;
    static std::array<float,4> sBefore{};
    auto beginEdit = [&](int f) {
        if (sField == f && sSlot == w.slot) return;
        sSlot = w.slot; sField = f; sBefore = w.values[(size_t)f];
    };
    auto endEdit = [&](int f) {
        if (sSlot != w.slot || sField != f) return;
        if (w.values[(size_t)f] != sBefore) {
            EditCmd c; c.kind = CMD_WEATHER; c.wSlot = w.slot; c.wField = f;
            c.w0 = sBefore; c.w1 = w.values[(size_t)f];
            pushCmd(std::move(c));
        }
        sSlot = sField = -1;
    };
    auto touched = [&](int f) {
        if (ImGui::IsItemActivated()) beginEdit(f);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            w.dirty[(size_t)f] = 1; g_scene.weatherEdited = true; endEdit(f);
        }
    };
    // kWeatherFields[] is in the engine's STREAM order, in which the groups
    // interleave (Fog, Sun, Colours, Fog, Sky, Colours, Sky, Fog...). Drive the
    // outer loop from a fixed group order and filter, so each heading appears
    // once — without reordering the table, which is the decode's source of truth.
    static const char* kGroups[] = {"Sun", "Colours", "Fog", "Sky", "Effects", "Post", "Unknown"};
    for (const char* group : kGroups) {
    ImGui::SeparatorText(group);
    for (int f = 0; f < kWeatherFieldCount; f++) {
        const WeatherField& F = kWeatherFields[f];
        if (strcmp(F.group, group)) continue;
        float* v = w.values[(size_t)f].data();
        ImGui::PushID(f);
        ImGui::SetNextItemWidth(200.0f);
        switch (F.kind) {
            case WFK_BOOL: {
                bool b = v[0] != 0.0f;
                if (ImGui::Checkbox(F.name, &b)) {
                    beginEdit(f); v[0] = b ? 1.0f : 0.0f;
                    w.dirty[(size_t)f] = 1; g_scene.weatherEdited = true; endEdit(f);
                }
                break;
            }
            case WFK_U32: {
                // EffectCount is the length of the array that follows it; changing
                // it would not resize anything, so it is shown, not edited.
                ImGui::Text("%-17s %u", F.name, (unsigned)v[0]);
                break;
            }
            case WFK_FLOAT: ImGui::DragFloat(F.name, v, 0.01f); touched(f); break;
            case WFK_VEC2:  ImGui::DragFloat2(F.name, v, 0.01f); touched(f); break;
            case WFK_DIR: {
                // Edited as a compass, then renormalized: the engine stores a unit
                // vector (|v| == 1 on 219/219) and a hand-typed triple would not be.
                float el = asinf(std::max(-1.0f, std::min(1.0f, -v[1]))) * 57.2957795f;
                float az = atan2f(v[2], v[0]) * 57.2957795f;
                bool ch = false;
                ImGui::SetNextItemWidth(96.0f);
                ch |= ImGui::DragFloat("azimuth", &az, 0.5f, -180.0f, 180.0f, "%.1f deg");
                if (ImGui::IsItemActivated()) beginEdit(f);
                ImGui::SameLine(); ImGui::SetNextItemWidth(96.0f);
                ch |= ImGui::DragFloat("elevation", &el, 0.5f, -89.9f, 89.9f, "%.1f deg");
                if (ImGui::IsItemActivated()) beginEdit(f);
                if (ch) {
                    float ce = cosf(el / 57.2957795f);
                    v[0] = ce * cosf(az / 57.2957795f);
                    v[1] = -sinf(el / 57.2957795f);
                    v[2] = ce * sinf(az / 57.2957795f);
                    w.dirty[(size_t)f] = 1; g_scene.weatherEdited = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) endEdit(f);
                ImGui::Text("%-17s %.4f, %.4f, %.4f", F.name, v[0], v[1], v[2]);
                break;
            }
            case WFK_RGBA: {
                // HDR + no clamping: SunColor reaches 2.78 in shipped presets, and
                // SunShadow's 4th component is a scalar in 0.25..2.58, not an alpha,
                // so it gets its own drag rather than an alpha slider.
                ImGui::ColorEdit3(F.name, v,
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                    ImGuiColorEditFlags_NoDragDrop | ImGuiColorEditFlags_NoInputs);
                touched(f);
                ImGui::SameLine(); ImGui::SetNextItemWidth(70.0f);
                ImGui::DragFloat("##w", &v[3], 0.01f, 0.0f, 0.0f, "w %.2f");
                touched(f);
                break;
            }
        }
        if (F.note && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", F.note);
        ImGui::PopID();
    }
    }
    // TimeOfTheDay reads as hours; showing the clock catches a mis-decode by eye.
    for (int f = 0; f < kWeatherFieldCount; f++)
        if (!strcmp(kWeatherFields[f].name, "TimeOfTheDay")) {
            float t = w.values[(size_t)f][0];
            ImGui::SeparatorText("");
            ImGui::TextDisabled("time of day  %02d:%02d", (int)t, (int)((t - (int)t) * 60));
        }
}

// --- River mode (GRVL/GRVR) --------------------------------------------------
// Read-only. The geometry is decoded (centreline, per-node width, water level),
// but nothing writes it yet, and the material -> texture mapping is unsolved.
static int g_riverSel = -1;

static void drawRiverPanel() {
    if (!g_scene.loaded) { ImGui::TextDisabled("Load a map."); return; }
    if (g_scene.rivers.empty()) {
        ImGui::TextWrapped("This map has no river splines. 28 of the 45 shipped maps "
                           "carry them; the rest ship an empty GRVL pool.");
        ImGui::TextDisabled("View > Rivers toggles them where they exist.");
        return;
    }
    ImGui::Text("%d river%s", (int)g_scene.rivers.size(),
                g_scene.rivers.size() == 1 ? "" : "s");
    ImGui::TextDisabled("read-only: decoded and drawn, no write path yet");
    ImGui::SeparatorText("Rivers");
    for (int k = 0; k < (int)g_scene.rivers.size(); k++) {
        const Scene::RiverSpline& r = g_scene.rivers[(size_t)k];
        const char* leaf = r.tex.c_str();
        if (const char* sl = strrchr(leaf, '/')) leaf = sl + 1;
        char lbl[192];
        snprintf(lbl, sizeof(lbl), "%d  %s  (%d nodes)##rv%d", k, leaf, (int)r.cx.size(), k);
        if (ImGui::Selectable(lbl, k == g_riverSel)) g_riverSel = k;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.tex.c_str());
    }
    if (g_riverSel < 0 || g_riverSel >= (int)g_scene.rivers.size()) return;
    const Scene::RiverSpline& r = g_scene.rivers[(size_t)g_riverSel];
    ImGui::SeparatorText("Nodes");
    float wmin = r.w.empty() ? 0 : r.w[0], wmax = wmin;
    for (float w : r.w) { wmin = std::min(wmin, w); wmax = std::max(wmax, w); }
    ImGui::Text("water level  %.2f", r.level);
    ImGui::Text("width        %.2f .. %.2f", wmin, wmax);
    ImGui::TextDisabled("pool slot %d", r.srcSlot);
    if (ImGui::BeginTable("rvnodes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("x");
        ImGui::TableSetupColumn("z"); ImGui::TableSetupColumn("width");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < r.cx.size(); i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", (int)i);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.cx[i]);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", r.cz[i]);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", i < r.w.size() ? r.w[i] : 0.0f);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Frame this river") && !r.cx.empty()) {
        float cx = 0, cz = 0;
        for (size_t i = 0; i < r.cx.size(); i++) { cx += r.cx[i]; cz += r.cz[i]; }
        cx /= r.cx.size(); cz /= r.cx.size();
        g_cam.target = V3{ cx, r.level, cz };
        if (g_cam.dist > 200.0f) g_cam.dist = 200.0f;
    }
}

// --- Shader / Decals mode ----------------------------------------------------
// Decals are editable: their five transform floats are fixed-width, so a move is
// 20 in-place bytes and needs no ancestor-size patching. Roads are still
// read-only — dragging a road node also has to re-derive the neighbouring
// Catmull-Rom handles, which is the next piece of work, not this one.

static void drawDecalPanel() {
    if (!g_scene.loaded) { ImGui::TextDisabled("Load a map."); return; }
    if (g_scene.decalRecs.empty()) {
        ImGui::TextWrapped("This map has no decals.");
        return;
    }
    ImGui::Text("%d decals, %d roads", (int)g_scene.decalRecs.size(),
                (int)(g_scene.roadSplines.size() + g_scene.roads.size()));
    if (!g_scene.decalPool.ok)
        ImGui::TextColored(ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
                           "The decal pool did not walk cleanly - read-only on this map.");
    else
        ImGui::TextDisabled("edits are in-place and byte-faithful; Ctrl+S writes them");
    ImGui::TextDisabled("Roads are read-only (node drag needs handle re-derivation).");

    ImGui::SeparatorText("Decals");
    ImGui::BeginChild("##decallist", ImVec2(0, 180), true);
    for (int k = 0; k < (int)g_scene.decalRecs.size(); k++) {
        const Scene::DecalRec& r = g_scene.decalRecs[(size_t)k];
        const char* leaf = r.tex.c_str();
        if (const char* sl = strrchr(leaf, '/')) leaf = sl + 1;
        char lbl[200];
        snprintf(lbl, sizeof(lbl), "%s%s##dc%d", leaf,
                 g_decalTouched.count(r.slot) ? "  *" : "", k);
        if (ImGui::Selectable(lbl, k == g_decalSel)) g_decalSel = k;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("slot %d\n%s\n(%.1f, %.1f)  %.1f x %.1f",
                              r.slot, r.tex.c_str(), r.cx, r.cz, r.sx, r.sy);
    }
    ImGui::EndChild();
    if (g_decalSel < 0 || g_decalSel >= (int)g_scene.decalRecs.size()) return;

    Scene::DecalRec& r = g_scene.decalRecs[(size_t)g_decalSel];
    ImGui::SeparatorText("Transform");
    if (!g_scene.decalPool.ok) {
        ImGui::Text("centre  %.2f, %.2f", r.cx, r.cz);
        ImGui::Text("size    %.2f x %.2f", r.sx, r.sy);
        ImGui::Text("rot     %.1f deg", r.rot * 57.2957795f);
        return;
    }
    // One undo step per interaction: snapshot on grab, commit on release.
    static int   sSlot = -1;
    static std::array<float,5> sBefore{};
    auto cur = [&]{ return std::array<float,5>{ r.cx, r.cz, r.sx, r.sy, r.rot }; };
    auto grab = [&]{ if (sSlot != r.slot) { sSlot = r.slot; sBefore = cur(); } };
    auto write = [&]{
        if (overlay_set_decal(g_scene, r.slot, r.cx, r.cz, r.sx, r.sy, r.rot)) {
            g_overlayDirty = true;
            g_decalTouched.insert(r.slot);
            g_scene.decalEdited = (int)g_decalTouched.size();
        }
    };
    auto release = [&]{
        if (sSlot != r.slot) return;
        std::array<float,5> now = cur();
        if (now != sBefore) {
            EditCmd c; c.kind = CMD_DECAL; c.dSlot = r.slot; c.d0 = sBefore; c.d1 = now;
            pushCmd(std::move(c));
        }
        sSlot = -1;
    };
    auto field = [&](const char* label, float* v, float step) {
        ImGui::SetNextItemWidth(200.0f);
        ImGui::DragFloat(label, v, step);
        if (ImGui::IsItemActivated()) grab();
        if (ImGui::IsItemEdited()) write();
        if (ImGui::IsItemDeactivatedAfterEdit()) release();
    };
    field("centre X", &r.cx, 0.25f);
    field("centre Z", &r.cz, 0.25f);
    field("size X",   &r.sx, 0.25f);
    field("size Y",   &r.sy, 0.25f);
    float deg = r.rot * 57.2957795f;
    ImGui::SetNextItemWidth(200.0f);
    ImGui::DragFloat("rotation", &deg, 1.0f, 0.0f, 0.0f, "%.1f deg");
    if (ImGui::IsItemActivated()) grab();
    if (ImGui::IsItemEdited()) { r.rot = deg / 57.2957795f; write(); }
    if (ImGui::IsItemDeactivatedAfterEdit()) release();
    ImGui::TextDisabled("pool slot %d   %s", r.slot, r.tex.c_str());
    if (ImGui::Button("Frame this decal")) {
        g_cam.target = V3{ r.cx, terrainHeightAt(r.cx, r.cz), r.cz };
        if (g_cam.dist > 60.0f) g_cam.dist = 60.0f;
    }
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
        // Modes whose data the format work has not reached yet get an honest note
        // and a pointer at what IS available, instead of controls that do nothing.
        if (modeIs(MK_LIGHT)) { drawLightPanel(); ImGui::End(); return; }
        if (modeIs(MK_RIVER)) { drawRiverPanel(); ImGui::End(); return; }
        if (modeIs(MK_OVERLAY)) { drawDecalPanel(); ImGui::End(); return; }
        if (!modeIs(MK_TERRAIN) && !modeIs(MK_OBJECT)) {
            const char* why = nullptr;
            bool retired = false;
            switch (modeKind()) {
                case MK_TRIGGER: why = "Triggers hold Lua bodies; the trigger system is not decoded yet."; break;
                case MK_RETIRED:
                    // Not "undecoded" — measured absent. Say which, so nobody
                    // spends a session looking for a chunk that is not there.
                    why = "Retired: no lake or water data exists in any of the 45 CPCW maps — "
                          "no schema declares it, and GRVL turned out to hold river splines, "
                          "not water. This is a Gepard-1 (S.W.I.N.E.) feature the engine dropped.";
                    retired = true; break;
                default: why = "Not implemented yet."; break;
            }
            ImGui::TextWrapped("%s", why);
            ImGui::Spacing();
            if (!retired) {
                if (ImGui::Button("Open the raw chunk inspector")) g_showChunks = true;
                ImGui::TextDisabled("Inspect the bytes rather than guess at fields.");
            }
            ImGui::End();
            return;
        }
        if (m.focus[0] == 't') {
            ImGui::SliderFloat("Size", &g_brushSize, 0.5f, 8.0f);
            float rad = g_brushSize * 4.0f;
            ImGui::TextDisabled("diameter %.0f world units  (radius %.1f)", rad * 2.0f, rad);
            ImGui::SliderFloat("Height", &g_brushHeight, -50.0f, 50.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Target altitude for SetPlane / Raise>Plane / Lower>Plane.");
            ImGui::SliderFloat("Pressure", &g_brushPress, 0.0f, 1.0f);
            if (modeIs(MK_TERRAIN)) {
                if (toolIsHeight(g_activeTool))
                    ImGui::TextDisabled("Hold Ctrl to invert raise/lower.");
                if (g_activeTool == TT_GRAB)
                    ImGui::TextDisabled("Grab: drag up/down to pull the surface.");
            }
            // Texture-blend layer picker (Blend / TileFill paint the chosen layer)
            if (modeIs(MK_TERRAIN) && !g_scene.terrainLayers.empty()) {
                ImGui::SeparatorText("Terrain layers");
                if (g_scene.splatOff < 0)
                    ImGui::TextColored(ImVec4(1,0.5f,0.3f,1),
                        "No splat grids located in this map — painting is disabled.");
                for (int i = 0; i < (int)g_scene.terrainLayers.size(); i++) {
                    const Scene::TerrainLayer& L = g_scene.terrainLayers[i];
                    if (!L.active) continue;
                    const char* leaf = L.path.c_str();
                    if (const char* sl = strrchr(leaf, '/')) leaf = sl + 1;
                    char lbl[160];
                    snprintf(lbl, sizeof(lbl), "%d  %s%s##L%d", i, leaf,
                             i == 0 ? "   (base)" : "", i);
                    if (ImGui::Selectable(lbl, g_paintLayer == i)) g_paintLayer = i;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.path.c_str());
                }
                if (toolIsPaint(g_activeTool)) {
                    if (g_paintLayer < 0) ImGui::TextDisabled("Pick a layer to paint.");
                    else ImGui::TextDisabled("Ctrl-drag erases the layer instead.");
                }
            }
        } else {
            drawPrototypeBrowser();
        }
    }
    ImGui::End();
}
// Everything that differs from what is on disk, by category, with a jump-to.
// Cheap to build (one pass over the entity list) and it answers the question the
// byte-faithful save model makes you ask constantly: "what am I about to write?"
static void drawChanges() {
    if (!g_showChanges) return;
    int added = 0, moved = 0, rotated = 0, fielded = 0;
    struct Row { int idx; int flags; };
    std::vector<Row> rows;
    std::set<long> live;
    for (int i = 0; i < (int)g_scene.entities.size(); i++) {
        const Entity& e = g_scene.entities[i];
        live.insert(e.id);
        int f = diffFlags(e);
        if (!f) continue;
        if (f & DIFF_ADDED)   added++;
        if (f & DIFF_MOVED)   moved++;
        if (f & DIFF_ROTATED) rotated++;
        if (f & DIFF_FIELD)   fielded++;
        rows.push_back({ i, f });
    }
    int deleted = 0;
    for (const auto& kv : g_savedState) if (!live.count(kv.first)) deleted++;
    // Non-entity edits have no row in the list, so count them separately or an
    // unsaved change is invisible in the one panel meant to show unsaved changes.
    int wFields = 0, wPresets = 0;
    for (const WeatherPreset& w : g_scene.weather) {
        int n = 0;
        for (unsigned char d : w.dirty) n += d ? 1 : 0;
        if (n) { wFields += n; wPresets++; }
    }

    char title[96];
    snprintf(title, sizeof(title), "Changes (%d)###changes",
             (int)rows.size() + deleted + (wFields ? 1 : 0) +
             (g_decalTouched.empty() ? 0 : 1));
    if (ImGui::Begin(title, &g_showChanges)) {
        if (g_srcMap.empty()) ImGui::TextDisabled("No .map loaded.");
        ImGui::Text("+%d added   -%d deleted   %d moved   %d rotated   %d field edits",
                    added, deleted, moved, rotated, fielded);
        if (g_scene.terrainEdited) ImGui::Text("terrain heights edited");
        if (g_scene.splatEdited)   ImGui::Text("terrain paint edited");
        if (wFields)
            ImGui::Text("%d weather field%s edited across %d preset%s",
                        wFields, wFields == 1 ? "" : "s", wPresets, wPresets == 1 ? "" : "s");
        if (!g_decalTouched.empty())
            ImGui::Text("%d decal%s moved", (int)g_decalTouched.size(),
                        g_decalTouched.size() == 1 ? "" : "s");
        ImGui::TextDisabled("versus %s", g_srcMap.empty() ? "(nothing)" : g_srcMap.c_str());
        ImGui::Separator();
        ImGuiListClipper clip; clip.Begin((int)rows.size());
        while (clip.Step())
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const Entity& e = g_scene.entities[rows[r].idx];
                int f = rows[r].flags;
                char tag[40] = "";
                snprintf(tag, sizeof(tag), "%s%s%s%s",
                         (f & DIFF_ADDED)   ? "new "   : "",
                         (f & DIFF_MOVED)   ? "moved " : "",
                         (f & DIFF_ROTATED) ? "rot "   : "",
                         (f & DIFF_FIELD)   ? "field " : "");
                char lbl[200];
                snprintf(lbl, sizeof(lbl), "%ld  %s  [%s]##c%d", e.id, e.type.c_str(), tag, r);
                if (ImGui::Selectable(lbl, g_selection.count(rows[r].idx) != 0)) {
                    selectOnly(rows[r].idx);
                    g_cam.target = V3{ e.pos[0], e.pos[2], e.pos[1] };   // jump to it
                    if (g_cam.dist > 120.0f) g_cam.dist = 120.0f;
                }
            }
    }
    ImGui::End();
}

// Read-only inspector over the raw chunk tree. The Lake/Water and Light modes have
// nothing to edit yet because GTRD water and the WTHR weather/lighting block are
// still undecoded (docs/MAP_FORMAT.md) — so show what is actually there, with a hex
// preview, instead of a panel of invented fields. This is the decode-first step.
static void drawChunkInspector() {
    if (!g_showChunks) return;
    static std::vector<ChunkNode> nodes;
    static size_t builtFor = (size_t)-1;
    static int sel = -1;
    if (builtFor != g_scene.raw.size()) {
        nodes.clear(); sel = -1;
        if (!g_scene.raw.empty()) map_chunk_outline(g_scene.raw, nodes);
        builtFor = g_scene.raw.size();
    }
    if (ImGui::Begin("Map chunks", &g_showChunks)) {
        if (nodes.empty()) ImGui::TextDisabled("No .map loaded.");
        ImGui::TextDisabled("Raw SCEN tree. WTHR (weather/light), CAMS and the water "
                            "data are not decoded yet — inspect, don't guess.");
        ImGui::Separator();
        ImGui::BeginChild("tree", ImVec2(0, 240));
        for (int i = 0; i < (int)nodes.size(); i++) {
            const ChunkNode& n = nodes[i];
            char lbl[160];
            snprintf(lbl, sizeof(lbl), "%*s%s   @%ld  %ld bytes##ck%d",
                     n.depth * 2, "", n.tag.c_str(), n.offset, n.size, i);
            if (ImGui::Selectable(lbl, sel == i)) sel = i;
        }
        ImGui::EndChild();
        if (sel >= 0 && sel < (int)nodes.size() && !g_scene.raw.empty()) {
            const ChunkNode& n = nodes[sel];
            ImGui::SeparatorText(n.tag.c_str());
            long start = n.offset + 8;
            long len = n.size < 256 ? n.size : 256;
            ImGui::TextDisabled("first %ld bytes of payload", len);
            ImGui::BeginChild("hex", ImVec2(0, 160), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar);
            for (long r = 0; r < len; r += 16) {
                char line[128]; int p = snprintf(line, sizeof(line), "%06lX  ", start + r);
                for (long c = 0; c < 16 && r + c < len; c++) {
                    long o = start + r + c;
                    if (o >= 0 && o < (long)g_scene.raw.size())
                        p += snprintf(line + p, sizeof(line) - p, "%02X ", g_scene.raw[o]);
                }
                ImGui::TextUnformatted(line);
            }
            ImGui::EndChild();
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
// Every schema field the record declares, typed and editable in place. The parser
// already decodes name/type/offset for all of them; this just surfaces them.
// Pos/Dir/Player/Scale are drawn separately above (the viewport mirrors those), and
// strings stay read-only because changing their length would resize the record.
static void drawSchemaFields(Entity& e) {
    if (e.fields.empty()) return;
    if (!ImGui::CollapsingHeader("All schema fields")) return;
    ImGui::PushID("schema");
    for (size_t k = 0; k < e.fields.size(); k++) {
        EntityField& f = e.fields[k];
        if (f.mirrored) continue;
        ImGui::PushID((int)k);
        bool changed = false;
        if (f.kind == FK_STR) {
            ImGui::LabelText(f.name.c_str(), "%s", f.s.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("strings are read-only (editing would resize the record)");
        } else if (!field_is_writable(f.ftype)) {
            ImGui::TextDisabled("%s  (type 0x%04X, not editable)", f.name.c_str(), f.ftype);
        } else if (f.kind == FK_VEC3) {
            float v[3] = { f.v3[0], f.v3[1], f.v3[2] };
            if (ImGui::DragFloat3(f.name.c_str(), v, 0.05f)) {
                for (int j = 0; j < 3; j++) f.v3[j] = v[j];
                changed = true;
            }
        } else if (f.kind == FK_FLOAT) {
            float v = (float)f.f;
            if (ImGui::DragFloat(f.name.c_str(), &v, 0.05f)) { f.f = v; changed = true; }
        } else if (f.ftype == 0x0003) {                    // FT_BOOL
            bool v = f.i != 0;
            if (ImGui::Checkbox(f.name.c_str(), &v)) { f.i = v ? 1 : 0; changed = true; }
        } else {
            int v = (int)f.i;
            if (ImGui::InputInt(f.name.c_str(), &v)) { f.i = v; changed = true; }
        }
        if (changed) { f.dirty = true; g_edited.insert(e.id); }
        ImGui::PopID();
    }
    ImGui::PopID();
}

static void drawProperties() {
    if (!g_showProps) return;
    if (ImGui::Begin("Properties", &g_showProps)) {
        if (g_selection.size() > 1)
            ImGui::TextDisabled("%d selected — editing the primary below.", (int)g_selection.size());
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
            if (e.scaleOff >= 0) {
                float sc = e.scale;
                if (ImGui::DragFloat("Scale", &sc, 0.01f, 0.05f, 20.0f)) {
                    e.scale = sc; g_edited.insert(e.id); g_modelsDirty = true;
                }
            }
            if (g_edited.count(e.id)) ImGui::TextDisabled("(edited — File > Save edits)");
            ImGui::Separator();
            drawSchemaFields(e);
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

// Sculpt the heightmap around (cx,cy) with a radial falloff. `invert` (Ctrl) flips
// raise<->lower and raise-to-plane<->lower-to-plane. `drag` carries the vertical
// mouse motion for the Grab tool. The "plane" tools all work against the panel's
// Height slider, which is what makes it a target altitude rather than dead UI.
static void applyTerrainBrush(float cx, float cy, int tool, bool invert, float drag) {
    int W = g_scene.grid_w, H = g_scene.grid_h;
    if (W < 2 || H < 2 || (int)g_scene.heights.size() != W * H) return;
    float radius = g_brushSize * 4.0f;
    float strength = (g_brushPress * 1.8f + 0.2f);
    const float plane = g_brushHeight;
    if (invert) {
        if (tool == TT_RAISE) tool = TT_LOWER; else if (tool == TT_LOWER) tool = TT_RAISE;
        else if (tool == TT_RAISE_TO_PLANE) tool = TT_LOWER_TO_PLANE;
        else if (tool == TT_LOWER_TO_PLANE) tool = TT_RAISE_TO_PLANE;
    }
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
        switch (tool) {
            case TT_GRAB:  h[gi] += drag * w; break;
            case TT_RAISE: h[gi] += strength * w; break;
            case TT_LOWER: h[gi] -= strength * w; break;
            case TT_SETPLANE:                       // ease toward the target altitude
                h[gi] += (plane - h[gi]) * w * (g_brushPress * 0.9f + 0.1f);
                break;
            case TT_RAISE_TO_PLANE:                 // raise, but never past the plane
                if (h[gi] < plane) h[gi] = std::min(plane, h[gi] + strength * w);
                break;
            case TT_LOWER_TO_PLANE:                 // lower, but never past the plane
                if (h[gi] > plane) h[gi] = std::max(plane, h[gi] - strength * w);
                break;
            default: {                              // Smooth
                float a = (h[(size_t)j*W + std::max(0,i-1)] + h[(size_t)j*W + std::min(W-1,i+1)] +
                           h[(size_t)std::max(0,j-1)*W + i] + h[(size_t)std::min(H-1,j+1)*W + i]) * 0.25f;
                h[gi] += (a - h[gi]) * w * 0.5f;
                break;
            }
        }
    }
    g_scene.terrainEdited = true; g_terrainDirty = true;
}

// --- splat layer painting ----------------------------------------------------
// GTRD stores one uint8 opacity grid per layer, composited in file order over the
// first active layer. Painting layer L therefore means raising L's own opacity AND
// clearing the layers drawn ON TOP of it, or the paint stays hidden. All of it is
// size-preserving (the grids are fixed-size), so the save stays byte-faithful.

static void ensureSplatDirty() {
    size_t n = (size_t)g_scene.grid_w * g_scene.grid_h;
    if (g_scene.splatDirty.size() != g_scene.splatWeights.size()) {
        g_scene.splatDirty.assign(g_scene.splatWeights.size(), std::vector<unsigned char>());
        for (auto& d : g_scene.splatDirty) d.assign(n, 0);
    }
}
static void applySplatBrush(float cx, float cy, bool fill, bool erase) {
    int W = g_scene.grid_w, H = g_scene.grid_h;
    if (W < 2 || H < 2) return;
    if (g_paintLayer < 0 || g_paintLayer >= (int)g_scene.splatWeights.size()) return;
    if (g_scene.splatOff < 0) return;                       // nowhere to write it back
    ensureSplatDirty();
    float radius = g_brushSize * 4.0f;
    float rate = fill ? 1.0f : (g_brushPress * 0.5f + 0.05f);
    int i0 = std::max(0, (int)(cx - radius)), i1 = std::min(W - 1, (int)(cx + radius));
    int j0 = std::max(0, (int)(cy - radius)), j1 = std::min(H - 1, (int)(cy + radius));
    for (int j = j0; j <= j1; j++) for (int i = i0; i <= i1; i++) {
        float dx = i - cx, dy = j - cy, r = std::sqrt(dx*dx + dy*dy);
        if (r > radius) continue;
        float w = fill ? 1.0f : (1.0f - r / radius);
        size_t gi = (size_t)j * W + i;
        auto blend = [&](int layer, float target) {
            if (layer < 0 || layer >= (int)g_scene.splatWeights.size()) return;
            auto& g = g_scene.splatWeights[layer];
            if (gi >= g.size()) return;
            float cur = g[gi] / 255.0f;
            float nv = cur + (target - cur) * w * rate;
            unsigned char b = (unsigned char)(std::min(1.0f, std::max(0.0f, nv)) * 255.0f + 0.5f);
            if (b != g[gi]) { g[gi] = b; g_scene.splatDirty[layer][gi] = 1; }
        };
        blend(g_paintLayer, erase ? 0.0f : 1.0f);
        if (!erase)                                  // uncover it: clear what is on top
            for (int L = g_paintLayer + 1; L < (int)g_scene.splatWeights.size(); L++)
                if (g_scene.terrainLayers.size() > (size_t)L && g_scene.terrainLayers[L].active)
                    blend(L, 0.0f);
    }
    g_scene.splatEdited = true; g_splatTexDirty = true;
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

// --- selection overlay -------------------------------------------------------
// Ported from the S.W.I.N.E. editor, which draws its selection markers the same
// way and does not have this editor's occlusion problem.
//
// The boxes used to be GL line geometry submitted mid-scene with depth-test off.
// Depth-test off disables depth WRITES too, so every later pass — terrain, road
// and decal overlays — painted straight over them; a box only survived where an
// already-drawn model happened to sit behind it. Drawing them as 2D overlays
// after the entire scene removes the ordering question rather than tuning it.
//
// Foreground draw list, clipped to the central dock node. The background list
// would be the tidier choice — under the panels automatically — but it renders
// beneath the dockspace host window and never appears (verified: geometry and
// projection were correct, the lines simply were not visible). ImGuizmo already
// uses the foreground list for the same reason. The explicit clip rect gives
// back what the background list would have done for free: nothing can spill
// outside the viewport onto the docked panels.

static bool projectToView(const M4& vp, const V3& w, const ImVec2& cmin,
                          float W, float H, ImVec2& out) {
    float cx = vp.m[0]*w.x + vp.m[4]*w.y + vp.m[8]*w.z + vp.m[12];
    float cy = vp.m[1]*w.x + vp.m[5]*w.y + vp.m[9]*w.z + vp.m[13];
    float cw = vp.m[3]*w.x + vp.m[7]*w.y + vp.m[11]*w.z + vp.m[15];
    if (cw <= 0.001f) return false;                 // behind the eye
    out = ImVec2(cmin.x + (cx/cw*0.5f + 0.5f) * W,
                 cmin.y + (1.0f - (cy/cw*0.5f + 0.5f)) * H);
    return true;
}

static void drawSelectionOverlay(const ImVec2& cmin, const ImVec2& cmax) {
    if (!g_scene.loaded) return;
    float W = cmax.x - cmin.x, H = cmax.y - cmin.y;
    if (W <= 8 || H <= 8) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->PushClipRect(cmin, cmax, true);
    static int dbg = getenv("MAPEDITOR_DEBUG_SEL") ? 1 : 0;
    M4 vp = g_cam.viewProj(W / H);

    // Primary selection is yellow — it is what Properties and the gizmo act on;
    // the rest of a multi-selection is orange, hover is cyan.
    struct Mark { int idx; ImU32 col; bool handles; };
    std::vector<Mark> marks;
    for (int si : g_selection)
        if (si != g_selected) marks.push_back({ si, IM_COL32(242, 140, 30, 220), false });
    if (g_hovered >= 0 && g_hovered != g_selected && !g_selection.count(g_hovered))
        marks.push_back({ g_hovered, IM_COL32(77, 230, 255, 200), false });
    if (g_selected >= 0) marks.push_back({ g_selected, IM_COL32(255, 210, 40, 235), true });

    const ImU32 kHandle = IM_COL32(255, 255, 255, 255);
    const ImU32 kEdge   = IM_COL32(0, 0, 0, 200);

    for (const Mark& mk : marks) {
        if (mk.idx < 0 || mk.idx >= (int)g_scene.entities.size()) continue;
        const Entity& e = g_scene.entities[mk.idx];
        if (!g_showKind[(e.kind >= 0 && e.kind < 3) ? e.kind : 2]) continue;

        // Scene stores Z-up; the viewport is Y-up. Same swap pickEntity uses.
        V3 org{ e.pos[0], e.pos[2], e.pos[1] };

        V3 lo, hi;
        bool hasBox = g_vp.entityWorldAABB(mk.idx, lo, hi);
        if (dbg) {
            ImVec2 dp; bool dok = projectToView(vp, org, cmin, W, H, dp);
            fprintf(stderr, "[sel] idx=%d kind=%d aabb=%d org=(%.1f,%.1f,%.1f) "
                    "proj=%d (%.0f,%.0f) rect=(%.0f,%.0f)-(%.0f,%.0f)\n",
                    mk.idx, e.kind, (int)hasBox, org.x, org.y, org.z,
                    (int)dok, dp.x, dp.y, cmin.x, cmin.y, cmax.x, cmax.y);
        }
        if (!hasBox) {
            // No geometry (effects, emitters) — mark the spot so a model-less
            // entity is still visibly selected. The old GL path skipped these
            // entirely because it iterated model instances.
            const float r = 1.0f;
            lo = V3{ org.x - r, org.y,            org.z - r };
            hi = V3{ org.x + r, org.y + r * 2.0f, org.z + r };
        }

        // 8 corners, ground four first, then 12 edges. Box edges are straight in
        // world space so projecting the endpoints is exact — no need to subdivide.
        const V3 c[8] = {
            {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,lo.y,hi.z},{lo.x,lo.y,hi.z},
            {lo.x,hi.y,lo.z},{hi.x,hi.y,lo.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}
        };
        ImVec2 pt[8]; bool ok[8];
        for (int i = 0; i < 8; i++) ok[i] = projectToView(vp, c[i], cmin, W, H, pt[i]);

        static const int E[12][2] = { {0,1},{1,2},{2,3},{3,0},
                                      {4,5},{5,6},{6,7},{7,4},
                                      {0,4},{1,5},{2,6},{3,7} };
        for (int i = 0; i < 12; i++)
            if (ok[E[i][0]] && ok[E[i][1]])
                dl->AddLine(pt[E[i][0]], pt[E[i][1]], mk.col, mk.handles ? 2.0f : 1.5f);

        if (!mk.handles) continue;

        // Handles are a fixed pixel size, so on a distant or small object the
        // four of them merge into an illegible blob that is larger than the box
        // itself. Below that threshold the outline alone reads better.
        float sx0 = pt[0].x, sx1 = pt[0].x, sy0 = pt[0].y, sy1 = pt[0].y;
        for (int i = 1; i < 8; i++) {
            if (!ok[i]) continue;
            sx0 = std::min(sx0, pt[i].x); sx1 = std::max(sx1, pt[i].x);
            sy0 = std::min(sy0, pt[i].y); sy1 = std::max(sy1, pt[i].y);
        }
        if (sx1 - sx0 < 26.0f && sy1 - sy0 < 26.0f) continue;

        // Control points on the ground corners of the primary selection only —
        // on every box in a large multi-selection they become noise.
        for (int i = 0; i < 4; i++) {
            if (!ok[i]) continue;
            dl->AddRectFilled(ImVec2(pt[i].x-4, pt[i].y-4), ImVec2(pt[i].x+4, pt[i].y+4), kHandle);
            dl->AddRect      (ImVec2(pt[i].x-4, pt[i].y-4), ImVec2(pt[i].x+4, pt[i].y+4), kEdge);
        }
        // Anchor dot at the entity's own origin, not the box centre — the origin
        // is what Pos stores and what rotation pivots around.
        ImVec2 base;
        if (projectToView(vp, org, cmin, W, H, base)) {
            dl->AddCircleFilled(base, 4.0f, kHandle);
            dl->AddCircle(base, 4.0f, kEdge);
        }
    }
    dl->PopClipRect();
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
    // Terrain mode + a sculpt or paint tool => left-drag brushes the terrain
    // instead of selecting entities.
    bool brushing = (modeIs(MK_TERRAIN) && (toolIsHeight(g_activeTool) || toolIsPaint(g_activeTool)));
    // Brush cursor ring: show the terrain area the brush covers whenever hovering
    // in Vertex/Terrain mode (any tool), matching applyTerrainBrush's radius.
    {
        float gx, gy;
        if (over && modeIs(MK_TERRAIN) && g_scene.loaded && terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
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
        float gx, gy;
        if (terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            if (toolIsPaint(g_activeTool)) {
                // Blend eases the active layer in; TileFill slams it to full opacity.
                // Ctrl erases instead of painting.
                applySplatBrush(gx, gy, g_activeTool == TT_TILEFILL, io.KeyCtrl);
            } else {
                if (!g_strokeActive) { g_strokeH0 = g_scene.heights; g_strokeActive = true; }
                // Grab drags the surface with vertical mouse motion; the others use
                // the panel's strength/height. Ctrl inverts raise<->lower.
                float drag = -io.MouseDelta.y * 0.25f;
                applyTerrainBrush(gx, gy, g_activeTool, io.KeyCtrl, drag);
            }
        }
        return;   // don't pick/move entities while brushing
    }
    // Place-on-click: with a browser prototype selected and the Place tool active
    // (Object/Unit/Ambient), left-click on terrain drops a grounded instance there.
    bool placing = g_scene.loaded && !g_placeProto.empty() && activeToolIsPlace();
    // Ghost preview: show WHERE and at WHAT ANGLE the click will drop the model,
    // before committing to it. Render-only — see Viewport3D::setGhost for why it
    // is deliberately not a scene instance.
    {
        float gx, gy;
        if (over && placing && terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            if (g_snapOn && g_snapGrid > 0.0f) {     // land where the gizmo would
                gx = std::round(gx / g_snapGrid) * g_snapGrid;
                gy = std::round(gy / g_snapGrid) * g_snapGrid;
            }
            std::string mp = modelPathForProto(g_placeProto);
            V3 wp{ gx, terrainHeightAt(gx, gy), gy };
            g_vp.setGhost(mp, g_dataRoot, wp, g_placeYaw, 1.0f);
        } else {
            g_vp.clearGhost();
        }
    }
    if (over && placing && ImGui::IsMouseClicked(0)) {
        float gx, gy;
        if (terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
            if (g_snapOn && g_snapGrid > 0.0f) {
                gx = std::round(gx / g_snapGrid) * g_snapGrid;
                gy = std::round(gy / g_snapGrid) * g_snapGrid;
            }
            placePrototypeAt(g_placeProto, gx, gy, g_placeYaw);
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

// dev: paint a splat cell, save, reload; only that byte may change, and the layer
// grids must be located where we think they are.
static int splatTest(const char* mapPath, const char* outPath) {
    Scene s;
    if (!load_map_native(mapPath, s)) { printf("splattest: load failed\n"); return 2; }
    if (s.splatOff < 0 || s.splatWeights.empty()) { printf("splattest: no splat grids\n"); return 2; }
    size_t need = (size_t)s.grid_w * s.grid_h;
    // pick an overlay layer (not the base) with a full-size grid
    int layer = -1;
    for (size_t L = 1; L < s.splatWeights.size(); L++)
        if (s.splatWeights[L].size() == need &&
            L < s.terrainLayers.size() && s.terrainLayers[L].active) { layer = (int)L; break; }
    if (layer < 0) { printf("splattest: no paintable overlay layer\n"); return 2; }
    size_t cell = need / 2;
    unsigned char before = s.splatWeights[layer][cell];
    unsigned char want = (unsigned char)(before ^ 0xFF);
    long expectOff = s.splatOff + (long)(layer * need + cell);
    printf("splattest layer %d '%s' cell %zu  %u -> %u  (offset %ld)\n",
           layer, s.terrainLayers[layer].path.c_str(), cell, before, want, expectOff);

    s.splatDirty.assign(s.splatWeights.size(), std::vector<unsigned char>());
    for (auto& d : s.splatDirty) d.assign(need, 0);
    s.splatWeights[layer][cell] = want;
    s.splatDirty[layer][cell] = 1;
    s.splatEdited = true;

    std::vector<unsigned char> bytes = s.raw;
    apply_edits_inplace(s, {}, bytes);
    int diff = 0, outside = 0;
    for (size_t i = 0; i < bytes.size(); i++)
        if (bytes[i] != s.raw[i]) { diff++; if ((long)i != expectOff) outside++; }
    { std::ofstream f(outPath, std::ios::binary); f.write((const char*)bytes.data(), (std::streamsize)bytes.size()); }
    Scene s2;
    if (!load_map_native(outPath, s2)) { printf("splattest: reload failed\n"); return 3; }
    unsigned char got = (layer < (int)s2.splatWeights.size() && cell < s2.splatWeights[layer].size())
                      ? s2.splatWeights[layer][cell] : 0;
    bool ok = diff == 1 && outside == 0 && got == want &&
              s2.raw.size() == s.raw.size() && s2.splatOff == s.splatOff;
    printf("splattest bytesChanged=%d outsideCell=%d readback=%u %s\n",
           diff, outside, got, ok ? "OK" : "FAIL");
    return ok ? 0 : 3;
}

// dev: place a prototype the map does NOT already use, by cloning a record of the
// matching entity schema and swapping the Prototype GUID. Verifies the swap is
// size-preserving, the reparse is clean, and the new entity reads back with the
// requested prototype.
static int protoPlaceTest(const char* mapPath, const char* protoDbPath, const char* outPath) {
    Scene s;
    if (!load_map_native(mapPath, s)) { printf("protoplacetest: load failed\n"); return 2; }
    auto db = protodb_full_index(protoDbPath);
    if (db.empty()) { printf("protoplacetest: ProtoDB empty (%s)\n", protoDbPath); return 2; }
    std::set<std::string> onMap;
    for (const auto& e : s.entities) {
        std::string g; for (char c : e.proto) g += (char)tolower((unsigned char)c);
        if (!g.empty()) onMap.insert(g);
    }
    // pick a prototype absent from the map whose schema IS represented on it
    std::string guid, wantType; long srcId = -1;
    for (const auto& kv : db) {
        if (kv.second.model.empty() || onMap.count(kv.first)) continue;
        std::string want = protodb_map_schema(kv.second.schema);
        if (want.empty()) continue;
        for (const auto& e : s.entities)
            if (e.type == want && e.protoOff >= 0 && e.idOff >= 0 && e.posOff >= 0) {
                guid = kv.first; wantType = want; srcId = e.id; break;
            }
        if (srcId >= 0) break;
    }
    if (srcId < 0) { printf("protoplacetest: no absent prototype with a template\n"); return 2; }
    printf("protoplacetest placing %s (%s -> %s) using template entity %ld\n",
           guid.c_str(), db[guid].schema.c_str(), wantType.c_str(), srcId);

    size_t n0 = s.entities.size(), rawBefore = s.raw.size();
    long newId = 1; for (const auto& e : s.entities) if (e.id >= newId) newId = e.id + 1;
    float p[3] = { 100.0f, 100.0f, 0.0f };
    if (!add_entity_bytes(s, srcId, p, newId, nullptr, guid)) {
        printf("protoplacetest: add_entity_bytes refused\n"); return 3;
    }
    // the source record's size must be exactly what the file grew by (GUID swap is
    // size-preserving), and the new entity must read back with the new prototype
    long grew = (long)s.raw.size() - (long)rawBefore;
    { std::ofstream f(outPath, std::ios::binary); f.write((const char*)s.raw.data(), (std::streamsize)s.raw.size()); }
    Scene s2;
    if (!load_map_native(outPath, s2)) { printf("protoplacetest: reparse failed\n"); return 3; }
    std::string got, gotType;
    for (const auto& e : s2.entities) if (e.id == newId) {
        for (char c : e.proto) got += (char)tolower((unsigned char)c);
        gotType = e.type;
    }
    bool ok = s2.entities.size() == n0 + 1 && got == guid && gotType == wantType && grew > 0;
    printf("protoplacetest ents %zu->%zu grew=%ld proto=%s type=%s %s\n",
           n0, s2.entities.size(), grew, got == guid ? "match" : got.c_str(),
           gotType.c_str(), ok ? "OK" : "FAIL");
    return ok ? 0 : 3;
}

// dev: schema-field editing — pick a writable non-mirrored field on one entity,
// change it, save, reload, and confirm exactly that field changed and nothing else.
static int fieldTest(const char* mapPath, const char* outPath) {
    Scene s;
    if (!load_map_native(mapPath, s)) { printf("fieldtest: load failed\n"); return 2; }
    int ei = -1, fi = -1;
    for (size_t i = 0; i < s.entities.size() && ei < 0; i++)
        for (size_t k = 0; k < s.entities[i].fields.size(); k++) {
            const EntityField& f = s.entities[i].fields[k];
            if (f.name == "ID") continue;   // changing the key would break the readback
            if (!f.mirrored && f.kind == FK_INT && field_is_writable(f.ftype)) {
                ei = (int)i; fi = (int)k; break;
            }
        }
    if (ei < 0) { printf("fieldtest: no writable int field found\n"); return 2; }
    Entity& e = s.entities[ei];
    EntityField& f = e.fields[fi];
    long before = f.i, want = before + 7;
    printf("fieldtest entity %ld field '%s' (type 0x%04X) %ld -> %ld\n",
           e.id, f.name.c_str(), f.ftype, before, want);
    f.i = want; f.dirty = true;

    std::vector<unsigned char> bytes = s.raw;
    apply_edits_inplace(s, {e.id}, bytes);
    // exactly the field's own bytes may differ
    int diff = 0, outside = 0;
    for (size_t i = 0; i < bytes.size(); i++)
        if (bytes[i] != s.raw[i]) { diff++; if ((long)i < f.off || (long)i >= f.off + 4) outside++; }
    { std::ofstream o(outPath, std::ios::binary); o.write((const char*)bytes.data(), (std::streamsize)bytes.size()); }
    Scene s2;
    if (!load_map_native(outPath, s2)) { printf("fieldtest: reload failed\n"); return 3; }
    long got = -12345;
    for (const auto& e2 : s2.entities) if (e2.id == e.id)
        for (const auto& f2 : e2.fields) if (f2.name == f.name) got = f2.i;
    bool ok = (got == want) && outside == 0 && diff > 0 &&
              s2.entities.size() == s.entities.size();
    printf("fieldtest bytesChanged=%d outsideField=%d readback=%ld %s\n",
           diff, outside, got, ok ? "OK" : "FAIL");
    return ok ? 0 : 3;
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

// --- settings persistence ----------------------------------------------------
static void loadSettings(const std::string& pathOverride = std::string()) {
    g_settingsPath = pathOverride.empty() ? settings_default_path() : pathOverride;
    g_settings.load(g_settingsPath);
    const Settings& S = g_settings;
    g_showModes    = S.getBool("panel.modes",    g_showModes);
    g_showPanel    = S.getBool("panel.tools",    g_showPanel);
    g_showProps    = S.getBool("panel.props",    g_showProps);
    g_showEntities = S.getBool("panel.entities", g_showEntities);
    g_showChanges  = S.getBool("panel.changes",  g_showChanges);
    g_showChunks   = S.getBool("panel.chunks",   g_showChunks);
    g_gizmoOn      = S.getBool("gizmo.on",     g_gizmoOn);
    g_gizmoOp      = S.getInt ("gizmo.op",     g_gizmoOp);
    g_gizmoLocal   = S.getBool("gizmo.local",  g_gizmoLocal);
    g_snapOn       = S.getBool("snap.on",      g_snapOn);
    g_snapGrid     = S.getFloat("snap.grid",   g_snapGrid);
    g_snapAngle    = S.getFloat("snap.angle",  g_snapAngle);
    g_brushSize    = S.getFloat("brush.size",     g_brushSize);
    g_brushHeight  = S.getFloat("brush.height",   g_brushHeight);
    g_brushPress   = S.getFloat("brush.pressure", g_brushPress);
    g_showModels   = S.getBool("view.models",   g_showModels);
    g_showDots     = S.getBool("view.dots",     g_showDots);
    g_wireframe    = S.getBool("view.wireframe", g_wireframe);
    for (int k = 0; k < 3; k++) {
        char key[32]; snprintf(key, sizeof(key), "view.kind%d", k);
        g_showKind[k] = S.getBool(key, g_showKind[k]);
    }
    g_vp.terrainMode = S.getInt ("view.terrain", g_vp.terrainMode);
    g_vp.showRoads   = S.getBool("view.roads",   g_vp.showRoads);
    g_vp.showDecals  = S.getBool("view.decals",  g_vp.showDecals);
    g_vp.showRivers  = S.getBool("view.rivers",  g_vp.showRivers);
    g_vp.cullMode    = S.getInt ("view.cull",    g_vp.cullMode);
    g_lightMode      = S.getInt ("view.lighting", g_lightMode);
    g_mode           = S.getInt ("mode", g_mode);
    if (g_mode < 0 || g_mode >= kNumModes) g_mode = 0;
    if (g_dataRoot.empty()) g_dataRoot = S.getStr("dataRoot");
    for (const std::string& f : S.favourites()) g_favourites.insert(f);
    g_winW = S.getInt("window.w", g_winW);
    g_winH = S.getInt("window.h", g_winH);
    if (g_winW < 320 || g_winW > 16384) g_winW = 1360;
    if (g_winH < 240 || g_winH > 16384) g_winH = 850;
}

static void saveSettings() {
    if (g_settingsPath.empty()) return;
    Settings& S = g_settings;
    S.setBool("panel.modes",    g_showModes);
    S.setBool("panel.tools",    g_showPanel);
    S.setBool("panel.props",    g_showProps);
    S.setBool("panel.entities", g_showEntities);
    S.setBool("panel.changes",  g_showChanges);
    S.setBool("panel.chunks",   g_showChunks);
    S.setBool("gizmo.on",    g_gizmoOn);
    S.setInt ("gizmo.op",    g_gizmoOp);
    S.setBool("gizmo.local", g_gizmoLocal);
    S.setBool("snap.on",     g_snapOn);
    S.setFloat("snap.grid",  g_snapGrid);
    S.setFloat("snap.angle", g_snapAngle);
    S.setFloat("brush.size",     g_brushSize);
    S.setFloat("brush.height",   g_brushHeight);
    S.setFloat("brush.pressure", g_brushPress);
    S.setBool("view.models",    g_showModels);
    S.setBool("view.dots",      g_showDots);
    S.setBool("view.wireframe", g_wireframe);
    for (int k = 0; k < 3; k++) {
        char key[32]; snprintf(key, sizeof(key), "view.kind%d", k);
        S.setBool(key, g_showKind[k]);
    }
    S.setInt ("view.terrain",  g_vp.terrainMode);
    S.setBool("view.roads",    g_vp.showRoads);
    S.setBool("view.decals",   g_vp.showDecals);
    S.setBool("view.rivers",   g_vp.showRivers);
    S.setInt ("view.cull",     g_vp.cullMode);
    S.setInt ("view.lighting", g_lightMode);
    S.setInt ("mode", g_mode);
    S.setStr ("dataRoot", g_dataRoot);
    S.setFavourites(std::vector<std::string>(g_favourites.begin(), g_favourites.end()));
    if (g_win) { int w = 0, h = 0; glfwGetWindowSize(g_win, &w, &h);
                 if (w > 0 && h > 0) { S.setInt("window.w", w); S.setInt("window.h", h); } }
    S.save(g_settingsPath);
}

// One line of "what was the editor doing", for the crash report. The stack says
// where it died; this says which map was open, which is usually the faster clue.
static const char* crashContext() {
    static char buf[900];
    snprintf(buf, sizeof(buf), "map=%s entities=%d mode=%s lighting=%d save=\"%s\"",
             g_mapPath, (int)g_scene.entities.size(),
             (g_mode >= 0 && g_mode < kNumModes) ? kModes[g_mode].name : "?",
             g_lightMode, g_saveStatus);
    return buf;
}

int main(int argc, char** argv) {
    // First statement, so a fault during startup is covered too.
    crashdump_install(crashContext);
    std::string loadPath, shotPath, pickDump, uiShotPath; bool selftest = false, pickTest = false;
    int uiShotFrames = 0, uiShotSelect = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--crashtest")) {
            // Check the handler against the thing it exists for: fault on
            // purpose and confirm the report names this function.
            printf("crashtest: faulting deliberately...\n"); fflush(stdout);
            crashdump_test_fault();
            printf("crashtest: FAIL — did not fault\n");
            return 3;
        }
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        // --shot renders the bare scene FBO with no ImGui, so it cannot show
        // anything drawn as a UI overlay — which now includes the selection
        // boxes. --uishot runs the real frame loop and captures the window.
        else if (!strcmp(argv[i], "--uishot") && i + 1 < argc) {
            uiShotPath = argv[++i]; if (uiShotFrames < 4) uiShotFrames = 4;
        }
        else if (!strcmp(argv[i], "--uishot-frames") && i + 1 < argc) uiShotFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--uishot-select") && i + 1 < argc) uiShotSelect = atoi(argv[++i]);
        // Overlay visibility from the command line, so a --shot pair can isolate
        // exactly one layer's contribution ("count pixels, not records").
        else if (!strcmp(argv[i], "--lighting") && i + 1 < argc) {
            const char* v = argv[++i];
            g_lightMode = !strcmp(v,"preset") ? 1 : !strcmp(v,"presetfog") ? 2 : 0;
        }
        // Accepts a name OR "#<slot>" — preset names are not unique (one map ships
        // two live presets both called Night_multi).
        else if (!strcmp(argv[i], "--preset") && i + 1 < argc) g_presetArg = argv[++i];
        // dev: --ghost <guid|#N> <wx> <wy> — arm the placement ghost at a fixed
        // world point, so a headless --shot / --picktest pair can check both that
        // it DRAWS and that it stays out of the pick buffer. There is no mouse in
        // a headless run, so the cursor path cannot be exercised any other way.
        else if (!strcmp(argv[i], "--ghost") && i + 3 < argc) {
            g_ghostArg = argv[++i];
            g_ghostAt[0] = (float)atof(argv[++i]);
            g_ghostAt[1] = (float)atof(argv[++i]);
        }
        // Force the asset root instead of walking up from the map path. A map
        // written somewhere else (a harness output in a temp dir) otherwise
        // resolves no ProtoDB and no textures, and renders untextured — which
        // silently invalidates any before/after pixel comparison against the
        // original. Set AFTER the load, since loadScene recomputes it.
        else if (!strcmp(argv[i], "--dataroot") && i + 1 < argc) g_dataRootArg = argv[++i];
        else if (!strcmp(argv[i], "--no-rivers")) g_vp.showRivers = false;
        else if (!strcmp(argv[i], "--no-roads"))  g_vp.showRoads  = false;
        else if (!strcmp(argv[i], "--no-decals")) g_vp.showDecals = false;
        // Which mode's panel to capture. Without this a --uishot always shows
        // mode 0, so a new mode's panel cannot be checked headlessly at all.
        else if (!strcmp(argv[i], "--uishot-mode") && i + 1 < argc) {
            int mv = atoi(argv[++i]);
            if (mv >= 0 && mv < kNumModes) { g_mode = mv; g_activeTool = 0; }
        }
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
        else if (!strcmp(argv[i], "--splattest") && i + 2 < argc) {
            return splatTest(argv[i+1], argv[i+2]);
        }
        else if (!strcmp(argv[i], "--protoplacetest") && i + 3 < argc) {
            return protoPlaceTest(argv[i+1], argv[i+2], argv[i+3]);
        }
        else if (!strcmp(argv[i], "--fieldtest") && i + 2 < argc) {
            return fieldTest(argv[i+1], argv[i+2]);
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
        else if (!strcmp(argv[i], "--settingstest") && i + 1 < argc) {
            // dev: --settingstest <tmpfile>  — the settings round-trip, on a temp
            // path so it never touches the user's real file.
            const std::string path = argv[i+1];
            { std::ofstream f(path, std::ios::trunc);
              f << "# hand-written\n"
                   "snap.grid 7.5\n"
                   "brush.size 6.25\n"
                   "view.rivers 0\n"
                   "mode 5\n"
                   "window.w 1024\n"
                   "window.h 720\n"
                   "some.future.key preserved value with spaces\n"
                   "dataRoot C:/a path/with spaces\n"; }
            loadSettings(path);
            int fail = 0;
            auto chk = [&](const char* what, bool ok) {
                printf("  %-34s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) fail++; };
            chk("float parses",        fabsf(g_snapGrid - 7.5f) < 1e-6f);
            chk("float parses (brush)",fabsf(g_brushSize - 6.25f) < 1e-6f);
            chk("bool parses",         g_vp.showRivers == false);
            chk("int parses",          g_mode == 5);
            chk("window size read",    g_winW == 1024 && g_winH == 720);
            chk("value keeps spaces",  g_settings.getStr("dataRoot") == "C:/a path/with spaces");
            // Round-trip: change something, save, reload, confirm it survived and
            // that a key this build knows nothing about was not dropped.
            g_snapGrid = 3.25f; g_mode = 2; g_vp.showRivers = true;
            g_winW = 800; g_winH = 600;
            saveSettings();
            g_snapGrid = 0; g_mode = 0; g_vp.showRivers = false;
            loadSettings(path);
            chk("survives save+reload", fabsf(g_snapGrid - 3.25f) < 1e-6f && g_mode == 2 &&
                                        g_vp.showRivers == true);
            chk("unknown key preserved",
                g_settings.getStr("some.future.key") == "preserved value with spaces");
            chk("version stamped",      g_settings.getInt("version", -1) == Settings::kVersion);
            // Recent list: most recent first, de-duplicated, capped.
            for (int k = 0; k < Settings::kMaxRecent + 3; k++)
                g_settings.pushRecentMap("map" + std::to_string(k) + ".map");
            g_settings.pushRecentMap("map5.map");        // re-open an older one
            std::vector<std::string> r = g_settings.recentMaps();
            chk("recent capped",        (int)r.size() == Settings::kMaxRecent);
            chk("recent most-recent 1st", !r.empty() && r[0] == "map5.map");
            chk("recent de-duplicated",
                std::set<std::string>(r.begin(), r.end()).size() == r.size());
            printf("settingstest: %s\n", fail ? "FAIL" : "PASS");
            return fail ? 3 : 0;
        }
        else if (!strcmp(argv[i], "--decalwritetest") && i + 2 < argc) {
            // dev: --decalwritetest <map> <out.map>
            // Move, resize and rotate one decal; exactly the five declared floats
            // may change, and the value must read back after a reload.
            Scene s;
            if (!load_map_native(argv[i+1], s)) { printf("decalwritetest: load failed\n"); return 2; }
            if (s.decalRecs.empty() || !s.decalPool.ok) {
                printf("decalwritetest: no writable decal pool\n"); return 2; }
            const std::vector<unsigned char> raw0 = s.raw;
            const Scene::DecalRec d0 = s.decalRecs[s.decalRecs.size() / 2];
            const float nx = d0.cx + 3.5f, nz = d0.cz - 2.25f;
            const float nsx = d0.sx * 1.5f, nsy = d0.sy * 0.75f, nrot = d0.rot + 0.4f;
            if (!overlay_set_decal(s, d0.slot, nx, nz, nsx, nsy, nrot)) {
                printf("decalwritetest: write refused\n"); return 3; }
            long changed = 0, outside = 0;
            for (size_t o = 0; o < s.raw.size() && o < raw0.size(); o++) {
                if (s.raw[o] == raw0[o]) continue;
                changed++;
                if ((long)o < d0.xformOff || (long)o >= d0.xformOff + 20) outside++;
            }
            std::ofstream f(argv[i+2], std::ios::binary);
            f.write((const char*)s.raw.data(), (std::streamsize)s.raw.size());
            f.close();
            Scene s2; bool re = load_map_native(argv[i+2], s2);
            const Scene::DecalRec* back = nullptr;
            if (re) for (const auto& r : s2.decalRecs) if (r.slot == d0.slot) { back = &r; break; }
            bool val = back && fabsf(back->cx - nx) < 1e-4f && fabsf(back->cz - nz) < 1e-4f &&
                       fabsf(back->sx - nsx) < 1e-4f && fabsf(back->sy - nsy) < 1e-4f &&
                       fabsf(back->rot - nrot) < 1e-4f;
            bool sameSize = s.raw.size() == raw0.size();
            bool poolSame = re && s2.decalPool.ok &&
                            s2.decalPool.used == s.decalPool.used &&
                            s2.decalPool.cap  == s.decalPool.cap &&
                            s2.decalRecs.size() == s.decalRecs.size();
            printf("decalwritetest slot %d: changed=%ld outsideXform=%ld sizeSame=%d "
                   "reload=%d readback=%d poolIntact=%d %s\n",
                   d0.slot, changed, outside, (int)sameSize, (int)re, (int)val, (int)poolSame,
                   (changed > 0 && outside == 0 && sameSize && val && poolSame) ? "OK" : "FAIL");
            return (changed > 0 && outside == 0 && sameSize && val && poolSame) ? 0 : 3;
        }
        else if (!strcmp(argv[i], "--sunprobe") && i + 1 < argc) {
            // dev: --sunprobe <map>
            //
            // WTHR stores the direction light TRAVELS, in the engine's LH Y-up
            // space; component 1 is vertical (< 0 on 219/219 records). That fixes
            // the vertical axis but leaves four horizontal sign/swap candidates.
            // The editor's documented engine->GL transform (loadModel negates X
            // and Z, a 180-deg Y rotation) picks one of them: L = (D.x, -D.y, D.z).
            //
            // This scores all four against the light that was hard-coded in the
            // renderer long before WTHR was decoded — an independently-arrived-at
            // vector that a correct swizzle must reproduce. Printing every
            // candidate keeps the choice falsifiable instead of merely plausible.
            Scene s;
            if (!load_map_native(argv[i+1], s)) return 2;
            if (s.weather.empty()) { printf("sunprobe: no WTHR presets\n"); return 3; }
            int di = -1;
            for (int f = 0; f < kWeatherFieldCount; f++)
                if (!strcmp(kWeatherFields[f].name, "SunDirection")) di = f;
            const float legacy[3] = { 0.4f, 0.8f, 0.35f };
            float ll = std::sqrt(legacy[0]*legacy[0]+legacy[1]*legacy[1]+legacy[2]*legacy[2]);
            struct Cand { const char* name; int sx, sz; bool swap; };
            const Cand cands[] = {
                { "( D.x, -D.y,  D.z)  [engine->GL: negate X and Z, as loadModel does]", +1, +1, false },
                { "(-D.x, -D.y, -D.z)  [plain -D, no handedness change]",                -1, -1, false },
                { "(-D.x, -D.y,  D.z)  [X only]",                                        -1, +1, false },
                { "( D.z, -D.y,  D.x)  [horizontal swap]",                               +1, +1, true  },
            };
            // Compare ONLY the engine-active preset. Averaging over every preset
            // in a map is meaningless: Rain, Sundown and Sunny legitimately point
            // different ways, and only one vector could ever have been the
            // reference for a single hard-coded constant.
            int ai = s.weatherActive >= 0 ? s.weatherActive : 0;
            const WeatherPreset& w = s.weather[(size_t)ai];
            const float* D = w.values[(size_t)di].data();
            printf("sunprobe %s  preset \"%s\" (slot %d)  D = %.4f %.4f %.4f\n",
                   s.name.c_str(), w.name.c_str(), w.slot, D[0], D[1], D[2]);
            int best = -1; float bestAng = 1e9f;
            for (int c = 0; c < 4; c++) {
                float v[3];
                v[0] = cands[c].swap ? D[2] * cands[c].sx : D[0] * cands[c].sx;
                v[1] = -D[1];
                v[2] = cands[c].swap ? D[0] * cands[c].sz : D[2] * cands[c].sz;
                float vl = std::sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
                if (vl < 1e-6f) continue;
                float dot = (v[0]*legacy[0]+v[1]*legacy[1]+v[2]*legacy[2])/(vl*ll);
                float ang = acosf(std::max(-1.0f, std::min(1.0f, dot))) * 57.2957795f;
                printf("  cand %d  %-58s  %.3f %.3f %.3f   %6.1f deg\n",
                       c, cands[c].name, v[0]/vl, v[1]/vl, v[2]/vl, ang);
                if (ang < bestAng) { bestAng = ang; best = c; }
            }
            printf("  legacy hard-coded light                                          "
                   "     %.3f %.3f %.3f\n", legacy[0]/ll, legacy[1]/ll, legacy[2]/ll);
            printf("  closest to legacy on this map: candidate %d (%.1f deg)\n", best, bestAng);
            // The legacy constant can only have been eyeballed from ONE map. The
            // stock Default (0.4156, -0.8090, 0.4156) recurs across maps and is
            // the plausible source, so a map carrying it discriminates; a map with
            // its own sun direction does not, and must not be read as evidence.
            bool stock = fabsf(D[0] - 0.4156f) < 1e-3f && fabsf(D[1] + 0.8090f) < 1e-3f &&
                         fabsf(D[2] - 0.4156f) < 1e-3f;
            if (stock) {
                printf("  this preset IS the stock Default -> discriminating. Note D.x == D.z,\n"
                       "  so candidates 0 and 3 are mathematically identical here and cannot\n"
                       "  be separated by this map; 1 and 2 are eliminated.\n");
            } else {
                printf("  this preset is map-specific, so distance to the legacy constant\n"
                       "  carries NO information about the swizzle. Reported, not scored.\n");
            }
            // NO pass/fail. The legacy constant is one vector somebody eyeballed;
            // it can only correspond to whichever map they had open, so agreement
            // is evidence on THAT map and silence everywhere else. Reporting a
            // winner per map would dress a single data point up as a corpus
            // result. The vertical axis IS settled (D[1] < 0 on 219/219); the
            // horizontal pairing is not, and the renderer stays in neutral mode by
            // default until someone reads the engine's draw-side light setup.
            printf("sunprobe: REPORT ONLY - the horizontal swizzle is NOT settled by this test.\n");
            return 0;
        }
        else if (!strcmp(argv[i], "--overlayscan") && i + 1 < argc) {
            // dev: --overlayscan <map|dir> — GROL/GDCL/GRVL pool health.
            // Beyond "the walk consumed the chunk", assert the list invariants:
            // the used chain must visit every live slot exactly once, the free
            // chain every free slot exactly once, and prev must be the exact
            // inverse of next. All hold on the shipped corpus, so this is a free
            // baseline that a future write path can regress against.
            struct Acc { int maps=0, pools=0, ok=0, roads=0, decals=0, rivers=0, riverRecs=0,
                         listOK=0, mono=0, monoTot=0, uhnz=0; std::vector<std::string> bad; } A;
            auto chk = [&](const char* what, const Scene::OverlayPool& P, const std::string& nm) {
                if (P.chunkOff < 0) return;
                A.pools++;
                if (!P.ok) { A.bad.push_back(nm + " " + what + ": pool walk failed (fell back to scan)"); return; }
                A.ok++;
                // Walk usedHead -> next and require it to visit every live slot
                // exactly once, with prev the exact inverse and the ends -1.
                std::map<int, const Scene::OverlaySlotRef*> bySlot;
                for (const auto& L : P.live) bySlot[L.slot] = &L;
                std::set<int> seen;
                int cur = P.usedHead, guard = 0, last = -1;
                bool chainOK = true;
                while (cur >= 0 && guard++ <= P.cap) {
                    auto it = bySlot.find(cur);
                    if (it == bySlot.end() || seen.count(cur)) { chainOK = false; break; }
                    if (it->second->prev != last) chainOK = false;   // prev is next's inverse
                    seen.insert(cur); last = cur; cur = it->second->next;
                }
                if (cur >= 0) chainOK = false;                      // ran past the guard
                if (seen.size() != P.live.size()) chainOK = false;  // missed a live slot
                if (last != P.usedTail) chainOK = false;
                if ((int)P.live.size() != P.used) chainOK = false;
                if (chainOK) A.listOK++;
                else A.bad.push_back(nm + " " + what + ": used-list invariant failed (live " +
                                     std::to_string(P.live.size()) + ", usedCount " +
                                     std::to_string(P.used) + ", visited " +
                                     std::to_string(seen.size()) + ")");
                if (strcmp(what, "GRVL")) {
                    // Is the USED-LIST order the same as slot (= file) order?
                    // `P.live` is built by scanning slots 0..cap, so testing it
                    // for monotonicity would be vacuous — walk the chain instead.
                    A.monoTot++;
                    bool mono = true;
                    { int c2 = P.usedHead, prev2 = -1, g2 = 0;
                      while (c2 >= 0 && g2++ <= P.cap) {
                          if (c2 < prev2) { mono = false; break; }
                          prev2 = c2;
                          auto it2 = bySlot.find(c2);
                          if (it2 == bySlot.end()) break;
                          c2 = it2->second->next;
                      } }
                    if (mono) A.mono++;
                    if (P.usedHead != 0) A.uhnz++;
                }
            };
            auto one = [&](const std::string& p) {
                Scene s; if (!load_map_native(p, s)) return;
                A.maps++;
                std::string nm = p.substr(p.find_last_of("/\\") + 1);
                chk("GROL", s.roadPool, nm); chk("GDCL", s.decalPool, nm); chk("GRVL", s.riverPool, nm);
                A.roads += (int)s.roadPool.live.size();
                A.decals += (int)s.decalPool.live.size();
                A.riverRecs += (int)s.riverPool.live.size();
                A.rivers += (int)s.rivers.size();
                if (!s.rivers.empty()) {
                    printf("  %-42s %2d river(s):", nm.c_str(), (int)s.rivers.size());
                    for (size_t k = 0; k < s.rivers.size() && k < 3; k++) {
                        const auto& r = s.rivers[k];
                        float wmin = r.w[0], wmax2 = r.w[0];
                        for (float w : r.w) { wmin = std::min(wmin, w); wmax2 = std::max(wmax2, w); }
                        printf("  [%d nodes, y=%.2f, w %.1f..%.1f]", (int)r.cx.size(), r.level, wmin, wmax2);
                    }
                    printf("\n");
                }
            };
            std::error_code ec;
            if (std::filesystem::is_directory(argv[i+1], ec)) {
                for (auto it = std::filesystem::recursive_directory_iterator(argv[i+1], ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec || !it->is_regular_file(ec) || it->path().extension() != ".map") continue;
                    one(it->path().string());
                }
            } else one(argv[i+1]);
            printf("overlayscan: maps=%d pools=%d walked-exact=%d counts-match=%d\n",
                   A.maps, A.pools, A.ok, A.listOK);
            printf("  records: %d GROA  %d GDEC  %d GRVR (%d renderable; the rest are "
                   "single-node degenerates)\n", A.roads, A.decals, A.riverRecs, A.rivers);
            // The engine's LOADER reads slots in file order; whether the RENDERER
            // walks the used list instead is not established, so this is reported,
            // not acted on. Reordering emission on a guess would be a regression
            // risk for a measured-tiny visual difference.
            printf("  road/decal pools whose used-list order == file order: %d/%d  (usedHead!=0 on %d)\n",
                   A.mono, A.monoTot, A.uhnz);
            for (const auto& b : A.bad) printf("  FAIL %s\n", b.c_str());
            bool pass = A.pools > 0 && A.ok == A.pools && A.listOK == A.pools && A.bad.empty();
            printf("overlayscan: %s\n", pass ? "PASS" : "FAIL");
            return pass ? 0 : 3;
        }
        else if (!strcmp(argv[i], "--wthrtest") && i + 1 < argc) {
            // dev: --wthrtest <map|dir> [out.map]
            //
            // Checking that the pool walk consumes the chunk exactly is NOT
            // enough: the engine's reflection order also sums to 194 bytes, so a
            // table in the wrong order walks every map cleanly and decodes every
            // colour into the wrong widget. These assertions pin the ORDER by
            // semantics — a permutation cannot satisfy them simultaneously.
            struct Acc {
                int maps=0, ok=0, recs=0, alphaN=0, alpha=0, eff=0, unit=0, sy=0,
                    bools=0, tod=0, bright=0, fogOn=0, fogOrder=0;
                float shadowLo=1e9f, shadowHi=-1e9f, fogEndLo=1e9f, cloudMax=0;
                std::string cloudWorst;
                std::vector<std::string> fails;
            } A;
            auto one = [&](const std::string& p) {
                std::vector<unsigned char> raw;
                { std::ifstream f(p, std::ios::binary);
                  if (!f) return;
                  raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
                if (raw.size() < 8 || memcmp(raw.data(), "SCEN", 4) != 0) return;
                A.maps++;
                Scene s; parse_weather(raw, s);
                if (s.weather.empty()) { A.fails.push_back(p + ": no presets decoded"); return; }
                A.ok++;
                auto F = [&](const WeatherPreset& w, const char* n) -> const std::array<float,4>* {
                    for (int f = 0; f < kWeatherFieldCount; f++)
                        if (!strcmp(kWeatherFields[f].name, n)) return &w.values[(size_t)f];
                    return nullptr;
                };
                for (const WeatherPreset& w : s.weather) {
                    A.recs++;
                    // alpha components of the three true colours are always 1.0
                    for (const char* n : {"SunColor", "SunAmbient", "FogColor", "SunSpecular"}) {
                        A.alphaN++; if (fabsf((*F(w, n))[3] - 1.0f) < 1e-6f) A.alpha++;
                    }
                    // ...but SunShadow's 4th is not an alpha at all
                    float sw = (*F(w, "SunShadow"))[3];
                    A.shadowLo = std::min(A.shadowLo, sw); A.shadowHi = std::max(A.shadowHi, sw);
                    if ((int)(*F(w, "EffectCount"))[0] == 4) A.eff++;
                    const auto& d = *F(w, "SunDirection");
                    float L = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
                    if (fabsf(L - 1.0f) < 0.02f) A.unit++;
                    if (d[1] < 0.0f) A.sy++;                       // index 1 is down
                    float fe = (*F(w, "FogEnabled"))[0], ni = (*F(w, "Night"))[0];
                    if ((fe == 0 || fe == 1) && (ni == 0 || ni == 1)) A.bools++;
                    float t = (*F(w, "TimeOfTheDay"))[0];
                    if (t >= 0.0f && t <= 24.0f) A.tod++;           // it is a clock
                    if ((*F(w, "Brightness"))[0] == 1.0f && (*F(w, "Contrast"))[0] == 1.0f) A.bright++;
                    if (fe == 1) {
                        A.fogOn++;
                        float fs = (*F(w, "FogStart"))[0], fend = (*F(w, "FogEnd"))[0];
                        if (fend > fs) A.fogOrder++;
                        A.fogEndLo = std::min(A.fogEndLo, fend);
                    }
                    // CloudMovementDir ~= WindDirection * CloudSpeed is a SOFT
                    // check: 8 of 219 shipped records are not the exact product,
                    // so report the residual and only fail past 1e-2.
                    const auto& wd = *F(w, "WindDirection");
                    const auto& cm = *F(w, "CloudMovementDir");
                    float cs = (*F(w, "CloudSpeed"))[0];
                    float cerr = std::max(fabsf(cm[0] - wd[0]*cs), fabsf(cm[1] - wd[1]*cs));
                    if (cerr > A.cloudMax)   // parse_weather does not set Scene::name
                        A.cloudMax = cerr, A.cloudWorst = p.substr(p.find_last_of("/\\") + 1) + " / " + w.name;
                }
            };
            std::error_code ec;
            bool dir = std::filesystem::is_directory(argv[i+1], ec);
            if (dir) {
                for (auto it = std::filesystem::recursive_directory_iterator(argv[i+1], ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec || !it->is_regular_file(ec) || it->path().extension() != ".map") continue;
                    one(it->path().string());
                }
            } else {
                one(argv[i+1]);
                Scene s; if (load_map_native(argv[i+1], s)) {
                    printf("WTHR @%ld  presets=%zu  free=%d  capacity=%d  engine-active=%s\n",
                           s.wthrOff, s.weather.size(), s.weatherFree, s.weatherCap,
                           s.weatherActive >= 0 ? s.weather[s.weatherActive].name.c_str()
                                                : "(none named \"Default\")");
                    for (size_t k = 0; k < s.weather.size(); k++) {
                        const WeatherPreset& w = s.weather[k];
                        printf("  [slot %2d] %-24s%s\n", w.slot, w.name.c_str(),
                               (int)k == s.weatherActive ? "  *" : "");
                    }
                    // write leg: flip a bool, nudge the sun, advance the clock, and
                    // prove nothing outside the three declared spans moved.
                    if (i + 2 < argc && !s.weather.empty()) {
                        auto idxOf = [](const char* n) {
                            for (int f = 0; f < kWeatherFieldCount; f++)
                                if (!strcmp(kWeatherFields[f].name, n)) return f;
                            return -1; };
                        int p0 = s.weatherActive >= 0 ? s.weatherActive : 0;
                        WeatherPreset& w = s.weather[(size_t)p0];
                        int iFog = idxOf("FogEnabled"), iDir = idxOf("SunDirection"),
                            iTod = idxOf("TimeOfTheDay");
                        w.values[iFog][0] = w.values[iFog][0] != 0.0f ? 0.0f : 1.0f;
                        float a = 1.0f * 3.14159265f / 180.0f, c = cosf(a), sn = sinf(a);
                        float x = w.values[iDir][0], z = w.values[iDir][2];
                        w.values[iDir][0] = x*c - z*sn; w.values[iDir][2] = x*sn + z*c;
                        w.values[iTod][0] += 1.0f;
                        w.dirty[iFog] = w.dirty[iDir] = w.dirty[iTod] = 1;
                        s.weatherEdited = true;
                        std::vector<unsigned char> b = s.raw;
                        apply_edits_inplace(s, {}, b);
                        // every changed byte must lie inside a declared span
                        long spans[3][2] = {
                            { w.tailOff + kWeatherFields[iFog].tail, 1 },
                            { w.tailOff + kWeatherFields[iDir].tail, 12 },
                            { w.tailOff + kWeatherFields[iTod].tail, 4 } };
                        long changed = 0, outside = 0;
                        for (size_t o = 0; o < b.size(); o++) {
                            if (b[o] == s.raw[o]) continue;
                            changed++;
                            bool in = false;
                            for (auto& sp : spans) if ((long)o >= sp[0] && (long)o < sp[0]+sp[1]) in = true;
                            if (!in) outside++;
                        }
                        std::ofstream f(argv[i+2], std::ios::binary);
                        f.write((const char*)b.data(), (std::streamsize)b.size());
                        // NOTE: assert `outside == 0`, never an exact changed count —
                        // the spans total 17 bytes but a float edit often differs in
                        // fewer, so the real number is data-dependent.
                        Scene s2; bool re = load_map_native(argv[i+2], s2);
                        bool val = re && s2.weather.size() == s.weather.size() &&
                                   fabsf(s2.weather[(size_t)p0].values[iTod][0]
                                         - w.values[iTod][0]) < 1e-4f;
                        printf("  write: changed=%ld outside-declared-spans=%ld reload=%d readback=%d -> %s\n",
                               changed, outside, (int)re, (int)val,
                               (outside == 0 && changed > 0 && val) ? "PASS" : "FAIL");
                        if (outside || !changed || !val) return 3;
                    }
                }
            }
            if (dir || A.maps > 1) {
                printf("wthrtest: maps=%d decoded=%d records=%d\n", A.maps, A.ok, A.recs);
                printf("  colour alphas == 1.0            %d/%d\n", A.alpha, A.alphaN);
                printf("  EffectCount == 4                %d/%d\n", A.eff, A.recs);
                printf("  |SunDirection| == 1 +-0.02      %d/%d\n", A.unit, A.recs);
                printf("  SunDirection[1] < 0 (downward)  %d/%d\n", A.sy, A.recs);
                printf("  FogEnabled/Night in {0,1}       %d/%d\n", A.bools, A.recs);
                printf("  TimeOfTheDay in 0..24           %d/%d\n", A.tod, A.recs);
                printf("  Brightness/Contrast == 1.0      %d/%d\n", A.bright, A.recs);
                printf("  FogEnd > FogStart when fog on   %d/%d  (min FogEnd %.1f)\n",
                       A.fogOrder, A.fogOn, A.fogEndLo);
                printf("  SunShadow.w range               %.4f .. %.4f (not an alpha)\n",
                       A.shadowLo, A.shadowHi);
                // Soft on purpose: 8 of the 219 shipped records are not the exact
                // product, and the worst (M_07 "02_cloudy") sits at 0.00999999 —
                // gating at 1e-2 would be a float-rounding coin flip, so the gate
                // is 2e-2 and the real worst case is always printed.
                printf("  max |CloudMovementDir - Wind*Speed| %.8f  (%s; soft, gate 2e-2)\n",
                       A.cloudMax, A.cloudWorst.c_str());
                for (const auto& f : A.fails) printf("  FAIL %s\n", f.c_str());
                bool pass = A.ok == A.maps && A.alpha == A.alphaN && A.eff == A.recs &&
                            A.unit == A.recs && A.sy == A.recs && A.bools == A.recs &&
                            A.tod == A.recs && A.bright == A.recs &&
                            A.fogOrder == A.fogOn && A.cloudMax <= 2e-2f;
                printf("wthrtest: %s\n", pass ? "PASS" : "FAIL");
                return pass ? 0 : 3;
            }
            return 0;
        }
        else if (!strcmp(argv[i], "--chunktile") && i + 1 < argc) {
            // dev: --chunktile <map|dir>  — the structural-integrity check that
            // `cpcw_map.py roundtrip` is NOT (see mapfile.h). Run it on the output
            // of every write path, not just the oracle.
            auto one = [](const std::string& p, bool verbose) -> bool {
                std::vector<unsigned char> raw;
                { std::ifstream f(p, std::ios::binary);
                  if (!f) { printf("chunktile: cannot open %s\n", p.c_str()); return false; }
                  raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
                if (raw.size() < 8 || memcmp(raw.data(), "SCEN", 4) != 0) return true;  // not a CPCW map: skip
                ChunkTileReport r;
                if (!map_chunk_tile(raw, r)) { printf("chunktile: %s PARSE FAIL\n", p.c_str()); return false; }
                printf("%-46s %s chunks=%d containers=%d objs=%d/%d gap=%ld overlap=%ld trailer=%ld\n",
                       p.substr(p.find_last_of("/\\") + 1).c_str(), r.ok ? "OK  " : "FAIL",
                       r.chunks, r.containers, r.objsSchemaOk, r.objs,
                       r.gapBytes, r.overlapBytes, r.trailerBytes);
                if (verbose || !r.ok)
                    for (size_t k = 0; k < r.issues.size() && k < 12; k++)
                        printf("    %s\n", r.issues[k].c_str());
                return r.ok;
            };
            std::error_code ec;
            int n = 0, bad = 0;
            if (std::filesystem::is_directory(argv[i+1], ec)) {
                for (auto it = std::filesystem::recursive_directory_iterator(argv[i+1], ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec || !it->is_regular_file(ec)) continue;
                    if (it->path().extension() != ".map") continue;
                    n++; if (!one(it->path().string(), false)) bad++;
                }
                printf("chunktile: %d map(s), %d failed\n", n, bad);
            } else { n = 1; if (!one(argv[i+1], true)) bad = 1; }
            return bad ? 3 : 0;
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
    if (selftest) {
        if (!loadPath.empty()) loadScene(loadPath);
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

    // Settings load BEFORE the window is created — the window size is one of the
    // things being restored, and after glfwCreateWindow it is too late.
    // It must also come before the initial loadScene: Settings::load() clears the
    // map, so a load afterwards would wipe the recent-map entry that scene just
    // pushed (and every other in-memory setting).
    // Deliberately not applied to the headless paths above: a --shot must not
    // pick up whatever view toggles the user last left set, or the same command
    // renders differently on two machines.
    // A headless render must not inherit whatever view toggles the user last left
    // set, or the same command produces different pixels on two machines and the
    // no-change regressions become meaningless. Such runs also must not write the
    // user's settings back.
    const bool headless = !shotPath.empty() || !uiShotPath.empty() || pickTest;
    if (!headless) loadSettings();
    if (!loadPath.empty()) loadScene(loadPath);
    if (!g_dataRootArg.empty()) g_dataRoot = g_dataRootArg;   // after the load

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* win = glfwCreateWindow(g_winW, g_winH, "CPCW Map Editor", nullptr, nullptr);
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
        syncEnvironment();
        // Frame the camera on the ghost for a --shot. Without this the default
        // whole-map view renders a doodad at one or two pixels and a pixel diff
        // cannot tell "drawn" from "not drawn" — the first run of this check
        // reported 1 changed pixel for a metal barrel and 0 for a bush, which
        // proved nothing either way. Deliberately NOT done for --picktest: moving
        // the camera there would push 3164 of 3191 instances off-screen and make
        // the with/without-ghost counts incomparable.
        if (applyGhostArg()) {
            g_cam.target = g_vp.ghostCenter();
            if (g_cam.dist > 25.0f) g_cam.dist = 25.0f;
        }
        // Clear to the fog colour when fog is on, or distant terrain fades into a
        // band of sky that the background then cuts off.
        if (g_vp.fogOn) glClearColor(g_vp.fogColor[0], g_vp.fogColor[1], g_vp.fogColor[2], 1.0f);
        else            glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
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
        // Arming the ghost here is the point of `--picktest --ghost ...`: the
        // numbers below must come out IDENTICAL with and without it. The ghost
        // being unpickable is a structural claim (it is not in `instances`, which
        // is the only thing renderPickBuffer walks) and this is what checks it.
        applyGhostArg();
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
    // ImGui 1.92 raises an error tooltip on conflicting widget IDs, and that
    // tooltip carries an "Item Picker" button whose picker fires IM_DEBUG_BREAK
    // on the next hovered item — one stray click away from killing the process
    // in a Release build. Duplicate IDs are still a bug (use ##suffix), but they
    // must not be able to reach a debug break.
    ImGui::GetIO().ConfigDebugHighlightIdConflictsShowItemPicker = false;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    int uiShotFrame = 0;
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
        drawChanges();
        drawChunkInspector();
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
        // clipboard + structural shortcuts (operate on the whole selection)
        if (!kio.WantCaptureKeyboard) {
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) copySelection();
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X)) cutSelection();
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
                if (kio.KeyShift) pasteClipboard(true, 0, 0);
                else pasteClipboard(false, g_cam.target.x, g_cam.target.z);
            }
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !g_selection.empty()) {
                copySelection();
                pasteClipboard(false, g_clipAnchor[0] + 8.0f, g_clipAnchor[1] + 8.0f);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelection();
        }
        // [ / ] rotate the primary selection 5 degrees (yaw)
        if (!kio.WantCaptureKeyboard && g_selected >= 0 &&
            g_selected < (int)g_scene.entities.size()) {
            Entity& e = g_scene.entities[g_selected];
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { snapEntity(g_selected); e.dir -= 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { snapEntity(g_selected); e.dir += 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
        }
        // ...and with nothing selected they aim the placement ghost instead, so a
        // model can be oriented BEFORE it is dropped rather than placed and then
        // fixed up.
        else if (!kio.WantCaptureKeyboard && !g_placeProto.empty() && activeToolIsPlace()) {
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  g_placeYaw -= 5.0f;
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) g_placeYaw += 5.0f;
            if (g_snapOn && g_snapAngle > 0.0f)
                g_placeYaw = std::round(g_placeYaw / g_snapAngle) * g_snapAngle;
            while (g_placeYaw >= 360.0f) g_placeYaw -= 360.0f;
            while (g_placeYaw < 0.0f)    g_placeYaw += 360.0f;
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
        if (g_overlayDirty && g_glReady) {  // after a decal edit
            // Re-decode from the mutated raw bytes, then rebuild the batches. The
            // decal meshes are BAKED geometry, so without this the write is
            // correct and completely invisible — this codebase's classic failure.
            parse_overlays(g_scene.raw, g_scene);
            g_vp.clearOverlays();
            g_vp.buildOverlays(g_scene, g_dataRoot);
            g_overlayDirty = false;
        }
        if (g_splatTexDirty && g_glReady) { // after a texture-blend stroke
            g_vp.refreshSplatWeights(g_scene); g_splatTexDirty = false;
        }

        // Scripted selection for --uishot: applied once the scene and its model
        // instances exist, so entityWorldAABB has something to measure.
        if (uiShotSelect >= 0 && g_scene.loaded && !g_sceneDirty) {
            selectOnly(uiShotSelect); uiShotSelect = -1;
        }

        // After the rebuild block, so a box tracks the model in the same frame a
        // drag moved it, and before Render() so it makes it into the draw data.
        drawSelectionOverlay(cmin, cmax);

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
            syncEnvironment();
            if (g_vp.fogOn) glClearColor(g_vp.fogColor[0], g_vp.fogColor[1], g_vp.fogColor[2], 1.0f);
            else            glClearColor(0.12f, 0.14f, 0.17f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            g_vp.render(g_cam, (float)vw / (float)vh, g_wireframe, g_selected,
                        g_showModels, g_showDots, g_hovered);
            glDisable(GL_SCISSOR_TEST);
            glViewport(0, 0, fbw, fbh);
        }

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Full-window capture: everything the user sees, UI overlays included.
        // Taken before the swap, from the back buffer we just finished drawing.
        if (!uiShotPath.empty() && ++uiShotFrame >= uiShotFrames) {
            std::vector<unsigned char> px((size_t)fbw * fbh * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, fbw, fbh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            writeBMP(uiShotPath.c_str(), fbw, fbh, px.data());
            printf("wrote %s (%dx%d)\n", uiShotPath.c_str(), fbw, fbh);
            glfwSwapBuffers(win);
            break;
        }
        glfwSwapBuffers(win);
    }
    // Before the window goes away — saveSettings() reads its size. Skipped for
    // headless runs, which never loaded them.
    if (!headless) saveSettings();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
