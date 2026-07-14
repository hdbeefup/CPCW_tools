// CPCW Map Editor — portable ImGui shell (GLFW + OpenGL 3.3).
//
// This is the editor front-end scaffold (milestone M4): a modern, cross-platform
// re-imagining of the S.W.I.N.E. editor's UX (see docs/MAP_EDITOR.md). It stands
// up the interface skeleton — a mode switcher driving a swappable tool/param
// panel over a central 3D viewport, with the File/Edit/View/Mode menus — so the
// per-mode tools and the map data wiring (via cpcw_map.py's verified core) can be
// filled in on top. No game/DX9 dependency; builds on Win/Linux/macOS.

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstring>

// The editor modes, mirrored from the S.W.I.N.E. editor (docs/MAP_EDITOR.md).
struct Mode { const char* name; const char* focus; const char* tools; };
static const Mode kModes[] = {
    {"Vertex / Terrain", "terrain", "Grab  Raise  Lower  SetPlane  Raise>Plane  Lower>Plane  Smooth  Blend  TileFill  Area"},
    {"Spline",           "terrain", "New river  Node move  Close loop  Altitude  Width  Texture"},
    {"Object / Doodad",  "object",  "Place  Move  Lift  Rotate  Tilt  Align"},
    {"Unit",             "object",  "Place  Move  Rotate"},
    {"Ambient",          "object",  "Place  Move  Lift  Distance"},
    {"Shader / Decals",  "terrain", "Place  Move  Rotate  Z-order"},
    {"Lake / Water",     "terrain", "Place  Move  Lift"},
    {"Light",            "global",  "(settings panel)"},
    {"Trigger",          "logic",   "Locations  Triggers  Conditions  Actions"},
};
static const int kNumModes = (int)(sizeof(kModes) / sizeof(kModes[0]));

static int   g_mode        = 0;
static int   g_activeTool  = 1;             // per-mode tool index (stub)
static float g_brushSize   = 2.0f;          // terrain brush params
static float g_brushHeight = 0.0f;
static float g_brushPress  = 0.5f;
static bool  g_wireframe   = false;
static int   g_texMode     = 2;             // 0 none / 1 sketch / 2 real
static bool  g_blockmap    = false;
static char  g_mapPath[512] = "(no map loaded)";
static bool  g_showViewport = true, g_showModes = true, g_showPanel = true, g_showProps = true;

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static void drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        ImGui::MenuItem("New");
        ImGui::MenuItem("Open...", "Ctrl+O");
        ImGui::MenuItem("Save", "Ctrl+S");
        ImGui::MenuItem("Save As...");
        ImGui::Separator();
        ImGui::MenuItem("Exit");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        ImGui::MenuItem("Undo", "Ctrl+Z");
        ImGui::MenuItem("Redo", "Ctrl+R");
        ImGui::Separator();
        ImGui::MenuItem("Cut", "Ctrl+X");
        ImGui::MenuItem("Copy", "Ctrl+C");
        ImGui::MenuItem("Paste", "Ctrl+V");
        ImGui::MenuItem("Delete", "Ctrl+Del");
        ImGui::Separator();
        ImGui::MenuItem("Select All", "Ctrl+A");
        ImGui::MenuItem("Select None", "Shift+Ctrl+A");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Modes panel", nullptr, &g_showModes);
        ImGui::MenuItem("Mode tools panel", nullptr, &g_showPanel);
        ImGui::MenuItem("Properties panel", nullptr, &g_showProps);
        ImGui::MenuItem("Viewport", nullptr, &g_showViewport);
        ImGui::Separator();
        ImGui::MenuItem("Wireframe", "W", &g_wireframe);
        ImGui::MenuItem("Blockmap", "B", &g_blockmap);
        if (ImGui::BeginMenu("Textures")) {
            ImGui::MenuItem("None", nullptr, g_texMode == 0);
            ImGui::MenuItem("Sketch", nullptr, g_texMode == 1);
            ImGui::MenuItem("Real", nullptr, g_texMode == 2);
            ImGui::EndMenu();
        }
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

static void drawModesPanel() {
    if (!g_showModes) return;
    if (ImGui::Begin("Modes", &g_showModes)) {
        for (int i = 0; i < kNumModes; i++) {
            if (ImGui::Selectable(kModes[i].name, g_mode == i)) { g_mode = i; g_activeTool = 0; }
        }
    }
    ImGui::End();
}

// The per-mode tool/param panel — swaps content with the active mode.
static void drawModePanel() {
    if (!g_showPanel) return;
    const Mode& m = kModes[g_mode];
    if (ImGui::Begin(m.name, &g_showPanel)) {
        ImGui::TextDisabled("%s mode", m.focus);
        ImGui::SeparatorText("Tools");
        // Split the tool list into selectable buttons.
        char buf[256]; snprintf(buf, sizeof(buf), "%s", m.tools);
        int idx = 0; char* tok = strtok(buf, " ");
        while (tok) {
            if (*tok) {
                if (ImGui::Selectable(tok, g_activeTool == idx, 0, ImVec2(0, 0)))
                    g_activeTool = idx;
                idx++;
            }
            tok = strtok(nullptr, " ");
        }
        ImGui::SeparatorText("Parameters");
        if (m.focus[0] == 't') {                     // terrain-ish: brush params
            ImGui::SliderFloat("Size", &g_brushSize, 0.5f, 8.0f);
            ImGui::SliderFloat("Height", &g_brushHeight, -50.0f, 50.0f);
            ImGui::SliderFloat("Pressure", &g_brushPress, 0.0f, 1.0f);
        } else if (g_mode == 2 || g_mode == 3 || g_mode == 4) {  // object modes
            ImGui::TextWrapped("Prototype browser (folder / file) goes here.");
            ImGui::BeginChild("proto", ImVec2(0, 120), ImGuiChildFlags_None);
            ImGui::TextDisabled("(no data loaded)");
            ImGui::EndChild();
        } else {
            ImGui::TextDisabled("(panel: %s)", m.name);
        }
    }
    ImGui::End();
}

static void drawProperties() {
    if (!g_showProps) return;
    if (ImGui::Begin("Properties", &g_showProps)) {
        ImGui::TextDisabled("Selection properties");
        ImGui::Separator();
        ImGui::TextWrapped("Nothing selected. Placed objects will expose their "
                           "schema-driven fields here (Player, Level, HP, ...).");
    }
    ImGui::End();
}

static void drawViewport() {
    if (!g_showViewport) return;
    if (ImGui::Begin("Viewport", &g_showViewport)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + avail.x, p.y + avail.y),
                          IM_COL32(28, 32, 38, 255));
        // placeholder grid
        const float step = 32.0f;
        for (float x = 0; x < avail.x; x += step)
            dl->AddLine(ImVec2(p.x + x, p.y), ImVec2(p.x + x, p.y + avail.y),
                        IM_COL32(50, 56, 64, 255));
        for (float y = 0; y < avail.y; y += step)
            dl->AddLine(ImVec2(p.x, p.y + y), ImVec2(p.x + avail.x, p.y + y),
                        IM_COL32(50, 56, 64, 255));
        dl->AddText(ImVec2(p.x + 12, p.y + 10), IM_COL32(150, 160, 170, 255),
                    "3D viewport — orbit (MMB) / pan (RMB) / zoom (wheel)  [renderer TODO]");
        ImGui::Dummy(avail);
    }
    ImGui::End();
}

int main(int, char**) {
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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
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
        drawModesPanel();
        drawModePanel();
        drawProperties();
        drawViewport();

        // status line (simple overlay window, bottom-left)
        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
            ImGui::Text("Mode: %s | Map: %s", kModes[g_mode].name, g_mapPath);
        }
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
