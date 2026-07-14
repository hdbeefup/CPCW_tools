// Minimal screen-space text + rectangle drawing for the fixed-function D3D9
// viewer, with NO dependency on d3dx9 (the deprecated DirectX SDK). A GDI-drawn
// monospace glyph atlas is uploaded once to a D3D texture; strings are emitted as
// batched XYZRHW quads. Colors are 0xAARRGGBB.
#pragma once
#include <d3d9.h>

bool text_init(IDirect3DDevice9* dev, DWORD texUsage, D3DPOOL pool);
void text_release();
int  text_cw();   // glyph cell width  (monospace)
int  text_ch();   // glyph cell height

// Draw an ASCII string at top-left (x,y). Newlines advance a line. Returns the
// pixel width of the widest line drawn.
int  text_draw(IDirect3DDevice9* dev, int x, int y, const char* s, DWORD color);

// Filled translucent rectangle (panel background). No texture.
void text_rect(IDirect3DDevice9* dev, int x, int y, int w, int h, DWORD color);
