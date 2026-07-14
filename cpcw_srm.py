#!/usr/bin/env python3
"""Parse and convert Codename: Panzers Cold War .srm model files to glTF/GLB."""

import argparse
import json
import math
import os
import struct
import sys

# Vertex semantic IDs (raw ``semantic`` word; normal/tangent/binormal all = 4).
SEM_TEXCOORD = 1
SEM_POSITION = 2
SEM_NORMAL = 3
SEM_TANGENT = 4
SEM_BINORMAL = 5
SEM_COLOR = 6

# D3DDECLUSAGE (the VERS ``usage`` word) — the real per-stream type. Prefer
# these over the semantic aliases above. See cpcw_srm_writer.py / FORMAT_SRM.md.
USAGE_POSITION = 0
USAGE_BLENDWEIGHT = 1
USAGE_BLENDINDICES = 2
USAGE_NORMAL = 3
USAGE_TEXCOORD = 4
USAGE_TANGENT = 5
USAGE_BINORMAL = 6


class SrmNode:
    __slots__ = ('name', 'parent_idx', 'unk', 'position', 'rotation', 'scale',
                 'unk3', 'unk4', 'unk5', 'unk6', 'mesh')

    def __init__(self):
        self.name = ''
        self.parent_idx = -1
        self.unk = 0
        self.position = (0, 0, 0)
        self.rotation = (0, 0, 0)
        self.scale = (1, 1, 1)
        self.unk3 = 0
        self.unk4 = 0
        self.unk5 = -1
        self.unk6 = -1
        self.mesh = None


class SrmMesh:
    __slots__ = ('stream_count', 'submesh_count', 'bones', 'indices', 'idx_stride',
                 'vertex_streams', 'submeshes')

    def __init__(self):
        self.stream_count = 0
        self.submesh_count = 0
        self.bones = []
        self.indices = []
        self.idx_stride = 2
        self.vertex_streams = []
        self.submeshes = []

    def stream(self, semantic):
        for vs in self.vertex_streams:
            if vs.semantic == semantic:
                return vs
        return None

    def stream_by_usage(self, usage):
        """Return the first vertex stream with the given D3DDECLUSAGE, or None."""
        for vs in self.vertex_streams:
            if vs.usage == usage:
                return vs
        return None

    def vertex_bone_indices(self):
        """Per-vertex bone-palette index (byte 3 of the NORMAL stream), or None.

        Rigid skinning stores one bone-palette index per vertex in byte 3 of the
        NORMAL stream (D3DDECLUSAGE 3). Verified corpus-wide: for every skinned
        mesh ``max(byte3) <= len(bones)-1``. (``semantic`` cannot select the
        normal stream: normal/tangent/binormal all share semantic 4 — use usage.)
        """
        if not self.bones:
            return None
        nrm = self.stream_by_usage(USAGE_NORMAL)
        if nrm is None or nrm.stride != 4:
            return None
        return [nrm.data[i * 4 + 3] for i in range(nrm.vertex_count)]


class VertexStream:
    __slots__ = ('semantic', 'usage', 'stride', 'vertex_count', 'data')

    def __init__(self, semantic, stride, vertex_count, data, usage=None):
        self.semantic = semantic
        self.usage = usage if usage is not None else semantic
        self.stride = stride
        self.vertex_count = vertex_count
        self.data = data  # raw bytes


class Submesh:
    __slots__ = ('face_start', 'face_count', 'vert_start', 'vert_count',
                 'material_name', 'textures')

    def __init__(self):
        self.face_start = 0
        self.face_count = 0
        self.vert_start = 0
        self.vert_count = 0
        self.material_name = ''
        self.textures = {}


def parse_srm(filepath):
    """Parse an SRM file and return list of nodes."""
    filepath = os.path.normpath(filepath)
    data = open(filepath, 'rb').read()

    if data[:4] != b'MAIN':
        raise ValueError("Not an SRM file")

    # Skip THMB
    thmb_size = struct.unpack_from('<I', data, 12)[0]
    pos = 16 + thmb_size

    nodes = []

    # Parse PMOD chunks
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        chunk_size = struct.unpack_from('<I', data, pos + 4)[0]
        chunk_end = pos + 8 + chunk_size

        if tag == b'PMOD':
            pmod_nodes = _parse_pmod(data, pos + 8, chunk_end)
            nodes.extend(pmod_nodes)
        pos = chunk_end

    return nodes


def _parse_pmod(data, start, end):
    """Parse a PMOD chunk.

    All node headers come first, followed by a block of MESH chunks. A node's
    ``unk5`` is the index into that MESH block (-1 = transform-only node), and
    ``unk3`` is the node's parent index (-1 = root). The meshes are NOT stored
    inline after each node header -- only the last header abuts the block, so an
    inline scan would miss every mesh but one (which is why multi-mesh models,
    e.g. most buildings, used to lose the bulk of their geometry).
    """
    p = start + 4  # skip version
    node_count = struct.unpack_from('<I', data, p)[0]; p += 4
    _unk1 = struct.unpack_from('<I', data, p)[0]; p += 4
    # 8 bytes of header padding/root info
    _h1 = struct.unpack_from('<I', data, p)[0]; p += 4
    _h2 = struct.unpack_from('<I', data, p)[0]; p += 4

    nodes = []
    for ni in range(node_count):
        node = SrmNode()
        # Name
        nlen = struct.unpack_from('<H', data, p)[0]; p += 2
        node.name = data[p:p + nlen].decode('ascii', errors='replace'); p += nlen

        # Transform data (56 bytes)
        node.unk3 = struct.unpack_from('<i', data, p)[0]; p += 4
        node.parent_idx = node.unk3  # unk3 is the parent node index
        node.position = struct.unpack_from('<3f', data, p); p += 12
        node.rotation = struct.unpack_from('<3f', data, p); p += 12
        sx, sy, sz, sw = struct.unpack_from('<4f', data, p); p += 16
        node.scale = (sx, sy, sz)
        node.unk4 = struct.unpack_from('<I', data, p)[0]; p += 4
        node.unk5 = struct.unpack_from('<i', data, p)[0]; p += 4
        node.unk6 = struct.unpack_from('<i', data, p)[0]; p += 4

        nodes.append(node)

    # MESH block: consecutive MESH chunks after the last node header.
    meshes = []
    while p + 8 <= end and data[p:p + 4] == b'MESH':
        mesh_size = struct.unpack_from('<I', data, p + 4)[0]
        mesh_end = p + 8 + mesh_size
        meshes.append(_parse_mesh(data, p + 8, mesh_end))
        p = mesh_end

    for node in nodes:
        if 0 <= node.unk5 < len(meshes):
            node.mesh = meshes[node.unk5]

    return nodes


def _parse_mesh(data, start, end):
    """Parse a MESH chunk content."""
    mesh = SrmMesh()
    p = start
    mesh.stream_count = struct.unpack_from('<I', data, p)[0]; p += 4
    mesh.submesh_count = struct.unpack_from('<I', data, p)[0]; p += 4

    # Optional BONE chunk
    if data[p:p + 4] == b'BONE':
        bone_size = struct.unpack_from('<I', data, p + 4)[0]
        bp = p + 8
        bone_count = struct.unpack_from('<I', data, bp)[0]
        mesh.bones = list(struct.unpack_from(f'<{bone_count}H', data, bp + 4))
        p = p + 8 + bone_size

    # INDS
    assert data[p:p + 4] == b'INDS', f"Expected INDS at 0x{p:x}, got {data[p:p+4]}"
    inds_size = struct.unpack_from('<I', data, p + 4)[0]; p += 8
    idx_count = struct.unpack_from('<I', data, p)[0]; p += 4
    mesh.idx_stride = struct.unpack_from('<I', data, p)[0]; p += 4

    if mesh.idx_stride == 2:
        mesh.indices = list(struct.unpack_from(f'<{idx_count}H', data, p))
    else:
        mesh.indices = list(struct.unpack_from(f'<{idx_count}I', data, p))
    p += idx_count * mesh.idx_stride

    # VERS streams
    for _ in range(mesh.stream_count):
        assert data[p:p + 4] == b'VERS', f"Expected VERS at 0x{p:x}"
        vs_size = struct.unpack_from('<I', data, p + 4)[0]; p += 8
        vs_end = p + vs_size
        _f1, vert_count, stride, usage, semantic = struct.unpack_from('<IIIII', data, p)
        p += 20
        vdata = data[p:p + vert_count * stride]
        mesh.vertex_streams.append(
            VertexStream(semantic, stride, vert_count, vdata, usage=usage))
        p = vs_end

    # Parse submesh/material data
    if mesh.vertex_streams:
        sm = Submesh()
        sm.vert_start = 0
        sm.vert_count = mesh.vertex_streams[0].vertex_count
        sm.face_start = 0
        sm.face_count = len(mesh.indices)
        sm.material_name = 'default'
        try:
            # Header: flag(1) + 6*u32(24) = 25 bytes, then name
            mp = p + 25
            mlen = struct.unpack_from('<H', data, mp)[0]; mp += 2
            if 0 < mlen < 256 and mp + mlen <= end:
                mname = data[mp:mp + mlen].decode('ascii', errors='replace'); mp += mlen
                if mname.endswith('\\'):
                    mname = mname[:-1]
                sm.material_name = mname

                # Property header: prop_count(u16) + unk(u16) + total(u32)
                prop_count_h = struct.unpack_from('<H', data, mp)[0]; mp += 2
                mp += 2  # skip unk u16
                prop_total = struct.unpack_from('<I', data, mp)[0]; mp += 4

                for _ in range(prop_total):
                    if mp + 2 > end:
                        break
                    plen = struct.unpack_from('<H', data, mp)[0]; mp += 2
                    if mp + plen > end:
                        break
                    pname = data[mp:mp + plen].decode('ascii', errors='replace'); mp += plen
                    ptype = struct.unpack_from('<I', data, mp)[0]; mp += 4
                    psize = struct.unpack_from('<I', data, mp)[0]; mp += 4

                    if ptype == 6:
                        # Texture reference
                        tlen = struct.unpack_from('<H', data, mp)[0]; mp += 2
                        tname = data[mp:mp + tlen].decode('ascii', errors='replace'); mp += tlen
                        sm.textures[pname] = tname
                    elif ptype == 0 and psize > 0:
                        mp += psize  # skip scalar value
        except Exception:
            pass
        mesh.submeshes.append(sm)

    return mesh


def _mat_mul(a, b):
    """Multiply two 4x4 matrices (nested lists)."""
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)]
            for i in range(4)]


def _node_local_matrix(node):
    """Local 4x4 transform: T @ Rx @ Ry @ Rz @ S (rotation order Rx@Ry@Rz)."""
    rx, ry, rz = node.rotation
    cx, sx = math.cos(rx), math.sin(rx)
    cy, sy = math.cos(ry), math.sin(ry)
    cz, sz = math.cos(rz), math.sin(rz)
    Rx = [[1, 0, 0, 0], [0, cx, -sx, 0], [0, sx, cx, 0], [0, 0, 0, 1]]
    Ry = [[cy, 0, sy, 0], [0, 1, 0, 0], [-sy, 0, cy, 0], [0, 0, 0, 1]]
    Rz = [[cz, -sz, 0, 0], [sz, cz, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
    R = _mat_mul(_mat_mul(Rx, Ry), Rz)
    sx_, sy_, sz_ = node.scale
    for i in range(3):
        R[i][0] *= sx_; R[i][1] *= sy_; R[i][2] *= sz_
    R[0][3], R[1][3], R[2][3] = node.position
    return R


def _build_world_matrices(nodes):
    """World matrix per node via the parent_idx (unk3) chain."""
    cache = {}

    def world(i):
        if i in cache:
            return cache[i]
        nd = nodes[i]
        lm = _node_local_matrix(nd)
        p = nd.parent_idx
        wm = _mat_mul(world(p), lm) if (0 <= p < len(nodes) and p != i) else lm
        cache[i] = wm
        return wm

    return [world(i) for i in range(len(nodes))]


def bone_node_list(nodes):
    """Map a BONE-palette value V to the file-order node index it refers to.

    Rigid skinning stores, per vertex, a small palette index (NORMAL byte 3) that
    selects an entry of the mesh's BONE palette. The engine does NOT use that
    palette value as a direct node index; it indexes a COMPACT per-bone
    world-matrix array (Ghidra ``model+0x180`` gather, see docs/FORMAT_SRM.md).
    That compact array is a file-order subset of nodes selected by the ``unk4``
    role bitfield:

        0x02 wheel bone | 0x08 generic deform bone | 0x10 scroll | 0x20 rotate
        0x04 = "Merged mesh" container (never a bone) | 0x01 owns-own-mesh flag
        0x00 = plain transform / gameplay marker (lamp/man/target/...)

    Two exporter regimes, disambiguated by whether any palette value overflows
    the bone-bit subset (both are strictly file-order subsets, so ``result[V]``
    is monotonic):
      * "skinned"  -> bone-bit nodes ``unk4 & 0x3A``            (the common case)
      * "merged"   -> all non-container nodes ``unk4 != 4``     (a few baked tanks)

    Returns a list ``bnl`` such that the correct node for palette value V is
    ``bnl[V]``. Verified corpus-wide (Vehicles/Buildings/Objects 100%).
    """
    c3a = [i for i, n in enumerate(nodes) if (n.unk4 & 0x3A)]
    maxpal = max([max(n.mesh.bones) for n in nodes
                  if n.mesh is not None and n.mesh.bones] or [-1])
    if maxpal < len(c3a):
        return c3a
    return [i for i, n in enumerate(nodes) if n.unk4 != 4]


def _transform_point(m, x, y, z):
    return (m[0][0]*x + m[0][1]*y + m[0][2]*z + m[0][3],
            m[1][0]*x + m[1][1]*y + m[1][2]*z + m[1][3],
            m[2][0]*x + m[2][1]*y + m[2][2]*z + m[2][3])


def bake_skinning(nodes, mode='full'):
    """Rigidly skin each mesh in place: rewrite POSITION streams so vertices are
    transformed by their bone's world matrix, assembling articulated models.

    Applies the exact engine rule ``v_world = boneWorld[node] . v_stored`` to
    every rigid vertex, where ``node = bone_node_list(nodes)[palette_value]``
    (the palette compaction -- see bone_node_list). Non-animated bones (car
    bodies/wheels, gun, turret, window frames) reconstruct pixel-exact; genuinely
    animated bones (tank road wheels / suspension travel) render in their static
    authoring pose because the settled runtime pose is not stored in the file.
    Static models (buildings) should not be skinned -- use --no-skin.

    ``mode`` is accepted for CLI compatibility but only 'none' (handled by the
    caller) skips skinning; every other value performs the faithful assembly.
    """
    world = _build_world_matrices(nodes)
    bnl = bone_node_list(nodes)
    for node in nodes:
        if node.mesh is None:
            continue
        pos = node.mesh.stream(SEM_POSITION)
        bidx = node.mesh.vertex_bone_indices()
        if pos is None or bidx is None:
            continue
        pal = node.mesh.bones
        # Remap each palette value to its real node once per mesh.
        rpal = [bnl[v] if 0 <= v < len(bnl) else -1 for v in pal]
        data = bytearray(pos.data)
        for vi in range(pos.vertex_count):
            bi = bidx[vi]
            if 0 <= bi < len(rpal) and 0 <= rpal[bi] < len(world):
                off = vi * pos.stride
                x, y, z = struct.unpack_from('<3f', data, off)
                nx, ny, nz = _transform_point(world[rpal[bi]], x, y, z)
                struct.pack_into('<3f', data, off, nx, ny, nz)
        pos.data = bytes(data)


def _euler_to_quaternion(rx, ry, rz):
    """Convert XYZ Euler angles (radians) to quaternion [x, y, z, w]."""
    cx, sx = math.cos(rx / 2), math.sin(rx / 2)
    cy, sy = math.cos(ry / 2), math.sin(ry / 2)
    cz, sz = math.cos(rz / 2), math.sin(rz / 2)
    return [
        sx * cy * cz - cx * sy * sz,
        cx * sy * cz + sx * cy * sz,
        cx * cy * sz - sx * sy * cz,
        cx * cy * cz + sx * sy * sz,
    ]


# --- Handedness: SRM is DirectX LEFT-handed Y-up; glTF is RIGHT-handed Y-up. ---
# Converting between them is a REFLECTION M = diag(1, 1, -1) (negate Z), NOT a
# rotation. Writing SRM coords verbatim into glTF leaves every model MIRRORED
# (v200 train lettering read reversed). Apply M consistently to positions,
# normals, node translation and rotation, and reverse triangle winding (a
# reflection flips orientation). Quaternion under M: (x,y,z,w) -> (-x,-y,z,w).

def _reflect_vec3_bytes(raw, stride, count):
    """Return a copy of a packed VEC3-float stream with Z negated."""
    out = bytearray(raw)
    for vi in range(count):
        off = vi * stride
        z = struct.unpack_from('<f', out, off + 8)[0]
        struct.pack_into('<f', out, off + 8, -z)
    return bytes(out)


def _reflect_translation(t):
    return [t[0], t[1], -t[2]]


def _reflect_quaternion(q):
    return [-q[0], -q[1], q[2], q[3]]


def _reverse_winding(indices):
    """Reverse each triangle's winding (a, b, c) -> (a, c, b)."""
    out = list(indices)
    for i in range(0, len(out) - 2, 3):
        out[i + 1], out[i + 2] = out[i + 2], out[i + 1]
    return out


def _find_dds(tex_name, search_dirs):
    """Find a DDS texture file by name in the given directories."""
    for d in search_dirs:
        for candidate in [
            os.path.join(d, tex_name + '.dds'),
            os.path.join(d, tex_name + '.DDS'),
            os.path.join(d, tex_name + '.tga'),
        ]:
            if os.path.isfile(candidate):
                return candidate
    return None


def _dds_to_png_bytes(dds_path):
    """Convert a DDS file to PNG bytes using Pillow."""
    try:
        from PIL import Image
        import io
        img = Image.open(dds_path)
        buf = io.BytesIO()
        img.save(buf, format='PNG')
        return buf.getvalue()
    except Exception:
        return None


def nodes_to_glb(nodes, output_path, texture_dirs=None):
    """Convert parsed SRM nodes to a GLB file. texture_dirs: list of dirs to search for DDS textures."""
    if texture_dirs is None:
        texture_dirs = []

    gltf_nodes = []
    gltf_meshes = []
    gltf_accessors = []
    gltf_buffer_views = []
    gltf_materials = []
    mat_map = {}
    bin_data = bytearray()

    def add_buffer(data_bytes, target=None):
        """Append data to the binary buffer, return buffer view index."""
        # Pad to 4-byte alignment
        while len(bin_data) % 4 != 0:
            bin_data.append(0)
        offset = len(bin_data)
        bin_data.extend(data_bytes)
        bv = {
            'buffer': 0,
            'byteOffset': offset,
            'byteLength': len(data_bytes),
        }
        if target is not None:
            bv['target'] = target
        idx = len(gltf_buffer_views)
        gltf_buffer_views.append(bv)
        return idx

    def add_accessor(bv_idx, comp_type, count, acc_type, min_val=None, max_val=None):
        """Add an accessor, return its index."""
        acc = {
            'bufferView': bv_idx,
            'componentType': comp_type,
            'count': count,
            'type': acc_type,
        }
        if min_val is not None:
            acc['min'] = min_val
        if max_val is not None:
            acc['max'] = max_val
        idx = len(gltf_accessors)
        gltf_accessors.append(acc)
        return idx

    gltf_textures = []
    gltf_images = []
    gltf_samplers = []
    tex_cache = {}  # tex_name -> gltf texture index

    def add_texture(tex_name):
        """Embed a DDS texture as PNG, return glTF texture index or None."""
        if tex_name in tex_cache:
            return tex_cache[tex_name]

        dds_path = _find_dds(tex_name, texture_dirs)
        if not dds_path:
            tex_cache[tex_name] = None
            return None

        png_bytes = _dds_to_png_bytes(dds_path)
        if not png_bytes:
            tex_cache[tex_name] = None
            return None

        # Add image as buffer view
        bv_idx = add_buffer(png_bytes)
        img_idx = len(gltf_images)
        gltf_images.append({
            'bufferView': bv_idx,
            'mimeType': 'image/png',
            'name': tex_name,
        })

        if not gltf_samplers:
            gltf_samplers.append({
                'magFilter': 9729,  # LINEAR
                'minFilter': 9987,  # LINEAR_MIPMAP_LINEAR
                'wrapS': 10497,     # REPEAT
                'wrapT': 10497,
            })

        tex_idx = len(gltf_textures)
        gltf_textures.append({
            'source': img_idx,
            'sampler': 0,
            'name': tex_name,
        })

        tex_cache[tex_name] = tex_idx
        return tex_idx

    scene_nodes = []

    for node in nodes:
        gn = {'name': node.name}

        # Transform (LH->RH reflection: negate Z of translation, reflect quat)
        t = _reflect_translation(list(node.position))
        r = _reflect_quaternion(_euler_to_quaternion(*node.rotation))
        s = list(node.scale)

        if any(v != 0 for v in t):
            gn['translation'] = t
        if any(abs(v) > 1e-6 for v in r[:3]) or abs(r[3] - 1.0) > 1e-6:
            gn['rotation'] = r
        if any(abs(v - 1.0) > 1e-6 for v in s):
            gn['scale'] = s

        if node.mesh and node.mesh.vertex_streams:
            mesh = node.mesh
            primitives = []

            # Find position, texcoord, normal streams
            pos_stream = None
            uv_stream = None
            norm_stream = None

            for vs in mesh.vertex_streams:
                if vs.semantic == SEM_POSITION and pos_stream is None:
                    pos_stream = vs
                elif vs.semantic == SEM_TEXCOORD and uv_stream is None:
                    uv_stream = vs
                elif vs.semantic == SEM_NORMAL and norm_stream is None:
                    norm_stream = vs

            if pos_stream is None:
                node_idx = len(gltf_nodes)
                gltf_nodes.append(gn)
                scene_nodes.append(node_idx)
                continue

            for sm in mesh.submeshes:
                attributes = {}

                # Position
                vs = pos_stream
                vert_start = sm.vert_start
                vert_count = sm.vert_count
                pos_bytes = vs.data[vert_start * vs.stride:(vert_start + vert_count) * vs.stride]
                # LH->RH reflection: negate Z on every position.
                pos_bytes = _reflect_vec3_bytes(pos_bytes, vs.stride, vert_count)

                # Compute bounds (after reflection)
                pos_min = [float('inf')] * 3
                pos_max = [float('-inf')] * 3
                for vi in range(vert_count):
                    x, y, z = struct.unpack_from('<3f', pos_bytes, vi * vs.stride)
                    pos_min = [min(pos_min[j], v) for j, v in enumerate([x, y, z])]
                    pos_max = [max(pos_max[j], v) for j, v in enumerate([x, y, z])]

                bv = add_buffer(pos_bytes, target=34962)
                if vs.stride != 12:
                    gltf_buffer_views[-1]['byteStride'] = vs.stride
                acc_pos = add_accessor(bv, 5126, vert_count, 'VEC3', pos_min, pos_max)
                attributes['POSITION'] = acc_pos

                # Texcoord
                if uv_stream and uv_stream.vertex_count > vert_start:
                    vs = uv_stream
                    uv_bytes = vs.data[vert_start * vs.stride:(vert_start + vert_count) * vs.stride]
                    bv = add_buffer(uv_bytes, target=34962)
                    if vs.stride != 8:
                        gltf_buffer_views[-1]['byteStride'] = vs.stride
                    acc_uv = add_accessor(bv, 5126, vert_count, 'VEC2')
                    attributes['TEXCOORD_0'] = acc_uv

                # Normal
                if norm_stream and norm_stream.vertex_count > vert_start:
                    vs = norm_stream
                    if vs.stride == 4:
                        # Packed normals: 4 bytes → convert to float3 (negate Z).
                        norm_floats = bytearray()
                        for vi in range(vert_count):
                            off = (vert_start + vi) * vs.stride
                            nx = (vs.data[off] / 127.5) - 1.0
                            ny = (vs.data[off + 1] / 127.5) - 1.0
                            nz = (vs.data[off + 2] / 127.5) - 1.0
                            norm_floats.extend(struct.pack('<3f', nx, ny, -nz))
                        bv = add_buffer(bytes(norm_floats), target=34962)
                        acc_norm = add_accessor(bv, 5126, vert_count, 'VEC3')
                    else:
                        norm_bytes = vs.data[vert_start * vs.stride:(vert_start + vert_count) * vs.stride]
                        # LH->RH reflection: negate Z on every normal.
                        norm_bytes = _reflect_vec3_bytes(norm_bytes, vs.stride, vert_count)
                        bv = add_buffer(norm_bytes, target=34962)
                        if vs.stride != 12:
                            gltf_buffer_views[-1]['byteStride'] = vs.stride
                        acc_norm = add_accessor(bv, 5126, vert_count, 'VEC3')
                    attributes['NORMAL'] = acc_norm

                # Indices (remap to submesh-local)
                face_start = sm.face_start
                face_count = sm.face_count
                sub_indices = mesh.indices[face_start:face_start + face_count]
                # Offset indices to be relative to vert_start
                sub_indices = [i - vert_start for i in sub_indices]
                # LH->RH reflection flips orientation: reverse triangle winding.
                sub_indices = _reverse_winding(sub_indices)

                if mesh.idx_stride == 2:
                    idx_bytes = struct.pack(f'<{len(sub_indices)}H', *sub_indices)
                    comp_type = 5123  # UNSIGNED_SHORT
                else:
                    idx_bytes = struct.pack(f'<{len(sub_indices)}I', *sub_indices)
                    comp_type = 5125  # UNSIGNED_INT

                bv = add_buffer(idx_bytes, target=34963)
                acc_idx = add_accessor(bv, comp_type, len(sub_indices), 'SCALAR')

                # Material (with embedded textures)
                mat_key = sm.material_name or 'default'
                # Include texture names in the key so different tex combos get separate materials
                tex_key = tuple(sorted(sm.textures.items()))
                mat_cache_key = (mat_key, tex_key)
                if mat_cache_key not in mat_map:
                    mat = {
                        'name': mat_key,
                        'pbrMetallicRoughness': {
                            'baseColorFactor': [0.8, 0.8, 0.8, 1.0],
                            'metallicFactor': 0.0,
                            'roughnessFactor': 0.7,
                        },
                    }

                    # Embed diffuse texture
                    diffuse_name = sm.textures.get('DiffuseTexture', '')
                    if diffuse_name and texture_dirs:
                        tex_idx = add_texture(diffuse_name)
                        if tex_idx is not None:
                            mat['pbrMetallicRoughness']['baseColorTexture'] = {'index': tex_idx}

                    # Embed normal map
                    normal_name = sm.textures.get('NormalTexture', '')
                    if normal_name and texture_dirs:
                        tex_idx = add_texture(normal_name)
                        if tex_idx is not None:
                            mat['normalTexture'] = {'index': tex_idx}

                    mat_map[mat_cache_key] = len(gltf_materials)
                    gltf_materials.append(mat)

                prim = {
                    'attributes': attributes,
                    'indices': acc_idx,
                    'material': mat_map[mat_cache_key],
                }
                primitives.append(prim)

            if primitives:
                mesh_idx = len(gltf_meshes)
                gltf_meshes.append({'name': node.name, 'primitives': primitives})
                gn['mesh'] = mesh_idx

        node_idx = len(gltf_nodes)
        gltf_nodes.append(gn)
        scene_nodes.append(node_idx)

    # Build glTF JSON
    gltf = {
        'asset': {'version': '2.0', 'generator': 'cpcw_srm.py'},
        'scene': 0,
        'scenes': [{'name': 'Scene', 'nodes': scene_nodes}],
        'nodes': gltf_nodes,
    }
    if gltf_meshes:
        gltf['meshes'] = gltf_meshes
    if gltf_materials:
        gltf['materials'] = gltf_materials
    if gltf_textures:
        gltf['textures'] = gltf_textures
    if gltf_images:
        gltf['images'] = gltf_images
    if gltf_samplers:
        gltf['samplers'] = gltf_samplers
    if gltf_accessors:
        gltf['accessors'] = gltf_accessors
    if gltf_buffer_views:
        gltf['bufferViews'] = gltf_buffer_views
    if bin_data:
        # Pad binary to 4-byte alignment
        while len(bin_data) % 4 != 0:
            bin_data.append(0)
        gltf['buffers'] = [{'byteLength': len(bin_data)}]

    json_str = json.dumps(gltf, separators=(',', ':'))
    json_bytes = json_str.encode('utf-8')
    # Pad JSON to 4-byte alignment
    while len(json_bytes) % 4 != 0:
        json_bytes += b' '

    # Write GLB
    with open(output_path, 'wb') as f:
        # GLB header
        total_len = 12 + 8 + len(json_bytes) + 8 + len(bin_data)
        f.write(struct.pack('<III', 0x46546C67, 2, total_len))  # magic, version, length
        # JSON chunk
        f.write(struct.pack('<II', len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        # BIN chunk
        f.write(struct.pack('<II', len(bin_data), 0x004E4942))
        f.write(bin_data)


def cmd_info(filepath):
    """Print info about an SRM file."""
    nodes = parse_srm(filepath)
    print(f"File: {filepath}")
    print(f"Nodes: {len(nodes)}")
    for i, node in enumerate(nodes):
        has_mesh = node.mesh is not None
        pos = [round(v, 3) for v in node.position]
        rot = [round(v, 3) for v in node.rotation]
        scl = [round(v, 3) for v in node.scale]
        print(f"  [{i}] '{node.name}' pos={pos} rot={rot} scale={scl}")
        if has_mesh:
            m = node.mesh
            total_verts = m.vertex_streams[0].vertex_count if m.vertex_streams else 0
            total_tris = len(m.indices) // 3
            print(f"      mesh: {total_verts} verts, {total_tris} tris, {m.submesh_count} submeshes")
            for si, sm in enumerate(m.submeshes):
                print(f"      submesh[{si}]: '{sm.material_name}' verts={sm.vert_count} tris={sm.face_count // 3}")
                for slot, tname in sm.textures.items():
                    print(f"        tex: {slot} = '{tname}'")
            for vs in m.vertex_streams:
                sem_names = {1: 'uv', 2: 'pos', 3: 'normal', 4: 'tangent', 5: 'binormal', 6: 'color'}
                sname = sem_names.get(vs.semantic, f'?{vs.semantic}')
                print(f"      stream: {sname} stride={vs.stride} verts={vs.vertex_count}")


def _get_texture_dirs(srm_path, data_root=None):
    """Build a list of directories to search for textures."""
    dirs = [os.path.dirname(os.path.abspath(srm_path))]
    if data_root:
        dirs.append(os.path.abspath(data_root))
    return dirs


def cmd_convert(filepath, output, batch=False, data_root=None, skin_mode='full'):
    """Convert SRM to GLB. skin_mode: 'full' or 'none'."""
    if batch:
        count = 0
        tex_found = 0
        for root, dirs, files in os.walk(filepath):
            for fn in files:
                if fn.lower().endswith('.srm'):
                    srm_path = os.path.join(root, fn)
                    rel = os.path.relpath(srm_path, filepath)
                    out_path = os.path.join(output, os.path.splitext(rel)[0] + '.glb')
                    os.makedirs(os.path.dirname(out_path), exist_ok=True)
                    try:
                        nodes = parse_srm(srm_path)
                        if skin_mode != 'none':
                            bake_skinning(nodes, skin_mode)
                        tex_dirs = _get_texture_dirs(srm_path, data_root)
                        nodes_to_glb(nodes, out_path, tex_dirs)
                        count += 1
                        if count % 100 == 0:
                            print(f"[{count}] {rel}")
                    except Exception as e:
                        print(f"FAIL: {rel}: {e}", file=sys.stderr)
        print(f"Converted {count} files")
    else:
        nodes = parse_srm(filepath)
        if skin_mode != 'none':
            bake_skinning(nodes, skin_mode)
        if not output:
            output = os.path.splitext(filepath)[0] + '.glb'
        tex_dirs = _get_texture_dirs(filepath, data_root)
        nodes_to_glb(nodes, output, tex_dirs)
        total_verts = sum(
            n.mesh.vertex_streams[0].vertex_count
            for n in nodes if n.mesh and n.mesh.vertex_streams
        )
        total_tris = sum(
            len(n.mesh.indices) // 3
            for n in nodes if n.mesh
        )
        tex_count = sum(1 for v in tex_cache.values() if v is not None) if 'tex_cache' in dir() else 0
        print(f"Wrote {output} ({total_verts} verts, {total_tris} tris)")


def cmd_roundtrip(filepath, output=None, batch=False):
    """Parse and re-serialize .srm byte-faithfully (the modding round-trip).

    Verified over the whole game corpus: parse -> write reproduces the original
    bytes exactly, so importing a game asset and exporting it never corrupts it.
    """
    import cpcw_srm_writer as W

    if batch or os.path.isdir(filepath):
        files = []
        for dp, _, fn in os.walk(filepath):
            files.extend(os.path.join(dp, f) for f in fn if f.lower().endswith('.srm'))
        ok = bad = err = 0
        for f in files:
            try:
                data = open(f, 'rb').read()
                if W.parse(data).pack() == data:
                    ok += 1
                else:
                    bad += 1
                    print("  MISMATCH", f)
            except Exception as e:
                err += 1
                print("  ERROR", os.path.basename(f), repr(e))
        print(f"roundtrip: {len(files)} files, exact={ok} mismatch={bad} error={err}")
        return

    data = open(filepath, 'rb').read()
    model = W.parse(data)
    out = model.pack()
    print(f"{filepath}: {len(data)} bytes, re-write "
          f"{'IDENTICAL' if out == data else 'DIFFERS (%d bytes)' % len(out)}")
    if output:
        model.write(output)
        print(f"wrote {output}")


def main():
    parser = argparse.ArgumentParser(description='CPCW .srm model viewer/converter')
    sub = parser.add_subparsers(dest='command', required=True)

    p_info = sub.add_parser('info', help='Show model info')
    p_info.add_argument('file')

    p_conv = sub.add_parser('convert', help='Convert to GLB')
    p_conv.add_argument('file', help='SRM file or directory for batch')
    p_conv.add_argument('-o', '--output', help='Output GLB file or directory')
    p_conv.add_argument('--batch', action='store_true', help='Batch convert directory')
    p_conv.add_argument('--no-tex', action='store_true', help='Skip texture embedding')
    p_conv.add_argument('--skin', choices=['full', 'none'], default='full',
                        help="Assembly: 'full' applies the exact engine skin rule "
                             "(bone-palette compaction) to every part; 'none' for "
                             "static models such as buildings")
    p_conv.add_argument('--no-skin', action='store_true',
                        help="Alias for --skin none")

    p_rt = sub.add_parser('roundtrip',
                          help='Byte-faithful re-write (verifies the writer)')
    p_rt.add_argument('file', help='SRM file or directory (with --batch)')
    p_rt.add_argument('-o', '--output', help='Output .srm (default: verify only)')
    p_rt.add_argument('--batch', action='store_true',
                      help='Verify every .srm under a directory re-writes identically')

    args = parser.parse_args()
    if args.command == 'info':
        cmd_info(args.file)
    elif args.command == 'convert':
        mode = 'none' if getattr(args, 'no_skin', False) else args.skin
        cmd_convert(args.file, args.output, getattr(args, 'batch', False),
                    data_root=None if getattr(args, 'no_tex', False) else None,
                    skin_mode=mode)
    elif args.command == 'roundtrip':
        cmd_roundtrip(args.file, args.output, getattr(args, 'batch', False))


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    main()
