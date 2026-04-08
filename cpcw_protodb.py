#!/usr/bin/env python3
"""Viewer for Codename: Panzers Cold War ProtoDB.bin prototype database."""

import argparse
import json
import struct
import sys

# Field type constants
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

FIELD_TYPE_NAMES = {
    FT_INT32: 'int32', FT_FLOAT: 'float', FT_BOOL: 'bool',
    FT_STRING: 'string', FT_GUID: 'GUID', FT_REF: 'ref',
    FT_UINT8: 'uint8', FT_INT16: 'int16',
    FT_INLINE1: 'object', FT_INLINE2: 'object',
    FT_FLAGS: 'flags', FT_BLOB: 'blob', FT_ARRAY: 'array',
}


class Schema:
    __slots__ = ('name', 'type_id', 'version', 'fields')

    def __init__(self, name, type_id, version, fields):
        self.name = name
        self.type_id = type_id
        self.version = version
        self.fields = fields  # list of (field_name, field_type, field_size)


class ProtoDB:
    def __init__(self, filepath):
        with open(filepath, 'rb') as f:
            self.data = f.read()
        self.schemas = {}
        self._parse_header()
        self._parse_schemas()

    def _parse_header(self):
        tag = self.data[0:4]
        if tag != b'OBJS':
            raise ValueError(f"Not a ProtoDB file (expected OBJS, got {tag})")
        self.total_size = struct.unpack_from('<I', self.data, 4)[0]
        self.schema_offset = struct.unpack_from('<I', self.data, 8)[0]

    def _parse_schemas(self):
        pos = self.schema_offset
        if self.data[pos:pos + 4] != b'SCHD':
            raise ValueError(f"Expected SCHD at 0x{pos:x}")
        schd_size = struct.unpack_from('<I', self.data, pos + 4)[0]
        pos += 8
        schema_count = struct.unpack_from('<H', self.data, pos)[0]
        pos += 4  # count(2) + unknown(2)

        for _ in range(schema_count):
            if pos + 8 > len(self.data) or self.data[pos:pos + 4] != b'SCHM':
                break
            content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
            cs = pos + 8
            ce = cs + content_sz

            p = cs
            name_len = struct.unpack_from('<H', self.data, p)[0]; p += 2
            name = self.data[p:p + name_len].decode('ascii', errors='replace'); p += name_len
            type_id, version, field_count = struct.unpack_from('<HHH', self.data, p); p += 6

            fields = []
            for _ in range(field_count):
                if p + 2 > ce:
                    break
                fn_len = struct.unpack_from('<H', self.data, p)[0]; p += 2
                if p + fn_len > ce:
                    break
                fn = self.data[p:p + fn_len].decode('ascii', errors='replace'); p += fn_len
                if p + 8 > ce:
                    break
                ftype, fsize = struct.unpack_from('<II', self.data, p); p += 8
                fields.append((fn, ftype, fsize))

            self.schemas[type_id] = Schema(name, type_id, version, fields)
            pos = ce

    def parse_root(self):
        """Parse the root OBJT and return the object tree."""
        obj, _ = self._parse_objt(12)
        return obj

    def _parse_objt(self, pos):
        """Parse an OBJT chunk at pos, return (obj_dict, end_pos)."""
        if self.data[pos:pos + 4] != b'OBJT':
            return None, pos
        content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
        type_id = struct.unpack_from('<H', self.data, pos + 8)[0]
        content_end = pos + 10 + content_sz - 2  # size includes type_id bytes?
        # Actually: OBJT tag(4) + size(4) + type_id(2). Content after type_id is size-2 bytes?
        # No. Let me reconsider. OBJT at 0x1b0: tag(4)+size(4)=8 bytes header.
        # size=0x67=103. type_id at pos+8 is the first 2 bytes of content.
        # So content runs from pos+8 for size bytes, ending at pos+8+size.
        content_end = pos + 8 + content_sz

        # Parse the VOBJ inside (starts after tag+size+type_id = 10 bytes)
        obj, vobj_end = self._parse_vobj(pos + 10)
        if obj is None:
            obj = {'_type_id': type_id, '_type': self.schemas.get(type_id, Schema('?', type_id, 0, [])).name}

        # There may be trailing VOBJs (siblings) between vobj_end and content_end
        p = vobj_end
        while p + 10 <= content_end and self.data[p:p + 4] == b'VOBJ':
            trailing, p = self._parse_vobj(p)
            # These are typically version-extension VOBJs, merge fields
            if trailing:
                for k, v in trailing.items():
                    if not k.startswith('_'):
                        obj[k] = v

        return obj, content_end

    def _parse_vobj(self, pos):
        """Parse a VOBJ chunk, decode fields using schema."""
        if self.data[pos:pos + 4] != b'VOBJ':
            return None, pos
        content_sz = struct.unpack_from('<I', self.data, pos + 4)[0]
        type_id = struct.unpack_from('<H', self.data, pos + 8)[0]
        content_end = pos + 8 + content_sz

        schema = self.schemas.get(type_id)
        obj = {
            '_type_id': type_id,
            '_type': schema.name if schema else f'Unknown_0x{type_id:04x}',
        }

        p = pos + 10  # after header
        if p + 2 > content_end:
            return obj, content_end

        version = struct.unpack_from('<H', self.data, p)[0]
        p += 2

        if schema:
            for fname, ftype, fsize in schema.fields:
                if p >= content_end:
                    break
                val, p = self._read_field(p, ftype, fsize, content_end)
                obj[fname] = val

        return obj, content_end

    def _read_field(self, pos, ftype, fsize, limit):
        """Read a single field value, return (value, new_pos)."""
        if pos >= limit:
            return None, pos

        if ftype == FT_INT32:
            if pos + 4 > limit:
                return None, limit
            return struct.unpack_from('<i', self.data, pos)[0], pos + 4

        elif ftype == FT_FLOAT:
            if pos + 4 > limit:
                return None, limit
            return round(struct.unpack_from('<f', self.data, pos)[0], 6), pos + 4

        elif ftype == FT_BOOL:
            if pos + 1 > limit:
                return None, limit
            return bool(self.data[pos]), pos + 1

        elif ftype in (FT_STRING, FT_GUID, FT_REF, FT_FLAGS):
            if pos + 2 > limit:
                return None, limit
            slen = struct.unpack_from('<H', self.data, pos)[0]
            pos += 2
            if slen == 0:
                return '', pos
            if pos + slen > limit:
                return self.data[pos:limit].decode('ascii', errors='replace'), limit
            val = self.data[pos:pos + slen].decode('ascii', errors='replace')
            return val, pos + slen

        elif ftype == FT_UINT8:
            if pos + 1 > limit:
                return None, limit
            return self.data[pos], pos + 1

        elif ftype == FT_INT16:
            if pos + 2 > limit:
                return None, limit
            return struct.unpack_from('<h', self.data, pos)[0], pos + 2

        elif ftype == FT_ARRAY:
            return self._read_array(pos, limit)

        elif ftype in (FT_INLINE1, FT_INLINE2):
            return self._read_inline_object(pos, limit)

        elif ftype == FT_BLOB:
            nbytes = fsize if fsize > 0 and fsize != 65535 else 0
            if nbytes > 0 and pos + nbytes <= limit:
                val = self.data[pos:pos + nbytes].hex()
                return val, pos + nbytes
            return None, pos

        else:
            # Unknown type — try to skip using fsize
            if fsize > 0 and fsize != 65535 and pos + fsize <= limit:
                val = self.data[pos:pos + fsize].hex()
                return f'<raw:0x{ftype:04x}>{val}', pos + fsize
            return f'<unknown:0x{ftype:04x}>', pos

    def _read_array(self, pos, limit):
        """Read an ARRY chunk."""
        if pos + 12 > limit or self.data[pos:pos + 4] != b'ARRY':
            return [], pos
        arr_size = struct.unpack_from('<I', self.data, pos + 4)[0]
        arr_count = struct.unpack_from('<I', self.data, pos + 8)[0]
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

    def _read_inline_object(self, pos, limit):
        """Read an inline OBJT chunk for 0x0088/0x0089 fields."""
        if pos + 8 > limit:
            return None, pos

        tag = self.data[pos:pos + 4]
        if tag == b'OBJT':
            obj, end = self._parse_objt(pos)
            return obj, end
        elif tag == b'VOBJ':
            # Sometimes inline is just a bare VOBJ
            obj, end = self._parse_vobj(pos)
            return obj, end
        else:
            # Null/empty inline — might be zero-size
            return None, pos


def cmd_schemas(db):
    """Print all schema definitions."""
    for tid in sorted(db.schemas.keys()):
        s = db.schemas[tid]
        print(f"0x{tid:04x} {s.name} (v{s.version}, {len(s.fields)} fields)")
        for fname, ftype, fsize in s.fields:
            tname = FIELD_TYPE_NAMES.get(ftype, f'0x{ftype:04x}')
            print(f"  {fname}: {tname} (size={fsize})")
        print()


def cmd_tree(db, max_depth=None):
    """Print folder tree with object counts."""
    root = db.parse_root()

    def print_tree(obj, depth=0):
        if max_depth is not None and depth > max_depth:
            return
        indent = '  ' * depth
        name = obj.get('Name', '') or '(root)'
        typ = obj.get('_type', '')
        subfolders = obj.get('SubFolders', [])
        objects = obj.get('Objects', [])

        sub_count = len(subfolders) if isinstance(subfolders, list) else 0
        obj_count = len(objects) if isinstance(objects, list) else 0

        extra = ''
        if sub_count:
            extra += f' [{sub_count} folders]'
        if obj_count:
            extra += f' [{obj_count} objects]'

        print(f"{indent}{name} ({typ}){extra}")

        if isinstance(subfolders, list):
            for sub in subfolders:
                if isinstance(sub, dict):
                    print_tree(sub, depth + 1)

    print_tree(root)


def _obj_to_serializable(obj):
    """Recursively convert an object dict to JSON-safe form."""
    if isinstance(obj, dict):
        return {k: _obj_to_serializable(v) for k, v in obj.items()}
    elif isinstance(obj, list):
        return [_obj_to_serializable(v) for v in obj]
    elif isinstance(obj, bytes):
        return obj.hex()
    elif isinstance(obj, float):
        if obj != obj:  # NaN
            return None
        return obj
    return obj


def cmd_dump(db, as_json=False):
    """Dump the full database."""
    root = db.parse_root()
    if as_json:
        print(json.dumps(_obj_to_serializable(root), indent=2, ensure_ascii=False))
    else:
        _print_obj(root, 0)


def _print_obj(obj, depth):
    """Pretty-print an object tree."""
    if not isinstance(obj, dict):
        print(f"{'  ' * depth}{obj}")
        return

    indent = '  ' * depth
    typ = obj.get('_type', '?')
    name = obj.get('Name', obj.get('GUID', ''))
    header = f"{typ}"
    if name:
        header += f': {name}'
    print(f"{indent}[{header}]")

    for k, v in obj.items():
        if k.startswith('_'):
            continue
        if isinstance(v, list):
            if len(v) == 0:
                print(f"{indent}  {k}: []")
            else:
                print(f"{indent}  {k}: ({len(v)} items)")
                for item in v:
                    _print_obj(item, depth + 2)
        elif isinstance(v, dict):
            print(f"{indent}  {k}:")
            _print_obj(v, depth + 2)
        else:
            print(f"{indent}  {k}: {v}")


def cmd_search(db, term):
    """Search for objects matching a term."""
    root = db.parse_root()
    term_lower = term.lower()
    results = []

    def walk(obj, path=''):
        if not isinstance(obj, dict):
            return
        name = obj.get('Name', '')
        guid = obj.get('GUID', '')
        typ = obj.get('_type', '')

        # Check if any string field matches
        matched = False
        for k, v in obj.items():
            if isinstance(v, str) and term_lower in v.lower():
                matched = True
                break

        if matched:
            results.append((path, obj))

        # Recurse into arrays and inline objects
        for k, v in obj.items():
            if isinstance(v, list):
                for i, item in enumerate(v):
                    walk(item, f"{path}/{name or typ}[{i}]" if path else f"{name or typ}[{i}]")
            elif isinstance(v, dict):
                walk(v, f"{path}/{k}" if path else k)

    walk(root)

    print(f"Found {len(results)} matches for '{term}':\n")
    for path, obj in results:
        typ = obj.get('_type', '?')
        name = obj.get('Name', obj.get('GUID', ''))
        print(f"  {path}")
        _print_obj(obj, 2)
        print()
        if len(results) > 50:
            print(f"  ... and {len(results) - 50} more")
            break


def cmd_gui(db):
    """Launch graphical viewer."""
    import tkinter as tk
    from tkinter import ttk

    root_obj = db.parse_root()

    win = tk.Tk()
    win.title('CPCW ProtoDB Viewer')
    win.geometry('1280x800')
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
    style.configure('Search.TEntry', fieldbackground='#2d2d2d', foreground='#d4d4d4',
                    insertcolor='#d4d4d4')
    style.configure('TPanedwindow', background='#1e1e1e')
    style.configure('Horizontal.TScrollbar', background='#333', troughcolor='#1e1e1e')
    style.configure('Vertical.TScrollbar', background='#333', troughcolor='#1e1e1e')

    # --- Top bar: search ---
    top_frame = ttk.Frame(win)
    top_frame.pack(fill=tk.X, padx=4, pady=(4, 0))
    ttk.Label(top_frame, text='Search:').pack(side=tk.LEFT, padx=(0, 4))
    search_var = tk.StringVar()
    search_entry = tk.Entry(top_frame, textvariable=search_var, bg='#2d2d2d',
                            fg='#d4d4d4', insertbackground='#d4d4d4',
                            font=('Consolas', 10), relief='flat')
    search_entry.pack(side=tk.LEFT, fill=tk.X, expand=True)

    # --- Main paned window ---
    paned = ttk.PanedWindow(win, orient=tk.HORIZONTAL)
    paned.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    # Left: tree view
    left_frame = ttk.Frame(paned)
    tree = ttk.Treeview(left_frame, show='tree', selectmode='browse')
    tree_vsb = ttk.Scrollbar(left_frame, orient=tk.VERTICAL, command=tree.yview)
    tree.configure(yscrollcommand=tree_vsb.set)
    tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    tree_vsb.pack(side=tk.RIGHT, fill=tk.Y)
    paned.add(left_frame, weight=1)

    # Right: detail view
    right_frame = ttk.Frame(paned)
    detail_text = tk.Text(right_frame, wrap=tk.WORD, bg='#1e1e1e', fg='#d4d4d4',
                          font=('Consolas', 10), relief='flat', padx=8, pady=8,
                          insertbackground='#d4d4d4', selectbackground='#264f78')
    detail_vsb = ttk.Scrollbar(right_frame, orient=tk.VERTICAL, command=detail_text.yview)
    detail_text.configure(yscrollcommand=detail_vsb.set)
    detail_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
    detail_vsb.pack(side=tk.RIGHT, fill=tk.Y)
    paned.add(right_frame, weight=2)

    # Text tags for syntax coloring
    detail_text.tag_configure('type_name', foreground='#569cd6', font=('Consolas', 11, 'bold'))
    detail_text.tag_configure('field_name', foreground='#9cdcfe')
    detail_text.tag_configure('field_value', foreground='#ce9178')
    detail_text.tag_configure('guid', foreground='#6a9955')
    detail_text.tag_configure('number', foreground='#b5cea8')
    detail_text.tag_configure('bool_true', foreground='#4ec9b0')
    detail_text.tag_configure('bool_false', foreground='#d16969')
    detail_text.tag_configure('section', foreground='#c586c0', font=('Consolas', 10, 'bold'))
    detail_text.tag_configure('array_header', foreground='#dcdcaa')

    # Map tree item IDs to object dicts
    item_map = {}

    def get_node_label(obj):
        typ = obj.get('_type', '?')
        name = obj.get('Name', '')
        if not name:
            name = obj.get('GUID', '')
            if name:
                name = name[:12] + '...'
        if name:
            return f'{name}  [{typ}]'
        return f'[{typ}]'

    def populate_tree(parent_id, obj):
        """Add an object and its children to the tree, with lazy loading for arrays."""
        label = get_node_label(obj)
        iid = tree.insert(parent_id, 'end', text=label)
        item_map[iid] = obj

        # Add child folders
        subfolders = obj.get('SubFolders', [])
        if isinstance(subfolders, list):
            for child in subfolders:
                if isinstance(child, dict):
                    populate_tree(iid, child)

        # Add child objects
        objects = obj.get('Objects', [])
        if isinstance(objects, list):
            for child in objects:
                if isinstance(child, dict):
                    populate_tree(iid, child)

        # Add array fields as expandable nodes (except SubFolders/Objects already shown)
        for k, v in obj.items():
            if k in ('SubFolders', 'Objects') or k.startswith('_'):
                continue
            if isinstance(v, list) and len(v) > 0:
                arr_iid = tree.insert(iid, 'end', text=f'{k} ({len(v)} items)')
                item_map[arr_iid] = {'_type': f'Array: {k}', '_items': v}
                for i, item in enumerate(v):
                    if isinstance(item, dict):
                        populate_tree(arr_iid, item)
            elif isinstance(v, dict) and '_type' in v:
                populate_tree(iid, v)

    def format_detail(obj):
        """Write formatted object details into the detail text widget."""
        detail_text.configure(state=tk.NORMAL)
        detail_text.delete('1.0', tk.END)

        if not isinstance(obj, dict):
            detail_text.insert(tk.END, str(obj))
            detail_text.configure(state=tk.DISABLED)
            return

        typ = obj.get('_type', '?')
        type_id = obj.get('_type_id', '')
        type_id_str = f'  (0x{type_id:04X})' if isinstance(type_id, int) else ''

        detail_text.insert(tk.END, f'{typ}{type_id_str}\n', 'type_name')
        detail_text.insert(tk.END, '\n')

        for k, v in obj.items():
            if k.startswith('_'):
                continue
            detail_text.insert(tk.END, f'  {k}', 'field_name')
            detail_text.insert(tk.END, ': ')

            if isinstance(v, bool):
                tag = 'bool_true' if v else 'bool_false'
                detail_text.insert(tk.END, str(v), tag)
            elif isinstance(v, int):
                detail_text.insert(tk.END, str(v), 'number')
            elif isinstance(v, float):
                detail_text.insert(tk.END, str(v), 'number')
            elif isinstance(v, str):
                if len(v) == 36 and v.count('-') == 4:
                    detail_text.insert(tk.END, v, 'guid')
                else:
                    detail_text.insert(tk.END, v, 'field_value')
            elif isinstance(v, list):
                detail_text.insert(tk.END, f'({len(v)} items)', 'array_header')
                # Show brief summary of array items
                for i, item in enumerate(v[:20]):
                    if isinstance(item, dict):
                        itype = item.get('_type', '?')
                        iname = item.get('Name', item.get('GUID', ''))
                        if iname:
                            detail_text.insert(tk.END, f'\n    [{i}] ', 'field_name')
                            detail_text.insert(tk.END, f'{itype}: ', 'section')
                            detail_text.insert(tk.END, iname, 'field_value')
                        else:
                            detail_text.insert(tk.END, f'\n    [{i}] ', 'field_name')
                            detail_text.insert(tk.END, itype, 'section')
                if len(v) > 20:
                    detail_text.insert(tk.END, f'\n    ... and {len(v) - 20} more', 'field_name')
            elif isinstance(v, dict):
                itype = v.get('_type', '?')
                iname = v.get('Name', '')
                detail_text.insert(tk.END, f'{itype}', 'section')
                if iname:
                    detail_text.insert(tk.END, f': {iname}', 'field_value')
                # Show inline object fields
                for ik, iv in v.items():
                    if ik.startswith('_'):
                        continue
                    detail_text.insert(tk.END, f'\n      {ik}', 'field_name')
                    detail_text.insert(tk.END, ': ')
                    if isinstance(iv, (list, dict)):
                        detail_text.insert(tk.END, f'({type(iv).__name__})', 'array_header')
                    elif isinstance(iv, bool):
                        detail_text.insert(tk.END, str(iv), 'bool_true' if iv else 'bool_false')
                    elif isinstance(iv, (int, float)):
                        detail_text.insert(tk.END, str(iv), 'number')
                    else:
                        detail_text.insert(tk.END, str(iv), 'field_value')
            elif v is None:
                detail_text.insert(tk.END, 'null', 'bool_false')
            else:
                detail_text.insert(tk.END, str(v), 'field_value')

            detail_text.insert(tk.END, '\n')

        detail_text.configure(state=tk.DISABLED)

    def on_tree_select(event):
        sel = tree.selection()
        if not sel:
            return
        obj = item_map.get(sel[0])
        if obj:
            format_detail(obj)

    tree.bind('<<TreeviewSelect>>', on_tree_select)

    # --- Search functionality ---
    all_items = []  # (iid, label_lower)

    def build_search_index():
        all_items.clear()
        for iid in item_map:
            label = tree.item(iid, 'text').lower()
            obj = item_map[iid]
            # Also index Name and GUID for deeper matching
            name = obj.get('Name', '') if isinstance(obj, dict) else ''
            guid = obj.get('GUID', '') if isinstance(obj, dict) else ''
            searchable = f'{label} {name} {guid}'.lower()
            all_items.append((iid, searchable))

    search_results = []
    search_idx = 0

    def do_search(*_):
        nonlocal search_results, search_idx
        term = search_var.get().strip().lower()
        if not term:
            return
        search_results = [iid for iid, text in all_items if term in text]
        search_idx = 0
        if search_results:
            goto_search_result()

    def goto_search_result():
        nonlocal search_idx
        if not search_results:
            return
        iid = search_results[search_idx % len(search_results)]
        # Expand parents
        parent = tree.parent(iid)
        while parent:
            tree.item(parent, open=True)
            parent = tree.parent(parent)
        tree.selection_set(iid)
        tree.see(iid)
        tree.focus(iid)
        win.title(f'CPCW ProtoDB Viewer  [{search_idx + 1}/{len(search_results)}]')

    def on_search_key(event):
        nonlocal search_idx
        if event.keysym == 'Return':
            if event.state & 0x1:  # Shift+Enter = prev
                search_idx = (search_idx - 1) % max(len(search_results), 1)
                goto_search_result()
            else:
                if search_results:
                    search_idx = (search_idx + 1) % len(search_results)
                    goto_search_result()
                else:
                    do_search()

    _search_after_id = None

    def on_search_changed(*_):
        nonlocal _search_after_id
        if _search_after_id:
            win.after_cancel(_search_after_id)
        _search_after_id = win.after(300, do_search)

    search_var.trace_add('write', on_search_changed)
    search_entry.bind('<Return>', on_search_key)

    # Build tree
    win.update_idletasks()
    populate_tree('', root_obj)
    build_search_index()

    # Open first level
    for child in tree.get_children(''):
        tree.item(child, open=True)

    search_entry.focus_set()
    win.mainloop()


def main():
    parser = argparse.ArgumentParser(description='CPCW ProtoDB.bin viewer')
    parser.add_argument('command', choices=['schemas', 'tree', 'dump', 'search', 'gui'])
    parser.add_argument('file', help='Path to ProtoDB.bin')
    parser.add_argument('term', nargs='?', help='Search term (for search command)')
    parser.add_argument('--json', action='store_true', help='Output as JSON (dump command)')
    parser.add_argument('--depth', type=int, default=None, help='Max depth for tree command')

    args = parser.parse_args()
    db = ProtoDB(args.file)

    if args.command == 'schemas':
        cmd_schemas(db)
    elif args.command == 'tree':
        cmd_tree(db, args.depth)
    elif args.command == 'dump':
        cmd_dump(db, args.json)
    elif args.command == 'search':
        if not args.term:
            parser.error('search requires a term')
        cmd_search(db, args.term)
    elif args.command == 'gui':
        cmd_gui(db)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    main()
