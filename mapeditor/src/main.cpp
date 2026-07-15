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
#include <nlohmann/json.hpp>
#include <iterator>
#include <algorithm>

#include <cstdio>
#include <cstring>
#include <cstdlib>
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
static int   g_mode = 0, g_activeTool = 0, g_selected = -1;
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

// --- undo/redo -------------------------------------------------------------
struct EditCmd {
    bool terrain = false;
    int idx = -1; float pos0[3]{}, pos1[3]{}; float dir0 = 0, dir1 = 0; int pl0 = 0, pl1 = 0;
    std::vector<int> cells; std::vector<float> h0, h1;   // terrain stroke
};
static std::vector<EditCmd> g_undo, g_redo;
static bool g_snapActive = false; static EditCmd g_snap;   // pending entity snapshot
static bool g_strokeActive = false; static std::vector<float> g_strokeH0;
static std::string g_dataRoot;                     // folder holding ProtoDB.bin + models
static std::string g_srcMap;                       // original .map (empty if .json)
static std::set<long> g_edited;                    // ids with pending field edits
static char  g_saveStatus[256] = "";

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static void applyCmd(const EditCmd& c, bool useNew) {
    if (c.terrain) {
        for (size_t i = 0; i < c.cells.size(); i++)
            if (c.cells[i] >= 0 && c.cells[i] < (int)g_scene.heights.size())
                g_scene.heights[c.cells[i]] = useNew ? c.h1[i] : c.h0[i];
        g_scene.terrainEdited = true; g_terrainDirty = true;
        if (g_scene.heightDirty.size() == g_scene.heights.size())
            for (int ci : c.cells) g_scene.heightDirty[ci] = 1;
    } else if (c.idx >= 0 && c.idx < (int)g_scene.entities.size()) {
        Entity& e = g_scene.entities[c.idx];
        for (int k = 0; k < 3; k++) e.pos[k] = useNew ? c.pos1[k] : c.pos0[k];
        e.dir = useNew ? c.dir1 : c.dir0; e.player = useNew ? c.pl1 : c.pl0;
        g_edited.insert(e.id); g_entDirty = true; g_modelsDirty = true;
    }
}
static void undoEdit() {
    if (g_undo.empty()) return;
    EditCmd c = g_undo.back(); g_undo.pop_back(); applyCmd(c, false); g_redo.push_back(c);
}
static void redoEdit() {
    if (g_redo.empty()) return;
    EditCmd c = g_redo.back(); g_redo.pop_back(); applyCmd(c, true); g_undo.push_back(c);
}
// snapshot the selected entity's state before an edit begins
static void snapEntity(int idx) {
    if (idx < 0 || idx >= (int)g_scene.entities.size()) { g_snapActive = false; return; }
    const Entity& e = g_scene.entities[idx];
    g_snap = EditCmd{}; g_snap.idx = idx;
    for (int k = 0; k < 3; k++) g_snap.pos0[k] = e.pos[k];
    g_snap.dir0 = e.dir; g_snap.pl0 = e.player; g_snapActive = true;
}
// commit the snapshot as an undo command if the entity actually changed
static void commitEntity() {
    if (!g_snapActive || g_snap.idx < 0 || g_snap.idx >= (int)g_scene.entities.size()) { g_snapActive = false; return; }
    const Entity& e = g_scene.entities[g_snap.idx];
    bool changed = e.dir != g_snap.dir0 || e.player != g_snap.pl0;
    for (int k = 0; k < 3; k++) if (e.pos[k] != g_snap.pos0[k]) changed = true;
    if (changed) {
        for (int k = 0; k < 3; k++) g_snap.pos1[k] = e.pos[k];
        g_snap.dir1 = e.dir; g_snap.pl1 = e.player;
        g_undo.push_back(g_snap); g_redo.clear();
    }
    g_snapActive = false;
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
    g_scene = std::move(s); g_selected = -1; g_sceneDirty = true;
    g_srcMap = endsWithI(path, ".json") ? std::string() : path;  // Save needs the .map
    if (!preserveView)
        g_dataRoot = g_srcMap.empty() ? std::string() : findDataRoot(g_srcMap);
    g_edited.clear(); g_saveStatus[0] = '\0';
    resetThumbCache();
    if (!preserveView) snprintf(g_mapPath, sizeof(g_mapPath), "%s", path.c_str());
    if (selectId >= 0)
        for (int i = 0; i < (int)g_scene.entities.size(); i++)
            if (g_scene.entities[i].id == selectId) { g_selected = i; break; }
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

// Save edited entity fields natively (no Python): overwrite Pos/Player in place
// in a copy of the loaded bytes and write <map>_edited.map (byte-faithful).
static void doSave() {
    if (g_scene.raw.empty() || g_srcMap.empty()) {
        snprintf(g_saveStatus, sizeof(g_saveStatus),
                 "Save needs a .map source (open a .map, not a .json)."); return;
    }
    if (g_edited.empty() && !g_scene.terrainEdited) {
        snprintf(g_saveStatus, sizeof(g_saveStatus), "No edits to save."); return;
    }
    std::vector<long> ids(g_edited.begin(), g_edited.end());
    std::string out = g_srcMap.substr(0, g_srcMap.size() - 4) + "_edited.map";
    if (save_map_native(g_scene, ids, out))
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Saved %d edit(s) -> %s",
                 (int)g_edited.size(), out.c_str());
    else
        snprintf(g_saveStatus, sizeof(g_saveStatus), "Save failed.");
}

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

// Place a copy of entity srcId at the camera target, grounded on the terrain
// (structural insert + reload; keeps the view and selects the new entity).
static void placeDuplicate(long srcId) {
    if (srcId < 0 || g_srcMap.empty()) return;
    long newId = 1; for (const auto& en : g_scene.entities) if (en.id >= newId) newId = en.id + 1;
    float wx = g_cam.target.x, wy = g_cam.target.z;
    float p[3] = { wx, wy, terrainHeightAt(wx, wy) };   // ground on the heightmap
    const char* tmp = getenv("TEMP"); if (!tmp) tmp = getenv("TMP"); if (!tmp) tmp = ".";
    std::string work = std::string(tmp) + "/cpcw_mapedit_work.map";
    if (add_entity_native(g_scene, srcId, p, newId, work)) loadScene(work, true, newId);
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
        ImGui::MenuItem("New");
        if (ImGui::MenuItem("Save edits", "Ctrl+S", false, !g_srcMap.empty())) doSave();
        ImGui::Separator(); ImGui::MenuItem("Exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !g_undo.empty())) undoEdit();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !g_redo.empty())) redoEdit();
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X"); ImGui::MenuItem("Copy", "Ctrl+C");
        ImGui::MenuItem("Paste", "Ctrl+V"); ImGui::MenuItem("Delete", "Ctrl+Del");
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
            if (ImGui::Button("Place copy at view center") && g_placeSrcId >= 0)
                placeDuplicate(g_placeSrcId);
            ImGui::TextDisabled("(placed at view center, grounded; Ctrl+D dup, Delete remove)");
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
                if (ImGui::Selectable(lbl, g_selected == i)) g_selected = i;
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

// Ray from the camera through the mouse, intersected with a horizontal plane at
// the camera target height -> terrain grid cell (gx,gy). Approximate (flat plane)
// but fine for brushing. Returns false if the ray doesn't hit.
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
    if (std::fabs(dir.y) < 1e-5f) return false;
    float d = (g_cam.target.y - eye.y) / dir.y;
    if (d < 0) return false;
    gx = eye.x + dir.x * d;   // world X = grid i
    gy = eye.z + dir.z * d;   // world Z = grid j
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

// camera navigation over the central viewport region
static void updateCamera(const ImVec2& cmin, const ImVec2& cmax) {
    ImGuiIO& io = ImGui::GetIO();
    bool over = ImGui::IsMouseHoveringRect(cmin, cmax, false) && !io.WantCaptureMouse;

    // WASD / arrow-key movement over the ground plane. Speed has a floor so it
    // does NOT crawl when zoomed in (was purely distance-scaled before).
    if (!io.WantCaptureKeyboard && !io.KeyCtrl) {
        float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 0.016f;
        float sp = (g_cam.dist * 0.5f + 15.0f) * dt * (io.KeyShift ? 3.0f : 1.0f);
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
        float k = g_cam.dist * 0.0016f + 0.02f;   // floor so it pans when zoomed in
        g_cam.target = g_cam.target - right * (io.MouseDelta.x * k)
                                    + up    * (io.MouseDelta.y * k);
    }
    if (over && io.MouseWheel != 0.0f) {
        g_cam.dist *= std::pow(0.88f, io.MouseWheel);
        if (g_cam.dist < 5.0f) g_cam.dist = 5.0f;
        if (g_cam.dist > 6000.0f) g_cam.dist = 6000.0f;
    }
    // Terrain mode + a brush tool (Raise/Lower/Smooth) => left-drag brushes the
    // heightmap instead of selecting entities.
    bool brushing = (g_mode == 0 && (g_activeTool == 1 || g_activeTool == 2 || g_activeTool == 6));
    // Brush cursor ring: show the exact terrain area the brush will modify while
    // hovering with a brush tool (matches applyTerrainBrush's radius).
    {
        float gx, gy;
        if (over && brushing && g_scene.loaded && terrainHit(io.MousePos, cmin, cmax, gx, gy)) {
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
    // Left-click picks the nearest entity by screen-space projection.
    if (over && ImGui::IsMouseClicked(0) && g_scene.loaded) {
        float W = cmax.x - cmin.x, H = cmax.y - cmin.y;
        if (W > 1 && H > 1) {
            M4 vp = g_cam.viewProj(W / H);
            ImVec2 mp = io.MousePos;
            float best = 16.0f; int bi = -1;
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
            if (bi >= 0) { g_selected = bi; g_draggingEnt = true; snapEntity(bi); }
        }
    }
    // Left-drag moves the selected entity along the ground plane.
    if (g_draggingEnt && ImGui::IsMouseDown(0) && g_selected >= 0 &&
        g_selected < (int)g_scene.entities.size()) {
        if (fabsf(io.MouseDelta.x) > 0 || fabsf(io.MouseDelta.y) > 0) {
            V3 fwd = norm(g_cam.target - g_cam.eye()); fwd.y = 0; fwd = norm(fwd);
            V3 right = norm(cross(fwd, V3{0, 1, 0}));
            float k = g_cam.dist * 0.0016f + 0.02f;
            Entity& e = g_scene.entities[g_selected];
            e.pos[0] += right.x * io.MouseDelta.x * k - fwd.x * io.MouseDelta.y * k;  // world X = pos0
            e.pos[1] += right.z * io.MouseDelta.x * k - fwd.z * io.MouseDelta.y * k;  // world Z = pos1
            g_edited.insert(e.id); g_entDirty = true;   // live marker; model on release
        }
    }
    if (g_draggingEnt && !ImGui::IsMouseDown(0)) { g_draggingEnt = false; g_modelsDirty = true; commitEntity(); }
    // finalize a terrain brush stroke into one undo command
    if (g_strokeActive && !ImGui::IsMouseDown(0)) {
        EditCmd c; c.terrain = true;
        for (size_t i = 0; i < g_scene.heights.size() && i < g_strokeH0.size(); i++)
            if (g_scene.heights[i] != g_strokeH0[i]) {
                c.cells.push_back((int)i); c.h0.push_back(g_strokeH0[i]); c.h1.push_back(g_scene.heights[i]);
            }
        if (!c.cells.empty()) { g_undo.push_back(c); g_redo.clear(); }
        g_strokeActive = false; g_strokeH0.clear();
    }
}

int main(int argc, char** argv) {
    std::string loadPath, shotPath; bool selftest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
        else if (!strcmp(argv[i], "--pakmap") && i + 2 < argc) {   // mount dir, load a pak map
            vfs_mount_dir(argv[i+1]);
            std::string tmp = vfs_resolve(argv[i+2], "");
            if (!tmp.empty()) loadPath = tmp;
            i += 2;
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
            printf("overlays: roads=%zu (%zu verts) decals=%zu (%zu verts) noTex=%d\n",
                   s.roads.size(), rv, s.decals.size(), dv, noTex);
            for (size_t k=0;k<s.roads.size() && k<3;k++) printf("  road[%zu] %s\n", k, s.roads[k].tex.c_str());
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
        printf("selftest: map=%s loaded=%d entities=%d terrain=%dx%d grid=%dx%d heights=%zu\n",
               g_scene.name.c_str(), (int)g_scene.loaded, (int)g_scene.entities.size(),
               g_scene.world_w, g_scene.world_h, g_scene.grid_w, g_scene.grid_h,
               g_scene.heights.size());
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
        updateCamera(cmin, cmax);

        ImGuiIO& kio = ImGui::GetIO();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) doSave();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O)) doOpen();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undoEdit();
        if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) redoEdit();
        // [ / ] rotate the selected entity 5 degrees (yaw)
        if (!kio.WantCaptureKeyboard && g_selected >= 0 &&
            g_selected < (int)g_scene.entities.size()) {
            Entity& e = g_scene.entities[g_selected];
            if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket))  { snapEntity(g_selected); e.dir -= 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
            if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) { snapEntity(g_selected); e.dir += 5; g_edited.insert(e.id); g_modelsDirty = true; commitEntity(); }
            const char* tmp = getenv("TEMP"); if (!tmp) tmp = getenv("TMP"); if (!tmp) tmp = ".";
            std::string work = std::string(tmp) + "/cpcw_mapedit_work.map";
            // Delete: remove the entity (structural) and reload the shortened map
            if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !g_srcMap.empty()) {
                if (delete_entity_native(g_scene, e.id, work)) loadScene(work, true);
            }
            // Ctrl+D: duplicate the selected entity nearby (structural insert)
            if (kio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D) && !g_srcMap.empty()) {
                long newId = 1; for (const auto& en : g_scene.entities) if (en.id >= newId) newId = en.id + 1;
                float p[3] = { e.pos[0] + 8.0f, e.pos[1] + 8.0f, e.pos[2] };
                if (add_entity_native(g_scene, e.id, p, newId, work)) loadScene(work, true, newId);
            }
        }

        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav))
            ImGui::Text("Mode: %s | Map: %s | %d entities | WASD/arrows move  MMB orbit  RMB pan  wheel zoom",
                        kModes[g_mode].name, g_mapPath, (int)g_scene.entities.size());
        ImGui::End();

        if (g_sceneDirty && g_glReady) {
            g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene, g_showKind);
            g_vp.buildSplatTextures(g_scene, g_dataRoot);
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
                        g_showModels, g_showDots);
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
