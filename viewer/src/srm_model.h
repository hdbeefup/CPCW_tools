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

struct SrmModel {
    std::vector<SrmNode> nodes;
    std::vector<SrmMesh> meshes;
    std::string path;
};

bool srm_parse(const std::string& path, SrmModel& out, std::string* err = nullptr);

// --- render build ---

enum SkinMode { SKIN_FULL, SKIN_NONE };   // FULL = exact game rule; NONE = raw bind pose

struct RVertex { float x, y, z, nx, ny, nz, u, v; };

struct RenderMesh {
    std::vector<RVertex> verts;
    std::vector<uint32_t> indices;
    int nodeIndex = -1;
    std::string diffuseTex;   // basename (no path), or empty
};

// Build world matrices per node (parent chain, T*R*S, R=Rx*Ry*Rz).
std::vector<Mat4> srm_world_matrices(const SrmModel& m);

// Produce world-space render meshes (skinned per `mode`).
void srm_build_render(const SrmModel& m, SkinMode mode, std::vector<RenderMesh>& out);
