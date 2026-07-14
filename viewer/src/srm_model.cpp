#include "srm_model.h"
#include <cstdio>
#include <cstring>
#include <set>

namespace {

inline uint32_t u32(const uint8_t* d, size_t o) {
    return (uint32_t)d[o] | ((uint32_t)d[o+1]<<8) | ((uint32_t)d[o+2]<<16) | ((uint32_t)d[o+3]<<24);
}
inline int32_t i32(const uint8_t* d, size_t o) { return (int32_t)u32(d, o); }
inline uint16_t u16(const uint8_t* d, size_t o) { return (uint16_t)d[o] | ((uint16_t)d[o+1]<<8); }
inline float f32(const uint8_t* d, size_t o) { uint32_t v = u32(d, o); float f; memcpy(&f, &v, 4); return f; }

// Parse a MESH chunk body [start,end) into `mesh`.
void parseMesh(const uint8_t* d, size_t start, size_t end, SrmMesh& mesh) {
    size_t p = start;
    mesh.streamCount = (int)u32(d, p); p += 4;
    mesh.submeshCount = (int)u32(d, p); p += 4;

    if (p + 8 <= end && memcmp(d + p, "BONE", 4) == 0) {
        uint32_t boneSize = u32(d, p + 4);
        size_t bp = p + 8;
        uint32_t boneCount = u32(d, bp);
        for (uint32_t i = 0; i < boneCount; i++) mesh.bones.push_back(u16(d, bp + 4 + i * 2));
        p = p + 8 + boneSize;
    }

    if (p + 8 > end || memcmp(d + p, "INDS", 4) != 0) return;
    p += 8;
    uint32_t idxCount = u32(d, p); p += 4;
    uint32_t idxStride = u32(d, p); p += 4;
    mesh.indices.reserve(idxCount);
    if (idxStride == 2) for (uint32_t i = 0; i < idxCount; i++) mesh.indices.push_back(u16(d, p + i*2));
    else                for (uint32_t i = 0; i < idxCount; i++) mesh.indices.push_back(u32(d, p + i*4));
    p += (size_t)idxCount * idxStride;

    for (int s = 0; s < mesh.streamCount; s++) {
        if (p + 8 > end || memcmp(d + p, "VERS", 4) != 0) break;
        uint32_t vsSize = u32(d, p + 4);
        size_t vp = p + 8;
        size_t vsEnd = vp + vsSize;
        uint32_t vertCount = u32(d, vp + 4);
        uint32_t stride = u32(d, vp + 8);
        uint32_t usage = u32(d, vp + 12);
        uint32_t semantic = u32(d, vp + 16);
        vp += 20;
        SrmStream st;
        st.usage = (int)usage; st.semantic = (int)semantic;
        st.stride = (int)stride; st.vertexCount = (int)vertCount;
        size_t need = (size_t)vertCount * stride;
        if (vp + need <= end) st.data.assign(d + vp, d + vp + need);
        mesh.streams.push_back(std::move(st));
        p = vsEnd;
    }

    // Single submesh + material/texture block (mirrors srm_format.py).
    if (!mesh.streams.empty()) {
        SrmSubmesh sm;
        sm.materialName = "default";
        // best-effort material parse; failures leave textures empty
        size_t mp = p + 25;   // flag(1) + 6*u32(24)
        if (mp + 2 <= end) {
            uint16_t mlen = u16(d, mp); mp += 2;
            if (mlen > 0 && mlen < 256 && mp + mlen <= end) {
                std::string mname((const char*)(d + mp), mlen); mp += mlen;
                if (!mname.empty() && mname.back() == '\\') mname.pop_back();
                sm.materialName = mname;
                if (mp + 8 <= end) {
                    mp += 2;  // prop_count u16
                    mp += 2;  // unk u16
                    uint32_t propTotal = u32(d, mp); mp += 4;
                    for (uint32_t i = 0; i < propTotal && mp + 2 <= end; i++) {
                        uint16_t plen = u16(d, mp); mp += 2;
                        if (mp + plen > end) break;
                        std::string pname((const char*)(d + mp), plen); mp += plen;
                        if (mp + 8 > end) break;
                        uint32_t ptype = u32(d, mp); mp += 4;
                        uint32_t psize = u32(d, mp); mp += 4;
                        if (ptype == 6) {
                            if (mp + 2 > end) break;
                            uint16_t tlen = u16(d, mp); mp += 2;
                            if (mp + tlen > end) break;
                            std::string tname((const char*)(d + mp), tlen); mp += tlen;
                            sm.textures[pname] = tname;
                        } else if (ptype == 0 && psize > 0) {
                            mp += psize;
                        }
                    }
                }
            }
        }
        mesh.submeshes.push_back(std::move(sm));
    }
}

// Parse a MOTS chunk body [start,end) (starts with 'v004') into `out`.
void parseMots(const uint8_t* d, size_t start, size_t end, std::vector<MotsMotion>& out) {
    if (start + 4 > end || memcmp(d + start, "v004", 4) != 0) return;
    size_t p = start + 4;
    while (p + 8 <= end) {
        uint32_t sz = u32(d, p + 4);
        size_t chunkEnd = p + 8 + sz;
        if (chunkEnd > end) break;
        if (memcmp(d + p, "MOTI", 4) != 0) { p = chunkEnd; continue; }
        size_t q = p + 8;
        MotsMotion mo;
        uint16_t nlen = u16(d, q); q += 2;
        mo.name.assign((const char*)(d + q), nlen); q += nlen;
        q += 4;                                   // target_obj
        q += 4;                                   // weight
        q += 4;                                   // zero
        uint32_t nodeCount = u32(d, q); q += 4;
        mo.nodeChannel.resize(nodeCount);
        for (uint32_t i = 0; i < nodeCount; i++) { mo.nodeChannel[i] = i32(d, q); q += 4; }
        if (q + 8 <= chunkEnd && memcmp(d + q, "ANIM", 4) == 0) {
            uint32_t animSize = u32(d, q + 4);
            size_t a = q + 8;                     // -> 'v002'
            size_t animEnd = a + animSize;
            if (animEnd > end) animEnd = end;
            if (a + 4 <= animEnd && memcmp(d + a, "v002", 4) == 0) {
                size_t r = a + 4;                 // key offsets are relative to here
                mo.duration = f32(d, r);
                uint32_t channelCount = u32(d, r + 16);
                size_t rec = r + 32;
                std::vector<int> keyCounts, keyOffsets;
                for (uint32_t c = 0; c < channelCount; c++) {
                    if (rec + 68 > animEnd) break;
                    keyCounts.push_back((int)i32(d, rec + 40));
                    keyOffsets.push_back((int)i32(d, rec + 48));
                    rec += 68;
                }
                mo.channels.resize(channelCount);
                for (size_t c = 0; c < keyCounts.size(); c++) {
                    size_t o = r + (size_t)(uint32_t)keyOffsets[c];
                    for (int k = 0; k < keyCounts[c]; k++) {
                        if (o + 68 > animEnd) break;
                        MotsKey key; key.v0 = f32(d, o + 28); key.v1 = f32(d, o + 32);
                        mo.channels[c].keys.push_back(key);
                        o += 68;
                    }
                }
            }
        }
        out.push_back(std::move(mo));
        p = chunkEnd;
    }
}

} // namespace

bool srm_parse(const std::string& path, SrmModel& out, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 16) { fclose(f); if (err) *err = "too small"; return false; }
    std::vector<uint8_t> data((size_t)sz);
    if (fread(data.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); if (err) *err = "read error"; return false; }
    fclose(f);

    const uint8_t* d = data.data();
    size_t n = data.size();
    if (memcmp(d, "MAIN", 4) != 0) { if (err) *err = "missing MAIN magic"; return false; }

    out.path = path;
    uint32_t thmbSize = u32(d, 12);
    size_t pos = 16 + thmbSize;

    while (pos + 8 <= n) {
        uint32_t chunkSize = u32(d, pos + 4);
        size_t chunkEnd = pos + 8 + chunkSize;
        if (chunkEnd > n) break;
        if (memcmp(d + pos, "PMOD", 4) == 0) {
            size_t p = pos + 8 + 4;  // skip version
            uint32_t nodeCount = u32(d, p); p += 4;
            p += 4;  // unk1
            p += 4;  // root parent
            p += 4;  // root unknown
            for (uint32_t i = 0; i < nodeCount; i++) {
                SrmNode node;
                uint16_t nlen = u16(d, p); p += 2;
                node.name.assign((const char*)(d + p), nlen); p += nlen;
                node.parentIdx = i32(d, p); p += 4;               // unk3 == parent index
                node.position = Vec3(f32(d,p), f32(d,p+4), f32(d,p+8)); p += 12;
                node.rotation = Vec3(f32(d,p), f32(d,p+4), f32(d,p+8)); p += 12;
                node.scale    = Vec3(f32(d,p), f32(d,p+4), f32(d,p+8)); p += 16; // sx,sy,sz,(sw)
                node.unk4 = u32(d, p); p += 4;
                node.meshIndex = i32(d, p); p += 4;               // unk5 == mesh index
                p += 4;                                           // unk6
                out.nodes.push_back(std::move(node));
            }
            while (p + 8 <= chunkEnd && memcmp(d + p, "MESH", 4) == 0) {
                uint32_t meshSize = u32(d, p + 4);
                size_t meshEnd = p + 8 + meshSize;
                SrmMesh mesh;
                parseMesh(d, p + 8, meshEnd, mesh);
                out.meshes.push_back(std::move(mesh));
                p = meshEnd;
            }
        } else if (memcmp(d + pos, "MOTS", 4) == 0) {
            parseMots(d, pos + 8, chunkEnd, out.motions);
        }
        pos = chunkEnd;
    }
    return true;
}

// --- MOTS animation helpers -------------------------------------------------

std::vector<NodeSpin> srm_node_spins(const MotsMotion& mo) {
    std::vector<NodeSpin> out;
    for (size_t ni = 0; ni < mo.nodeChannel.size(); ni++) {
        int ch = mo.nodeChannel[ni];
        if (ch < 0 || ch >= (int)mo.channels.size()) continue;
        const auto& keys = mo.channels[ch].keys;
        if (keys.empty()) continue;
        int best = -1; float bestSpan = 0;
        int lim = (int)keys.size(); if (lim > 6) lim = 6;
        for (int i = 0; i < lim; i++) {
            float s = std::fabs(keys[i].v1 - keys[i].v0);
            if (s > bestSpan) { bestSpan = s; best = i; }
        }
        if (best < 0 || bestSpan < 1e-3f) continue;
        NodeSpin ns; ns.nodeIndex = (int)ni; ns.dof = best;
        ns.v0 = keys[best].v0; ns.v1 = keys[best].v1; ns.duration = mo.duration;
        out.push_back(ns);
    }
    return out;
}

int srm_loop_motion(const SrmModel& m) {
    int best = -1, bestRot = 0;
    for (size_t i = 0; i < m.motions.size(); i++) {
        int rot = 0;
        for (auto& ns : srm_node_spins(m.motions[i])) if (ns.isRotation()) rot++;
        if (rot > bestRot) { bestRot = rot; best = (int)i; }
    }
    return best;
}

static Mat4 node_local(const SrmNode& nd) {
    Mat4 R = Mat4::rotX(nd.rotation.x) * Mat4::rotY(nd.rotation.y) * Mat4::rotZ(nd.rotation.z);
    Mat4 T = Mat4::translation(nd.position.x, nd.position.y, nd.position.z);
    Mat4 S = Mat4::scale(nd.scale.x, nd.scale.y, nd.scale.z);
    return T * R * S;
}

// Compose per-node world matrices from provided local matrices (parent chain,
// iterative with a cycle guard).
static std::vector<Mat4> compose_world(const SrmModel& m, const std::vector<Mat4>& local) {
    size_t n = m.nodes.size();
    std::vector<Mat4> world(n);
    std::vector<char> done(n, 0);
    for (size_t i = 0; i < n; i++) {
        std::vector<int> chain;
        int cur = (int)i;
        while (cur >= 0 && cur < (int)n && !done[cur]) {
            chain.push_back(cur);
            int par = m.nodes[cur].parentIdx;
            if (par == cur) break;
            cur = par;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            int idx = *it;
            int par = m.nodes[idx].parentIdx;
            if (par >= 0 && par < (int)n && par != idx && done[par])
                world[idx] = world[par] * local[idx];
            else
                world[idx] = local[idx];
            done[idx] = 1;
        }
    }
    return world;
}

std::vector<Mat4> srm_world_matrices(const SrmModel& m) {
    std::vector<Mat4> local(m.nodes.size());
    for (size_t i = 0; i < m.nodes.size(); i++) local[i] = node_local(m.nodes[i]);
    return compose_world(m, local);
}

static Vec3 readPos(const SrmStream& s, int i);   // fwd

// Descendants of `node` (inclusive), by parent chain.
static std::vector<int> nodeGroup(const SrmModel& m, int node) {
    std::vector<int> grp, stack{ node };
    while (!stack.empty()) {
        int c = stack.back(); stack.pop_back();
        grp.push_back(c);
        for (int i = 0; i < (int)m.nodes.size(); i++)
            if (m.nodes[i].parentIdx == c && i != c) stack.push_back(i);
    }
    return grp;
}

// Rotation by `ang` about a world axis (0=X,1=Y,2=Z) through `pivot`.
static Mat4 rotAboutAxis(int axis, float ang, Vec3 pivot) {
    Mat4 R = axis == 0 ? Mat4::rotX(ang) : (axis == 1 ? Mat4::rotY(ang) : Mat4::rotZ(ang));
    return Mat4::translation(pivot.x, pivot.y, pivot.z) * R *
           Mat4::translation(-pivot.x, -pivot.y, -pivot.z);
}

std::vector<Mat4> srm_animated_world(const SrmModel& m, int motionIdx, float timeSec) {
    std::vector<Mat4> world = srm_world_matrices(m);
    if (motionIdx < 0 || motionIdx >= (int)m.motions.size()) return world;

    // A rotor/dish spins about its shaft. We don't yet have the engine's exact
    // node-local axis (the stored DOF is in a converted space -> naive euler
    // override tumbles it), so pick the axis GEOMETRICALLY: a disc is thinnest
    // along its shaft, so rotate the node+descendants about the group's
    // minimum-extent world axis through the group centroid. Matches the Blender
    // demo (blendertools/mots_play.py) and is robust. Provisional.
    for (const NodeSpin& ns : srm_node_spins(m.motions[motionIdx])) {
        if (!ns.isRotation()) continue;
        if (ns.nodeIndex < 0 || ns.nodeIndex >= (int)m.nodes.size()) continue;
        std::vector<int> grp = nodeGroup(m, ns.nodeIndex);
        Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        bool any = false;
        for (int gi : grp) {
            const SrmNode& nd = m.nodes[gi];
            if (nd.meshIndex < 0 || nd.meshIndex >= (int)m.meshes.size()) continue;
            const SrmStream* pos = m.meshes[nd.meshIndex].byUsage(USAGE_POSITION);
            if (!pos) continue;
            for (int v = 0; v < pos->vertexCount; v++) {
                Vec3 wp = world[gi].point(readPos(*pos, v));
                if (!any) { lo = hi = wp; any = true; }
                lo.x=std::min(lo.x,wp.x); lo.y=std::min(lo.y,wp.y); lo.z=std::min(lo.z,wp.z);
                hi.x=std::max(hi.x,wp.x); hi.y=std::max(hi.y,wp.y); hi.z=std::max(hi.z,wp.z);
            }
        }
        if (!any) continue;
        Vec3 c = (lo + hi) * 0.5f, e = hi - lo;
        int axis = (e.x < e.y) ? (e.x < e.z ? 0 : 2) : (e.y < e.z ? 1 : 2);
        Mat4 R = rotAboutAxis(axis, ns.sample(timeSec), c);
        for (int gi : grp) world[gi] = R * world[gi];   // rigid group spin
    }
    return world;
}

static Vec3 readPos(const SrmStream& s, int i) {
    const uint8_t* d = s.data.data() + (size_t)i * s.stride;
    return Vec3(f32(d,0), f32(d,4), f32(d,8));
}
static Vec3 readNormal(const SrmStream& s, int i) {
    const uint8_t* d = s.data.data() + (size_t)i * s.stride;
    if (s.stride == 4)
        return Vec3(d[0]/127.5f - 1.0f, d[1]/127.5f - 1.0f, d[2]/127.5f - 1.0f);
    return Vec3(f32(d,0), f32(d,4), f32(d,8));
}
static void readUV(const SrmStream& s, int i, float& u, float& v) {
    const uint8_t* d = s.data.data() + (size_t)i * s.stride;
    u = f32(d, 0); v = f32(d, 4);
}

static std::string pickDiffuse(const SrmMesh& mesh) {
    if (mesh.submeshes.empty()) return "";
    const auto& tex = mesh.submeshes[0].textures;
    const char* keys[] = { "DiffuseTexture", "DiffuseSpecTexture", "DiffuseSpec", "Diffuse" };
    for (const char* k : keys) { auto it = tex.find(k); if (it != tex.end()) return it->second; }
    // fall back to any texture that isn't clearly a normal/aux map
    for (const auto& kv : tex) {
        std::string kl = kv.first;
        if (kl.find("Normal") == std::string::npos && kl.find("Bump") == std::string::npos)
            return kv.second;
    }
    return "";
}

// 0 = BASE, 1 = STD, 2 = UPG  (from a bone/node name's suffix).
static int variantTag(const std::string& name) {
    std::string n; n.reserve(name.size());
    for (char c : name) n += (char)tolower((unsigned char)c);
    if (n.find("_upg") != std::string::npos) return 2;
    if (n.find("_std") != std::string::npos) return 1;
    return 0;
}
static bool variantKeep(int tag, Variant v) {
    if (v == VAR_ALL) return true;
    if (v == VAR_STANDARD) return tag == 0 || tag == 1;
    return tag == 0 || tag == 2;   // VAR_UPGRADED
}

// Palette compaction: a BONE-palette value V does NOT index nodes directly; it
// indexes a COMPACT bone-node array (the Ghidra model+0x180 gather). That array
// is a file-order subset of nodes selected by the unk4 role bitfield:
//   bone bits 0x02 (wheel) | 0x08 (deform) | 0x10 (scroll) | 0x20 (rotate).
// A few baked "merged" tanks reference plain nodes too; there the palette
// overflows the bone-bit subset and the array is all non-container nodes
// (unk4 != 4). Both branches are strictly file-order subsets so result[V] is
// monotonic. Verified corpus-wide (Vehicles/Buildings/Objects 100%).
std::vector<int> srm_bone_node_list(const SrmModel& m) {
    std::vector<int> c3a;
    for (size_t i = 0; i < m.nodes.size(); i++)
        if (m.nodes[i].unk4 & 0x3A) c3a.push_back((int)i);
    int maxpal = -1;
    for (const auto& mesh : m.meshes)
        for (uint16_t v : mesh.bones) if ((int)v > maxpal) maxpal = (int)v;
    if (maxpal < (int)c3a.size()) return c3a;
    std::vector<int> all;
    for (size_t i = 0; i < m.nodes.size(); i++)
        if (m.nodes[i].unk4 != 4) all.push_back((int)i);
    return all;
}

void srm_build_render(const SrmModel& m, SkinMode mode, Variant variant,
                      std::vector<RenderMesh>& out) {
    srm_build_render_w(m, mode, variant, nullptr, out);
}

void srm_build_render_w(const SrmModel& m, SkinMode mode, Variant variant,
                        const std::vector<Mat4>* worldOverride,
                        std::vector<RenderMesh>& out) {
    std::vector<Mat4> world = (worldOverride && worldOverride->size() == m.nodes.size())
                              ? *worldOverride : srm_world_matrices(m);
    std::vector<Mat4> rot(world.size());
    for (size_t i = 0; i < world.size(); i++) { rot[i] = world[i]; for (int r=0;r<3;r++){ rot[i].m[r][3]=0; } rot[i].m[3][0]=rot[i].m[3][1]=rot[i].m[3][2]=0; }
    // Palette value -> real node index (compaction). node = bnl[palette_value].
    std::vector<int> bnl = srm_bone_node_list(m);

    for (size_t ni = 0; ni < m.nodes.size(); ni++) {
        const SrmNode& node = m.nodes[ni];
        if (node.meshIndex < 0 || node.meshIndex >= (int)m.meshes.size()) continue;
        const SrmMesh& mesh = m.meshes[node.meshIndex];
        const SrmStream* pos = mesh.byUsage(USAGE_POSITION);
        if (!pos) continue;
        const SrmStream* nrm = mesh.byUsage(USAGE_NORMAL);
        const SrmStream* uv  = mesh.byUsage(USAGE_TEXCOORD);
        const SrmStream* bi  = mesh.byUsage(USAGE_BLENDINDICES);
        const SrmStream* bw  = mesh.byUsage(USAGE_BLENDWEIGHT);

        int vcount = pos->vertexCount;
        bool haveBones = !mesh.bones.empty();
        // A BLENDINDICES stream = smooth skinning: verts are in MODEL space and
        // the game's skin matrix is boneWorld*inverseBind, which at the static
        // rest pose is identity -> render the bind pose (do NOT apply boneWorld,
        // which would double-transform, mangling characters). Rigid meshes have
        // no BLENDINDICES: their verts are in bone-LOCAL space -> skin normally.
        bool smooth = haveBones && bi && bi->stride == 4;
        bool rigid = haveBones && !smooth && nrm && nrm->stride == 4;

        RenderMesh rm;
        rm.nodeIndex = (int)ni;
        rm.diffuseTex = pickDiffuse(mesh);
        rm.verts.resize(vcount);
        std::vector<int> vtag(vcount, 0);   // per-vertex variant tag (bone node suffix)

        for (int vi = 0; vi < vcount; vi++) {
            Vec3 p = readPos(*pos, vi);
            Vec3 nrmv = nrm ? readNormal(*nrm, vi) : Vec3(0, 0, 1);
            Vec3 wp, wn;
            int boneNode = (int)ni;   // for variant tagging

            if (haveBones && smooth) {
                const uint8_t* bid = bi->data.data() + (size_t)vi * bi->stride;
                float w[4] = {1,0,0,0};
                if (bw && bw->stride == 4) {
                    const uint8_t* bwd = bw->data.data() + (size_t)vi * bw->stride;
                    for (int k = 0; k < 4; k++) w[k] = bwd[k] / 255.0f;
                }
                // dominant influence -> variant tag (remap palette value -> node)
                int best = 0; for (int k = 1; k < 4; k++) if (w[k] > w[best]) best = k;
                int bl = bid[best];
                if (bl < (int)mesh.bones.size()) {
                    int V = mesh.bones[bl];
                    if (V >= 0 && V < (int)bnl.size()) boneNode = bnl[V];
                }
                // Smooth: always bind pose (static skin matrix == identity).
                wp = world[ni].point(p); wn = rot[ni].dir(nrmv);
            } else if (haveBones && rigid) {
                const uint8_t* nd = nrm->data.data() + (size_t)vi * nrm->stride;
                int local = nd[3];
                int V = (local < (int)mesh.bones.size()) ? mesh.bones[local] : -1;
                int bn = (V >= 0 && V < (int)bnl.size()) ? bnl[V] : -1;
                if (bn >= 0 && bn < (int)world.size()) boneNode = bn;
                bool doSkin = (mode == SKIN_FULL);
                if (doSkin && bn >= 0 && bn < (int)world.size()) {
                    wp = world[bn].point(p); wn = rot[bn].dir(nrmv);
                } else { wp = world[ni].point(p); wn = rot[ni].dir(nrmv); }
            } else {
                // unskinned mesh, or NONE mode: place by owning node's world matrix
                wp = world[ni].point(p);
                wn = rot[ni].dir(nrmv);
            }

            if (boneNode >= 0 && boneNode < (int)m.nodes.size())
                vtag[vi] = variantTag(m.nodes[boneNode].name);

            RVertex& rv = rm.verts[vi];
            rv.x = wp.x; rv.y = wp.y; rv.z = wp.z;
            Vec3 nn = normalize(wn);
            rv.nx = nn.x; rv.ny = nn.y; rv.nz = nn.z;
            if (uv) readUV(*uv, vi, rv.u, rv.v); else { rv.u = 0; rv.v = 0; }
        }

        // Variant face filter (drop tris whose verts belong to an excluded loadout).
        if (variant == VAR_ALL) {
            rm.indices = mesh.indices;
        } else {
            const auto& src = mesh.indices;
            rm.indices.reserve(src.size());
            for (size_t i = 0; i + 2 < src.size(); i += 3) {
                uint32_t a = src[i], b = src[i+1], c = src[i+2];
                if (a < (uint32_t)vcount && b < (uint32_t)vcount && c < (uint32_t)vcount &&
                    variantKeep(vtag[a], variant) && variantKeep(vtag[b], variant) &&
                    variantKeep(vtag[c], variant)) {
                    rm.indices.push_back(a); rm.indices.push_back(b); rm.indices.push_back(c);
                }
            }
        }
        if (rm.indices.size() >= 3) out.push_back(std::move(rm));
    }
}
