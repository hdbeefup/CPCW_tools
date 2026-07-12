"""Byte-faithful CPCW .srm container model + writer (stdlib only).

This is the *round-trip* half of the SRM support: parse a file into a fully
editable model and re-serialize it. Verified byte-exact over the whole game
corpus (2087/2087 files: parse -> pack reproduces the original bytes), so
"import a game asset then export it" never corrupts anything the tool did not
deliberately change.

Container layout::

    MAIN { THMB, PMOD, PBND, BMSK, ... }            (ordered child chunks)
    PMOD:  ver(u32) count(u32) unk1(u32) rootParent(u32) rootUnk(u32)
           node[count]   MESH-block(consecutive MESH chunks)   trailing
    node:  nameLen(u16) name  parent(i32) pos(3f) rot(3f) scale(4f)
           unk4(u32) unk5(i32) unk6(i32)
    MESH:  streamCount(u32) submeshCount(u32)
           [BONE]  INDS  VERS*streamCount  material-trailer
    VERS:  f1(u32) vcount(u32) stride(u32) usage(u32) semantic(u32) data tail

``usage`` (the 4th VERS word) is the D3DDECLUSAGE: 0=POSITION 1=BLENDWEIGHT
2=BLENDINDICES 3=NORMAL 4=TEXCOORD 5=TANGENT 6=BINORMAL. For rigidly skinned
meshes the per-vertex bone-palette index is byte 3 of the NORMAL (usage 3)
stream. See docs/FORMAT_SRM.md.

Chunks that a mod does not touch keep their raw bytes, so anything this model
does not explicitly understand (THMB thumbnail, PBND bounds, BMSK, material
trailers, unknown fields) survives a round-trip untouched.
"""

import struct

# D3DDECLUSAGE values carried in the VERS ``usage`` word.
USAGE_POSITION = 0
USAGE_BLENDWEIGHT = 1
USAGE_BLENDINDICES = 2
USAGE_NORMAL = 3
USAGE_TEXCOORD = 4
USAGE_TANGENT = 5
USAGE_BINORMAL = 6


def _u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


class WNode:
    """One PMOD node header (transform + hierarchy + mesh reference)."""
    __slots__ = ('name', 'parent', 'pos', 'rot', 'scale', 'unk4', 'mesh_index',
                 'unk6')

    def __init__(self):
        self.name = b''
        self.parent = -1
        self.pos = (0.0, 0.0, 0.0)
        self.rot = (0.0, 0.0, 0.0)
        self.scale = (1.0, 1.0, 1.0, 0.0)
        self.unk4 = 0
        self.mesh_index = -1
        self.unk6 = -1

    def pack(self):
        b = struct.pack('<H', len(self.name)) + self.name
        b += struct.pack('<i', self.parent)
        b += struct.pack('<3f', *self.pos)
        b += struct.pack('<3f', *self.rot)
        b += struct.pack('<4f', *self.scale)
        b += struct.pack('<Iii', self.unk4, self.mesh_index, self.unk6)
        return b


class WVers:
    """One vertex stream (VERS chunk)."""
    __slots__ = ('f1', 'vcount', 'stride', 'usage', 'semantic', 'data', 'tail')

    def __init__(self):
        self.f1 = 1
        self.vcount = 0
        self.stride = 0
        self.usage = 0
        self.semantic = 0
        self.data = b''
        self.tail = b''

    def pack(self):
        body = struct.pack('<IIIII', self.f1, self.vcount, self.stride,
                           self.usage, self.semantic) + self.data + self.tail
        return b'VERS' + struct.pack('<I', len(body)) + body


class WMesh:
    """One MESH chunk (bone palette + index buffer + vertex streams)."""
    __slots__ = ('stream_count', 'submesh_count', 'bone_raw', 'inds_raw',
                 'streams', 'trailing')

    def __init__(self):
        self.stream_count = 0
        self.submesh_count = 0
        self.bone_raw = None    # full BONE sub-chunk bytes, or None
        self.inds_raw = b''     # full INDS sub-chunk bytes
        self.streams = []       # list[WVers]
        self.trailing = b''     # material trailer etc.

    def bones(self):
        """Return the u16 bone palette (node indices), or []."""
        if not self.bone_raw:
            return []
        n = _u32(self.bone_raw, 8)
        return list(struct.unpack_from('<%dH' % n, self.bone_raw, 12))

    def stream_by_usage(self, usage):
        for v in self.streams:
            if v.usage == usage:
                return v
        return None

    def pack(self):
        body = struct.pack('<II', self.stream_count, self.submesh_count)
        if self.bone_raw is not None:
            body += self.bone_raw
        body += self.inds_raw
        for v in self.streams:
            body += v.pack()
        body += self.trailing
        return b'MESH' + struct.pack('<I', len(body)) + body


class WPmod:
    """PMOD chunk: node headers followed by a block of MESH chunks."""
    __slots__ = ('ver', 'count', 'unk1', 'root_parent', 'root_unk', 'nodes',
                 'meshes', '_mesh_trailing')

    def __init__(self):
        self.ver = 0
        self.count = 0
        self.unk1 = 0
        self.root_parent = 0
        self.root_unk = 0
        self.nodes = []          # list[WNode]
        self.meshes = []         # list[WMesh]
        self._mesh_trailing = b''  # bytes in the mesh block after the last MESH

    def pack(self):
        b = struct.pack('<IIIII', self.ver, self.count, self.unk1,
                        self.root_parent, self.root_unk)
        for n in self.nodes:
            b += n.pack()
        for m in self.meshes:
            b += m.pack()
        b += self._mesh_trailing
        return b


class WChunk:
    """A top-level MAIN child. ``pmod`` is set for the PMOD chunk."""
    __slots__ = ('tag', 'raw', 'pmod')

    def __init__(self, tag, raw):
        self.tag = tag
        self.raw = raw
        self.pmod = None


class SrmFile:
    """Full editable SRM. ``pack()`` reproduces the file byte-for-byte when
    nothing was modified."""
    __slots__ = ('children', 'tail')

    def __init__(self):
        self.children = []   # list[WChunk]
        self.tail = b''      # bytes after MAIN (normally empty)

    def pmod(self):
        for c in self.children:
            if c.pmod is not None:
                return c.pmod
        return None

    def pack(self):
        body = b''
        for c in self.children:
            raw = c.pmod.pack() if c.pmod is not None else c.raw
            body += c.tag + struct.pack('<I', len(raw)) + raw
        return b'MAIN' + struct.pack('<I', len(body)) + body + self.tail

    def write(self, path):
        with open(path, 'wb') as f:
            f.write(self.pack())


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def _parse_vers(body):
    v = WVers()
    v.f1, v.vcount, v.stride, v.usage, v.semantic = struct.unpack_from('<IIIII', body, 0)
    n = v.vcount * v.stride
    v.data = body[20:20 + n]
    v.tail = body[20 + n:]
    return v


def _parse_mesh(body):
    m = WMesh()
    m.stream_count, m.submesh_count = struct.unpack_from('<II', body, 0)
    o = 8
    if body[o:o + 4] == b'BONE':
        bsize = _u32(body, o + 4)
        m.bone_raw = body[o:o + 8 + bsize]
        o += 8 + bsize
    if body[o:o + 4] != b'INDS':
        raise ValueError("expected INDS at %d" % o)
    isize = _u32(body, o + 4)
    m.inds_raw = body[o:o + 8 + isize]
    o += 8 + isize
    for _ in range(m.stream_count):
        if body[o:o + 4] != b'VERS':
            raise ValueError("expected VERS at %d" % o)
        vsize = _u32(body, o + 4)
        m.streams.append(_parse_vers(body[o + 8:o + 8 + vsize]))
        o += 8 + vsize
    m.trailing = body[o:]
    return m


def _parse_pmod(raw):
    p = WPmod()
    (p.ver, p.count, p.unk1, p.root_parent, p.root_unk) = struct.unpack_from('<IIIII', raw, 0)
    o = 20
    for _ in range(p.count):
        n = WNode()
        nlen = _u16(raw, o); o += 2
        n.name = raw[o:o + nlen]; o += nlen
        n.parent = struct.unpack_from('<i', raw, o)[0]; o += 4
        n.pos = struct.unpack_from('<3f', raw, o); o += 12
        n.rot = struct.unpack_from('<3f', raw, o); o += 12
        n.scale = struct.unpack_from('<4f', raw, o); o += 16
        n.unk4, n.mesh_index, n.unk6 = struct.unpack_from('<Iii', raw, o); o += 12
        p.nodes.append(n)
    mb_start = o
    while o + 8 <= len(raw) and raw[o:o + 4] == b'MESH':
        msize = _u32(raw, o + 4)
        p.meshes.append(_parse_mesh(raw[o + 8:o + 8 + msize]))
        o += 8 + msize
    p._mesh_trailing = raw[o:]
    del mb_start
    return p


def parse(data):
    """Parse SRM bytes into an editable :class:`SrmFile`."""
    if data[:4] != b'MAIN':
        raise ValueError("not an SRM file (missing MAIN magic)")
    main_size = _u32(data, 4)
    s = SrmFile()
    p = 8
    end = 8 + main_size
    while p + 8 <= end:
        tag = data[p:p + 4]
        size = _u32(data, p + 4)
        raw = data[p + 8:p + 8 + size]
        c = WChunk(tag, raw)
        if tag == b'PMOD':
            c.pmod = _parse_pmod(raw)
        s.children.append(c)
        p += 8 + size
    s.tail = data[end:]
    return s


def read(path):
    with open(path, 'rb') as f:
        return parse(f.read())
