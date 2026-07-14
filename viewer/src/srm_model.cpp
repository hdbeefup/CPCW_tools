#include "srm_model.h"
#include <cstdio>
#include <cstring>

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
        }
        pos = chunkEnd;
    }
    return true;
}

std::vector<Mat4> srm_world_matrices(const SrmModel& m) {
    size_t n = m.nodes.size();
    std::vector<Mat4> world(n);
    std::vector<char> done(n, 0);
    // iterative resolve with cycle guard
    for (size_t i = 0; i < n; i++) {
        // build chain
        std::vector<int> chain;
        int cur = (int)i;
        while (cur >= 0 && cur < (int)n && !done[cur]) {
            chain.push_back(cur);
            int par = m.nodes[cur].parentIdx;
            if (par == cur) break;
            cur = par;
        }
        // resolve from top of chain down
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            int idx = *it;
            const SrmNode& nd = m.nodes[idx];
            Mat4 R = Mat4::rotX(nd.rotation.x) * Mat4::rotY(nd.rotation.y) * Mat4::rotZ(nd.rotation.z);
            Mat4 T = Mat4::translation(nd.position.x, nd.position.y, nd.position.z);
            Mat4 S = Mat4::scale(nd.scale.x, nd.scale.y, nd.scale.z);
            Mat4 local = T * R * S;
            int par = nd.parentIdx;
            if (par >= 0 && par < (int)n && par != idx && done[par])
                world[idx] = world[par] * local;
            else
                world[idx] = local;
            done[idx] = 1;
        }
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

void srm_build_render(const SrmModel& m, SkinMode mode, Variant variant,
                      std::vector<RenderMesh>& out) {
    std::vector<Mat4> world = srm_world_matrices(m);
    std::vector<Mat4> rot(world.size());
    for (size_t i = 0; i < world.size(); i++) { rot[i] = world[i]; for (int r=0;r<3;r++){ rot[i].m[r][3]=0; } rot[i].m[3][0]=rot[i].m[3][1]=rot[i].m[3][2]=0; }

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
        bool rigid = haveBones && nrm && nrm->stride == 4;
        bool smooth = haveBones && bi && bi->stride == 4;

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

            bool skinned = (mode == SKIN_FULL) && haveBones;
            if (haveBones && smooth) {
                const uint8_t* bid = bi->data.data() + (size_t)vi * bi->stride;
                float w[4] = {1,0,0,0};
                if (bw && bw->stride == 4) {
                    const uint8_t* bwd = bw->data.data() + (size_t)vi * bw->stride;
                    for (int k = 0; k < 4; k++) w[k] = bwd[k] / 255.0f;
                }
                // dominant influence -> variant tag
                int best = 0; for (int k = 1; k < 4; k++) if (w[k] > w[best]) best = k;
                int bl = bid[best];
                if (bl < (int)mesh.bones.size()) boneNode = mesh.bones[bl];
                if (skinned) {
                    wp = Vec3(0,0,0); wn = Vec3(0,0,0);
                    float wsum = 0;
                    for (int k = 0; k < 4; k++) {
                        if (w[k] <= 0) continue;
                        int local = bid[k];
                        int bn = (local < (int)mesh.bones.size()) ? mesh.bones[local] : -1;
                        if (bn < 0 || bn >= (int)world.size()) continue;
                        wp = wp + world[bn].point(p) * w[k];
                        wn = wn + rot[bn].dir(nrmv) * w[k];
                        wsum += w[k];
                    }
                    if (wsum <= 1e-6f) { wp = p; wn = nrmv; }
                } else { wp = world[ni].point(p); wn = rot[ni].dir(nrmv); }
            } else if (haveBones && rigid) {
                const uint8_t* nd = nrm->data.data() + (size_t)vi * nrm->stride;
                int local = nd[3];
                int bn = (local < (int)mesh.bones.size()) ? mesh.bones[local] : -1;
                if (bn >= 0 && bn < (int)world.size()) boneNode = bn;
                if (skinned) {
                    if (bn >= 0 && bn < (int)world.size()) { wp = world[bn].point(p); wn = rot[bn].dir(nrmv); }
                    else { wp = p; wn = nrmv; }
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
