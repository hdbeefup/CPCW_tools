#include "text.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <cstring>
#include <cstdint>

// ASCII 32..126 laid out in a 16-wide grid.
static const int FIRST = 32, LAST = 126;
static const int COLS = 16;

static IDirect3DTexture9* g_atlas = nullptr;
static int g_cw = 8, g_ch = 14;     // glyph cell size
static int g_texW = 0, g_texH = 0;
static int g_rows = 0;

struct TVtx { float x, y, z, rhw; DWORD col; float u, v; };
#define FVF_TEXT (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

int text_cw() { return g_cw; }
int text_ch() { return g_ch; }

bool text_init(IDirect3DDevice9* dev, DWORD texUsage, D3DPOOL pool) {
    // A fixed-pitch face so every cell is the same width.
    HFONT font = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        FIXED_PITCH | FF_MODERN, "Consolas");
    if (!font) font = (HFONT)GetStockObject(ANSI_FIXED_FONT);

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    HFONT oldFont = (HFONT)SelectObject(dc, font);

    TEXTMETRICA tm; GetTextMetricsA(dc, &tm);
    g_cw = tm.tmAveCharWidth;
    g_ch = tm.tmHeight;
    if (g_cw < 4) g_cw = 8;
    if (g_ch < 6) g_ch = 14;

    int count = LAST - FIRST + 1;
    g_rows = (count + COLS - 1) / COLS;
    g_texW = COLS * g_cw;
    g_texH = g_rows * g_ch;

    // 32-bit top-down DIB to render glyphs into.
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_texW;
    bi.bmiHeader.biHeight = -g_texH;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, dib);

    RECT full = { 0, 0, g_texW, g_texH };
    SetBkColor(dc, RGB(0, 0, 0));
    ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &full, "", 0, nullptr);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, font);
    for (int c = FIRST; c <= LAST; c++) {
        int idx = c - FIRST;
        int cx = (idx % COLS) * g_cw;
        int cy = (idx / COLS) * g_ch;
        char ch = (char)c;
        ExtTextOutA(dc, cx, cy, 0, nullptr, &ch, 1, nullptr);
    }
    GdiFlush();

    bool ok = false;
    if (SUCCEEDED(dev->CreateTexture(g_texW, g_texH, 1, texUsage, D3DFMT_A8R8G8B8,
                                     pool, &g_atlas, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_atlas->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* srcRow = (const uint32_t*)bits;
            for (int y = 0; y < g_texH; y++) {
                uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
                const uint32_t* src = srcRow + (size_t)y * g_texW;
                for (int x = 0; x < g_texW; x++) {
                    uint8_t lum = (uint8_t)(src[x] & 0xff);   // white glyph on black
                    dst[x*4+0] = 255; dst[x*4+1] = 255; dst[x*4+2] = 255; // BGR white
                    dst[x*4+3] = lum;                                     // alpha = coverage
                }
            }
            g_atlas->UnlockRect(0);
            ok = true;
        }
    }

    SelectObject(dc, oldBmp);
    SelectObject(dc, oldFont);
    DeleteObject(dib);
    DeleteObject(font);
    DeleteDC(dc);
    return ok;
}

void text_release() {
    if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
}

static void beginBatch(IDirect3DDevice9* dev, bool textured) {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetFVF(FVF_TEXT);
    if (textured) {
        dev->SetTexture(0, g_atlas);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TEXTURE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    } else {
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    }
}

void text_rect(IDirect3DDevice9* dev, int x, int y, int w, int h, DWORD color) {
    beginBatch(dev, false);
    float fx = (float)x - 0.5f, fy = (float)y - 0.5f;
    float fr = fx + w, fb = fy + h;
    TVtx q[6] = {
        { fx, fy, 0, 1, color, 0, 0 }, { fr, fy, 0, 1, color, 0, 0 }, { fx, fb, 0, 1, color, 0, 0 },
        { fr, fy, 0, 1, color, 0, 0 }, { fr, fb, 0, 1, color, 0, 0 }, { fx, fb, 0, 1, color, 0, 0 },
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(TVtx));
}

int text_draw(IDirect3DDevice9* dev, int x, int y, const char* s, DWORD color) {
    if (!g_atlas || !s) return 0;
    std::vector<TVtx> v;
    v.reserve(strlen(s) * 6);
    int cx = x, cy = y, maxw = 0;
    float du = (float)g_cw / g_texW, dv = (float)g_ch / g_texH;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '\n') { if (cx - x > maxw) maxw = cx - x; cx = x; cy += g_ch; continue; }
        if (c < FIRST || c > LAST) { cx += g_cw; continue; }
        int idx = c - FIRST;
        float u0 = (idx % COLS) * du, v0 = (idx / COLS) * dv;
        float u1 = u0 + du, v1 = v0 + dv;
        float x0 = (float)cx - 0.5f, y0 = (float)cy - 0.5f;
        float x1 = x0 + g_cw, y1 = y0 + g_ch;
        TVtx a = { x0, y0, 0, 1, color, u0, v0 };
        TVtx b = { x1, y0, 0, 1, color, u1, v0 };
        TVtx cc= { x0, y1, 0, 1, color, u0, v1 };
        TVtx d = { x1, y1, 0, 1, color, u1, v1 };
        v.push_back(a); v.push_back(b); v.push_back(cc);
        v.push_back(b); v.push_back(d); v.push_back(cc);
        cx += g_cw;
    }
    if (cx - x > maxw) maxw = cx - x;
    if (!v.empty()) {
        beginBatch(dev, true);
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(v.size() / 3), v.data(), sizeof(TVtx));
    }
    return maxw;
}
