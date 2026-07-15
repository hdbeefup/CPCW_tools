// Native THMB thumbnail decoder. Mirrors read_thumbnail() in cpcw_srm.py.
//
// THMB layout (after the 8-byte "THMB" + u32 size header):
//   "v001", u32 width, u32 height, u32 unk,
//   1 flag byte (0x01), u32 encoded-length,
//   then an RLE stream of 3-byte BGR pixels. Control byte C:
//     high bit set  -> run of (C & 0x7F)+1 copies of the next single pixel;
//     high bit clear -> (C+1) literal pixels follow.
// Background pixels are white.
#include "thumb.h"
#include <cstdint>
#include <cstring>
#include <fstream>

static uint32_t rd_u32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool load_thmb(const std::string& path, int& w, int& h,
               std::vector<unsigned char>& rgba) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
    if (d.size() < 21) return false;
    if (memcmp(d.data(), "MAIN", 4) != 0) return false;
    if (memcmp(d.data() + 8, "THMB", 4) != 0) return false;
    uint32_t size = rd_u32(&d[12]);
    if (16 + (size_t)size > d.size()) return false;
    const unsigned char* b = &d[16];
    if (memcmp(b, "v001", 4) != 0) return false;
    uint32_t ww = rd_u32(b + 4), hh = rd_u32(b + 8);
    if (ww == 0 || hh == 0 || ww > 4096 || hh > 4096) return false;
    uint32_t enc = rd_u32(b + 17);                 // after v001+w+h+unk+flag
    const unsigned char* s = b + 21;
    if (21 + (size_t)enc > size) return false;

    size_t need = (size_t)ww * hh;
    rgba.assign(need * 4, 255);
    size_t out = 0, i = 0;
    while (i < enc && out < need) {
        unsigned char c = s[i++];
        int cnt; bool run = (c & 0x80) != 0;
        cnt = run ? ((c & 0x7F) + 1) : (c + 1);
        for (int k = 0; k < cnt && out < need; k++) {
            if (i + 3 > enc) return false;
            unsigned char bl = s[i], gr = s[i + 1], re = s[i + 2];
            rgba[out * 4 + 0] = re;
            rgba[out * 4 + 1] = gr;
            rgba[out * 4 + 2] = bl;
            rgba[out * 4 + 3] = 255;
            out++;
            if (!run) i += 3;
        }
        if (run) i += 3;
    }
    if (out != need) return false;
    w = (int)ww; h = (int)hh;
    return true;
}
