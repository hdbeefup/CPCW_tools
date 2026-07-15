// Road (GROL/GROA) & decal (GDCL/GDEC) overlay decoder.
//
// Layout reverse-engineered from the shipped maps (M_01 et al.), cross-checked by
// bounding decoded floats against the map's world extent:
//
//   GTRN  (1-byte version, then children GTRD, GROL, GDCL, GRVL)
//     GROL / GDCL container:
//       u32  recordCount               (matches the number of GROA/GDEC records)
//       u32  ?, u32 ?, u32 0, u32 ?, u32 ?   (5 more header dwords -> 24-byte header)
//       recordCount ×:
//         u32 index(1-based) + u32 flag + u8 flag   (9-byte per-record prefix)
//         "GROA"/"GDEC" + u32 size + body
//
//   GROA (road) body:
//     u32 type(=11) + u32 nodeCount +
//     nodeCount × 36 bytes { float x,y,z (world; y=0 -> terrain-projected) + 6 aux
//       floats (segment length, tangent dx/dz, ...) } +
//     trailer (transform matrix / bbox) + u16 material path + u16 shader path + tail.
//     The nodeCount points trace the road centreline; we extrude a textured ribbon.
//
//   GDEC (decal) body:
//     u32 count(=6) + float centerX + float centerZ + float sizeX + float sizeY +
//     float rot(radians) + u32 flag + padding + u16 material path + ...
//     -> one rotated, terrain-projected textured quad.
#include "overlays.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>

namespace {
uint32_t u32(const std::vector<unsigned char>& d, size_t p) {
    return p + 4 <= d.size() ? (uint32_t)(d[p] | (d[p+1]<<8) | (d[p+2]<<16) | ((uint32_t)d[p+3]<<24)) : 0;
}
uint16_t u16(const std::vector<unsigned char>& d, size_t p) {
    return p + 2 <= d.size() ? (uint16_t)(d[p] | (d[p+1]<<8)) : 0;
}
float f32(const std::vector<unsigned char>& d, size_t p) {
    uint32_t v = u32(d, p); float f; std::memcpy(&f, &v, 4); return f;
}
bool tagAt(const std::vector<unsigned char>& d, size_t p, const char* t) {
    return p + 4 <= d.size() && std::memcmp(d.data() + p, t, 4) == 0;
}
// u16-length-prefixed ASCII string at p (advances np past it)
std::string rstr(const std::vector<unsigned char>& d, size_t p, size_t& np) {
    uint16_t L = u16(d, p); size_t s = p + 2; std::string r;
    if (s + L <= d.size()) r.assign((const char*)d.data() + s, L);
    np = s + L; return r;
}
// The material path is the FIRST "Terrain/..." u16-length-prefixed string in the
// record (the second such string is the shader, e.g. .../BumpDisplace).
std::string findMaterial(const std::vector<unsigned char>& d, size_t b, size_t e) {
    for (size_t p = b; p + 2 < e; p++) {
        if (d[p]=='T' && p+8<e && std::memcmp(d.data()+p,"Terrain/",8)==0) {
            size_t q = p - 2; uint16_t L = u16(d, q);     // preceding u16 length
            if (q >= b && L >= 8 && q + 2 + L <= e)
                return std::string((const char*)d.data()+p, L);
        }
    }
    return std::string();
}

// bilinear terrain height at world (x, y-in-plane)
float heightAt(const Scene& s, float x, float y) {
    if (s.heights.empty() || s.grid_w < 2 || s.grid_h < 2) return 0.0f;
    if (x < 0) x = 0; if (x > s.grid_w-1) x = (float)(s.grid_w-1);
    if (y < 0) y = 0; if (y > s.grid_h-1) y = (float)(s.grid_h-1);
    int x0=(int)x, y0=(int)y, x1=x0+1<s.grid_w?x0+1:x0, y1=y0+1<s.grid_h?y0+1:y0;
    float tx=x-x0, ty=y-y0;
    auto H=[&](int i,int j){ return s.heights[(size_t)j*s.grid_w+i]; };
    float a=H(x0,y0)*(1-tx)+H(x1,y0)*tx, b=H(x0,y1)*(1-tx)+H(x1,y1)*tx;
    return a*(1-ty)+b*ty;
}

const float BIAS = 0.25f;   // lift overlays above the terrain to avoid z-fighting

// Walk a GROL/GDCL container: 24-byte header, then a run of `subtag` chunks
// (tag + u32 size + body) separated by small variable-length per-record prefixes
// (9 or 18 bytes observed). Rather than assume a fixed prefix, scan forward for
// the next subtag after each chunk. The 4-byte subtag is distinctive enough that
// false hits inside float data are effectively impossible with the size check.
struct Rec { size_t bodyOff, bodySize; };
std::vector<Rec> walkRecords(const std::vector<unsigned char>& d, size_t off, size_t size,
                             const char* subtag) {
    std::vector<Rec> recs;
    size_t end = off + size;
    size_t p = off + 24;                    // skip container header
    while (p + 8 <= end) {
        if (tagAt(d, p, subtag)) {
            uint32_t sz = u32(d, p + 4);
            size_t body = p + 8;
            if (sz > 0 && body + sz <= end) { recs.push_back({body, sz}); p = body + sz; continue; }
        }
        p++;                                // scan to the next subtag
    }
    return recs;
}
} // namespace

void parse_overlays(const std::vector<unsigned char>& d, Scene& s) {
    s.roads.clear(); s.decals.clear();
    // find GTRN, then its children GROL/GDCL (GTRN body has a 1-byte version)
    size_t gtrn = std::string::npos;
    for (size_t i = 0; i + 4 <= d.size(); i++)
        if (d[i]=='G'&&d[i+1]=='T'&&d[i+2]=='R'&&d[i+3]=='N') { gtrn = i; break; }
    if (gtrn == std::string::npos) return;
    size_t gbody = gtrn + 8, gend = gbody + u32(d, gtrn + 4);
    size_t grolOff=0, grolSz=0, gdclOff=0, gdclSz=0;
    size_t p = gbody + 1;                    // 1-byte version
    while (p + 8 <= gend) {
        const unsigned char* t = d.data() + p; uint32_t sz = u32(d, p + 4);
        if (!memcmp(t,"GROL",4)) { grolOff=p+8; grolSz=sz; }
        else if (!memcmp(t,"GDCL",4)) { gdclOff=p+8; gdclSz=sz; }
        else if (memcmp(t,"GTRD",4) && memcmp(t,"GRVL",4)) break;
        p = p + 8 + sz;
    }
    float wmax = (float)(s.world_w > 0 ? s.world_w : 4096);
    float hmax = (float)(s.world_h > 0 ? s.world_h : 4096);

    // ---- roads: GROA polyline -> textured ribbon --------------------------
    if (grolOff) {
        for (const Rec& r : walkRecords(d, grolOff, grolSz, "GROA")) {
            size_t b = r.bodyOff, e = b + r.bodySize;
            uint32_t nv = u32(d, b + 4);
            if (nv < 2 || nv > 20000) continue;
            std::vector<float> px, pz;
            size_t node = b + 8;
            for (uint32_t i = 0; i < nv; i++, node += 36) {
                if (node + 12 > e) break;
                float x = f32(d, node), z = f32(d, node + 8);
                if (!(x==x) || !(z==z) || x < -64 || x > wmax+64 || z < -64 || z > hmax+64) break;
                px.push_back(x); pz.push_back(z);
            }
            if (px.size() < 2) continue;
            std::string mat = findMaterial(d, b, e);
            // half-width heuristic from the material name
            float hw = 2.2f;
            { std::string m; for(char c:mat) m+=(char)tolower((unsigned char)c);
              if (m.find("wide")!=std::string::npos) hw=3.2f;
              else if (m.find("narrow")!=std::string::npos) hw=1.4f;
              else if (m.find("high")!=std::string::npos||m.find("road")!=std::string::npos) hw=3.6f; }
            Scene::OverlayMesh om; om.tex = mat;
            float vrun = 0.0f;
            for (size_t i = 0; i < px.size(); i++) {
                // tangent from neighbours
                size_t a = i>0?i-1:i, c = i+1<px.size()?i+1:i;
                float dx = px[c]-px[a], dz = pz[c]-pz[a];
                float len = std::sqrt(dx*dx+dz*dz); if (len<1e-4f) len=1e-4f;
                float nx = -dz/len, nz = dx/len;                 // left normal
                if (i>0){ float sx=px[i]-px[i-1], sz2=pz[i]-pz[i-1]; vrun += std::sqrt(sx*sx+sz2*sz2); }
                float lx=px[i]-nx*hw, lz=pz[i]-nz*hw, rx=px[i]+nx*hw, rz=pz[i]+nz*hw;
                float vy = vrun/(hw*2.0f);
                om.verts.insert(om.verts.end(), { lx, heightAt(s,lx,lz)+BIAS, lz, 0.0f, vy });
                om.verts.insert(om.verts.end(), { rx, heightAt(s,rx,rz)+BIAS, rz, 1.0f, vy });
            }
            for (size_t i = 0; i + 1 < px.size(); i++) {
                unsigned a=(unsigned)(i*2), b2=a+1, c=a+2, dd=a+3;
                om.idx.insert(om.idx.end(), { a,c,b2, b2,c,dd });
            }
            if (!om.idx.empty()) s.roads.push_back(std::move(om));
        }
    }

    // ---- decals: GDEC quad ------------------------------------------------
    if (gdclOff) {
        for (const Rec& r : walkRecords(d, gdclOff, gdclSz, "GDEC")) {
            size_t b = r.bodyOff, e = b + r.bodySize;
            float cx = f32(d, b + 4), cz = f32(d, b + 8);
            float sx = f32(d, b + 12), sy = f32(d, b + 16), rot = f32(d, b + 20);
            if (!(cx==cx)||!(cz==cz)||cx<-64||cx>wmax+64||cz<-64||cz>hmax+64) continue;
            if (!(sx==sx)||!(sy==sy)||sx<=0||sy<=0||sx>256||sy>256) continue;
            if (!(rot==rot)) rot = 0;
            std::string mat = findMaterial(d, b, e);
            float c = std::cos(rot), sn = std::sin(rot), hx=sx*0.5f, hy=sy*0.5f;
            Scene::OverlayMesh om; om.tex = mat;
            const float corner[4][2] = {{-hx,-hy},{hx,-hy},{hx,hy},{-hx,hy}};
            const float uv[4][2] = {{0,0},{1,0},{1,1},{0,1}};
            for (int k=0;k<4;k++){
                float lx=corner[k][0], ly=corner[k][1];
                float wx=cx + lx*c - ly*sn, wz=cz + lx*sn + ly*c;
                om.verts.insert(om.verts.end(), { wx, heightAt(s,wx,wz)+BIAS, wz, uv[k][0], uv[k][1] });
            }
            om.idx = {0,1,2, 0,2,3};
            s.decals.push_back(std::move(om));
        }
    }
}
