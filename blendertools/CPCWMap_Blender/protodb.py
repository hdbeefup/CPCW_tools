"""Minimal ProtoDB.bin reader (stdlib only, Blender-safe).

Vendored from cpcw_protodb.py — just enough to build a GUID -> model-path index
so the map importer can resolve each entity's Prototype to its .srm model.
"""

import struct

FT_INT32 = 0x0001
FT_FLOAT = 0x0002
FT_BOOL = 0x0003
FT_STRING = 0x0004
FT_GUID = 0x0011
FT_REF = 0x0012
FT_UINT8 = 0x0017
FT_INT16 = 0x0019
FT_INLINE1 = 0x0088
FT_INLINE2 = 0x0089
FT_FLAGS = 0x039c
FT_BLOB = 0x0165
FT_ARRAY = 0x898a


class Schema:
    __slots__ = ('name', 'type_id', 'version', 'fields')

    def __init__(self, name, type_id, version, fields):
        self.name = name
        self.type_id = type_id
        self.version = version
        self.fields = fields


class ProtoDB:
    def __init__(self, filepath):
        with open(filepath, 'rb') as f:
            self.data = f.read()
        self.schemas = {}
        self._parse_header()
        self._parse_schemas()

    def _parse_header(self):
        if self.data[0:4] != b'OBJS':
            raise ValueError("Not a ProtoDB file (expected OBJS)")
        self.total_size = struct.unpack_from('<I', self.data, 4)[0]
        self.schema_offset = struct.unpack_from('<I', self.data, 8)[0]

    def _parse_schemas(self):
        pos = self.schema_offset
        if self.data[pos:pos + 4] != b'SCHD':
            raise ValueError("Expected SCHD at 0x%x" % pos)
        pos += 8
        schema_count = struct.unpack_from('<H', self.data, pos)[0]
        pos += 4
        for _ in range(schema_count):
            if pos + 8 > len(self.data) or self.data[pos:pos + 4] != b'SCHM':
                break
            content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
            cs = pos + 8
            ce = cs + content_sz
            p = cs
            name_len = struct.unpack_from('<H', self.data, p)[0]; p += 2
            name = self.data[p:p + name_len].decode('ascii', 'replace'); p += name_len
            type_id, version, field_count = struct.unpack_from('<HHH', self.data, p); p += 6
            fields = []
            for _ in range(field_count):
                if p + 2 > ce:
                    break
                fn_len = struct.unpack_from('<H', self.data, p)[0]; p += 2
                if p + fn_len > ce:
                    break
                fn = self.data[p:p + fn_len].decode('ascii', 'replace'); p += fn_len
                if p + 8 > ce:
                    break
                ftype, fsize = struct.unpack_from('<II', self.data, p); p += 8
                fields.append((fn, ftype, fsize))
            self.schemas[type_id] = Schema(name, type_id, version, fields)
            pos = ce

    def parse_root(self):
        obj, _ = self._parse_objt(12)
        return obj

    def _parse_objt(self, pos):
        if self.data[pos:pos + 4] != b'OBJT':
            return None, pos
        content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
        type_id = struct.unpack_from('<H', self.data, pos + 8)[0]
        content_end = pos + 8 + content_sz
        obj, vobj_end = self._parse_vobj(pos + 10)
        if obj is None:
            obj = {'_type_id': type_id}
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
        content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
        type_id = struct.unpack_from('<H', self.data, pos + 8)[0]
        content_end = pos + 8 + content_sz
        schema = self.schemas.get(type_id)
        obj = {'_type_id': type_id,
               '_type': schema.name if schema else 'Unknown_0x%04x' % type_id}
        p = pos + 10
        if p + 2 > content_end:
            return obj, content_end
        p += 2  # version
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
            return (struct.unpack_from('<i', self.data, pos)[0], pos + 4) if pos + 4 <= limit else (None, limit)
        elif ftype == FT_FLOAT:
            return (round(struct.unpack_from('<f', self.data, pos)[0], 6), pos + 4) if pos + 4 <= limit else (None, limit)
        elif ftype == FT_BOOL:
            return (bool(self.data[pos]), pos + 1) if pos + 1 <= limit else (None, limit)
        elif ftype in (FT_STRING, FT_GUID, FT_REF, FT_FLAGS):
            if pos + 2 > limit:
                return None, limit
            slen = struct.unpack_from('<H', self.data, pos)[0]; pos += 2
            if slen == 0:
                return '', pos
            end = min(pos + slen, limit)
            return self.data[pos:end].decode('ascii', 'replace'), end
        elif ftype == FT_UINT8:
            return (self.data[pos], pos + 1) if pos + 1 <= limit else (None, limit)
        elif ftype == FT_INT16:
            return (struct.unpack_from('<h', self.data, pos)[0], pos + 2) if pos + 2 <= limit else (None, limit)
        elif ftype == FT_ARRAY:
            return self._read_array(pos, limit)
        elif ftype in (FT_INLINE1, FT_INLINE2):
            return self._read_inline_object(pos, limit)
        elif ftype == FT_BLOB:
            nbytes = fsize if 0 < fsize != 65535 else 0
            if nbytes and pos + nbytes <= limit:
                return self.data[pos:pos + nbytes].hex(), pos + nbytes
            return None, pos
        else:
            if 0 < fsize != 65535 and pos + fsize <= limit:
                return None, pos + fsize
            return None, pos

    def _read_array(self, pos, limit):
        if pos + 12 > limit or self.data[pos:pos + 4] != b'ARRY':
            return [], pos
        arr_size = struct.unpack_from('<I', self.data, pos + 4)[0]
        arr_count = struct.unpack_from('<I', self.data, pos + 8)[0]
        arr_end = pos + 8 + arr_size
        items = []
        p = pos + 12
        for _ in range(arr_count):
            if p >= arr_end or self.data[p:p + 4] != b'OBJT':
                break
            obj, p = self._parse_objt(p)
            if obj:
                items.append(obj)
        return items, arr_end

    def _read_inline_object(self, pos, limit):
        if pos + 8 > limit:
            return None, pos
        tag = self.data[pos:pos + 4]
        if tag == b'OBJT':
            return self._parse_objt(pos)
        elif tag == b'VOBJ':
            return self._parse_vobj(pos)
        return None, pos


def build_model_index(filepath):
    """Return {guid_lower: model_path} for every prototype that names a model."""
    db = ProtoDB(filepath)
    root = db.parse_root()
    idx = {}

    def walk(o):
        if isinstance(o, dict):
            g = o.get('GUID')
            if g:
                model = o.get('ModelName') or o.get('MarketModelName') or ''
                if model:
                    idx[g.lower()] = model.replace('\\', '/')
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(root)
    return idx
