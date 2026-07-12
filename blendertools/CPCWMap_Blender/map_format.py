"""Parser for Codename: Panzers Cold War .map scenario files (Blender-safe).

Vendored parsing core of cpcw_map.py — stdlib only, no CLI/tkinter/PIL. The
.map format is a hierarchical chunk-based container using the Gepard engine's
OBJT/VOBJ/ARRY/SCHM serialization system (same as ProtoDB.bin).

Key entry points used by the importer:
    MapFile(path)          - parse a .map file
    .find_chunk('WRLD')    - world dims live in chunk.meta['width'/'height']
    .get_entities()        - list of placed-entity dicts (Prototype/Pos/Dir/...)
    .get_blck_grid()       - (grid, w, h) passability grid, 6x uint16 per cell
"""

import argparse
import json
import math
import struct
import sys
from collections import OrderedDict

# ---------------------------------------------------------------------------
# Field type constants
# ---------------------------------------------------------------------------

FT_INT32    = 0x0001
FT_FLOAT    = 0x0002
FT_BOOL     = 0x0003
FT_STRING   = 0x0004
FT_FLOAT64  = 0x0005   # 8 bytes, possibly vec2<double> or double
FT_VEC3     = 0x0006   # 12 bytes, 3x float
FT_GUID     = 0x0011
FT_REF      = 0x0012
FT_IID      = 0x0013   # internal ID, uint32
FT_ENTREF   = 0x0014   # entity reference, uint32
FT_VEC2F    = 0x0015   # 8 bytes, 2x float  (guess)
FT_VEC2I    = 0x0016   # 8 bytes, 2x int32
FT_UINT8    = 0x0017
FT_COLOR    = 0x0018   # 4 bytes RGBA
FT_INT16    = 0x0019
FT_LOCSTR   = 0x002B   # localised string
FT_INLINE1  = 0x0088   # inline object
FT_INLINE2  = 0x0089   # inline object (variant)
FT_BLOB     = 0x0165
FT_INT_ARR  = 0x018A   # packed int array
FT_FLAGS    = 0x039C
FT_ARRAY    = 0x898A

FIELD_TYPE_NAMES = {
    FT_INT32:   'int32',    FT_FLOAT:   'float',    FT_BOOL:    'bool',
    FT_STRING:  'string',   FT_FLOAT64: 'float64',  FT_VEC3:    'vec3',
    FT_GUID:    'GUID',     FT_REF:     'ref',      FT_IID:     'IID',
    FT_ENTREF:  'entref',   FT_VEC2F:   'vec2f',    FT_VEC2I:   'vec2i',
    FT_UINT8:   'uint8',    FT_COLOR:   'color',    FT_INT16:   'int16',
    FT_LOCSTR:  'locstr',   FT_INLINE1: 'object',   FT_INLINE2: 'object',
    FT_BLOB:    'blob',     FT_INT_ARR: 'int_arr',  FT_FLAGS:   'flags',
    FT_ARRAY:   'array',
}

# Map of all known types that use variable-length (length-prefix + payload)
# encoding like strings do.  Anything else below 0x88 with size=0xFFFF is
# assumed to follow the same pattern.
_STRING_LIKE = {FT_STRING, FT_GUID, FT_REF, FT_FLAGS, FT_LOCSTR}

# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

def _u8(data, pos):
    return data[pos], pos + 1

def _u16(data, pos):
    return struct.unpack_from('<H', data, pos)[0], pos + 2

def _i16(data, pos):
    return struct.unpack_from('<h', data, pos)[0], pos + 2

def _u32(data, pos):
    return struct.unpack_from('<I', data, pos)[0], pos + 4

def _i32(data, pos):
    return struct.unpack_from('<i', data, pos)[0], pos + 4

def _f32(data, pos):
    return struct.unpack_from('<f', data, pos)[0], pos + 4

def _f64(data, pos):
    return struct.unpack_from('<d', data, pos)[0], pos + 8

def _tag(data, pos):
    return data[pos:pos + 4], pos + 4

def _str(data, pos):
    """Read a uint16-length-prefixed ASCII string."""
    slen, pos = _u16(data, pos)
    s = data[pos:pos + slen].decode('ascii', errors='replace')
    return s, pos + slen


# ---------------------------------------------------------------------------
# Schema
# ---------------------------------------------------------------------------

class Schema:
    __slots__ = ('name', 'type_id', 'version', 'fields')

    def __init__(self, name, type_id, version, fields):
        self.name = name
        self.type_id = type_id
        self.version = version
        self.fields = fields  # [(field_name, field_type, field_size), ...]


def parse_schd(data, pos, limit):
    """Parse a SCHD chunk and return a dict of {type_id: Schema}."""
    schemas = {}
    count, pos = _u16(data, pos)
    _unk, pos = _u16(data, pos)

    for _ in range(count):
        if pos >= limit or data[pos:pos + 4] != b'SCHM':
            break
        content_sz, p = _u32(data, pos + 4)
        cs = pos + 8
        ce = cs + content_sz

        name, p = _str(data, cs)
        type_id, p = _u16(data, p)
        version, p = _u16(data, p)
        field_count, p = _u16(data, p)

        fields = []
        for _ in range(field_count):
            if p + 2 > ce:
                break
            fn, p = _str(data, p)
            if p + 8 > ce:
                break
            ftype, p = _u32(data, p)
            fsize, p = _u32(data, p)
            fields.append((fn, ftype, fsize))

        schemas[type_id] = Schema(name, type_id, version, fields)
        pos = ce

    return schemas


# ---------------------------------------------------------------------------
# OBJT / VOBJ / ARRY parser  (mirrors cpcw_protodb.py logic)
# ---------------------------------------------------------------------------

class ObjParser:
    """Stateful parser that reads OBJT/VOBJ/ARRY trees using local schemas."""

    def __init__(self, data, schemas):
        self.data = data
        self.schemas = schemas

    # -- public entry -------------------------------------------------------

    def parse_objt(self, pos):
        return self._parse_objt(pos)

    # -- internals ----------------------------------------------------------

    def _parse_objt(self, pos):
        if self.data[pos:pos + 4] != b'OBJT':
            return None, pos
        content_sz, _ = _u32(self.data, pos + 4)
        content_end = pos + 8 + content_sz
        type_id, _ = _u16(self.data, pos + 8)

        obj, vobj_end = self._parse_vobj(pos + 10)
        if obj is None:
            s = self.schemas.get(type_id, Schema('?', type_id, 0, []))
            obj = OrderedDict([('_type_id', type_id), ('_type', s.name)])

        # trailing VOBJs (version extensions)
        p = vobj_end
        while p + 10 <= content_end and self.data[p:p + 4] == b'VOBJ':
            trailing, p = self._parse_vobj(p)
            if trailing:
                for k, v in trailing.items():
                    if not k.startswith('_'):
                        obj[k] = v

        return obj, content_end

    def _parse_vobj(self, pos):
        if self.data[pos:pos + 4] != b'VOBJ':
            return None, pos
        content_sz, _ = _u32(self.data, pos + 4)
        content_end = pos + 8 + content_sz
        type_id, _ = _u16(self.data, pos + 8)

        schema = self.schemas.get(type_id)
        obj = OrderedDict([
            ('_type_id', type_id),
            ('_type', schema.name if schema else f'Unknown_0x{type_id:04x}'),
        ])

        p = pos + 10
        if p + 2 > content_end:
            return obj, content_end
        _version, p = _u16(self.data, p)

        if schema:
            for fname, ftype, fsize in schema.fields:
                if p >= content_end:
                    break
                val, p = self._read_field(p, ftype, fsize, content_end)
                obj[fname] = val

        return obj, content_end

    def _read_field(self, pos, ftype, fsize, limit):
        if pos >= limit:
            return None, pos

        if ftype == FT_INT32:
            if pos + 4 > limit: return None, limit
            return _i32(self.data, pos)

        if ftype == FT_FLOAT:
            if pos + 4 > limit: return None, limit
            v, p = _f32(self.data, pos)
            return round(v, 6), p

        if ftype == FT_BOOL:
            if pos + 1 > limit: return None, limit
            return bool(self.data[pos]), pos + 1

        if ftype in _STRING_LIKE:
            if pos + 2 > limit: return None, limit
            return _str(self.data, pos)

        if ftype == FT_UINT8:
            if pos + 1 > limit: return None, limit
            return self.data[pos], pos + 1

        if ftype == FT_INT16:
            if pos + 2 > limit: return None, limit
            return _i16(self.data, pos)

        if ftype == FT_COLOR:
            if pos + 4 > limit: return None, limit
            v, p = _u32(self.data, pos)
            return f'#{v:08x}', p

        if ftype == FT_IID or ftype == FT_ENTREF:
            if pos + 4 > limit: return None, limit
            return _u32(self.data, pos)

        if ftype == FT_FLOAT64:
            if pos + 8 > limit: return None, limit
            v, p = _f64(self.data, pos)
            return round(v, 6), p

        if ftype == FT_VEC3:
            if pos + 12 > limit: return None, limit
            x, _ = _f32(self.data, pos)
            y, _ = _f32(self.data, pos + 4)
            z, _ = _f32(self.data, pos + 8)
            return [round(x, 4), round(y, 4), round(z, 4)], pos + 12

        if ftype == FT_VEC2I:
            if pos + 8 > limit: return None, limit
            a, _ = _i32(self.data, pos)
            b, _ = _i32(self.data, pos + 4)
            return [a, b], pos + 8

        if ftype == FT_VEC2F:
            if pos + 8 > limit: return None, limit
            a, _ = _f32(self.data, pos)
            b, _ = _f32(self.data, pos + 4)
            return [round(a, 4), round(b, 4)], pos + 8

        if ftype == FT_ARRAY:
            return self._read_array(pos, limit)

        if ftype in (FT_INLINE1, FT_INLINE2):
            return self._read_inline(pos, limit)

        if ftype == FT_BLOB:
            nbytes = fsize if 0 < fsize < 0xFFFF else 0
            if nbytes > 0 and pos + nbytes <= limit:
                return self.data[pos:pos + nbytes].hex(), pos + nbytes
            return None, pos

        # Unknown but has known size in schema
        if 0 < fsize < 0xFFFF and pos + fsize <= limit:
            return self.data[pos:pos + fsize].hex(), pos + fsize

        # Variable-length unknown: try length-prefixed string pattern
        if fsize == 0xFFFF or fsize == 0:
            # Could be a string-like type or a nested chunk
            tag = self.data[pos:pos + 4]
            if tag in (b'ARRY', b'OBJT', b'VOBJ'):
                return self._read_inline(pos, limit)
            if pos + 2 <= limit:
                slen, _ = _u16(self.data, pos)
                if slen < 4096 and pos + 2 + slen <= limit:
                    s = self.data[pos + 2:pos + 2 + slen].decode('ascii', errors='replace')
                    return s, pos + 2 + slen

        return f'<unknown:0x{ftype:04x}>', pos

    def _read_array(self, pos, limit):
        if pos + 12 > limit or self.data[pos:pos + 4] != b'ARRY':
            return [], pos
        arr_size, _ = _u32(self.data, pos + 4)
        arr_count, _ = _u32(self.data, pos + 8)
        arr_end = pos + 8 + arr_size

        items = []
        p = pos + 12
        for _ in range(arr_count):
            if p >= arr_end:
                break
            if self.data[p:p + 4] == b'OBJT':
                obj, p = self._parse_objt(p)
                if obj:
                    items.append(obj)
            else:
                break
        return items, arr_end

    def _read_inline(self, pos, limit):
        if pos + 8 > limit:
            return None, pos
        tag = self.data[pos:pos + 4]
        if tag == b'OBJT':
            return self._parse_objt(pos)
        if tag == b'VOBJ':
            return self._parse_vobj(pos)
        if tag == b'ARRY':
            return self._read_array(pos, limit)
        return None, pos


# ---------------------------------------------------------------------------
# Chunk-level map parser
# ---------------------------------------------------------------------------

class Chunk:
    """Represents a parsed chunk in the file."""
    __slots__ = ('tag', 'offset', 'size', 'children', 'data_offset', 'meta')

    def __init__(self, tag, offset, size):
        self.tag = tag
        self.offset = offset          # offset of tag byte
        self.size = size              # content size (after 8-byte header)
        self.children = []
        self.data_offset = offset + 8  # start of content
        self.meta = {}                # parsed metadata


class MapFile:
    """Full parser for a .map scenario file."""

    def __init__(self, filepath):
        with open(filepath, 'rb') as f:
            self.data = f.read()
        self.filepath = filepath
        self.schemas = {}       # merged across all SCHD sections
        self.root = None        # root Chunk
        self._parse()

    # -- high-level parse ---------------------------------------------------

    def _parse(self):
        d = self.data
        if d[0:4] != b'SCEN':
            raise ValueError(f'Not a CPCW map file (expected SCEN, got {d[0:4]})')

        scen_size, _ = _u32(d, 4)
        scen_ver, _ = _u32(d, 8)

        self.root = Chunk('SCEN', 0, scen_size)
        self.root.meta['version'] = scen_ver

        pos = 12
        file_end = min(8 + scen_size, len(d))
        self._parse_children(self.root, pos, file_end)

    def _parse_children(self, parent, pos, end):
        """Recursively discover chunks inside a container."""
        d = self.data
        while pos + 8 <= end:
            tag = d[pos:pos + 4]
            if not all(0x20 <= b < 0x7F for b in tag):
                break
            tag_s = tag.decode('ascii')
            size, _ = _u32(d, pos + 4)
            chunk_end = pos + 8 + size
            if chunk_end > end + 4:  # small tolerance
                break

            chunk = Chunk(tag_s, pos, size)
            parent.children.append(chunk)

            # Parse version / count header for known containers
            if tag_s in ('PREC', 'SETS', 'OJTS'):
                if pos + 12 <= len(d):
                    chunk.meta['version'], _ = _u32(d, pos + 8)
                self._parse_children(chunk, pos + 12, chunk_end)
                pos = chunk_end

            elif tag_s == 'OBJS':
                if pos + 12 <= len(d):
                    chunk.meta['schema_offset'], _ = _u32(d, pos + 8)
                self._parse_children(chunk, pos + 12, chunk_end)
                # Collect schemas from any SCHD children
                for child in chunk.children:
                    if child.tag == 'SCHD':
                        schemas = parse_schd(d, child.data_offset, child.offset + 8 + child.size)
                        self.schemas.update(schemas)
                pos = chunk_end

            elif tag_s == 'WRLD':
                if pos + 20 <= len(d):
                    chunk.meta['version'], _ = _u32(d, pos + 8)
                    chunk.meta['width'], _ = _u32(d, pos + 12)
                    chunk.meta['height'], _ = _u32(d, pos + 16)
                self._parse_children(chunk, pos + 20, chunk_end)
                pos = chunk_end

            elif tag_s == 'GTRN':
                if pos + 9 <= len(d):
                    chunk.meta['version'] = d[pos + 8]
                self._parse_children(chunk, pos + 9, chunk_end)
                pos = chunk_end

            elif tag_s == 'GROL':
                self._parse_children(chunk, pos + 8, chunk_end)
                pos = chunk_end

            elif tag_s == 'UNTS':
                if pos + 16 <= len(d):
                    chunk.meta['version'], _ = _u32(d, pos + 8)
                    chunk.meta['entity_count'], _ = _u32(d, pos + 12)
                self._parse_children(chunk, pos + 16, chunk_end)
                pos = chunk_end

            elif tag_s in ('PATH', 'CAMS', 'WTHR'):
                if pos + 16 <= len(d):
                    chunk.meta['version'], _ = _u32(d, pos + 8)
                    chunk.meta['count'], _ = _u32(d, pos + 12)
                pos = chunk_end

            elif tag_s == 'SCHD':
                if pos + 12 <= len(d):
                    chunk.meta['schema_count'], _ = _u16(d, pos + 8)
                schemas = parse_schd(d, pos + 8, chunk_end)
                self.schemas.update(schemas)
                pos = chunk_end

            elif tag_s == 'STOR':
                if pos + 16 <= len(d):
                    chunk.meta['count'], _ = _u32(d, pos + 8)
                    chunk.meta['next_id'], _ = _u32(d, pos + 12)
                pos = chunk_end

            elif tag_s == 'BLCK':
                self._parse_blck(chunk)
                pos = chunk_end

            elif tag_s == 'GTRD':
                self._parse_gtrd(chunk)
                pos = chunk_end

            else:
                pos = chunk_end

    # -- GTRD ---------------------------------------------------------------

    def _parse_gtrd(self, chunk):
        d = self.data
        pos = chunk.data_offset
        end = chunk.offset + 8 + chunk.size

        version, pos = _u8(d, pos)
        grid_w, pos = _u32(d, pos)
        grid_h, pos = _u32(d, pos)
        world_x, pos = _f32(d, pos)
        world_y, pos = _f32(d, pos)
        layer_count, pos = _u32(d, pos)

        chunk.meta.update({
            'version': version,
            'grid_w': grid_w,
            'grid_h': grid_h,
            'world_x': round(world_x, 2),
            'world_y': round(world_y, 2),
            'layer_count': layer_count,
        })

        layers = []
        for _ in range(layer_count):
            if pos >= end:
                break
            name, pos = _str(d, pos)
            unk_int, pos = _u32(d, pos)
            uv_scale, pos = _f32(d, pos)
            detail, pos = _str(d, pos)
            flag, pos = _u8(d, pos)
            layers.append({
                'name': name,
                'type': unk_int,
                'uv_scale': round(uv_scale, 4),
                'detail': detail,
                'active': bool(flag),
            })
        chunk.meta['layers'] = layers
        chunk.meta['splatmap_offset'] = pos
        chunk.meta['splatmap_size'] = end - pos

    # -- BLCK ---------------------------------------------------------------

    def _parse_blck(self, chunk):
        d = self.data
        pos = chunk.data_offset

        version, pos = _u32(d, pos)
        dim1, pos = _u32(d, pos)
        dim2, pos = _u32(d, pos)

        grid_w = dim1 // 2 if dim1 > 0 else 0
        grid_h = dim2 // 2 if dim2 > 0 else 0

        chunk.meta.update({
            'version': version,
            'vertex_w': dim1,
            'vertex_h': dim2,
            'grid_w': grid_w,
            'grid_h': grid_h,
            'grid_offset': pos,
        })

    # -- Object tree access -------------------------------------------------

    def find_chunks(self, tag, root=None):
        """Find all chunks with the given tag (recursive)."""
        results = []
        def walk(c):
            if c.tag == tag:
                results.append(c)
            for child in c.children:
                walk(child)
        walk(root or self.root)
        return results

    def find_chunk(self, tag, root=None):
        """Find first chunk with the given tag."""
        results = self.find_chunks(tag, root)
        return results[0] if results else None

    def parse_objs_tree(self, objs_chunk):
        """Parse the first OBJT tree inside an OBJS chunk."""
        parser = ObjParser(self.data, self.schemas)
        pos = objs_chunk.data_offset + 4  # skip schema_offset
        end = objs_chunk.offset + 8 + objs_chunk.size
        # Skip to the SCHD to know where data ends
        schd_off = struct.unpack_from('<I', self.data, objs_chunk.data_offset)[0]
        if schd_off > 0 and objs_chunk.data_offset - 8 + schd_off < end:
            # schema_offset is absolute file offset
            data_end = schd_off
        else:
            data_end = end
        while pos < data_end - 8:
            if self.data[pos:pos + 4] == b'OBJT':
                obj, _ = parser.parse_objt(pos)
                return obj
            pos += 1
        return None

    def parse_objs_all(self, objs_chunk):
        """Parse ALL top-level OBJTs inside an OBJS chunk (flat list)."""
        parser = ObjParser(self.data, self.schemas)
        pos = objs_chunk.data_offset + 4  # skip schema_offset
        end = objs_chunk.offset + 8 + objs_chunk.size
        # Use schema_offset as absolute file offset to find data boundary
        schd_off = struct.unpack_from('<I', self.data, objs_chunk.data_offset)[0]
        if schd_off > 0 and schd_off < end:
            data_end = schd_off
        else:
            data_end = end

        objects = []
        while pos < data_end - 8:
            if self.data[pos:pos + 4] == b'OBJT':
                obj, pos = parser.parse_objt(pos)
                if obj:
                    objects.append(obj)
            else:
                break
        return objects

    def get_scenario_settings(self):
        """Parse the SP2ScenarioSettings object from PREC/SETS."""
        prec = self.find_chunk('PREC')
        if not prec:
            return None
        sets = self.find_chunk('SETS', prec)
        if not sets:
            return None
        objs = self.find_chunk('OBJS', sets)
        if not objs:
            return None
        return self.parse_objs_tree(objs)

    def get_entities(self):
        """Parse entities from the UNTS section."""
        unts = self.find_chunk('UNTS')
        if not unts:
            return []
        objs = self.find_chunk('OBJS', unts)
        if not objs:
            return []
        return self.parse_objs_all(objs)

    def get_heightmap(self):
        """Return (heights, W, H) for the terrain, or None.

        The terrain elevation is a contiguous f32 grid of (world_w+1) x
        (world_h+1) vertices, row-major, in world units, stored inside the GTRD
        chunk (after the layer table) at a byte offset that is not necessarily
        4-aligned. Located as the longest run of "height-like" f32 values (finite,
        |v| < 500) across all four byte phases; the exact start is pinned by
        correlating against the map's own entity elevations (units sit on the
        terrain). Verified against entity Z at R^2 ~ 0.9 across many maps.
        """
        import array as _array
        d = self.data
        wrld = self.find_chunk('WRLD')
        gtrd = self.find_chunk('GTRD')
        if not wrld or not gtrd:
            return None
        WW = wrld.meta.get('width'); WH = wrld.meta.get('height')
        if not WW or not WH:
            return None
        W, H = WW + 1, WH + 1
        need = W * H
        gs = gtrd.meta.get('splatmap_offset', gtrd.data_offset)
        ge = gtrd.offset + 8 + gtrd.size

        runs = []
        for phase in range(4):
            start = gs + phase
            n = (ge - start) // 4
            if n <= 0:
                continue
            a = _array.array('f')
            a.frombytes(d[start:start + n * 4])
            run = 0; rstart = 0
            for i in range(n):
                v = a[i]
                if v == v and -500.0 < v < 500.0:
                    if run == 0:
                        rstart = i
                    run += 1
                else:
                    if run >= need // 2:
                        runs.append((run, start + rstart * 4))
                    run = 0
            if run >= need // 2:
                runs.append((run, start + rstart * 4))
        if not runs:
            return None

        E = self._entity_grid_samples(W, H, WW, WH)
        if len(E) < 20:
            runs.sort(reverse=True)
            off = runs[0][1]
        else:
            best = (-9.0, runs[0][1])
            row = 4 * W
            runs.sort(reverse=True)
            for runlen, r0 in runs:
                if best[0] > 0.9:
                    break
                hi = r0 + max((runlen - need) * 4, 0) + row * 16
                hi = min(hi, r0 + row * 40)
                for off in range(r0, hi + 1, 4):
                    r2 = self._height_fit(off, W, E)
                    if r2 > best[0]:
                        best = (r2, off)
            if best[0] < 0.4:
                return None
            off = best[1]
        heights = list(struct.unpack_from('<%df' % need, d, off))
        return heights, W, H

    def _entity_grid_samples(self, W, H, WW, WH, limit=200):
        E = []
        for e in self.get_entities():
            p = e.get('Pos')
            if isinstance(p, (list, tuple)) and len(p) >= 3:
                fx = p[0] / WW * (W - 1); fy = p[1] / WH * (H - 1)
                if 0 <= fx < W - 1 and 0 <= fy < H - 1:
                    E.append((fx, fy, p[2]))
        return E[:limit]

    def _height_fit(self, off, W, E):
        """R^2 of the grid at ``off`` predicting entity Z (bilinear). Early-out."""
        d = self.data
        xs = []; ys = []
        for fx, fy, ez in E:
            x0 = int(fx); y0 = int(fy); b = off + (y0 * W + x0) * 4
            if b + W * 4 + 4 > len(d):
                return -9.0
            h00, h10 = struct.unpack_from('<2f', d, b)
            if h00 != h00 or abs(h00) > 500:
                return -9.0
            h01, h11 = struct.unpack_from('<2f', d, b + W * 4)
            tx = fx - x0; ty = fy - y0
            xs.append(h00 * (1 - tx) * (1 - ty) + h10 * tx * (1 - ty) +
                      h01 * (1 - tx) * ty + h11 * tx * ty)
            ys.append(ez)
        n = len(xs)
        if n < 20:
            return -9.0
        mx = sum(xs) / n; my = sum(ys) / n
        sxx = sum((x - mx) ** 2 for x in xs) or 1e-9
        syy = sum((y - my) ** 2 for y in ys) or 1e-9
        return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) ** 2 / (sxx * syy)

    def get_blck_grid(self):
        """Return the BLCK grid as a list of rows, each row a list of 6-tuples."""
        chunk = self.find_chunk('BLCK')
        if not chunk or 'grid_offset' not in chunk.meta:
            return None, 0, 0

        d = self.data
        w = chunk.meta['grid_w']
        h = chunk.meta['grid_h']
        offset = chunk.meta['grid_offset']

        grid = []
        pos = offset
        for y in range(h):
            row = []
            for x in range(w):
                cell = struct.unpack_from('<6H', d, pos)
                row.append(cell)
                pos += 12
            grid.append(row)
        return grid, w, h

