// CPCW SRM viewer — Win32 + Direct3D 9 fixed-function pipeline.
//
// Renders a .srm model the way the Gepard engine does: native DirectX
// left-handed Y-up space, rigid/smooth skinning by the proven boneWorld rule.
// This is the ground-truth companion to the Blender importer.
//
// Usage:
//   cpcw_viewer <model.srm> [dataRoot] [--shot out.bmp] [--skin full|none]
//
// Interactive keys: LMB orbit, RMB pan, wheel zoom,
//   W wireframe, T textures, F skin full/none, C cull cycle, R reset, Esc quit.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include "srm_model.h"
#include "dds.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

#define FVF_MODEL (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static IDirect3D9*        g_d3d = nullptr;
static IDirect3DDevice9*  g_dev = nullptr;
static IDirect3D9Ex*      g_d3dEx = nullptr;   // non-null when running the Ex path (e.g. under RDP)
static HWND               g_hwnd = nullptr;
// D3D9Ex forbids D3DPOOL_MANAGED, so pick pools/usage per device type.
static D3DPOOL            g_bufPool = D3DPOOL_MANAGED;   // VB/IB pool
static D3DPOOL            g_texPool = D3DPOOL_MANAGED;   // texture pool
static DWORD              g_texUsage = 0;                // D3DUSAGE_DYNAMIC on Ex (so DEFAULT is lockable)
static int                g_w = 1280, g_h = 960;

struct GpuMesh {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9*  ib = nullptr;
    IDirect3DTexture9*      tex = nullptr;   // shared (not owned) pointer into g_texCache
    int numVerts = 0, numTris = 0;
};

static SrmModel            g_model;
static std::vector<GpuMesh> g_gpu;
static std::map<std::string, IDirect3DTexture9*> g_texCache;
static std::map<std::string, std::string> g_texIndex;   // basename(lower,no ext) -> full path
static std::string         g_dataRoot;
static std::string         g_srmPath;
static bool                g_explicitRoot = false;   // dataRoot came from the command line

// camera / state
static float g_yaw = 35, g_pitch = 22, g_dist = 10;
static Vec3  g_center;
static float g_radius = 5;
static bool  g_wire = false, g_useTex = true;
static SkinMode g_skin = SKIN_AUTO;
static Variant g_variant = VAR_ALL;
static const char* skinName(SkinMode m) { return m==SKIN_AUTO?"AUTO":(m==SKIN_FULL?"FULL":"NONE"); }
static int   g_cull = 1;   // 0=none 1=CCW 2=CW
static bool  g_dragL = false, g_dragR = false;
static POINT g_lastMouse;

// ---------------------------------------------------------------------------
// Texture index (recursive) + resolution
// ---------------------------------------------------------------------------
static std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string baseNoExt(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    std::string b = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = b.find_last_of('.');
    if (dot != std::string::npos) b = b.substr(0, dot);
    return lower(b);
}
static void indexDir(const std::string& dir, int& budget) {
    if (budget <= 0) return;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> subdirs;
    do {
        if (fd.cFileName[0] == '.' && (fd.cFileName[1] == 0 || (fd.cFileName[1]=='.'&&fd.cFileName[2]==0))) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subdirs.push_back(full);
        else {
            std::string nm = fd.cFileName;
            if (nm.size() > 4 && lower(nm.substr(nm.size()-4)) == ".dds") {
                std::string key = baseNoExt(nm);
                if (!g_texIndex.count(key)) g_texIndex[key] = full;
                if (--budget <= 0) break;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (auto& sd : subdirs) { if (budget <= 0) break; indexDir(sd, budget); }
}
static std::string parentDir(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? "" : p.substr(0, slash);
}
static std::string baseName(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

static void updateTitle();   // forward decl

// ---------------------------------------------------------------------------
// Matrices (D3D row-vector, left-handed)
// ---------------------------------------------------------------------------
static D3DMATRIX perspectiveFovLH(float fovY, float aspect, float zn, float zf) {
    float yS = 1.0f / tanf(fovY / 2), xS = yS / aspect;
    D3DMATRIX m = {};
    m._11 = xS; m._22 = yS; m._33 = zf/(zf-zn); m._34 = 1; m._43 = -zn*zf/(zf-zn);
    return m;
}
static D3DMATRIX lookAtLH(Vec3 eye, Vec3 at, Vec3 up) {
    Vec3 z = normalize(at - eye);
    Vec3 x = normalize(cross(up, z));
    Vec3 y = cross(z, x);
    D3DMATRIX m = {};
    m._11=x.x; m._12=y.x; m._13=z.x; m._14=0;
    m._21=x.y; m._22=y.y; m._23=z.y; m._24=0;
    m._31=x.z; m._32=y.z; m._33=z.z; m._34=0;
    m._41=-dot(x,eye); m._42=-dot(y,eye); m._43=-dot(z,eye); m._44=1;
    return m;
}

// ---------------------------------------------------------------------------
// GPU build
// ---------------------------------------------------------------------------
static IDirect3DTexture9* loadTexture(const std::string& basename) {
    if (basename.empty()) return nullptr;
    std::string key = baseNoExt(basename);
    auto c = g_texCache.find(key);
    if (c != g_texCache.end()) return c->second;
    IDirect3DTexture9* tex = nullptr;
    auto it = g_texIndex.find(key);
    if (it != g_texIndex.end()) {
        DdsImage img = dds_load(it->second);
        if (img.ok) {
            if (SUCCEEDED(g_dev->CreateTexture(img.width, img.height, 1, g_texUsage,
                    D3DFMT_A8R8G8B8, g_texPool, &tex, nullptr))) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(tex->LockRect(0, &lr, nullptr, 0))) {
                    for (int y = 0; y < img.height; y++) {
                        uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
                        const uint8_t* src = &img.rgba[(size_t)y * img.width * 4];
                        for (int x = 0; x < img.width; x++) {
                            // rgba(src) -> BGRA(dst) for A8R8G8B8
                            dst[x*4+0] = src[x*4+2];
                            dst[x*4+1] = src[x*4+1];
                            dst[x*4+2] = src[x*4+0];
                            dst[x*4+3] = src[x*4+3];
                        }
                    }
                    tex->UnlockRect(0);
                }
            }
        }
    }
    g_texCache[key] = tex;   // cache misses too (as null)
    return tex;
}

static void releaseGpu() {
    for (auto& g : g_gpu) { if (g.vb) g.vb->Release(); if (g.ib) g.ib->Release(); }
    g_gpu.clear();
}

static void buildGpu() {
    releaseGpu();
    std::vector<RenderMesh> meshes;
    srm_build_render(g_model, g_skin, g_variant, meshes);

    // bounds over *referenced* verts (so filtered-out floaters don't skew framing)
    bool first = true;
    Vec3 lo, hi;
    for (auto& rm : meshes)
        for (uint32_t idx : rm.indices) {
            if (idx >= rm.verts.size()) continue;
            const RVertex& v = rm.verts[idx];
            Vec3 p(v.x, v.y, v.z);
            if (first) { lo = hi = p; first = false; }
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
    if (!first) {
        g_center = (lo + hi) * 0.5f;
        Vec3 d = hi - lo;
        g_radius = std::max(0.001f, std::max(d.x, std::max(d.y, d.z)) * 0.5f);
    }

    for (auto& rm : meshes) {
        if (rm.verts.empty() || rm.indices.size() < 3) continue;
        GpuMesh gm;
        gm.numVerts = (int)rm.verts.size();
        gm.numTris = (int)rm.indices.size() / 3;
        int vbSize = gm.numVerts * (int)sizeof(RVertex);
        if (FAILED(g_dev->CreateVertexBuffer(vbSize, D3DUSAGE_WRITEONLY, FVF_MODEL,
                g_bufPool, &gm.vb, nullptr))) continue;
        void* pv = nullptr; gm.vb->Lock(0, 0, &pv, 0);
        memcpy(pv, rm.verts.data(), vbSize); gm.vb->Unlock();

        int ibSize = (int)rm.indices.size() * (int)sizeof(uint32_t);
        if (FAILED(g_dev->CreateIndexBuffer(ibSize, D3DUSAGE_WRITEONLY, D3DFMT_INDEX32,
                g_bufPool, &gm.ib, nullptr))) { gm.vb->Release(); continue; }
        void* pi = nullptr; gm.ib->Lock(0, 0, &pi, 0);
        memcpy(pi, rm.indices.data(), ibSize); gm.ib->Unlock();

        gm.tex = loadTexture(rm.diffuseTex);
        g_gpu.push_back(gm);
    }
    printf("built %d gpu meshes (skin=%s)\n", (int)g_gpu.size(), skinName(g_skin));
}

static void resetCamera() {
    g_yaw = 35; g_pitch = 22; g_dist = g_radius * 3;
}

// Load (or reload) a model + its textures and rebuild GPU buffers. Used at
// startup and on drag-and-drop.
static bool loadModel(const std::string& path) {
    // drop old GPU + textures
    releaseGpu();
    for (auto& kv : g_texCache) if (kv.second) kv.second->Release();
    g_texCache.clear();
    g_texIndex.clear();

    SrmModel m; std::string err;
    if (!srm_parse(path, m, &err)) { printf("parse failed: %s\n", err.c_str()); return false; }
    g_model = std::move(m);
    g_srmPath = path;
    printf("parsed %s: %d nodes, %d meshes\n", path.c_str(),
           (int)g_model.nodes.size(), (int)g_model.meshes.size());

    if (!g_explicitRoot) g_dataRoot = parentDir(path);
    { int budget = 60000; indexDir(g_dataRoot, budget); }
    { int budget = 60000; indexDir(parentDir(path), budget); }   // local textures win
    printf("texture index: %d dds under %s\n", (int)g_texIndex.size(), g_dataRoot.c_str());

    buildGpu();
    resetCamera();
    if (g_hwnd) updateTitle();
    return true;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
static void setStates() {
    g_dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_LIGHTING, TRUE);
    g_dev->SetRenderState(D3DRS_NORMALIZENORMALS, TRUE);
    g_dev->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(70, 72, 78));
    g_dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    D3DCULL cull = g_cull == 0 ? D3DCULL_NONE : (g_cull == 1 ? D3DCULL_CCW : D3DCULL_CW);
    g_dev->SetRenderState(D3DRS_CULLMODE, cull);
    g_dev->SetRenderState(D3DRS_FILLMODE, g_wire ? D3DFILL_WIREFRAME : D3DFILL_SOLID);

    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
}

static void render() {
    // camera
    float cp = cosf(g_pitch * 3.14159265f/180), sp = sinf(g_pitch * 3.14159265f/180);
    float cy = cosf(g_yaw   * 3.14159265f/180), sy = sinf(g_yaw   * 3.14159265f/180);
    Vec3 dir(cp * sy, sp, cp * cy);
    Vec3 eye = g_center + dir * g_dist;
    D3DMATRIX view = lookAtLH(eye, g_center, Vec3(0, 1, 0));
    D3DMATRIX proj = perspectiveFovLH(3.14159265f/4, (float)g_w/g_h, g_radius*0.02f, g_radius*40 + g_dist*4);
    D3DMATRIX world; ZeroMemory(&world, sizeof(world)); world._11=world._22=world._33=world._44=1;

    g_dev->SetTransform(D3DTS_WORLD, &world);
    g_dev->SetTransform(D3DTS_VIEW, &view);
    g_dev->SetTransform(D3DTS_PROJECTION, &proj);

    // headlight from the camera
    D3DLIGHT9 light = {};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = light.Diffuse.g = light.Diffuse.b = 1.0f;
    Vec3 ld = normalize(g_center - eye);
    light.Direction.x = ld.x; light.Direction.y = ld.y; light.Direction.z = ld.z;
    g_dev->SetLight(0, &light);
    g_dev->LightEnable(0, TRUE);

    D3DMATERIAL9 mat = {};
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1.0f;
    mat.Ambient = mat.Diffuse;
    g_dev->SetMaterial(&mat);

    g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(30, 32, 38), 1.0f, 0);
    g_dev->BeginScene();
    setStates();
    g_dev->SetFVF(FVF_MODEL);
    for (auto& g : g_gpu) {
        bool textured = g_useTex && g.tex && !g_wire;
        if (textured) {
            g_dev->SetTexture(0, g.tex);
            g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        } else {
            g_dev->SetTexture(0, nullptr);
            g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        }
        g_dev->SetStreamSource(0, g.vb, 0, sizeof(RVertex));
        g_dev->SetIndices(g.ib);
        g_dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, g.numVerts, 0, g.numTris);
    }
    g_dev->EndScene();
    g_dev->Present(nullptr, nullptr, nullptr, nullptr);
}

// ---------------------------------------------------------------------------
// Screenshot (offscreen backbuffer capture -> 24-bit BMP)
// ---------------------------------------------------------------------------
static bool saveShot(const std::string& path) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return false;
    D3DSURFACE_DESC desc; bb->GetDesc(&desc);
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(g_dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
            D3DPOOL_SYSTEMMEM, &sys, nullptr))) { bb->Release(); return false; }
    bool ok = SUCCEEDED(g_dev->GetRenderTargetData(bb, sys));
    if (ok) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            int W = desc.Width, H = desc.Height;
            int rowBytes = (W * 3 + 3) & ~3;
            int imgSize = rowBytes * H;
            BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
            fh.bfType = 0x4D42;
            fh.bfOffBits = sizeof(fh) + sizeof(ih);
            fh.bfSize = fh.bfOffBits + imgSize;
            ih.biSize = sizeof(ih); ih.biWidth = W; ih.biHeight = H;
            ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                std::vector<uint8_t> row(rowBytes, 0);
                for (int y = H - 1; y >= 0; y--) {   // BMP bottom-up
                    uint8_t* src = (uint8_t*)lr.pBits + y * lr.Pitch;  // BGRA
                    for (int x = 0; x < W; x++) {
                        row[x*3+0] = src[x*4+0];
                        row[x*3+1] = src[x*4+1];
                        row[x*3+2] = src[x*4+2];
                    }
                    fwrite(row.data(), 1, rowBytes, f);
                }
                fclose(f);
                printf("wrote %s (%dx%d)\n", path.c_str(), W, H);
            } else ok = false;
            sys->UnlockRect();
        } else ok = false;
    }
    sys->Release(); bb->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Win32
// ---------------------------------------------------------------------------
static void updateTitle() {
    const char* var = g_variant==VAR_ALL?"all":(g_variant==VAR_STANDARD?"standard":"upgraded");
    char buf[400];
    snprintf(buf, sizeof(buf), "CPCW Viewer  -  %s  |  skin=%s  variant=%s  tex=%s  cull=%s  %s",
             g_srmPath.empty() ? "(no model)" : baseName(g_srmPath).c_str(),
             skinName(g_skin), var, g_useTex?"on":"off",
             g_cull==0?"none":(g_cull==1?"CCW":"CW"), g_wire?"[wire]":"");
    SetWindowTextA(g_hwnd, buf);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        char path[MAX_PATH] = {0};
        if (DragQueryFileA(drop, 0, path, MAX_PATH)) loadModel(path);
        DragFinish(drop);
        return 0;
    }
    case WM_LBUTTONDOWN: g_dragL = true; SetCapture(h); GetCursorPos(&g_lastMouse); return 0;
    case WM_RBUTTONDOWN: g_dragR = true; SetCapture(h); GetCursorPos(&g_lastMouse); return 0;
    case WM_LBUTTONUP: g_dragL = false; ReleaseCapture(); return 0;
    case WM_RBUTTONUP: g_dragR = false; ReleaseCapture(); return 0;
    case WM_MOUSEMOVE: {
        POINT p; GetCursorPos(&p);
        int dx = p.x - g_lastMouse.x, dy = p.y - g_lastMouse.y;
        if (g_dragL) { g_yaw -= dx * 0.4f; g_pitch += dy * 0.4f;
            if (g_pitch > 89) g_pitch = 89; if (g_pitch < -89) g_pitch = -89; }
        else if (g_dragR) { float s = g_dist * 0.0015f;
            float cy = cosf(g_yaw*3.14159f/180), sy = sinf(g_yaw*3.14159f/180);
            g_center = g_center + Vec3(cy, 0, -sy) * (dx * s);
            g_center = g_center + Vec3(0, 1, 0) * (dy * s); }
        g_lastMouse = p; return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = GET_WHEEL_DELTA_WPARAM(w);
        g_dist *= (d > 0) ? 0.9f : 1.1f;
        if (g_dist < g_radius * 0.05f) g_dist = g_radius * 0.05f;
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_ESCAPE: PostQuitMessage(0); break;
        case 'W': g_wire = !g_wire; updateTitle(); break;
        case 'T': g_useTex = !g_useTex; updateTitle(); break;
        case 'C': g_cull = (g_cull + 1) % 3; updateTitle(); break;
        case 'R': resetCamera(); break;
        case 'F': g_skin = (SkinMode)((g_skin + 1) % 3); buildGpu(); updateTitle(); break;
        case 'V': g_variant = (Variant)((g_variant + 1) % 3); buildGpu(); updateTitle(); break;
        case 'P': {
            static int n = 0;
            char name[64]; snprintf(name, sizeof(name), "cpcw_shot_%03d.bmp", n++);
            render(); saveShot(name);
            break;
        }
        }
        return 0;
    }
    return DefWindowProcA(h, msg, w, l);
}

static bool initD3D(bool visible) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "CPCWViewerWnd"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, g_w, g_h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("CPCWViewerWnd", "CPCW Viewer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right-r.left, r.bottom-r.top,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    ShowWindow(g_hwnd, visible ? SW_SHOW : SW_HIDE);
    DragAcceptFiles(g_hwnd, TRUE);

    // Prefer Direct3D9Ex: plain D3D9 enumerates ZERO adapters inside a Remote
    // Desktop session (D3DERR_INVALIDCALL on CreateDevice); the Ex interface
    // works against the RDP WDDM display. Fall back to legacy D3D9 otherwise.
    typedef HRESULT (WINAPI *PFN_D3D9EX)(UINT, IDirect3D9Ex**);
    HMODULE d3d9mod = GetModuleHandleA("d3d9.dll");
    if (!d3d9mod) d3d9mod = LoadLibraryA("d3d9.dll");
    PFN_D3D9EX pCreate9Ex = d3d9mod ? (PFN_D3D9EX)GetProcAddress(d3d9mod, "Direct3DCreate9Ex") : nullptr;
    HRESULT hrEx = pCreate9Ex ? pCreate9Ex(D3D_SDK_VERSION, &g_d3dEx) : E_NOTIMPL;
    if (SUCCEEDED(hrEx) && g_d3dEx) {
        g_d3d = g_d3dEx;   // IDirect3D9Ex derives IDirect3D9
    } else {
        g_d3dEx = nullptr;
        g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    }
    if (!g_d3d) return false;

    // In windowed mode the backbuffer format must match the current desktop
    // display mode (hardcoding X8R8G8B8 gives D3DERR_INVALIDCALL when the
    // desktop — e.g. an RDP session — is a different depth).
    UINT adapters = g_d3d->GetAdapterCount();
    D3DDISPLAYMODE mode = {};
    HRESULT hrMode = g_d3d->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &mode);
    D3DFORMAT bbFmt = D3DFMT_X8R8G8B8;
    if (SUCCEEDED(hrMode) && mode.Format != D3DFMT_UNKNOWN)
        bbFmt = mode.Format;

    // Pick a supported depth format for that backbuffer.
    D3DFORMAT depthFmt = D3DFMT_D24S8;
    const D3DFORMAT depthCandidates[] = { D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16 };
    for (D3DFORMAT df : depthCandidates) {
        if (SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, bbFmt,
                D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, df))) { depthFmt = df; break; }
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = bbFmt;
    pp.BackBufferWidth = g_w; pp.BackBufferHeight = g_h;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = depthFmt;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // Try HAL (hardware, then software VP); fall back to the REF rasterizer.
    // The REF path matters under Remote Desktop, where the physical GPU / HAL
    // is often unavailable to the session (headless --shot still works).
    struct Attempt { D3DDEVTYPE type; DWORD vp; const char* name; };
    const Attempt attempts[] = {
        { D3DDEVTYPE_HAL, D3DCREATE_HARDWARE_VERTEXPROCESSING, "HAL/hw" },
        { D3DDEVTYPE_HAL, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "HAL/sw" },
        { D3DDEVTYPE_REF, D3DCREATE_SOFTWARE_VERTEXPROCESSING, "REF" },
        { D3DDEVTYPE_SW,  D3DCREATE_SOFTWARE_VERTEXPROCESSING, "SW"  },
    };
    for (const auto& a : attempts) {
        HRESULT hr;
        if (g_d3dEx) {
            IDirect3DDevice9Ex* devEx = nullptr;
            hr = g_d3dEx->CreateDeviceEx(D3DADAPTER_DEFAULT, a.type, g_hwnd, a.vp, &pp, nullptr, &devEx);
            if (SUCCEEDED(hr)) g_dev = devEx;   // IDirect3DDevice9Ex derives IDirect3DDevice9
        } else {
            hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, a.type, g_hwnd, a.vp, &pp, &g_dev);
        }
        if (SUCCEEDED(hr)) {
            if (g_d3dEx) {   // Ex disallows MANAGED; use DEFAULT (+ dynamic textures so they lock)
                g_bufPool = D3DPOOL_DEFAULT; g_texPool = D3DPOOL_DEFAULT; g_texUsage = D3DUSAGE_DYNAMIC;
            } else {
                g_bufPool = D3DPOOL_MANAGED; g_texPool = D3DPOOL_MANAGED; g_texUsage = 0;
            }
            printf("D3D device: %s (%s)\n", a.name, g_d3dEx ? "D3D9Ex" : "D3D9");
            return true;
        }
    }
    printf("D3D device creation failed. adapters=%u exHr=0x%08lX\n",
           adapters, (unsigned long)hrEx);
    if (adapters == 0)
        printf("  No display available to Direct3D. If you are on Remote Desktop,\n"
               "  the session must be CONNECTED (not disconnected) for D3D9 to work.\n"
               "  (Use --info for headless model inspection without a display.)\n");
    return false;
}

int main(int argc, char** argv) {
    std::string srmPath, shotPath;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--skin" && i + 1 < argc) { std::string s = argv[++i];
            g_skin = (s=="none")?SKIN_NONE:(s=="full")?SKIN_FULL:SKIN_AUTO; }
        else if (a == "--variant" && i + 1 < argc) { std::string s = argv[++i];
            g_variant = (s=="standard")?VAR_STANDARD:(s=="upgraded")?VAR_UPGRADED:VAR_ALL; }
        else if (srmPath.empty()) srmPath = a;
        else if (g_dataRoot.empty()) g_dataRoot = a;
    }
    g_explicitRoot = !g_dataRoot.empty();

    // Headless info mode: no D3D device needed. Verifies parse + skinning +
    // variant filtering (usable even when no display is available, e.g. a
    // disconnected RDP session).
    bool infoMode = false;
    for (int i = 1; i < argc; i++) if (std::string(argv[i]) == "--info") infoMode = true;
    if (infoMode) {
        if (srmPath.empty()) { printf("--info needs a model path\n"); return 1; }
        SrmModel m; std::string err;
        if (!srm_parse(srmPath, m, &err)) { printf("parse failed: %s\n", err.c_str()); return 1; }
        printf("model: %s\n  nodes=%d meshes=%d\n", srmPath.c_str(),
               (int)m.nodes.size(), (int)m.meshes.size());
        struct { const char* n; Variant v; } vs[] = {
            {"ALL", VAR_ALL}, {"STANDARD", VAR_STANDARD}, {"UPGRADED", VAR_UPGRADED} };
        for (auto& e : vs) {
            std::vector<RenderMesh> rm;
            srm_build_render(m, SKIN_FULL, e.v, rm);
            long tris = 0, verts = 0; int textured = 0;
            for (auto& r : rm) { tris += (long)r.indices.size()/3; verts += (long)r.verts.size();
                                 if (!r.diffuseTex.empty()) textured++; }
            printf("  variant %-9s: %2d meshes, %6ld tris, %6ld verts, %d textured\n",
                   e.n, (int)rm.size(), tris, verts, textured);
        }
        return 0;
    }

    if (srmPath.empty() && !shotPath.empty()) { printf("--shot needs a model path\n"); return 1; }
    if (srmPath.empty()) {
        char file[MAX_PATH] = {0};
        OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFilter = "SRM models\0*.srm\0All\0*.*\0"; ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST;
        if (GetOpenFileNameA(&ofn)) srmPath = file;   // empty is OK: open with no model, drag one in
    }

    bool interactive = shotPath.empty();
    if (!initD3D(interactive)) { printf("D3D init failed\n"); return 1; }

    if (!srmPath.empty() && !loadModel(srmPath) && !interactive) {
        if (g_dev) g_dev->Release(); if (g_d3d) g_d3d->Release();
        return 1;
    }
    updateTitle();

    if (!shotPath.empty()) {
        render(); render();   // a couple of frames to settle, then capture
        bool ok = saveShot(shotPath);
        if (g_dev) g_dev->Release(); if (g_d3d) g_d3d->Release();
        return ok ? 0 : 2;
    }

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        else render();
    }
    releaseGpu();
    for (auto& kv : g_texCache) if (kv.second) kv.second->Release();
    if (g_dev) g_dev->Release(); if (g_d3d) g_d3d->Release();
    return 0;
}
