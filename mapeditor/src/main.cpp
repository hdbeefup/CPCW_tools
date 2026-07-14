// CPCW Map Editor — portable ImGui shell (GLFW + OpenGL 3.3).
//
// Editor front-end (see docs/MAP_EDITOR.md): a modern, cross-platform
// re-imagining of the S.W.I.N.E. editor UX — a mode switcher driving a swappable
// tool/param panel over a central viewport, with File/Edit/View/Mode menus. Map
// data is loaded through cpcw_map.py's verified core via a JSON scene bridge
// (interim, until the parser is ported to C++). No game/DX9 dependency.
//
// Usage:
//   cpcw_mapeditor [--load <map.map|scene.json>] [--selftest]
//   --load      open a scene on startup (.json direct, or .map via `python
//               cpcw_map.py scene` — set CPCW_MAP_PY to the script path).
//   --selftest  load, print a summary, and exit (headless CI check; no window).

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// --- editor modes (mirrored from the S.W.I.N.E. editor; docs/MAP_EDITOR.md) --
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

// --- scene model (loaded via the JSON bridge) --------------------------------
struct Entity { std::string type, proto; float pos[3] = {0, 0, 0}; float dir = 0;
                int player = 0; long id = 0; };
struct Scene { std::string name = "(none)"; int world_w = 0, world_h = 0,
               grid_w = 0, grid_h = 0; std::vector<Entity> entities; bool loaded = false; };

static Scene g_scene;
static int   g_mode = 0, g_activeTool = 0, g_selected = -1;
static float g_brushSize = 2.0f, g_brushHeight = 0.0f, g_brushPress = 0.5f;
static bool  g_wireframe = false, g_blockmap = false;
static int   g_texMode = 2;
static char  g_mapPath[512] = "(no map loaded)";
static bool  g_showModes = true, g_showPanel = true, g_showProps = true,
             g_showViewport = true, g_showEntities = true;
static char  g_openPath[512] = "";
static bool  g_openPopup = false;

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static bool endsWithI(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)s[s.size() - n + i]) != tolower((unsigned char)suf[i]))
            return false;
    return true;
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

static bool parseScene(const std::string& txt, Scene& s) {
    try {
        auto j = nlohmann::json::parse(txt);
        s = Scene{};
        s.name = j.value("name", std::string("(map)"));
        auto t = j.value("terrain", nlohmann::json::object());
        s.world_w = t.value("world_w", 0); s.world_h = t.value("world_h", 0);
        s.grid_w = t.value("grid_w", 0);   s.grid_h = t.value("grid_h", 0);
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

static bool loadScene(const std::string& path) {
    std::string txt;
    if (endsWithI(path, ".json")) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
        std::stringstream ss; ss << f.rdbuf(); txt = ss.str();
    } else {
        const char* env = getenv("CPCW_MAP_PY");
        std::string script = env ? env : "cpcw_map.py";
        std::string cmd = "python \"" + script + "\" scene \"" + path + "\" -";
        txt = runCapture(cmd);
        if (txt.empty()) {
            fprintf(stderr, "no scene output for %s (is python + CPCW_MAP_PY set?)\n",
                    path.c_str());
            return false;
        }
    }
    Scene s;
    if (!parseScene(txt, s)) return false;
    g_scene = std::move(s); g_selected = -1;
    snprintf(g_mapPath, sizeof(g_mapPath), "%s", path.c_str());
    return true;
}

// player color palette for entity markers
static ImU32 playerColor(int p) {
    static const ImU32 c[] = {
        IM_COL32(200,200,200,255), IM_COL32(220, 70, 70,255),
        IM_COL32(70,120,220,255),  IM_COL32(80,200,90,255),
        IM_COL32(220,200,70,255),  IM_COL32(200,90,210,255),
        IM_COL32(80,210,210,255),  IM_COL32(230,140,60,255) };
    return c[((p % 8) + 8) % 8];
}

static void drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) g_openPopup = true;
        ImGui::MenuItem("New"); ImGui::MenuItem("Save", "Ctrl+S");
        ImGui::MenuItem("Save As..."); ImGui::Separator(); ImGui::MenuItem("Exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo", "Ctrl+Z"); ImGui::MenuItem("Redo", "Ctrl+R");
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X"); ImGui::MenuItem("Copy", "Ctrl+C");
        ImGui::MenuItem("Paste", "Ctrl+V"); ImGui::MenuItem("Delete", "Ctrl+Del");
        ImGui::Separator();
        ImGui::MenuItem("Select All", "Ctrl+A");
        ImGui::MenuItem("Select None", "Shift+Ctrl+A");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Modes", nullptr, &g_showModes);
        ImGui::MenuItem("Mode tools", nullptr, &g_showPanel);
        ImGui::MenuItem("Entities", nullptr, &g_showEntities);
        ImGui::MenuItem("Properties", nullptr, &g_showProps);
        ImGui::MenuItem("Viewport", nullptr, &g_showViewport);
        ImGui::Separator();
        ImGui::MenuItem("Wireframe", "W", &g_wireframe);
        ImGui::MenuItem("Blockmap", "B", &g_blockmap);
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
        ImGui::SetNextItemWidth(520);
        ImGui::InputText("##path", g_openPath, sizeof(g_openPath));
        if (ImGui::Button("Load", ImVec2(120, 0))) {
            if (loadScene(g_openPath)) ImGui::CloseCurrentPopup();
        }
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
            ImGui::SliderFloat("Height", &g_brushHeight, -50.0f, 50.0f);
            ImGui::SliderFloat("Pressure", &g_brushPress, 0.0f, 1.0f);
        } else {
            ImGui::TextDisabled("(prototype browser TODO)");
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
        for (int i = 0; i < (int)g_scene.entities.size(); i++) {
            const Entity& e = g_scene.entities[i];
            char lbl[160];
            snprintf(lbl, sizeof(lbl), "%d  %s  p%d##e%d", (int)e.id, e.type.c_str(),
                     e.player, i);
            if (ImGui::Selectable(lbl, g_selected == i)) g_selected = i;
        }
    }
    ImGui::End();
}

static void drawProperties() {
    if (!g_showProps) return;
    if (ImGui::Begin("Properties", &g_showProps)) {
        if (g_selected < 0 || g_selected >= (int)g_scene.entities.size()) {
            ImGui::TextDisabled("Nothing selected.");
        } else {
            const Entity& e = g_scene.entities[g_selected];
            ImGui::Text("Type:  %s", e.type.c_str());
            ImGui::Text("ID:    %ld", e.id);
            ImGui::Text("Player:%d", e.player);
            ImGui::Text("Proto: %s", e.proto.c_str());
            ImGui::Text("Pos:   %.2f  %.2f  %.2f", e.pos[0], e.pos[1], e.pos[2]);
            ImGui::TextDisabled("(editing writes back via cpcw_map.py — TODO)");
        }
    }
    ImGui::End();
}

static void drawViewport() {
    if (!g_showViewport) return;
    if (ImGui::Begin("Viewport", &g_showViewport)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 16) avail.x = 16;
        if (avail.y < 16) avail.y = 16;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y), IM_COL32(28, 32, 38, 255));

        // top-down map bounds
        float W = g_scene.world_w > 0 ? (float)g_scene.world_w : 512.0f;
        float H = g_scene.world_h > 0 ? (float)g_scene.world_h : 512.0f;
        float pad = 12.0f;
        float sx = (avail.x - 2 * pad) / W, sy = (avail.y - 2 * pad) / H;
        float s = sx < sy ? sx : sy;
        auto toScreen = [&](float wx, float wy) {
            return ImVec2(p.x + pad + wx * s, p.y + pad + (H - wy) * s);
        };
        // grid
        for (int gx = 0; gx <= 8; gx++) {
            ImVec2 a = toScreen(W * gx / 8, 0), b = toScreen(W * gx / 8, H);
            dl->AddLine(a, b, IM_COL32(50, 56, 64, 255));
        }
        for (int gy = 0; gy <= 8; gy++) {
            ImVec2 a = toScreen(0, H * gy / 8), b = toScreen(W, H * gy / 8);
            dl->AddLine(a, b, IM_COL32(50, 56, 64, 255));
        }
        // entity markers
        for (int i = 0; i < (int)g_scene.entities.size(); i++) {
            const Entity& e = g_scene.entities[i];
            ImVec2 c = toScreen(e.pos[0], e.pos[1]);
            dl->AddCircleFilled(c, i == g_selected ? 5.0f : 3.0f, playerColor(e.player));
            if (i == g_selected)
                dl->AddCircle(c, 8.0f, IM_COL32(255, 255, 255, 255), 0, 2.0f);
        }
        dl->AddText(ImVec2(p.x + 12, p.y + 8), IM_COL32(150, 160, 170, 255),
                    g_scene.loaded ? g_scene.name.c_str()
                                   : "3D viewport (top-down preview) — File > Open");
        // click-select nearest marker
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(0) && g_scene.loaded) {
            ImVec2 mp = ImGui::GetMousePos(); float best = 12.0f; int bi = -1;
            for (int i = 0; i < (int)g_scene.entities.size(); i++) {
                ImVec2 c = toScreen(g_scene.entities[i].pos[0], g_scene.entities[i].pos[1]);
                float d = fabsf(c.x - mp.x) + fabsf(c.y - mp.y);
                if (d < best) { best = d; bi = i; }
            }
            if (bi >= 0) g_selected = bi;
        }
        ImGui::Dummy(avail);
    }
    ImGui::End();
}

int main(int argc, char** argv) {
    std::string loadPath; bool selftest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
    }
    if (!loadPath.empty()) loadScene(loadPath);

    if (selftest) {                          // headless CI check, no window
        printf("selftest: map=%s loaded=%d entities=%d terrain=%dx%d grid=%dx%d\n",
               g_scene.name.c_str(), (int)g_scene.loaded, (int)g_scene.entities.size(),
               g_scene.world_w, g_scene.world_h, g_scene.grid_w, g_scene.grid_h);
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
    GLFWwindow* win = glfwCreateWindow(1280, 800, "CPCW Map Editor", nullptr, nullptr);
    if (!win) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

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

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        drawMenuBar();
        drawOpenPopup();
        drawModesPanel();
        drawModePanel();
        drawEntities();
        drawProperties();
        drawViewport();

        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav))
            ImGui::Text("Mode: %s | Map: %s | %d entities",
                        kModes[g_mode].name, g_mapPath, (int)g_scene.entities.size());
        ImGui::End();

        ImGui::Render();
        int w, h; glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
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
