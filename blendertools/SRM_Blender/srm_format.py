"""CPCW .srm model parser (stdlib only, Blender-safe).

Ported from cpcw_srm.py — parsing half only. Builds a list of SrmNode objects
with transforms, meshes, vertex streams and material/texture references. No
glTF/PIL code; Blender builds geometry directly from these structures.

See docs/FORMAT_SRM.md for the binary layout.
"""

import os
import struct

# Vertex semantic IDs
SEM_TEXCOORD = 1
SEM_POSITION = 2
SEM_NORMAL = 3
SEM_TANGENT = 4
SEM_BINORMAL = 5
SEM_COLOR = 6

SEM_NAMES = {1: 'uv', 2: 'pos', 3: 'normal', 4: 'tangent', 5: 'binormal', 6: 'color'}


class SrmNode:
    __slots__ = ('name', 'parent_idx', 'unk', 'position', 'rotation', 'scale',
                 'unk3', 'unk4', 'unk5', 'unk6', 'mesh')

    def __init__(self):
        self.name = ''
        self.parent_idx = -1
        self.unk = 0
        self.position = (0.0, 0.0, 0.0)
        self.rotation = (0.0, 0.0, 0.0)
        self.scale = (1.0, 1.0, 1.0)
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
        """Return the first vertex stream matching a semantic, or None."""
        for vs in self.vertex_streams:
            if vs.semantic == semantic:
                return vs
        return None

    def vertex_bone_indices(self):
        """Return a per-vertex bone-palette index list, or None if unskinned.

        Rigid skinning stores one bone index per vertex in byte 3 of the normal
        stream (a stride-4 stream). The correct stream is the stride-4 stream
        whose byte-3 values span [0, len(bones)-1]; other stride-4 streams
        (tangent/binormal) have byte 3 == 0 throughout.
        """
        if not self.bones:
            return None
        pos = self.stream(SEM_POSITION)
        if pos is None:
            return None
        vcount = pos.vertex_count
        nb = len(self.bones)
        best = None
        best_max = -1
        for vs in self.vertex_streams:
            if vs.stride == 4 and vs.vertex_count == vcount:
                mx = 0
                for i in range(vcount):
                    b = vs.data[i * 4 + 3]
                    if b > mx:
                        mx = b
                if mx < nb and mx > best_max:
                    best_max = mx
                    best = vs
        if best is None:
            return None
        return [best.data[i * 4 + 3] for i in range(vcount)]


class VertexStream:
    __slots__ = ('semantic', 'stride', 'vertex_count', 'data')

    def __init__(self, semantic, stride, vertex_count, data):
        self.semantic = semantic
        self.stride = stride
        self.vertex_count = vertex_count
        self.data = data  # raw bytes

    def positions(self):
        """Yield (x, y, z) floats for a POSITION stream (stride >= 12)."""
        st = self.stride
        data = self.data
        for i in range(self.vertex_count):
            yield struct.unpack_from('<3f', data, i * st)

    def uvs(self):
        """Yield (u, v) floats for a TEXCOORD stream (stride >= 8)."""
        st = self.stride
        data = self.data
        for i in range(self.vertex_count):
            yield struct.unpack_from('<2f', data, i * st)

    def normals(self):
        """Yield (nx, ny, nz) floats for a NORMAL stream.

        stride==4 -> packed 4x u8 decoded as (byte/127.5)-1.0.
        else       -> 3x f32.
        """
        st = self.stride
        data = self.data
        if st == 4:
            for i in range(self.vertex_count):
                off = i * st
                yield ((data[off] / 127.5) - 1.0,
                       (data[off + 1] / 127.5) - 1.0,
                       (data[off + 2] / 127.5) - 1.0)
        else:
            for i in range(self.vertex_count):
                yield struct.unpack_from('<3f', data, i * st)


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
    """Parse an SRM file and return a list of SrmNode."""
    filepath = os.path.normpath(filepath)
    with open(filepath, 'rb') as f:
        data = f.read()

    if data[:4] != b'MAIN':
        raise ValueError("Not an SRM file (missing MAIN magic)")

    # Skip THMB thumbnail
    thmb_size = struct.unpack_from('<I', data, 12)[0]
    pos = 16 + thmb_size

    nodes = []
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        chunk_size = struct.unpack_from('<I', data, pos + 4)[0]
        chunk_end = pos + 8 + chunk_size
        if tag == b'PMOD':
            nodes.extend(_parse_pmod(data, pos + 8, chunk_end))
        pos = chunk_end

    return nodes


def _parse_pmod(data, start, end):
    """Parse a PMOD chunk into a list of SrmNode.

    Layout: version + counts, then all node *headers* (transform + unk fields),
    then a block of MESH chunks. A node's ``unk5`` is the index into that MESH
    block (or -1 for a transform-only node). The meshes are NOT stored inline
    after each node header — only the last header happens to abut the block,
    so an inline scan would miss every mesh but the first (this is why buildings
    used to import as a single tiny fragment).
    """
    p = start + 4  # skip version
    node_count = struct.unpack_from('<I', data, p)[0]; p += 4
    p += 4  # unk1
    p += 4  # root parent
    p += 4  # root unknown

    nodes = []
    for _ in range(node_count):
        node = SrmNode()
        nlen = struct.unpack_from('<H', data, p)[0]; p += 2
        node.name = data[p:p + nlen].decode('ascii', errors='replace'); p += nlen

        node.unk3 = struct.unpack_from('<i', data, p)[0]; p += 4
        node.parent_idx = node.unk3  # unk3 is the parent node index (-1 = root)
        node.position = struct.unpack_from('<3f', data, p); p += 12
        node.rotation = struct.unpack_from('<3f', data, p); p += 12
        sx, sy, sz, _sw = struct.unpack_from('<4f', data, p); p += 16
        node.scale = (sx, sy, sz)
        node.unk4 = struct.unpack_from('<I', data, p)[0]; p += 4
        node.unk5 = struct.unpack_from('<i', data, p)[0]; p += 4
        node.unk6 = struct.unpack_from('<i', data, p)[0]; p += 4

        nodes.append(node)

    # MESH block: consecutive MESH chunks following the last node header.
    meshes = []
    while p + 8 <= end and data[p:p + 4] == b'MESH':
        mesh_size = struct.unpack_from('<I', data, p + 4)[0]
        mesh_end = p + 8 + mesh_size
        meshes.append(_parse_mesh(data, p + 8, mesh_end))
        p = mesh_end

    # Assign meshes to their nodes by index.
    for node in nodes:
        if 0 <= node.unk5 < len(meshes):
            node.mesh = meshes[node.unk5]

    return nodes


def _parse_mesh(data, start, end):
    """Parse a MESH chunk's content into an SrmMesh."""
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

    # INDS index buffer
    assert data[p:p + 4] == b'INDS', f"Expected INDS at 0x{p:x}, got {data[p:p+4]!r}"
    p += 8  # tag + size
    idx_count = struct.unpack_from('<I', data, p)[0]; p += 4
    mesh.idx_stride = struct.unpack_from('<I', data, p)[0]; p += 4
    if mesh.idx_stride == 2:
        mesh.indices = list(struct.unpack_from(f'<{idx_count}H', data, p))
    else:
        mesh.indices = list(struct.unpack_from(f'<{idx_count}I', data, p))
    p += idx_count * mesh.idx_stride

    # VERS vertex streams
    for _ in range(mesh.stream_count):
        assert data[p:p + 4] == b'VERS', f"Expected VERS at 0x{p:x}"
        vs_size = struct.unpack_from('<I', data, p + 4)[0]; p += 8
        vs_end = p + vs_size
        _f1, vert_count, stride, _f4, semantic = struct.unpack_from('<IIIII', data, p)
        p += 20
        vdata = data[p:p + vert_count * stride]
        mesh.vertex_streams.append(VertexStream(semantic, stride, vert_count, vdata))
        p = vs_end

    # Submesh / material data (single submesh spanning the whole mesh; matches
    # the reference converter — see plan "Deferred: multi-submesh splitting").
    if mesh.vertex_streams:
        sm = Submesh()
        sm.vert_start = 0
        sm.vert_count = mesh.vertex_streams[0].vertex_count
        sm.face_start = 0
        sm.face_count = len(mesh.indices)
        sm.material_name = 'default'
        try:
            mp = p + 25  # flag(1) + 6*u32(24)
            mlen = struct.unpack_from('<H', data, mp)[0]; mp += 2
            if 0 < mlen < 256 and mp + mlen <= end:
                mname = data[mp:mp + mlen].decode('ascii', errors='replace'); mp += mlen
                if mname.endswith('\\'):
                    mname = mname[:-1]
                sm.material_name = mname

                prop_count_h = struct.unpack_from('<H', data, mp)[0]; mp += 2
                mp += 2  # unk u16
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
                        tlen = struct.unpack_from('<H', data, mp)[0]; mp += 2
                        tname = data[mp:mp + tlen].decode('ascii', errors='replace'); mp += tlen
                        sm.textures[pname] = tname
                    elif ptype == 0 and psize > 0:
                        mp += psize
        except Exception:
            pass
        mesh.submeshes.append(sm)

    return mesh
