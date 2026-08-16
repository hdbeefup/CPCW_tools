#!/usr/bin/env python3
"""Parser for Codename: Panzers Cold War .map scenario files.

The .map format is a hierarchical chunk-based container using the Gepard engine's
OBJT/VOBJ/ARRY/SCHM serialization system (same as ProtoDB.bin).

Usage:
    python cpcw_map.py info       FILE.map          # print map summary
    python cpcw_map.py structure  FILE.map          # print chunk tree
    python cpcw_map.py schemas    FILE.map          # print all schemas
    python cpcw_map.py entities   FILE.map          # list placed entities
    python cpcw_map.py terrain    FILE.map          # print terrain layer info
    python cpcw_map.py dump       FILE.map [--json] # dump full object tree
    python cpcw_map.py blck       FILE.map FILE.png # render block grid as image
    python cpcw_map.py gui        FILE.map          # graphical viewer
"""

import argparse
import array
import glob
import json
import math
import os
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

# Types that use uint16-length-prefix + payload encoding, like strings.
# FT_FLAGS and FT_LOCSTR are deliberately NOT here: 0x039C is an on-disk ARRY of
# bools and 0x002B is a uint32 character count + 2 bytes/char UTF-16LE. Reading
# either as a u16-prefixed string desynchronises the rest of the record. Neither
# appears in an entity schema, which is why the entity path never noticed.
# See docs/MAP_FORMAT.md §6.1.
_STRING_LIKE = {FT_STRING, FT_GUID, FT_REF}

# A composite field type id's LOW byte selects the container kind; the bytes
# above it give the element (and for HASH, the key) type.
_KIND_ARRY = (0x8A, 0x90, 0x9C)
_KIND_HEAP = 0xA5
_KIND_HASH = 0xA6

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

    def __init__(self, data, schemas, record_offsets=False):
        self.data = data
        self.schemas = schemas
        # When True, each parsed object gets an ``_field_offsets`` dict mapping
        # field name -> (byte_offset, ftype) so the editor can write fields back
        # in place. Off by default (zero behaviour change for readers).
        self.record_offsets = record_offsets

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

        # trailing VOBJs (version extensions). Merge their fields -- and, when
        # recording, their field offsets too, so placement fields carried by a
        # trailing VOBJ (Pos/Dir/Elevation/Prototype/...) stay editable.
        p = vobj_end
        while p + 10 <= content_end and self.data[p:p + 4] == b'VOBJ':
            trailing, p = self._parse_vobj(p)
            if trailing:
                t_off = trailing.get('_field_offsets')
                for k, v in trailing.items():
                    if not k.startswith('_'):
                        obj[k] = v
                if t_off:
                    obj.setdefault('_field_offsets', OrderedDict()).update(t_off)

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
            foff = OrderedDict() if self.record_offsets else None
            for fname, ftype, fsize in schema.fields:
                if p >= content_end:
                    break
                fstart = p
                val, p = self._read_field(p, ftype, fsize, content_end)
                obj[fname] = val
                if foff is not None:
                    foff[fname] = (fstart, ftype)
            if foff is not None:
                obj['_field_offsets'] = foff

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
            # 8 bytes, but a PAIR OF FLOATS -- not a double. Both readings are the
            # same width so no structural walk can tell them apart; the values do.
            # SLocation.Size reads 0..697 x 0..677 as vec2f (ellipse half-extents,
            # world tops out near 512x672) and 7.5e9 / 2.3e20 as a double.
            if pos + 8 > limit: return None, limit
            a, _ = _f32(self.data, pos)
            b, _ = _f32(self.data, pos + 4)
            return [round(a, 4), round(b, 4)], pos + 8

        if ftype == FT_LOCSTR:
            if pos + 4 > limit: return None, limit
            n, p = _u32(self.data, pos)
            raw = self.data[p:p + 2 * n]
            return raw.decode('utf-16-le', errors='replace'), p + 2 * n

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

        kind = ftype & 0xFF
        if kind in _KIND_ARRY:
            return self._read_array(pos, limit, (ftype >> 8) & 0xFF)
        if kind == _KIND_HEAP:
            return self._read_heap(pos, limit)
        if kind == _KIND_HASH:
            return self._read_hash(pos, limit, (ftype >> 8) & 0xFF,
                                   (ftype >> 16) & 0xFF)

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
            if tag in (b'ARRY', b'OBJT', b'VOBJ', b'HEAP', b'HASH'):
                return self._read_inline(pos, limit)
            if pos + 2 <= limit:
                slen, _ = _u16(self.data, pos)
                if slen < 4096 and pos + 2 + slen <= limit:
                    s = self.data[pos + 2:pos + 2 + slen].decode('ascii', errors='replace')
                    return s, pos + 2 + slen

        return f'<unknown:0x{ftype:04x}>', pos

    def _read_array(self, pos, limit, elem=0x89):
        """Read an ARRY. `elem` is the element type from the field's composite id
        (high byte) -- elements are NOT always OBJT. Deriving a width from
        (size-4)/count instead gives non-integral results on real data."""
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
            if elem in (FT_INLINE1, FT_INLINE2):
                if self.data[p:p + 4] != b'OBJT':
                    break
                obj, p = self._parse_objt(p)
                if obj:
                    items.append(obj)
            else:
                val, p = self._read_field(p, elem, 0, arr_end)
                items.append(val)
        return items, arr_end

    def _read_heap(self, pos, limit):
        """Read a HEAP slot pool. See docs/MAP_FORMAT.md §5.6. Returns the LIVE
        records, each tagged with its slot index -- the slot index is the stable
        key (SLocation carries a field literally named HeapIndex), and the used
        chain is non-monotonic in slot index on 58 of the 924 shipped heaps."""
        if pos + 32 > limit or self.data[pos:pos + 4] != b'HEAP':
            return [], pos
        size, _ = _u32(self.data, pos + 4)
        end = pos + 8 + size
        slot_count, _ = _u32(self.data, pos + 8)
        p = pos + 32
        items = []
        for slot in range(slot_count):
            if p + 9 > end:
                break
            is_free = self.data[p + 8]
            p += 9
            if is_free == 0:
                obj, p = self._parse_objt(p)
                if obj:
                    obj['_heapIndex'] = slot
                    items.append(obj)
        return items, end

    def _read_hash(self, pos, limit, keyft=FT_STRING, valft=FT_INLINE2):
        """Read a HASH (string-keyed object map). See docs/MAP_FORMAT.md §5.7."""
        if pos + 12 > limit or self.data[pos:pos + 4] != b'HASH':
            return OrderedDict(), pos
        size, _ = _u32(self.data, pos + 4)
        end = pos + 8 + size
        count, _ = _u32(self.data, pos + 8)
        p = pos + 12
        out = OrderedDict()
        for _ in range(count):
            if p >= end:
                break
            key, p = self._read_field(p, keyft, 0, end)
            if valft in (FT_INLINE1, FT_INLINE2):
                val, p = self._parse_objt(p)
            else:
                val, p = self._read_field(p, valft, 0, end)
            out[str(key)] = val
        return out, end

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
        if tag == b'HEAP':
            return self._read_heap(pos, limit)
        if tag == b'HASH':
            return self._read_hash(pos, limit)
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
            self.data = bytearray(f.read())   # mutable: supports in-place edits
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

        # BLCK is TWO planes at ITS OWN header dims -- a uint16 flag plane then a
        # uint8 type plane -- not (w/2 x h/2) cells of six uint16s. Same byte
        # count, wrong stride; see docs/MAP_FORMAT.md section 8.3 for the
        # coherence measurement that separates them. The header dims are world*2
        # on only 41/45 maps, so never derive them from WRLD.
        chunk.meta.update({
            'version': version,
            'grid_w': dim1,
            'grid_h': dim2,
            'vertex_w': dim1,           # kept: older callers read these names
            'vertex_h': dim2,
            'flags_offset': pos,
            'types_offset': pos + dim1 * dim2 * 2,
            'grid_offset': pos,
            'payload_size': dim1 * dim2 * 3,
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

    def parse_objs_all(self, objs_chunk, with_offsets=False):
        """Parse ALL top-level OBJTs inside an OBJS chunk (flat list)."""
        parser = ObjParser(self.data, self.schemas, record_offsets=with_offsets)
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

    def get_entities(self, with_offsets=False):
        """Parse entities from the UNTS section.

        ``with_offsets=True`` attaches an ``_field_offsets`` dict to each entity
        (field -> (byte_offset, ftype)) so :meth:`set_entity_field` can edit
        fields in place. The editor uses this; readers get the plain values.
        """
        unts = self.find_chunk('UNTS')
        if not unts:
            return []
        objs = self.find_chunk('OBJS', unts)
        if not objs:
            return []
        return self.parse_objs_all(objs, with_offsets=with_offsets)

    def get_heightmap(self):
        """Return (heights, W, H) for the terrain, or None.

        The terrain elevation is a contiguous **f32 grid of (world_w+1) x
        (world_h+1)** vertices, row-major, in world units, stored inside the GTRD
        chunk (after the layer table) at a byte offset that is not necessarily
        4-aligned. It is located as the longest run of "height-like" f32 values
        (finite, |v| < 500) across all four byte phases; when that run is longer
        than the grid (a small preamble precedes it on some maps) the exact start
        is pinned by correlating against the map's own entity elevations
        (entities sit on the terrain). Verified against entity Z at R^2 ~ 0.9.
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

        # Collect every "height-like" run >= need/2 across all four byte phases.
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

        # Entity elevations are ground truth (units sit on the terrain): pick the
        # candidate offset whose grid best predicts them. Falls back to the
        # longest run's start if there are too few entities to calibrate.
        E = self._entity_grid_samples(W, H, WW, WH)
        if len(E) < 20:
            runs.sort(reverse=True)
            off = runs[0][1]
        else:
            best = (-9.0, runs[0][1])
            row = 4 * W
            runs.sort(reverse=True)   # try the longest (usually the grid) first
            for runlen, r0 in runs:
                if best[0] > 0.9:     # already a confident fit
                    break
                # The grid may start a few rows into the run (a small preamble of
                # height-like values precedes it on some maps), or the run may be
                # the whole grid (start == grid). Search a margin of rows past the
                # start plus any slack, capped to keep import fast.
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
        # Cache the located grid so the editor can write heights back in place.
        self._heightmap = {'offset': off, 'w': W, 'h': H}
        return heights, W, H

    def get_splatmap(self):
        """Return (layers, weights, W, H) terrain paint, or None.

        Immediately after the f32 heightmap, GTRD stores one row-major W*H uint8
        opacity grid per terrain layer (all slots, incl. inactive), then ~4 dense
        trailing grids (baked normals/AO, ignored). ``weights`` is one ``bytes``
        of length W*H per layer.
        """
        info = self.heightmap_info()
        if not info:
            return None
        off, W, H = info
        gtrd = self.find_chunk('GTRD')
        layers = gtrd.meta.get('layers', []) if gtrd else []
        if not layers:
            return None
        need = W * H
        ge = gtrd.offset + 8 + gtrd.size
        splat_start = off + need * 4
        weights = []
        for i in range(len(layers)):
            a = splat_start + i * need
            b = a + need
            if b > ge:
                break
            weights.append(bytes(self.data[a:b]))
        return (layers, weights, W, H) if weights else None

    def heightmap_info(self):
        """(offset, W, H) of the located elevation grid, or None. Locates it on
        first use (see get_heightmap). Enables in-place height edits."""
        if getattr(self, '_heightmap', None) is None:
            if self.get_heightmap() is None:
                return None
        hm = self._heightmap
        return hm['offset'], hm['w'], hm['h']

    def set_height(self, x, y, value):
        """Set the elevation of grid vertex (x, y) in place (round-trip safe)."""
        info = self.heightmap_info()
        if info is None:
            raise ValueError('no locatable heightmap in this map')
        off, w, h = info
        if not (0 <= x < w and 0 <= y < h):
            raise IndexError('vertex (%d,%d) out of %dx%d grid' % (x, y, w, h))
        struct.pack_into('<f', self.data, off + (y * w + x) * 4, value)

    def set_heights(self, heights):
        """Overwrite the whole elevation grid (list of W*H floats) in place."""
        info = self.heightmap_info()
        if info is None:
            raise ValueError('no locatable heightmap in this map')
        off, w, h = info
        if len(heights) != w * h:
            raise ValueError('expected %d heights, got %d' % (w * h, len(heights)))
        struct.pack_into('<%df' % (w * h), self.data, off, *heights)

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
        """Return (flags, types, w, h) -- the two BLCK planes, row-major.

        `flags` is a uint16 plane and `types` a uint8 plane, both w*h at BLCK's
        OWN header dims (docs/MAP_FORMAT.md section 8). Both are returned as flat
        `array`s; index a cell with [y * w + x].

        Semantics are not decoded: BLCK is read-only. The old 6-uint16-per-cell
        reading (and its (1,1,1,1,1,1)=passable table) was a misalignment.
        """
        chunk = self.find_chunk('BLCK')
        if not chunk or 'flags_offset' not in chunk.meta:
            return None, None, 0, 0

        d = self.data
        w = chunk.meta['grid_w']
        h = chunk.meta['grid_h']
        fo = chunk.meta['flags_offset']
        to = chunk.meta['types_offset']
        if to + w * h > len(d):
            return None, None, 0, 0

        flags = array.array('H'); flags.frombytes(bytes(d[fo:fo + w * h * 2]))
        if sys.byteorder != 'little':
            flags.byteswap()
        types = array.array('B'); types.frombytes(bytes(d[to:to + w * h]))
        return flags, types, w, h

    # -- lossless re-serialization (writer foundation) ----------------------

    def _serialize_chunk(self, chunk):
        """Emit a chunk (and its subtree) back to bytes.

        Reconstructs from the parsed chunk boundaries rather than copying the
        whole span, so edits to a nested chunk re-serialize correctly while an
        untouched tree is byte-identical: for a leaf (no children) the original
        [tag..content-end] span is emitted verbatim; for a container we emit its
        8-byte tag+size header, then the bytes between ``data_offset`` and the
        first child (the version/count sub-header), then each child recursively
        with any inter-child gap bytes preserved, then the trailing bytes up to
        the chunk end. This keeps *every* byte accounted for -- header padding,
        gaps and unknown tails included -- which is what makes the round-trip
        exact and safe to edit on top of.
        """
        d = self.data
        start = chunk.offset
        end = start + 8 + chunk.size
        if not chunk.children:
            return d[start:end]
        out = bytearray()
        out += d[start:chunk.children[0].offset]   # tag+size + sub-header
        for i, ch in enumerate(chunk.children):
            out += self._serialize_chunk(ch)
            nxt = (chunk.children[i + 1].offset if i + 1 < len(chunk.children)
                   else end)
            out += d[ch.offset + 8 + ch.size:nxt]  # gap / tail after child
        return bytes(out)

    def pack(self):
        """Re-serialize the whole map to bytes. Identity for an unedited file."""
        out = bytearray(self._serialize_chunk(self.root))
        root_end = self.root.offset + 8 + self.root.size
        if root_end < len(self.data):
            out += self.data[root_end:]            # preserve any file trailer
        return bytes(out)

    def write(self, path):
        with open(path, 'wb') as f:
            f.write(self.pack())

    # -- in-place field editing (size-preserving) ---------------------------

    # Fixed-width field types the editor can rewrite without changing any chunk
    # size (so the round-trip machinery stays trivial). Variable-width types
    # (strings, arrays, nested objects) are size-changing -> handled later by
    # the structural-edit path, not here.
    _EDIT_PACK = {
        FT_FLOAT:   ('<f', 1),
        FT_FLOAT64: ('<d', 1),
        FT_INT32:   ('<i', 1),
        FT_INT16:   ('<h', 1),
        FT_UINT8:   ('<B', 1),
        FT_BOOL:    ('<B', 1),
        FT_IID:     ('<I', 1),
        FT_ENTREF:  ('<I', 1),
        FT_VEC3:    ('<fff', 3),
        FT_VEC2F:   ('<ff', 2),
        FT_VEC2I:   ('<ii', 2),
    }

    def read_field(self, offset, ftype):
        """Read a fixed-width field's exact (unrounded) value from the buffer."""
        fmt = self._EDIT_PACK.get(ftype)
        if fmt is None:
            raise ValueError('field type 0x%04x is not fixed-width-editable' % ftype)
        vals = struct.unpack_from(fmt[0], self.data, offset)
        return vals[0] if fmt[1] == 1 else list(vals)

    def set_field(self, offset, ftype, value):
        """Write a fixed-width field in place (same byte length -> round-trip safe).

        Raises ValueError for variable-width / unsupported types. ``value`` is a
        scalar for scalar types or a sequence for VEC2/VEC3.
        """
        fmt, n = self._EDIT_PACK.get(ftype, (None, 0))
        if fmt is None:
            raise ValueError('field type 0x%04x is not in-place editable' % ftype)
        if ftype == FT_BOOL:
            value = 1 if value else 0
        if n == 1:
            struct.pack_into(fmt, self.data, offset, value)
        else:
            if len(value) != n:
                raise ValueError('expected %d components, got %d' % (n, len(value)))
            struct.pack_into(fmt, self.data, offset, *value)

    def set_entity_field(self, entity, field, value):
        """Edit one field of an entity parsed with ``get_entities(with_offsets=True)``.

        Returns the (offset, ftype) written. Raises if the entity carries no
        offset map or the field isn't a fixed-width type.
        """
        foff = entity.get('_field_offsets')
        if not foff:
            raise ValueError('entity has no _field_offsets '
                             '(parse with get_entities(with_offsets=True))')
        if field not in foff:
            raise KeyError('entity has no field %r' % field)
        offset, ftype = foff[field]
        self.set_field(offset, ftype, value)
        entity[field] = value            # keep the in-memory dict consistent
        return offset, ftype

    def move_entity(self, entity, dx=0.0, dy=0.0, dz=0.0):
        """Translate an entity by (dx,dy,dz) using its exact stored Pos."""
        foff = entity.get('_field_offsets') or {}
        if 'Pos' not in foff:
            raise KeyError('entity has no Pos field')
        offset, ftype = foff['Pos']
        x, y, z = self.read_field(offset, ftype)
        self.set_field(offset, ftype, [x + dx, y + dy, z + dz])
        entity['Pos'] = [round(x + dx, 4), round(y + dy, 4), round(z + dz, 4)]


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_info(mf):
    """Print a summary of the map file."""
    settings = mf.get_scenario_settings()
    wrld = mf.find_chunk('WRLD')
    gtrd = mf.find_chunk('GTRD')
    blck = mf.find_chunk('BLCK')
    unts = mf.find_chunk('UNTS')

    print(f'File:    {mf.filepath}')
    print(f'Size:    {len(mf.data):,} bytes')
    print(f'Version: {mf.root.meta.get("version", "?")}')

    if settings:
        print(f'Name:    {settings.get("Name", "?")}')
        print(f'Desc:    {settings.get("Description", "?")}')
        print(f'Music:   {settings.get("MusicFileName", "")}')
        print(f'Skybox:  {settings.get("SkyboxFileName", "")}')
        print(f'Unit limit:       {settings.get("UnitLimit", "?")}')
        print(f'Prestige limit:   {settings.get("PrestigeLimit", "?")}')
        print(f'Starting prestige:{settings.get("StartingPrestige", "?")}')

    if wrld:
        w = wrld.meta.get('width', '?')
        h = wrld.meta.get('height', '?')
        print(f'World:   {w} x {h}')

    if gtrd:
        m = gtrd.meta
        print(f'Terrain: {m.get("world_x","?")} x {m.get("world_y","?")} world units')
        print(f'  Grid:   {m.get("grid_w","?")} x {m.get("grid_h","?")}')
        print(f'  Layers: {m.get("layer_count","?")}')
        for i, layer in enumerate(m.get('layers', [])):
            if layer['name']:
                print(f'    [{i}] {layer["name"]} (scale={layer["uv_scale"]}, detail={layer["detail"]})')

    if blck:
        m = blck.meta
        print(f'Block grid: {m.get("grid_w","?")} x {m.get("grid_h","?")} cells')

    if unts:
        print(f'Entities: {unts.meta.get("entity_count", "?")}')

    print(f'Schemas: {len(mf.schemas)} types')


def cmd_structure(mf, max_depth=None):
    """Print the chunk tree."""
    def walk(chunk, depth=0):
        if max_depth is not None and depth > max_depth:
            return
        indent = '  ' * depth
        meta_str = ''
        for k, v in chunk.meta.items():
            if k == 'layers':
                meta_str += f' layers={len(v)}'
            elif k in ('grid_offset', 'splatmap_offset', 'splatmap_size'):
                continue
            else:
                meta_str += f' {k}={v}'
        size_str = _fmt_size(chunk.size)
        print(f'{indent}{chunk.tag}  @0x{chunk.offset:08X}  size={size_str}{meta_str}')
        for child in chunk.children:
            walk(child, depth + 1)

    walk(mf.root)


def cmd_schemas(mf):
    """Print all schema definitions."""
    for tid in sorted(mf.schemas.keys()):
        s = mf.schemas[tid]
        print(f'0x{tid:04X} {s.name} (v{s.version}, {len(s.fields)} fields)')
        for fname, ftype, fsize in s.fields:
            tname = FIELD_TYPE_NAMES.get(ftype, f'0x{ftype:04x}')
            print(f'  {fname}: {tname} (size={fsize})')
        print()


def cmd_entities(mf):
    """List placed entities with positions."""
    entities = mf.get_entities()
    if not entities:
        print('No entities found.')
        return

    # Each entity may have nested fields from inheritance (SUnitDesc contains
    # SEntityDesc fields via VOBJ merging).  Flatten: walk each entity and
    # collect fields at any depth that look like descriptor data.

    # Group by outermost _type
    by_type = {}
    for e in entities:
        t = e.get('_type', '?')
        by_type.setdefault(t, []).append(e)

    print(f'Total entities: {len(entities)}\n')
    for t in sorted(by_type.keys()):
        ents = by_type[t]
        print(f'{t}: {len(ents)}')
        for e in ents[:10]:
            proto = e.get('Prototype', '')
            pos = e.get('Pos', '')
            eid = e.get('ID', '')
            player = e.get('Player', '')
            parts = []
            if proto:
                parts.append(f'proto={proto}')
            if isinstance(pos, list) and len(pos) >= 3:
                parts.append(f'pos=({pos[0]:.1f}, {pos[1]:.1f}, {pos[2]:.1f})')
            if player != '':
                parts.append(f'player={player}')
            if eid != '':
                parts.append(f'id={eid}')
            print(f'  {", ".join(parts)}')
        if len(ents) > 10:
            print(f'  ... and {len(ents) - 10} more')
        print()


def cmd_terrain(mf):
    """Print terrain layer details."""
    gtrd = mf.find_chunk('GTRD')
    if not gtrd:
        print('No GTRD chunk found.')
        return

    m = gtrd.meta
    print(f'GTRD version: {m["version"]}')
    print(f'Grid: {m["grid_w"]} x {m["grid_h"]}')
    print(f'World size: {m["world_x"]} x {m["world_y"]}')
    print(f'Layer count: {m["layer_count"]}')
    print(f'Splatmap data: {m["splatmap_size"]:,} bytes at 0x{m["splatmap_offset"]:X}')

    print(f'\nLayers:')
    for i, layer in enumerate(m.get('layers', [])):
        status = 'active' if layer['active'] else 'inactive'
        name = layer['name'] or '(empty)'
        print(f'  [{i}] {name}')
        print(f'      type={layer["type"]} scale={layer["uv_scale"]} detail="{layer["detail"]}" [{status}]')

    # Splatmap statistics
    d = mf.data
    start = m['splatmap_offset']
    end = start + m['splatmap_size']
    nonzero = sum(1 for i in range(start, end) if d[i] != 0)
    print(f'\nSplatmap: {nonzero:,} non-zero bytes of {m["splatmap_size"]:,} ({nonzero / m["splatmap_size"] * 100:.1f}% used)')


def cmd_dump(mf, as_json=False):
    """Dump the full object tree from all OBJS sections."""
    all_trees = OrderedDict()

    # Settings
    settings = mf.get_scenario_settings()
    if settings:
        all_trees['settings'] = settings

    # Objectives
    prec = mf.find_chunk('PREC')
    if prec:
        ojts = mf.find_chunk('OJTS', prec)
        if ojts:
            objs = mf.find_chunk('OBJS', ojts)
            if objs:
                tree = mf.parse_objs_tree(objs)
                if tree:
                    all_trees['objectives'] = tree

    # Game state
    for objs_chunk in mf.find_chunks('OBJS'):
        parent_tags = set()
        # check parent context
        def find_parent(root, target):
            for c in root.children:
                if c is target:
                    return root
                r = find_parent(c, target)
                if r:
                    return r
            return None
        parent = find_parent(mf.root, objs_chunk)
        if parent and parent.tag == 'SCEN':
            # This is the main game state OBJS
            tree = mf.parse_objs_tree(objs_chunk)
            if tree:
                all_trees['game_state'] = tree

    # World objects
    wrld = mf.find_chunk('WRLD')
    if wrld:
        for objs_chunk in mf.find_chunks('OBJS', wrld):
            # skip UNTS sub-OBJS
            parent = find_parent(wrld, objs_chunk)
            if parent and parent.tag == 'WRLD':
                tree = mf.parse_objs_tree(objs_chunk)
                if tree:
                    all_trees['world'] = tree

    # Entities
    entities = mf.get_entities()
    if entities:
        all_trees['entities'] = entities

    if as_json:
        print(json.dumps(_to_serializable(all_trees), indent=2, ensure_ascii=False))
    else:
        for section, tree in all_trees.items():
            print(f'=== {section} ===')
            if isinstance(tree, list):
                for item in tree:
                    _print_obj(item, 1)
            else:
                _print_obj(tree, 1)
            print()


def cmd_blck(mf, output_path):
    """Render the BLCK grid as a PNG image."""
    try:
        from PIL import Image
    except ImportError:
        print('Error: Pillow is required for image output.  pip install Pillow')
        return

    flags, types, w, h = mf.get_blck_grid()
    if flags is None:
        print('No BLCK data found.')
        return

    # The plane values are not decoded (docs/MAP_FORMAT.md section 8.4), so this
    # is a legibility rendering, not a semantic one: a stable colour per distinct
    # value so the spatial structure is visible and two maps can be compared.
    # Assign colours by descending frequency, so the dominant value is always the
    # dark background whatever its numeric code happens to be.
    from collections import Counter
    palette = [(30, 30, 30), (60, 120, 60), (170, 140, 60), (60, 110, 170),
               (170, 70, 70), (140, 80, 170), (80, 170, 170), (200, 200, 120)]
    order = [v for v, _ in Counter(types).most_common()]
    colour = {v: palette[i % len(palette)] for i, v in enumerate(order)}

    img = Image.new('RGB', (w, h))
    pixels = img.load()
    for y in range(h):
        row = y * w
        for x in range(w):
            pixels[x, y] = colour[types[row + x]]

    img.save(output_path)
    fh = Counter(flags).most_common(6)
    th = Counter(types).most_common(6)
    print(f'Saved {w}x{h} block grid (type plane) to {output_path}')
    print(f'  flags plane: {len(set(flags))} distinct, top {fh}')
    print(f'  types plane: {len(set(types))} distinct, top {th}')


def cmd_gui(mf):
    """Launch a graphical chunk/object viewer."""
    import tkinter as tk
    from tkinter import ttk

    win = tk.Tk()
    win.title(f'CPCW Map Viewer — {mf.filepath}')
    win.geometry('1400x900')
    win.configure(bg='#1e1e1e')

    style = ttk.Style()
    style.theme_use('clam')
    style.configure('Treeview', background='#1e1e1e', foreground='#d4d4d4',
                    fieldbackground='#1e1e1e', font=('Consolas', 10), rowheight=22)
    style.configure('Treeview.Heading', background='#333', foreground='#ccc',
                    font=('Consolas', 10, 'bold'))
    style.map('Treeview', background=[('selected', '#264f78')],
              foreground=[('selected', '#ffffff')])
    style.configure('TFrame', background='#1e1e1e')
    style.configure('TLabel', background='#1e1e1e', foreground='#d4d4d4',
                    font=('Consolas', 10))

    # Paned layout
    paned = ttk.PanedWindow(win, orient=tk.HORIZONTAL)
    paned.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    # Left: chunk tree
    left = ttk.Frame(paned)
    tree = ttk.Treeview(left, show='tree', selectmode='browse')
    vsb = ttk.Scrollbar(left, orient=tk.VERTICAL, command=tree.yview)
    tree.configure(yscrollcommand=vsb.set)
    tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    vsb.pack(side=tk.RIGHT, fill=tk.Y)
    paned.add(left, weight=1)

    # Right: detail
    right = ttk.Frame(paned)
    detail = tk.Text(right, wrap=tk.WORD, bg='#1e1e1e', fg='#d4d4d4',
                     font=('Consolas', 10), relief='flat', padx=8, pady=8,
                     insertbackground='#d4d4d4', selectbackground='#264f78')
    detail_vsb = ttk.Scrollbar(right, orient=tk.VERTICAL, command=detail.yview)
    detail.configure(yscrollcommand=detail_vsb.set)
    detail.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    detail_vsb.pack(side=tk.RIGHT, fill=tk.Y)
    paned.add(right, weight=2)

    detail.tag_configure('heading', foreground='#569cd6', font=('Consolas', 11, 'bold'))
    detail.tag_configure('key', foreground='#9cdcfe')
    detail.tag_configure('value', foreground='#ce9178')
    detail.tag_configure('number', foreground='#b5cea8')
    detail.tag_configure('dim', foreground='#dcdcaa')

    item_data = {}  # tree iid -> chunk or object dict

    def add_chunk(parent_iid, chunk):
        meta_str = ''
        for k, v in chunk.meta.items():
            if k == 'layers':
                meta_str += f'  [{len(v)} layers]'
            elif k in ('grid_offset', 'splatmap_offset', 'splatmap_size', 'schema_offset'):
                continue
            else:
                meta_str += f'  {k}={v}'
        label = f'{chunk.tag}  ({_fmt_size(chunk.size)}){meta_str}'
        iid = tree.insert(parent_iid, 'end', text=label)
        item_data[iid] = chunk
        for child in chunk.children:
            add_chunk(iid, child)
        return iid

    def show_chunk(chunk):
        detail.configure(state=tk.NORMAL)
        detail.delete('1.0', tk.END)
        detail.insert(tk.END, f'{chunk.tag}\n', 'heading')
        detail.insert(tk.END, f'Offset: 0x{chunk.offset:08X}\n', 'dim')
        detail.insert(tk.END, f'Size:   {chunk.size:,} bytes\n\n', 'dim')

        for k, v in chunk.meta.items():
            if k == 'layers':
                detail.insert(tk.END, f'Layers ({len(v)}):\n', 'key')
                for i, layer in enumerate(v):
                    detail.insert(tk.END, f'  [{i}] ', 'key')
                    detail.insert(tk.END, f'{layer["name"] or "(empty)"}\n', 'value')
                    detail.insert(tk.END, f'      scale={layer["uv_scale"]}  detail={layer["detail"]}  active={layer["active"]}\n', 'dim')
            else:
                detail.insert(tk.END, f'{k}: ', 'key')
                detail.insert(tk.END, f'{v}\n', 'number' if isinstance(v, (int, float)) else 'value')

        # If it's an OBJS, try to parse and show the object tree
        if chunk.tag == 'OBJS':
            detail.insert(tk.END, '\n--- Object Tree ---\n\n', 'heading')
            obj = mf.parse_objs_tree(chunk)
            if obj:
                _show_obj_in_text(detail, obj, 0)

        detail.configure(state=tk.DISABLED)

    def _show_obj_in_text(widget, obj, depth):
        if not isinstance(obj, dict):
            widget.insert(tk.END, f'{"  " * depth}{obj}\n')
            return
        typ = obj.get('_type', '?')
        name = obj.get('Name', obj.get('Prototype', obj.get('GUID', '')))
        header = typ
        if name:
            header += f': {name}'
        widget.insert(tk.END, f'{"  " * depth}[{header}]\n', 'heading')
        for k, v in obj.items():
            if k.startswith('_'):
                continue
            if isinstance(v, list):
                widget.insert(tk.END, f'{"  " * (depth + 1)}{k}: ({len(v)} items)\n', 'key')
                for item in v[:20]:
                    _show_obj_in_text(widget, item, depth + 2)
                if len(v) > 20:
                    widget.insert(tk.END, f'{"  " * (depth + 2)}... +{len(v) - 20} more\n', 'dim')
            elif isinstance(v, dict):
                widget.insert(tk.END, f'{"  " * (depth + 1)}{k}:\n', 'key')
                _show_obj_in_text(widget, v, depth + 2)
            else:
                widget.insert(tk.END, f'{"  " * (depth + 1)}{k}', 'key')
                widget.insert(tk.END, f': {v}\n', 'number' if isinstance(v, (int, float)) else 'value')

    def on_select(event):
        sel = tree.selection()
        if sel and sel[0] in item_data:
            show_chunk(item_data[sel[0]])

    tree.bind('<<TreeviewSelect>>', on_select)

    # Build tree
    root_iid = add_chunk('', mf.root)
    tree.item(root_iid, open=True)
    for child_iid in tree.get_children(root_iid):
        tree.item(child_iid, open=True)

    win.mainloop()


# ---------------------------------------------------------------------------
# Utility
# ---------------------------------------------------------------------------

def _fmt_size(size):
    if size >= 1048576:
        return f'{size / 1048576:.1f} MB'
    if size >= 1024:
        return f'{size / 1024:.1f} KB'
    return f'{size} B'


def _to_serializable(obj):
    if isinstance(obj, OrderedDict):
        return {k: _to_serializable(v) for k, v in obj.items()}
    if isinstance(obj, dict):
        return {k: _to_serializable(v) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_to_serializable(v) for v in obj]
    if isinstance(obj, bytes):
        return obj.hex()
    if isinstance(obj, float):
        if obj != obj:  # NaN
            return None
        return obj
    return obj


def _print_obj(obj, depth):
    if not isinstance(obj, dict):
        print(f'{"  " * depth}{obj}')
        return

    typ = obj.get('_type', '?')
    name = obj.get('Name', obj.get('Prototype', obj.get('GUID', '')))
    header = typ
    if name:
        header += f': {name}'
    print(f'{"  " * depth}[{header}]')

    for k, v in obj.items():
        if k.startswith('_'):
            continue
        indent = '  ' * (depth + 1)
        if isinstance(v, list):
            if not v:
                print(f'{indent}{k}: []')
            else:
                print(f'{indent}{k}: ({len(v)} items)')
                for item in v:
                    _print_obj(item, depth + 2)
        elif isinstance(v, dict):
            print(f'{indent}{k}:')
            _print_obj(v, depth + 2)
        else:
            print(f'{indent}{k}: {v}')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Representative ground-type colour by layer-name keyword (sRGB, 0-1). Mirrors
# blendertools/CPCWMap_Blender/import_map.py so the editor's terrain reads like
# the game's ground without decoding the layer .dds textures.
_LAYER_PALETTE = (
    (('grass', 'foliage', 'long_grass', 'meadow'), (0.27, 0.39, 0.17)),
    (('tillage', 'ploughland', 'soil', 'muddy', 'mud', 'dirt', 'field'), (0.34, 0.25, 0.16)),
    (('gritty', 'ground', 'straw', 'sand', 'dry', 'default'), (0.52, 0.44, 0.30)),
    (('cobble', 'road', 'pavement', 'stone', 'rock', 'ruin', 'gravel', 'mine'), (0.44, 0.43, 0.42)),
    (('water', 'puddle', 'river', 'sea'), (0.20, 0.29, 0.33)),
    (('snow', 'winter', 'ice'), (0.80, 0.82, 0.85)),
    (('bump', 'normal', 'wind'), (0.40, 0.38, 0.33)),
)


def _layer_keyword_color(name):
    s = (name or '').lower()
    for keys, col in _LAYER_PALETTE:
        if any(k in s for k in keys):
            return col
    return (0.35, 0.33, 0.28)


def _bake_terrain_colormap(mf):
    """Composite the GTRD paint layers into a row-major W*H RGB byte grid, or None."""
    sp = mf.get_splatmap()
    if not sp:
        return None
    layers, weights, W, H = sp
    active = [i for i, l in enumerate(layers) if l.get('active')]
    if not active:
        return None
    cols = [_layer_keyword_color(l.get('name', '')) for l in layers]
    base, overlays = active[0], active[1:]
    buf = bytearray(W * H * 3)
    br, bg, bb = cols[base]
    for gi in range(W * H):
        r, g, b = br, bg, bb
        for li in overlays:
            w = weights[li][gi] / 255.0
            if w > 0.0:
                lr, lg, lb = cols[li]
                r = r * (1 - w) + lr * w
                g = g * (1 - w) + lg * w
                b = b * (1 - w) + lb * w
        o = gi * 3
        buf[o] = int(r * 255); buf[o+1] = int(g * 255); buf[o+2] = int(b * 255)
    return buf, W, H


def cmd_scene(mf, output=None):
    """Export an editor-facing scene as JSON (terrain dims + entity list).

    This is the interim data bridge the C++/ImGui editor loads until the parser
    is ported to C++ (see docs/MAP_EDITOR.md). '-' or no output prints to stdout.
    """
    wrld = mf.find_chunk('WRLD')
    scene = {
        'name': os.path.splitext(os.path.basename(mf.filepath))[0],
        'terrain': {
            'world_w': (wrld.meta.get('width') if wrld else 0) or 0,
            'world_h': (wrld.meta.get('height') if wrld else 0) or 0,
        },
        'entities': [],
    }
    hm = mf.heightmap_info()
    if hm:
        scene['terrain']['grid_w'] = hm[1]
        scene['terrain']['grid_h'] = hm[2]
        # For a file export, dump the elevation grid as a compact raw f32 sidecar
        # (row-major, grid_w*grid_h) the 3D editor loads directly -- far smaller
        # and faster than embedding ~333k floats in JSON.
        if output and output != '-':
            import array as _arr
            heights, W, H = mf.get_heightmap()
            r32 = os.path.splitext(output)[0] + '.r32'
            with open(r32, 'wb') as hf:
                _arr.array('f', heights).tofile(hf)
            scene['terrain']['heightmap'] = os.path.basename(r32)
            # bake the splat paint into a per-vertex RGB sidecar for the 3D editor
            cm = _bake_terrain_colormap(mf)
            if cm is not None:
                cbuf, cW, cH = cm
                if cW == W and cH == H:
                    rgb = os.path.splitext(output)[0] + '.rgb'
                    with open(rgb, 'wb') as cf:
                        cf.write(cbuf)
                    scene['terrain']['colormap'] = os.path.basename(rgb)
    for e in mf.get_entities():
        pos = e.get('Pos') or [0, 0, 0]
        t = e.get('_type', '?')
        kind = 1 if t == 'SBuildingUnitDesc' else (0 if t == 'SDoodadDesc' else 2)
        scene['entities'].append({
            'type': t,
            'proto': e.get('Prototype', ''),
            'pos': [round(float(pos[0]), 3), round(float(pos[1]), 3),
                    round(float(pos[2]), 3)] if len(pos) >= 3 else [0, 0, 0],
            'dir': e.get('Dir', 0.0),
            'player': e.get('Player', 0),
            'id': e.get('ID', 0),
            'kind': kind,   # 0 doodad, 1 building/unit, 2 effect/sound/deformer
        })
    text = json.dumps(scene, separators=(',', ':'))
    if output and output != '-':
        with open(output, 'w', encoding='utf-8') as f:
            f.write(text)
        print('wrote %s (%d entities)' % (output, len(scene['entities'])))
    else:
        print(text)


def cmd_apply(mappath, edits_json, output):
    """Apply an edit list to a .map and write it out (the editor's Save path).

    ``edits_json`` is a JSON file: {"edits":[{"id":N, "pos":[x,y,z], "player":P,
    "hp":H, "level":L}, ...]} (or a bare list). Each edit targets an entity by
    its ID; only the given fixed-width fields are changed, in place, so the rest
    of the file stays byte-identical. Verifies each entity is found.
    """
    field_alias = {'pos': 'Pos', 'dir': 'Dir', 'player': 'Player',
                   'hp': 'HP', 'level': 'Level', 'ammo': 'Ammo',
                   'elevation': 'Elevation'}
    with open(edits_json, 'r', encoding='utf-8') as f:
        doc = json.load(f)
    edits = doc.get('edits', doc) if isinstance(doc, dict) else doc

    mf = MapFile(mappath)
    ents = mf.get_entities(with_offsets=True)
    by_id = {e.get('ID'): e for e in ents if 'ID' in e}
    applied = 0
    for ed in edits:
        eid = ed.get('id')
        ent = by_id.get(eid)
        if ent is None:
            print('  skip: no entity with id %r' % eid)
            continue
        for key, val in ed.items():
            if key == 'id':
                continue
            field = field_alias.get(key, key)
            try:
                mf.set_entity_field(ent, field, val)
                applied += 1
            except (KeyError, ValueError) as e:
                print('  skip id %r field %r: %s' % (eid, key, e))
    mf.write(output)
    print('applied %d field edit(s) across %d edit record(s) -> %s'
          % (applied, len(edits), output))


def cmd_roundtrip(path, output=None, batch=False):
    """Verify the writer: pack() must reproduce the original bytes exactly.

    This is the foundation the editor stands on -- until parse->pack is identity
    across every shipped map, no edit can be saved safely. --batch scans a dir.
    """
    if batch:
        files = glob.glob(os.path.join(path, '**', '*.map'), recursive=True)
        ok = bad = err = skip = 0
        notes = []
        for f in files:
            try:
                with open(f, 'rb') as fh:
                    magic = fh.read(4)
                if magic != b'SCEN':          # foreign .map (WASM/JSON/older engine)
                    skip += 1
                    continue
                mf = MapFile(f)
                if mf.pack() == mf.data:
                    ok += 1
                else:
                    bad += 1
                    if len(notes) < 12:
                        notes.append('MISMATCH %s' % os.path.basename(f))
            except Exception as e:
                err += 1
                if len(notes) < 12:
                    notes.append('ERROR %s: %s' % (os.path.basename(f), e))
        print('roundtrip: %d CPCW maps, exact=%d mismatch=%d error=%d '
              '(skipped %d non-SCEN .map)' % (ok + bad + err, ok, bad, err, skip))
        for n in notes:
            print('  ', n)
        return

    mf = MapFile(path)
    out = mf.pack()
    print('%s: %d bytes, re-write %s' % (path, len(mf.data),
          'IDENTICAL' if out == mf.data else 'DIFFERS (%d bytes)' % len(out)))
    if output:
        mf.write(output)
        print('wrote', output)


def main():
    parser = argparse.ArgumentParser(
        description='Codename: Panzers Cold War .map file parser',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument('command',
                        choices=['info', 'structure', 'schemas', 'entities',
                                 'terrain', 'dump', 'blck', 'gui', 'roundtrip',
                                 'scene', 'apply'])
    parser.add_argument('file', help='Path to .map file (or directory for roundtrip --batch)')
    parser.add_argument('output', nargs='?', help='Output path (blck / roundtrip / scene / apply)')
    parser.add_argument('--edits', help='Edit-list JSON (apply command)')
    parser.add_argument('--json', action='store_true', help='JSON output (dump)')
    parser.add_argument('--depth', type=int, default=None, help='Max depth (structure)')
    parser.add_argument('--batch', action='store_true', help='Recurse a directory (roundtrip)')

    args = parser.parse_args()

    if args.command == 'roundtrip':
        cmd_roundtrip(args.file, args.output, args.batch)
        return
    if args.command == 'apply':
        if not args.edits or not args.output:
            parser.error('apply needs --edits <edits.json> and an output path')
        cmd_apply(args.file, args.edits, args.output)
        return

    mf = MapFile(args.file)

    if args.command == 'info':
        cmd_info(mf)
    elif args.command == 'structure':
        cmd_structure(mf, args.depth)
    elif args.command == 'schemas':
        cmd_schemas(mf)
    elif args.command == 'entities':
        cmd_entities(mf)
    elif args.command == 'terrain':
        cmd_terrain(mf)
    elif args.command == 'dump':
        cmd_dump(mf, args.json)
    elif args.command == 'blck':
        if not args.output:
            parser.error('blck command requires an output path')
        cmd_blck(mf, args.output)
    elif args.command == 'gui':
        cmd_gui(mf)
    elif args.command == 'scene':
        cmd_scene(mf, args.output)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    main()
