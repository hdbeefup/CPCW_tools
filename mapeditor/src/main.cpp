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
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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
static bool  g_orbiting = false, g_panning = false;

static void glfwError(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

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
    std::string txt, baseDir;
    if (endsWithI(path, ".json")) {
        std::ifstream f(path, std::ios::binary);
        if (!f) { fprintf(stderr, "cannot open %s\n", path.c_str()); return false; }
        std::stringstream ss; ss << f.rdbuf(); txt = ss.str();
        baseDir = dirOf(path);
    } else {
        const char* env = getenv("CPCW_MAP_PY");
        std::string script = env ? env : "cpcw_map.py";
        const char* tmp = getenv("TEMP"); if (!tmp) tmp = getenv("TMP"); if (!tmp) tmp = ".";
        std::string tj = std::string(tmp) + "/cpcw_mapedit_scene.json";
        std::string cmd = "python \"" + script + "\" scene \"" + path + "\" \"" + tj + "\"";
        runCapture(cmd);                                   // writes tj + .r32 sidecar
        std::ifstream f(tj, std::ios::binary);
        if (!f) { fprintf(stderr, "scene export failed for %s (python/CPCW_MAP_PY?)\n",
                          path.c_str()); return false; }
        std::stringstream ss; ss << f.rdbuf(); txt = ss.str();
        baseDir = dirOf(tj);
    }
    Scene s;
    if (!parseScene(txt, baseDir, s)) return false;
    g_scene = std::move(s); g_selected = -1; g_sceneDirty = true;
    snprintf(g_mapPath, sizeof(g_mapPath), "%s", path.c_str());
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
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Modes", nullptr, &g_showModes);
        ImGui::MenuItem("Mode tools", nullptr, &g_showPanel);
        ImGui::MenuItem("Entities", nullptr, &g_showEntities);
        ImGui::MenuItem("Properties", nullptr, &g_showProps);
        ImGui::Separator();
        ImGui::MenuItem("Wireframe", "W", &g_wireframe);
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
            ImGui::SliderFloat("Height", &g_brushHeight, -50.0f, 50.0f);
            ImGui::SliderFloat("Pressure", &g_brushPress, 0.0f, 1.0f);
        } else ImGui::TextDisabled("(prototype browser TODO)");
    }
    ImGui::End();
}
static void drawEntities() {
    if (!g_showEntities) return;
    char title[64];
    snprintf(title, sizeof(title), "Entities (%d)###ents", (int)g_scene.entities.size());
    if (ImGui::Begin(title, &g_showEntities)) {
        if (!g_scene.loaded) ImGui::TextDisabled("No map loaded (File > Open).");
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
            const Entity& e = g_scene.entities[g_selected];
            ImGui::Text("Type:  %s", e.type.c_str());
            ImGui::Text("ID:    %ld", e.id);
            ImGui::Text("Player:%d", e.player);
            ImGui::TextWrapped("Proto: %s", e.proto.c_str());
            ImGui::Text("Pos:   %.2f  %.2f  %.2f", e.pos[0], e.pos[1], e.pos[2]);
            ImGui::TextDisabled("(editing writes back via cpcw_map.py apply — TODO)");
        }
    }
    ImGui::End();
}

// camera navigation over the central viewport region
static void updateCamera(const ImVec2& cmin, const ImVec2& cmax) {
    ImGuiIO& io = ImGui::GetIO();
    bool over = ImGui::IsMouseHoveringRect(cmin, cmax, false) && !io.WantCaptureMouse;
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
        float k = g_cam.dist * 0.0016f;
        g_cam.target = g_cam.target - right * (io.MouseDelta.x * k)
                                    + up    * (io.MouseDelta.y * k);
    }
    if (over && io.MouseWheel != 0.0f) {
        g_cam.dist *= std::pow(0.88f, io.MouseWheel);
        if (g_cam.dist < 5.0f) g_cam.dist = 5.0f;
        if (g_cam.dist > 6000.0f) g_cam.dist = 6000.0f;
    }
}

int main(int argc, char** argv) {
    std::string loadPath, shotPath; bool selftest = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--load") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shotPath = argv[++i];
        else if (!strcmp(argv[i], "--selftest")) selftest = true;
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
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    if (!loadGLCore()) fprintf(stderr, "warning: some GL functions failed to load\n");
    g_glReady = true;
    g_vp.init();

    // headless render-to-BMP: one frame of the 3D scene, full framebuffer, exit.
    if (!shotPath.empty()) {
        g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene); g_sceneDirty = false;
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
        g_vp.render(g_cam, (float)fbw / (float)fbh, g_wireframe, g_selected);
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

        ImGui::SetNextWindowBgAlpha(0.35f);
        if (ImGui::Begin("##status", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav))
            ImGui::Text("Mode: %s | Map: %s | %d entities | MMB orbit  RMB pan  wheel zoom",
                        kModes[g_mode].name, g_mapPath, (int)g_scene.entities.size());
        ImGui::End();

        if (g_sceneDirty && g_glReady) {
            g_vp.buildTerrain(g_scene); g_vp.buildEntities(g_scene); g_sceneDirty = false;
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
            g_vp.render(g_cam, (float)vw / (float)vh, g_wireframe, g_selected);
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
