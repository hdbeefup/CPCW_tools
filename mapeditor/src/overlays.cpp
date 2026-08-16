// Road (GROL/GROA), decal (GDCL/GDEC) and river (GRVL/GRVR) overlay decoder.
//
//   GTRN  (1-byte version, then children GTRD, GROL, GDCL, GRVL)
//
// All three overlay children are the SAME slot pool (docs/MAP_FORMAT.md §4.9),
// which the engine reads with one templated routine instantiated three times
// (FUN_004bdba0 roads / FUN_004bdd80 decals / FUN_004bdf60 rivers — identical
// but for the record size they allocate: 0x120 / 0x184 / 0x11c):
//
//     u32 usedCount, freeHead, freeTail, usedHead, usedTail, slotCount   (24 bytes)
//     slotCount × { u32 next, u32 prev, u8 isFree [, "GROA"/"GDEC"/"GRVR" chunk] }
//
// The old reader here guessed at a "9 or 18 byte per-record prefix" and scanned
// forward for the next subtag; that prefix was a free slot plus a real slot
// header. Walking the pool properly consumes all 135 shipped containers to their
// exact byte and yields the per-record offsets a write path needs.
//
// **Emission order is SLOT order**, matching the engine's loader, which iterates
// the slot array rather than following usedHead. The used list is not in slot
// order (71 of 90 road/decal containers), but nothing establishes that the
// RENDERER walks the list, and the measured visual impact is tiny — see
// `--overlayscan`. Do not reorder without evidence from the draw side.
//
//   GROA (road) body:
//     u32 version(=11) + u32 N + N × 36-byte node + u32 N2 + N × 16-byte params
//     + u8 + u16-str material + u16-str shader + 22-byte tail.
//     Node = float x,y,z (world centreline; y≈0 -> projected onto the heightmap)
//     + two 3-float Catmull-Rom handles (in, out) whose magnitudes are the
//     distances to the previous and next node.
//
//   GDEC (decal) body:
//     u32 count(=6) + float centerX, centerZ, sizeX, sizeY, rot(radians) + flags
//     + u16-str material -> one rotated, terrain-projected textured quad.
//
//   GRVR (river) body: the same shape as GROA with version 2. The centreline
//     sits at a CONSTANT y (the water level, e.g. -10.0) and params[i].float0 is
//     a real per-node width in world units (22.0, 25.0 observed) — so unlike a
//     road, a river needs no texture-dimension width derivation.
#include "overlays.h"
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>

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

// Walk the slot pool exactly. `off` is the container's CONTENT start, `size` its
// content size. Fills `pool` (header + every live record's byte offsets) and
// returns the live records in slot order. `pool.ok` is false unless the walk
// lands precisely on the content end and the live count matches usedCount — a
// half-understood pool must not be handed to a write path.
struct Rec { size_t bodyOff, bodySize; int slot; };
std::vector<Rec> walkPool(const std::vector<unsigned char>& d, size_t off, size_t size,
                          const char* subtag, Scene::OverlayPool& pool) {
    std::vector<Rec> recs;
    size_t end = off + size;
    pool.hdrOff = (long)off; pool.contentEnd = (long)end; pool.ok = false;
    if (off + 24 > end) return recs;
    auto i32 = [&](size_t p){ return (int32_t)u32(d, p); };
    pool.used     = i32(off);
    pool.freeHead = i32(off + 4);
    pool.freeTail = i32(off + 8);
    pool.usedHead = i32(off + 12);
    pool.usedTail = i32(off + 16);
    pool.cap      = i32(off + 20);
    if (pool.cap < 0 || pool.cap > (int)((end - off) / 9) + 1) return recs;
    size_t p = off + 24;
    for (int s = 0; s < pool.cap; s++) {
        if (p + 9 > end) return recs;
        int nxt = i32(p), prv = i32(p + 4);
        bool free = d[p + 8] != 0;
        p += 9;
        if (free) continue;
        if (!tagAt(d, p, subtag)) return recs;
        uint32_t sz = u32(d, p + 4);
        size_t body = p + 8;
        if (body + sz > end) return recs;
        recs.push_back({ body, sz, s });
        pool.live.push_back({ s, (long)p, (long)body, (long)sz, nxt, prv });
        p = body + sz;
    }
    pool.ok = (p == end) && ((int)recs.size() == pool.used);
    return recs;
}

// Fallback for a pool that does not walk: the original forward scan for the next
// subtag. Kept so a map the pool reader cannot parse still RENDERS (read-only) —
// it just gets no byte offsets, so nothing may write to it.
std::vector<Rec> scanRecords(const std::vector<unsigned char>& d, size_t off, size_t size,
                             const char* subtag) {
    std::vector<Rec> recs;
    size_t end = off + size, p = off + 24;
    while (p + 8 <= end) {
        if (tagAt(d, p, subtag)) {
            uint32_t sz = u32(d, p + 4);
            size_t body = p + 8;
            if (sz > 0 && body + sz <= end) { recs.push_back({body, sz, -1}); p = body + sz; continue; }
        }
        p++;
    }
    return recs;
}

std::vector<Rec> readPool(const std::vector<unsigned char>& d, size_t off, size_t size,
                          const char* subtag, Scene::OverlayPool& pool) {
    std::vector<Rec> r = walkPool(d, off, size, subtag, pool);
    if (pool.ok) return r;
    pool.live.clear();
    return scanRecords(d, off, size, subtag);
}
} // namespace

bool overlay_set_decal(Scene& s, int slot, float cx, float cz,
                       float sx, float sy, float rot) {
    if (!s.decalPool.ok || s.raw.empty()) return false;   // fail closed
    long off = -1;
    for (const Scene::DecalRec& r : s.decalRecs)
        if (r.slot == slot) { off = r.xformOff; break; }
    if (off < 0 || off + 20 > (long)s.raw.size()) return false;
    const float v[5] = { cx, cz, sx, sy, rot };
    for (int k = 0; k < 5; k++) std::memcpy(&s.raw[(size_t)off + 4 * k], &v[k], 4);
    return true;
}

bool overlay_road_node_span(const Scene& s, int slot, int node, long& off, long& len) {
    for (const Scene::RoadRec& r : s.roadRecs) {
        if (r.slot != slot) continue;
        if (node < 0 || node >= r.nodeCount || r.nodesOff < 0) return false;
        const int first = std::max(0, node - 1);
        const int last  = std::min(r.nodeCount - 1, node + 1);
        off = r.nodesOff + (long)first * 36;
        len = (long)(last - first + 1) * 36;
        return off >= 0 && off + len <= (long)s.raw.size();
    }
    return false;
}

bool overlay_set_road_node(Scene& s, int slot, int node, float x, float z) {
    if (!s.roadPool.ok || s.raw.empty()) return false;          // fail closed
    const Scene::RoadRec* rr = nullptr;
    for (const Scene::RoadRec& r : s.roadRecs) if (r.slot == slot) { rr = &r; break; }
    if (!rr || rr->nodeCount < 2) return false;
    const int N = rr->nodeCount;
    if (node < 0 || node >= N) return false;
    const long base = rr->nodesOff;
    if (base < 0 || base + (long)N * 36 > (long)s.raw.size()) return false;

    auto posAt = [&](int i, float out[3]) {
        std::memcpy(out, &s.raw[(size_t)base + (size_t)i * 36], 12);
    };
    auto dist = [](const float a[3], const float b[3]) {
        float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
        return std::sqrt(dx*dx + dy*dy + dz*dz);
    };

    // Old positions of the three nodes whose handles can change, captured BEFORE
    // the move so an endpoint's segment-length ratio is measurable.
    float oldP[3][3];
    for (int k = -1; k <= 1; k++) {
        int i = node + k;
        if (i >= 0 && i < N) posAt(i, oldP[k + 1]);
    }

    // Move the node. pos.y is 0.0 on all 35668 shipped nodes — roads are 2D and
    // projected onto the heightmap at render time — so it is deliberately not
    // written here; a drag must not invent an elevation the format never carries.
    std::memcpy(&s.raw[(size_t)base + (size_t)node * 36 + 0], &x, 4);
    std::memcpy(&s.raw[(size_t)base + (size_t)node * 36 + 8], &z, 4);

    // Re-derive the Catmull-Rom handles of the moved node and both neighbours.
    // Interior nodes are reproducible from the neighbouring positions on
    // 27790/27792 shipped nodes; endpoints are NOT (the stored direction departs
    // from the chord by up to 83 degrees), so an endpoint keeps its direction
    // bit-for-bit and only its magnitude is scaled by the segment-length ratio —
    // exact for a drag next to an endpoint, a no-op when the length is unchanged.
    for (int k = -1; k <= 1; k++) {
        const int i = node + k;
        if (i < 0 || i >= N) continue;
        const size_t hoff = (size_t)base + (size_t)i * 36 + 12;   // in[3], out[3]
        if (i > 0 && i < N - 1) {
            float Pm[3], P[3], Pp[3];
            posAt(i - 1, Pm); posAt(i, P); posAt(i + 1, Pp);
            float T[3] = { Pp[0]-Pm[0], Pp[1]-Pm[1], Pp[2]-Pm[2] };
            float L = std::sqrt(T[0]*T[0] + T[1]*T[1] + T[2]*T[2]);
            if (L < 1e-9f) continue;                    // degenerate: leave as-is
            T[0]/=L; T[1]/=L; T[2]/=L;
            const float dPrev = dist(P, Pm), dNext = dist(Pp, P);
            float h[6] = { -T[0]*dPrev, -T[1]*dPrev, -T[2]*dPrev,
                            T[0]*dNext,  T[1]*dNext,  T[2]*dNext };
            std::memcpy(&s.raw[hoff], h, 24);
        } else {
            // Endpoint: scale magnitude, preserve direction exactly. Its only
            // adjacent segment is i–j, and j is always inside the [node-1,node+1]
            // window we captured: an endpoint reached from that window is either
            // `node` itself or one step from it, and its neighbour steps back
            // toward `node`. Guarded anyway rather than argued.
            const int j = (i == 0) ? 1 : N - 2;
            const int oi = i - node + 1, oj = j - node + 1;
            if (oi < 0 || oi > 2 || oj < 0 || oj > 2) continue;
            float Pj[3], Pi[3];
            posAt(j, Pj); posAt(i, Pi);
            const float newLen = dist(Pj, Pi);
            const float oldLen = dist(oldP[oj], oldP[oi]);
            if (oldLen < 1e-6f || newLen < 1e-6f) continue;
            const float ratio = newLen / oldLen;
            if (ratio == 1.0f) continue;                // bit-exact no-op
            float h[6];
            std::memcpy(h, &s.raw[hoff], 24);
            for (int c = 0; c < 6; c++) h[c] *= ratio;
            std::memcpy(&s.raw[hoff], h, 24);
        }
    }
    return true;
}

void parse_overlays(const std::vector<unsigned char>& d, Scene& s) {
    s.roads.clear(); s.decals.clear(); s.roadSplines.clear(); s.rivers.clear();
    s.decalRecs.clear(); s.roadRecs.clear();
    s.roadPool = Scene::OverlayPool{}; s.decalPool = Scene::OverlayPool{};
    s.riverPool = Scene::OverlayPool{};
    // Locate GTRN through the chunk tree (SCEN -> WRLD -> GTRN) rather than
    // scanning the file for the bytes "GTRN": terrain float data can spell a tag
    // by coincidence, and the first hit is not necessarily the chunk.
    if (d.size() < 8 || memcmp(d.data(), "SCEN", 4) != 0) return;
    size_t scenEnd = 8 + u32(d, 4), wrld = 0, wrldEnd = 0;
    for (size_t q = 12; q + 8 <= scenEnd && q + 8 <= d.size(); ) {
        uint32_t sz = u32(d, q + 4);
        if (tagAt(d, q, "WRLD")) { wrld = q; wrldEnd = q + 8 + sz; break; }
        q += 8 + sz;
    }
    if (!wrld) return;
    size_t gtrn = 0, gend = 0;
    for (size_t q = wrld + 20; q + 8 <= wrldEnd && q + 8 <= d.size(); ) {
        uint32_t sz = u32(d, q + 4);
        if (tagAt(d, q, "GTRN")) { gtrn = q; gend = q + 8 + sz; break; }
        q += 8 + sz;
    }
    if (!gtrn) return;
    size_t grolOff=0, grolSz=0, gdclOff=0, gdclSz=0, grvlOff=0, grvlSz=0;
    size_t p = gtrn + 9;                     // GTRN body has a 1-byte version
    while (p + 8 <= gend) {
        const unsigned char* t = d.data() + p; uint32_t sz = u32(d, p + 4);
        if (!memcmp(t,"GROL",4)) { grolOff=p+8; grolSz=sz; s.roadPool.chunkOff=(long)p; }
        else if (!memcmp(t,"GDCL",4)) { gdclOff=p+8; gdclSz=sz; s.decalPool.chunkOff=(long)p; }
        else if (!memcmp(t,"GRVL",4)) { grvlOff=p+8; grvlSz=sz; s.riverPool.chunkOff=(long)p; }
        else if (memcmp(t,"GTRD",4)) break;
        p = p + 8 + sz;
    }
    float wmax = (float)(s.world_w > 0 ? s.world_w : 4096);
    float hmax = (float)(s.world_h > 0 ? s.world_h : 4096);

    // ---- roads: GROA centreline polyline -> textured ribbon ---------------
    if (grolOff) {
        for (const Rec& r : readPool(d, grolOff, grolSz, "GROA", s.roadPool)) {
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
            std::string m; for (char c : mat) m += (char)tolower((unsigned char)c);
            auto has = [&](const char* t){ return m.find(t) != std::string::npos; };

            // Some GROA records are 2D area fills (airfield aprons, parking/plaza
            // concrete) rather than centrelines — their nodes trace the region
            // boundary. Detect by material family (concrete/park/apron) AND a
            // genuinely 2D bounding box, then triangulate as a filled polygon.
            // (Shoelace area alone misclassifies curvy roads, so gate on name.)
            float minx=px[0],maxx=px[0],minz=pz[0],maxz=pz[0];
            for (size_t i=1;i<px.size();i++){ minx=std::min(minx,px[i]); maxx=std::max(maxx,px[i]);
                                              minz=std::min(minz,pz[i]); maxz=std::max(maxz,pz[i]); }
            bool areaMat = has("concr") || has("park") || has("apron") || has("plaza");
            bool isArea = areaMat && px.size() >= 4 &&
                          std::min(maxx-minx, maxz-minz) > 15.0f;

            if (isArea) {
                // centroid-fan triangulation (parking/apron regions are convex-ish)
                Scene::OverlayMesh om; om.tex = mat;
                const float TILE = 1.0f/16.0f;      // texture repeat for area fills
                float cx=0,cz=0; for (size_t i=0;i<px.size();i++){ cx+=px[i]; cz+=pz[i]; }
                cx/=px.size(); cz/=px.size();
                om.verts.insert(om.verts.end(), { cx, heightAt(s,cx,cz)+BIAS, cz, cx*TILE, cz*TILE });
                for (size_t i=0;i<px.size();i++)
                    om.verts.insert(om.verts.end(),
                        { px[i], heightAt(s,px[i],pz[i])+BIAS, pz[i], px[i]*TILE, pz[i]*TILE });
                unsigned n=(unsigned)px.size();
                for (unsigned i=0;i<n;i++)
                    om.idx.insert(om.idx.end(), { 0u, 1u+i, 1u+((i+1)%n) });
                if (!om.idx.empty()) s.roads.push_back(std::move(om));
            } else {
                // Store the centreline; the ribbon is extruded at render time in
                // buildOverlays() using the road TEXTURE's height for width.
                Scene::RoadSpline rs; rs.tex = mat; rs.srcSlot = r.slot;
                rs.cx = std::move(px); rs.cz = std::move(pz);
                s.roadSplines.push_back(std::move(rs));
            }
            // Where the editable nodes live, kept for BOTH shapes: an area fill
            // is still a node array, it is only drawn differently.
            Scene::RoadRec rr;
            rr.slot = r.slot; rr.nodesOff = (long)(b + 8);
            rr.nodeCount = (int)nv; rr.isArea = isArea; rr.tex = mat;
            s.roadRecs.push_back(std::move(rr));
        }
    }

    // ---- decals: GDEC quad ------------------------------------------------
    if (gdclOff) {
        for (const Rec& r : readPool(d, gdclOff, gdclSz, "GDEC", s.decalPool)) {
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
            om.srcSlot = r.slot;
            s.decals.push_back(std::move(om));
            // Keep the editable floats and where they live, alongside the baked
            // geometry — the mesh cannot be edited back into a transform.
            Scene::DecalRec dr;
            dr.slot = r.slot; dr.xformOff = (long)(b + 4);
            dr.cx = cx; dr.cz = cz; dr.sx = sx; dr.sy = sy; dr.rot = rot;
            dr.tex = mat;
            s.decalRecs.push_back(std::move(dr));
        }
    }

    // ---- rivers: GRVR centreline at a constant water level ------------------
    // Same record shape as GROA (version 2), but the width is real: the trailing
    // params array carries one per node in world units, so no texture-dimension
    // derivation is needed. y is constant per record and IS the water surface —
    // rivers are not projected onto the heightmap the way roads and decals are.
    if (grvlOff) {
        for (const Rec& r : readPool(d, grvlOff, grvlSz, "GRVR", s.riverPool)) {
            size_t b = r.bodyOff, e = b + r.bodySize;
            uint32_t nv = u32(d, b + 4);
            if (nv < 2 || nv > 20000) continue;
            size_t nodes = b + 8;
            if (nodes + (size_t)nv * 36 + 4 > e) continue;
            Scene::RiverSpline rv; rv.srcSlot = r.slot;
            float level = 0.0f; bool haveLevel = false;
            bool bad = false;
            for (uint32_t i = 0; i < nv; i++) {
                size_t n = nodes + (size_t)i * 36;
                float x = f32(d, n), y = f32(d, n + 4), z = f32(d, n + 8);
                if (!(x==x) || !(z==z) || !(y==y) ||
                    x < -64 || x > wmax+64 || z < -64 || z > hmax+64) { bad = true; break; }
                if (!haveLevel) { level = y; haveLevel = true; }
                rv.cx.push_back(x); rv.cz.push_back(z);
            }
            if (bad || rv.cx.size() < 2) continue;
            rv.level = level;
            // params: u32 count2 (== nv) then nv x 16 bytes, float0 = width
            size_t par = nodes + (size_t)nv * 36;
            uint32_t n2 = u32(d, par);
            rv.w.assign(rv.cx.size(), 1.0f);
            if (n2 == nv && par + 4 + (size_t)nv * 16 <= e)
                for (uint32_t i = 0; i < nv && i < rv.w.size(); i++) {
                    float w = f32(d, par + 4 + (size_t)i * 16);
                    rv.w[i] = (w == w && w > 0.0f && w < 4096.0f) ? w : 1.0f;
                }
            rv.tex = findMaterial(d, b, e);
            s.rivers.push_back(std::move(rv));
        }
    }
}
