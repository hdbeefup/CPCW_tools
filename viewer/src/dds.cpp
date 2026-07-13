#include "dds.h"
#include <cstdio>
#include <cstring>

namespace {

struct RGB { int r, g, b; };

inline uint32_t rd32(const uint8_t* d, size_t off) {
    return (uint32_t)d[off] | ((uint32_t)d[off+1] << 8) |
           ((uint32_t)d[off+2] << 16) | ((uint32_t)d[off+3] << 24);
}
inline uint16_t rd16(const uint8_t* d, size_t off) {
    return (uint16_t)d[off] | ((uint16_t)d[off+1] << 8);
}
inline uint64_t rd64(const uint8_t* d, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)d[off + i] << (8 * i);
    return v;
}

RGB rgb565(uint16_t c) {
    int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    return { (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2) };
}
RGB lerp(RGB a, RGB b, int wa, int wb) {
    int t = wa + wb;
    return { (a.r*wa + b.r*wb)/t, (a.g*wa + b.g*wb)/t, (a.b*wa + b.b*wb)/t };
}

// Build the 4-entry color palette + implicit per-block alpha for a color block.
void colorPalette(const uint8_t* d, size_t off, bool punchthrough,
                  RGB pal[4], int blockAlpha[4]) {
    uint16_t c0raw = rd16(d, off), c1raw = rd16(d, off + 2);
    RGB c0 = rgb565(c0raw), c1 = rgb565(c1raw);
    if (punchthrough && c0raw <= c1raw) {
        pal[0]=c0; pal[1]=c1; pal[2]=lerp(c0,c1,1,1); pal[3]={0,0,0};
        blockAlpha[0]=255; blockAlpha[1]=255; blockAlpha[2]=255; blockAlpha[3]=0;
    } else {
        pal[0]=c0; pal[1]=c1; pal[2]=lerp(c0,c1,2,1); pal[3]=lerp(c0,c1,1,2);
        blockAlpha[0]=255; blockAlpha[1]=255; blockAlpha[2]=255; blockAlpha[3]=255;
    }
}

void blit(std::vector<uint8_t>& out, int W, int H, int bx, int by,
          const RGB pal[4], uint32_t colorIdx, const int alpha[16]) {
    for (int pi = 0; pi < 16; pi++) {
        int px = bx * 4 + (pi & 3);
        int py = by * 4 + (pi >> 2);
        if (px >= W || py >= H) continue;
        int ci = (colorIdx >> (2 * pi)) & 0x3;
        size_t o = (size_t)(py * W + px) * 4;
        out[o]   = (uint8_t)pal[ci].r;
        out[o+1] = (uint8_t)pal[ci].g;
        out[o+2] = (uint8_t)pal[ci].b;
        out[o+3] = (uint8_t)alpha[pi];
    }
}

std::vector<uint8_t> decodeDXT1(const uint8_t* d, size_t off, int W, int H) {
    std::vector<uint8_t> out((size_t)W * H * 4, 0);
    int bw = (W + 3) / 4, bh = (H + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            RGB pal[4]; int ba[4];
            colorPalette(d, off, true, pal, ba);
            uint32_t colorIdx = rd32(d, off + 4);
            int alpha[16];
            for (int pi = 0; pi < 16; pi++) alpha[pi] = ba[(colorIdx >> (2*pi)) & 0x3];
            blit(out, W, H, bx, by, pal, colorIdx, alpha);
            off += 8;
        }
    return out;
}

std::vector<uint8_t> decodeDXT3(const uint8_t* d, size_t off, int W, int H) {
    std::vector<uint8_t> out((size_t)W * H * 4, 0);
    int bw = (W + 3) / 4, bh = (H + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            uint64_t ab = rd64(d, off);
            int alpha[16];
            for (int pi = 0; pi < 16; pi++) alpha[pi] = (int)((ab >> (4*pi)) & 0xF) * 17;
            RGB pal[4]; int ba[4];
            colorPalette(d, off + 8, false, pal, ba);
            uint32_t colorIdx = rd32(d, off + 12);
            blit(out, W, H, bx, by, pal, colorIdx, alpha);
            off += 16;
        }
    return out;
}

std::vector<uint8_t> decodeDXT5(const uint8_t* d, size_t off, int W, int H) {
    std::vector<uint8_t> out((size_t)W * H * 4, 0);
    int bw = (W + 3) / 4, bh = (H + 3) / 4;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
            int a0 = d[off], a1 = d[off + 1];
            int ap[8]; ap[0] = a0; ap[1] = a1;
            if (a0 > a1) for (int i = 2; i < 8; i++) ap[i] = (a0*(8-i) + a1*(i-1)) / 7;
            else { for (int i = 2; i < 6; i++) ap[i] = (a0*(6-i) + a1*(i-1)) / 5; ap[6]=0; ap[7]=255; }
            uint64_t abits = 0;
            for (int i = 0; i < 6; i++) abits |= (uint64_t)d[off + 2 + i] << (8 * i);
            int alpha[16];
            for (int pi = 0; pi < 16; pi++) alpha[pi] = ap[(abits >> (3*pi)) & 0x7];
            RGB pal[4]; int ba[4];
            colorPalette(d, off + 8, false, pal, ba);
            uint32_t colorIdx = rd32(d, off + 12);
            blit(out, W, H, bx, by, pal, colorIdx, alpha);
            off += 16;
        }
    return out;
}

void maskShift(uint32_t mask, int& shift, int& bits) {
    if (mask == 0) { shift = 0; bits = 0; return; }
    shift = 0; uint32_t m = mask;
    while (!(m & 1)) { m >>= 1; shift++; }
    bits = 0; while (m & 1) { m >>= 1; bits++; }
}
int scalec(uint32_t val, int bits) {
    if (bits == 0) return 255;
    return (int)((val * 255) / ((1u << bits) - 1));
}

std::vector<uint8_t> decodeUncompressed(const uint8_t* d, size_t off, int W, int H,
        int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask,
        bool hasAlpha) {
    std::vector<uint8_t> out((size_t)W * H * 4, 0);
    int bpp8 = bpp / 8;
    int rs, rb, gs, gb, bs, bb, as, ab;
    maskShift(rmask, rs, rb); maskShift(gmask, gs, gb);
    maskShift(bmask, bs, bb); maskShift(amask, as, ab);
    for (int i = 0; i < W * H; i++) {
        uint32_t px = 0;
        for (int k = 0; k < bpp8; k++) px |= (uint32_t)d[off + k] << (8 * k);
        off += bpp8;
        size_t o = (size_t)i * 4;
        out[o]   = (uint8_t)scalec((px & rmask) >> rs, rb);
        out[o+1] = (uint8_t)scalec((px & gmask) >> gs, gb);
        out[o+2] = (uint8_t)scalec((px & bmask) >> bs, bb);
        out[o+3] = hasAlpha ? (uint8_t)scalec((px & amask) >> as, ab) : 255;
    }
    return out;
}

} // namespace

DdsImage dds_load(const std::string& path) {
    DdsImage img;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return img;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 128) { fclose(f); return img; }
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return img; }
    fclose(f);

    if (memcmp(data.data(), "DDS ", 4) != 0) return img;
    const uint8_t* d = data.data();
    int H = (int)rd32(d, 12), W = (int)rd32(d, 16);
    uint32_t pfFlags = rd32(d, 80);
    char fourcc[5] = {0}; memcpy(fourcc, d + 84, 4);
    int rgbBits = (int)rd32(d, 88);
    uint32_t rmask = rd32(d, 92), gmask = rd32(d, 96), bmask = rd32(d, 100), amask = rd32(d, 104);
    const size_t pixel = 128;

    const uint32_t DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_ALPHAPIXELS = 0x1;
    if (pfFlags & DDPF_FOURCC) {
        if (!memcmp(fourcc, "DXT1", 4)) img.rgba = decodeDXT1(d, pixel, W, H);
        else if (!memcmp(fourcc, "DXT2", 4) || !memcmp(fourcc, "DXT3", 4)) img.rgba = decodeDXT3(d, pixel, W, H);
        else if (!memcmp(fourcc, "DXT4", 4) || !memcmp(fourcc, "DXT5", 4)) img.rgba = decodeDXT5(d, pixel, W, H);
        else return img;
    } else if (pfFlags & DDPF_RGB) {
        bool hasAlpha = (pfFlags & DDPF_ALPHAPIXELS) && amask != 0;
        img.rgba = decodeUncompressed(d, pixel, W, H, rgbBits, rmask, gmask, bmask, amask, hasAlpha);
    } else {
        return img;
    }
    img.width = W; img.height = H; img.ok = true;
    return img;
}
