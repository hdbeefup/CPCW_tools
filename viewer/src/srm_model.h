// CPCW .srm parser + skinning, ported from
// blendertools/SRM_Blender/{srm_format,import_srm}.py.
//
// Renders in the game's NATIVE DirectX left-handed Y-up space (no LH->RH
// reflection — that is a Blender-only step), so geometry matches how the
// engine draws it.  Skinning uses the rule proven in Ghidra:
//     v_world = boneWorld[ BONE_palette[boneIdx] ] * v_stored     (no inverse-bind)
// which is exactly what the importer's apply_skin='FULL' does.
#pragma once
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include "mathx.h"

// D3DDECLUSAGE values (the VERS "usage" word).
enum {
    USAGE_POSITION = 0, USAGE_BLENDWEIGHT = 1, USAGE_BLENDINDICES = 2,
    USAGE_NORMAL = 3, USAGE_TEXCOORD = 4, USAGE_TANGENT = 5, USAGE_BINORMAL = 6
};

struct SrmStream {
    int usage = 0, semantic = 0, stride = 0, vertexCount = 0;
    std::vector<uint8_t> data;
};

struct SrmSubmesh {
    std::string materialName;
    std::map<std::string, std::string> textures;   // e.g. "DiffuseTexture" -> "foo.dds"
};

struct SrmMesh {
    int streamCount = 0, submeshCount = 0;
    std::vector<uint16_t> bones;      // palette: local index -> node index
    std::vector<uint32_t> indices;
    std::vector<SrmStream> streams;
    std::vector<SrmSubmesh> submeshes;
    const SrmStream* byUsage(int u) const {
        for (const auto& s : streams) if (s.usage == u) return &s;
        return nullptr;
    }
};

struct SrmNode {
    std::string name;
    int parentIdx = -1;
    Vec3 position, rotation;
    Vec3 scale = Vec3(1, 1, 1);
    uint32_t unk4 = 0;
    int meshIndex = -1;     // index into SrmModel::meshes, or -1
};

// --- MOTS node animation (see cpcw_mots.py; reverse-engineered + Ghidra-checked)
// A channel's keys are the up-to-6 per-DOF component tracks (tx,ty,tz,rx,ry,rz);
// the moving one carries the animation. Provisional playback: rotation DOF value
// = angle in radians about the node's local axis, linear/uniform over duration.
struct MotsKey { float v0 = 0, v1 = 0; };
struct MotsChannel { std::vector<MotsKey> keys; };
struct MotsMotion {
    std::string name;
    float duration = 0;
    std::vector<int> nodeChannel;        // per model node -> channel idx (-1 static)
    std::vector<MotsChannel> channels;
};

struct SrmModel {
    std::vector<SrmNode> nodes;
    std::vector<SrmMesh> meshes;
    std::vector<MotsMotion> motions;
    std::string path;
};

// The dominant animated DOF of one node over a motion (provisional).
struct NodeSpin { int nodeIndex = -1; int dof = 0; float v0 = 0, v1 = 0, duration = 0;
    bool isRotation() const { return dof >= 3; }
    float sample(float t) const {                      // linear, looping
        if (duration <= 0) return v1;
        float f = (t - duration * std::floor(t / duration)) / duration;
        return v0 + (v1 - v0) * f;
    }
};
std::vector<NodeSpin> srm_node_spins(const MotsMotion& mo);
// Index of the motion best suited to a looping spin preview (most rotation
// tracks), or -1 if the model has none.
int srm_loop_motion(const SrmModel& m);

bool srm_parse(const std::string& path, SrmModel& out, std::string* err = nullptr);

// --- render build ---

// AUTO  = per-mesh heuristic: skin bone-local (vehicle) meshes, leave
//         model-space (building/character) meshes in bind pose. Default.
// FULL  = apply the exact skin rule to every rigid mesh (assembles vehicles;
//         scatters model-space buildings — use to see the raw rule).
// NONE  = raw bind pose, nothing skinned.
enum SkinMode { SKIN_AUTO, SKIN_FULL, SKIN_NONE };

// Upgrade-variant filter. Vehicles pack every loadout in one .srm; a part's
// variant is read from the SUFFIX of the bone it is skinned to (_std / _upg;
// camo-net parts always carry _upg). Matches import_srm.py's convention.
enum Variant { VAR_ALL, VAR_STANDARD, VAR_UPGRADED };

struct RVertex { float x, y, z, nx, ny, nz, u, v; };

struct RenderMesh {
    std::vector<RVertex> verts;
    std::vector<uint32_t> indices;
    int nodeIndex = -1;
    std::string diffuseTex;   // basename (no path), or empty
};

// Build world matrices per node (parent chain, T*R*S, R=Rx*Ry*Rz).
std::vector<Mat4> srm_world_matrices(const SrmModel& m);

// World matrices with `motionIdx` evaluated at time t: each animated node's
// local euler gets its rotation-DOF component overridden by the sampled angle
// (native SRM space -> faithful). timeSec loops over the clip. motionIdx < 0
// returns the rest matrices.
std::vector<Mat4> srm_animated_world(const SrmModel& m, int motionIdx, float timeSec);

// Produce world-space render meshes (skinned per `mode`, filtered per `variant`).
void srm_build_render(const SrmModel& m, SkinMode mode, Variant variant,
                      std::vector<RenderMesh>& out);
// Same, but with caller-provided per-node world matrices (e.g. animated).
// `worldOverride` size must equal m.nodes.size(); nullptr = compute rest.
void srm_build_render_w(const SrmModel& m, SkinMode mode, Variant variant,
                        const std::vector<Mat4>* worldOverride,
                        std::vector<RenderMesh>& out);
