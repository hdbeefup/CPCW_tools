"""CPCW .map scenario viewer for Blender (basic first pass).

Builds a flat, correct-extent terrain plane (optionally tinted by the BLCK
passability grid) and places every map entity as a labelled Empty at its world
position and facing. Real per-entity model instancing (resolving Prototype ->
.srm via ProtoDB) is a documented future step — the entity loop is structured
so a resolver can later swap an Empty for a model without restructuring.

Map coordinate system: X-Y is the ground plane (0..world_size), Z is up
(elevation). That already matches Blender's Z-up, so entities are placed
directly at their Pos; Dir[0] is a yaw angle in degrees about Z.
"""

import math
import os
import re

import bpy
from bpy.props import StringProperty, BoolProperty, FloatProperty, IntProperty
from bpy_extras.io_utils import ImportHelper

from . import map_format
from . import protodb


# Entity descriptor type -> (collection name, empty display, display size)
_TYPE_STYLE = {
    'SUnitDesc':           ('Units', 'SINGLE_ARROW', 2.0),
    'SVehicleUnitDesc':    ('Units', 'SINGLE_ARROW', 2.5),
    'SBuildingUnitDesc':   ('Buildings', 'CUBE', 2.5),
    'SDoodadDesc':         ('Doodads', 'PLAIN_AXES', 1.0),
    'SEffectEntityDesc':   ('Effects', 'SPHERE', 1.0),
    'SSquadDesc':          ('Squads', 'SINGLE_ARROW', 2.0),
}
_DEFAULT_STYLE = ('Misc', 'PLAIN_AXES', 1.0)


def _get_world_size(mf):
    wrld = mf.find_chunk('WRLD')
    if wrld:
        w = wrld.meta.get('width')
        h = wrld.meta.get('height')
        if w and h:
            return float(w), float(h)
    return 512.0, 512.0


# ---------------------------------------------------------------------------
# Terrain splatmap colouring (GTRD paint layers -> per-vertex colour)
# ---------------------------------------------------------------------------

# Keyword -> representative sRGB colour, used when a layer's .dds can't be found.
_LAYER_PALETTE = (
    (('grass', 'foliage', 'long_grass', 'meadow'), (0.27, 0.39, 0.17)),
    (('tillage', 'ploughland', 'soil', 'muddy', 'mud', 'dirt', 'field'),
     (0.34, 0.25, 0.16)),
    (('gritty', 'ground', 'straw', 'sand', 'dry', 'default'), (0.52, 0.44, 0.30)),
    (('cobble', 'road', 'pavement', 'stone', 'rock', 'ruin', 'gravel', 'mine'),
     (0.44, 0.43, 0.42)),
    (('water', 'puddle', 'river', 'sea'), (0.20, 0.29, 0.33)),
    (('snow', 'winter', 'ice'), (0.80, 0.82, 0.85)),
    (('bump', 'normal', 'wind'), (0.40, 0.38, 0.33)),
)

_dds_index_cache = {}
_dds_avg_cache = {}


def _dds_index(data_root):
    """Map every extracted .dds basename (lower, no ext) -> full path (cached)."""
    if data_root in _dds_index_cache:
        return _dds_index_cache[data_root]
    idx = {}
    try:
        for dp, _dirs, fns in os.walk(data_root):
            for fn in fns:
                if fn.lower().endswith('.dds'):
                    idx.setdefault(fn[:-4].lower(), os.path.join(dp, fn))
    except OSError:
        pass
    _dds_index_cache[data_root] = idx
    return idx


def _resolve_layer_dds(name, idx):
    """Resolve a GTRD layer name to a .dds path, tolerating map prefixes."""
    base = name.replace('\\', '/').split('/')[-1]
    cands = [base, base.replace(' ', '_')]
    # strip leading map tag: "M2_", "M06_", "Tutor_1_", "Tutor 1 ", "T_01_"
    m = re.match(r'(?i)^(m\d+|tutor[_ ]?\d+|t_?\d+)[_ ]?(.+)$', base)
    if m:
        cands.append(m.group(2))
        cands.append(m.group(2).replace(' ', '_'))
    for c in cands:
        p = idx.get(c.lower())
        if p:
            return p
    return None


def _dds_avg_color(path):
    """Average sRGB colour of a .dds top mip (sampled, cached). None on failure."""
    if path in _dds_avg_cache:
        return _dds_avg_cache[path]
    col = None
    try:
        from . import dds
        img = dds.DDS.read(path)
        rgba = img.rgba
        n = len(rgba) // 4
        if n:
            step = max(1, n // 4096)
            r = g = b = cnt = 0
            for i in range(0, n, step):
                o = i * 4
                r += rgba[o]; g += rgba[o + 1]; b += rgba[o + 2]
                cnt += 1
            col = (r / cnt / 255.0, g / cnt / 255.0, b / cnt / 255.0)
    except Exception as e:
        print("  dds avg failed for %s: %s" % (os.path.basename(path), e))
    _dds_avg_cache[path] = col
    return col


def _keyword_color(name):
    s = name.lower()
    for keys, col in _LAYER_PALETTE:
        if any(k in s for k in keys):
            return col
    return (0.35, 0.33, 0.28)


def _srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _layer_colors(layers, data_root):
    """Per-layer representative colour (LINEAR): real .dds average else palette.

    Colours are converted sRGB->linear because a FLOAT_COLOR vertex attribute is
    fed to Base Color as linear (no colour management), so storing the sRGB value
    directly would render ~2x too bright.
    """
    idx = _dds_index(data_root) if data_root else {}
    cols = []
    for lay in layers:
        col = None
        if idx:
            p = _resolve_layer_dds(lay['name'], idx)
            if p:
                col = _dds_avg_color(p)
        if col is None:
            col = _keyword_color(lay['name'])
        cols.append(tuple(_srgb_to_linear(x) for x in col))
    return cols


def _splat_vertex_colors(mf, data_root, uvs):
    """Composite the GTRD paint layers into a per-vertex RGB list.

    ``uvs`` is a list of (u, v) in [0,1] per terrain vertex (row-major order the
    mesh was built in). Returns a list of (r, g, b) the same length, or None if
    no splatmap is available. Layers are alpha-composited in file order ("over"),
    starting from the base layer (layer 0), matching how the paint stacks.
    """
    try:
        sp = mf.get_splatmap()
    except Exception as e:
        print("  splatmap decode failed: %s" % e)
        sp = None
    if not sp:
        return None
    layers, weights, W, H = sp
    active = [i for i, l in enumerate(layers) if l.get('active')]
    if not active:
        return None
    colors = _layer_colors(layers, data_root)
    base_i = active[0]
    overlays = active[1:]
    out = []
    for u, v in uvs:
        gx = int(round(u * (W - 1)))
        gy = int(round(v * (H - 1)))
        gi = gy * W + gx
        r, g, b = colors[base_i]
        for li in overlays:
            w = weights[li][gi] / 255.0
            if w <= 0.0:
                continue
            lr, lg, lb = colors[li]
            r = r * (1.0 - w) + lr * w
            g = g * (1.0 - w) + lg * w
            b = b * (1.0 - w) + lb * w
        out.append((r, g, b))
    return out


def _dds_to_bpy_image(path, name):
    """Decode a DDS via the vendored decoder into a Blender image (or None)."""
    try:
        from . import dds
        img = dds.DDS.read(path)
    except Exception as e:
        print("  dds->image failed for %s: %s" % (os.path.basename(path), e))
        return None
    bi = bpy.data.images.new(name, img.width, img.height, alpha=False)
    n = img.width * img.height
    px = [0.0] * (n * 4)
    rgba = img.rgba
    # DDS is top-down; Blender images are bottom-up -> flip rows. sRGB->linear.
    def s2l(c):
        c /= 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    w = img.width
    for y in range(img.height):
        sy = (img.height - 1 - y)
        for x in range(w):
            si = (sy * w + x) * 4
            di = (y * w + x) * 4
            px[di] = s2l(rgba[si]); px[di+1] = s2l(rgba[si+1]); px[di+2] = s2l(rgba[si+2])
            px[di+3] = 1.0
    bi.pixels.foreach_set(px)
    bi.pack()
    return bi


def _splat_mask_image(name, weight_bytes, W, H):
    """Build a WxH single-channel (in RGB) Blender image from uint8 splat weights."""
    bi = bpy.data.images.new(name, W, H, alpha=False, is_data=True)
    px = [0.0] * (W * H * 4)
    # Blender's pixel array is bottom-up: row 0 == UV v=0 == grid row 0. The
    # terrain UVs map v=j/ny (grid row j) directly, and the heightmap /
    # _splat_vertex_colors read grid row j with NO flip -- so the mask must NOT
    # flip either. (The old `sy = H-1-y` mirrored the paint N-S vs the terrain.)
    for y in range(H):
        row = y * W
        for x in range(W):
            w = weight_bytes[row + x] / 255.0
            di = (y * W + x) * 4
            px[di] = px[di+1] = px[di+2] = w
            px[di+3] = 1.0
    bi.pixels.foreach_set(px)
    bi.pack()
    return bi


def _build_textured_terrain_material(mf, data_root, world_w, world_h, max_layers=6):
    """Build a tiled-texture terrain material from the GTRD splatmap, or None.

    Each active layer's real .dds is tiled across the surface (world-scaled UV)
    and the layers are alpha-composited ("over", layer 0 base) using per-layer
    splat masks as mix factors -- so roads/fields/grass show real texture detail,
    matching the game, instead of a flat per-vertex tint.
    """
    if not data_root:
        return None
    try:
        sp = mf.get_splatmap()
    except Exception:
        sp = None
    if not sp:
        return None
    layers, weights, W, H = sp
    idx = _dds_index(data_root)
    if not idx:
        return None
    # Skip auxiliary (non-diffuse) layers -- bump/normal/wind/parallax maps are
    # not ground colour and would paint spurious blobs if shown as diffuse.
    _AUX = ('bump', 'normal', 'wind', 'parallax', 'detail', '_spec')
    active = [i for i, l in enumerate(layers)
              if l.get('active') and not any(a in l['name'].lower() for a in _AUX)]
    if not active:
        return None
    # Choose which layers to show: always the base (first active), then the most
    # heavily-PAINTED overlays (by non-zero weight count) up to the cap -- picking
    # the first N would drop a heavily-used later layer for a near-empty early one.
    base_i = active[0]
    overlays = active[1:]
    cover = {i: sum(1 for b in weights[i] if b) for i in overlays}
    top = sorted(overlays, key=lambda i: -cover[i])[:max(0, max_layers - 1)]
    chosen = sorted([base_i] + top)  # composite in file order
    # Every chosen layer contributes: real tiled .dds when it resolves, else its
    # by-ground-type palette colour (sRGB->linear) as a solid -- so a layer whose
    # texture can't be found (e.g. a map-specific/typo'd name) still shows the
    # right ground colour rather than vanishing.
    resolved = [(i, _resolve_layer_dds(layers[i]['name'], idx),
                 float(layers[i].get('uv_scale') or 1.0)) for i in chosen]

    mat = bpy.data.materials.new("TerrainTextured")
    mat.use_nodes = True
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links
    for n in list(nodes):
        nodes.remove(n)
    out = nodes.new('ShaderNodeOutputMaterial'); out.location = (900, 0)
    bsdf = nodes.new('ShaderNodeBsdfPrincipled'); bsdf.location = (620, 0)
    bsdf.inputs['Roughness'].default_value = 1.0
    links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    uv = nodes.new('ShaderNodeUVMap'); uv.location = (-1100, 0); uv.uv_map = "UVMap"

    # tiling: repeat each texture ~ every TILE world units (scaled by uv_scale)
    TILE = 12.0
    prev_color = None
    y = 0
    for li, path, uvs in resolved:
        # per-layer colour source: tiled image, or a solid palette colour
        color_out = None
        if path:
            tex_img = _dds_to_bpy_image(path, "terr_%02d_%s" % (li, os.path.basename(path)))
            if tex_img is not None:
                mapn = nodes.new('ShaderNodeMapping'); mapn.location = (-850, y)
                reps_x = max(1.0, world_w / TILE * uvs)
                reps_y = max(1.0, world_h / TILE * uvs)
                mapn.inputs['Scale'].default_value = (reps_x, reps_y, 1.0)
                links.new(uv.outputs['UV'], mapn.inputs['Vector'])
                tnode = nodes.new('ShaderNodeTexImage'); tnode.location = (-650, y)
                tnode.image = tex_img; tnode.extension = 'REPEAT'
                links.new(mapn.outputs['Vector'], tnode.inputs['Vector'])
                color_out = tnode.outputs['Color']
        if color_out is None:
            rgbn = nodes.new('ShaderNodeRGB'); rgbn.location = (-650, y)
            r, g, b = _keyword_color(layers[li]['name'])
            rgbn.outputs['Color'].default_value = (
                _srgb_to_linear(r), _srgb_to_linear(g), _srgb_to_linear(b), 1.0)
            color_out = rgbn.outputs['Color']
        if prev_color is None:
            prev_color = color_out
        else:
            mask = _splat_mask_image("mask_%02d" % li, weights[li], W, H)
            mnode = nodes.new('ShaderNodeTexImage'); mnode.location = (-650, y - 130)
            mnode.image = mask; mnode.extension = 'EXTEND'
            links.new(uv.outputs['UV'], mnode.inputs['Vector'])
            mix = nodes.new('ShaderNodeMixRGB'); mix.location = (200, y)
            mix.blend_type = 'MIX'
            links.new(mnode.outputs['Color'], mix.inputs['Fac'])
            links.new(prev_color, mix.inputs['Color1'])
            links.new(color_out, mix.inputs['Color2'])
            prev_color = mix.outputs['Color']
        y -= 320
    if prev_color is not None:
        links.new(prev_color, bsdf.inputs['Base Color'])
    return mat


def _build_terrain(mf, collection, world_w, world_h, tint_passability,
                   use_heightmap=True, height_res=256, data_root=None,
                   paint_splatmap=True):
    """Create a ground plane over the world extent.

    With ``use_heightmap`` the plane is a subdivided grid displaced by the real
    terrain heightmap (decoded from GTRD); otherwise it is flat at Z=0. With
    ``paint_splatmap`` the heightmap mesh is tinted per-vertex from the GTRD
    terrain paint layers (real .dds averages when found under ``data_root``).
    """
    verts = [(0.0, 0.0, 0.0), (world_w, 0.0, 0.0),
             (world_w, world_h, 0.0), (0.0, world_h, 0.0)]
    faces = [(0, 1, 2, 3)]

    heightmap = None
    if use_heightmap:
        try:
            heightmap = mf.get_heightmap()
        except Exception as e:
            print("  heightmap decode failed: %s" % e)
            heightmap = None

    if heightmap:
        heights, HW, HH = heightmap

        def sample(u, v):  # bilinear sample at normalized (u,v) in [0,1]
            fx = u * (HW - 1); fy = v * (HH - 1)
            x0 = int(fx); y0 = int(fy)
            x1 = min(x0 + 1, HW - 1); y1 = min(y0 + 1, HH - 1)
            tx = fx - x0; ty = fy - y0
            h00 = heights[y0 * HW + x0]; h10 = heights[y0 * HW + x1]
            h01 = heights[y1 * HW + x0]; h11 = heights[y1 * HW + x1]
            return (h00 * (1 - tx) * (1 - ty) + h10 * tx * (1 - ty) +
                    h01 * (1 - tx) * ty + h11 * tx * ty)

        # Subdivide to at most height_res per axis (full grid can be >500k verts).
        nx = min(height_res, HW - 1)
        ny = min(height_res, HH - 1)
        verts = []
        uvs = []
        for j in range(ny + 1):
            v = j / ny
            for i in range(nx + 1):
                u = i / nx
                verts.append((u * world_w, v * world_h, sample(u, v)))
                uvs.append((u, v))
        faces = []
        for j in range(ny):
            for i in range(nx):
                a = j * (nx + 1) + i
                faces.append((a, a + 1, a + (nx + 1) + 1, a + (nx + 1)))

        mesh = bpy.data.meshes.new("Terrain")
        mesh.from_pydata(verts, [], faces)
        mesh.update()

        # UV layer (normalized grid coords) for tiled textures / splat masks.
        uvl = mesh.uv_layers.new(name="UVMap")
        for loop in mesh.loops:
            uvl.data[loop.index].uv = uvs[loop.vertex_index]

        # Prefer a real tiled-texture material; keep the per-vertex tint as a
        # fallback (and for when the layer .dds can't be resolved).
        tex_mat = None
        if paint_splatmap:
            try:
                tex_mat = _build_textured_terrain_material(mf, data_root,
                                                           world_w, world_h)
            except Exception as e:
                print("  textured terrain material failed: %s" % e)
                tex_mat = None

        vcolors = None
        if paint_splatmap and tex_mat is None:
            vcolors = _splat_vertex_colors(mf, data_root, uvs)
        if vcolors:
            try:
                attr = mesh.color_attributes.new(name="Terrain",
                                                 type='FLOAT_COLOR', domain='POINT')
                for vi, (r, g, b) in enumerate(vcolors):
                    attr.data[vi].color = (r, g, b, 1.0)
            except (AttributeError, RuntimeError, IndexError) as e:
                print("  terrain vertex colours failed: %s" % e)
                vcolors = None

        obj = bpy.data.objects.new("Terrain", mesh)
        if tex_mat is not None:
            obj.data.materials.append(tex_mat)
            collection.objects.link(obj)
            return obj
        mat = bpy.data.materials.new("TerrainMat")
        mat.use_nodes = True
        pr = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        if pr:
            pr.inputs['Roughness'].default_value = 1.0
            if vcolors and mesh.color_attributes:
                vc = mat.node_tree.nodes.new('ShaderNodeVertexColor')
                vc.layer_name = "Terrain"
                vc.location = (-300, 0)
                mat.node_tree.links.new(vc.outputs['Color'],
                                        pr.inputs['Base Color'])
            else:
                pr.inputs['Base Color'].default_value = (0.32, 0.36, 0.28, 1.0)
        obj.data.materials.append(mat)
        collection.objects.link(obj)
        return obj

    # BLCK is a uint16 flag plane + a uint8 type plane at BLCK's own dims (see
    # docs/MAP_FORMAT.md section 8). The plane VALUES are not decoded, so this
    # tints by block type rather than claiming to know what is passable.
    types = None
    gw = gh = 0
    if tint_passability:
        _flags, types, gw, gh = mf.get_blck_grid()

    grid = types
    if grid and gw and gh:
        # Build a gw x gh quad grid so we can paint passability per cell.
        verts = []
        for j in range(gh + 1):
            for i in range(gw + 1):
                verts.append((i / gw * world_w, j / gh * world_h, 0.0))
        faces = []
        for j in range(gh):
            for i in range(gw):
                a = j * (gw + 1) + i
                b = a + 1
                c = a + (gw + 1) + 1
                d = a + (gw + 1)
                faces.append((a, b, c, d))

    mesh = bpy.data.meshes.new("Terrain")
    mesh.from_pydata(verts, [], faces)
    mesh.update()

    if grid and gw and gh:
        try:
            attr = mesh.color_attributes.new(name="BlockType", type='BYTE_COLOR',
                                             domain='CORNER')
            # Stable colour per distinct type code; type 0 is the dominant
            # background value on every map measured.
            palette = [(0.12, 0.12, 0.12, 1.0), (0.24, 0.47, 0.24, 1.0),
                       (0.67, 0.55, 0.24, 1.0), (0.24, 0.43, 0.67, 1.0),
                       (0.67, 0.27, 0.27, 1.0), (0.55, 0.31, 0.67, 1.0),
                       (0.31, 0.67, 0.67, 1.0), (0.78, 0.78, 0.47, 1.0)]
            for poly in mesh.polygons:
                # cell index from the polygon order (row-major, matches faces)
                cell_i = poly.index % gw
                cell_j = poly.index // gw
                t = grid[cell_j * gw + cell_i] if cell_j < gh else 0
                col = palette[t % len(palette)]
                for loop_idx in poly.loop_indices:
                    attr.data[loop_idx].color = col
        except (AttributeError, RuntimeError, IndexError):
            pass

    obj = bpy.data.objects.new("Terrain", mesh)
    mat = bpy.data.materials.new("TerrainMat")
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    principled = next((n for n in nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if principled:
        principled.inputs['Roughness'].default_value = 1.0
        if grid and gw and gh and mesh.color_attributes:
            # wire the passability colour attribute into Base Color
            vcol = nodes.new('ShaderNodeVertexColor')
            vcol.layer_name = "Passability"
            vcol.location = (-300, 0)
            links.new(vcol.outputs['Color'], principled.inputs['Base Color'])
        else:
            principled.inputs['Base Color'].default_value = (0.3, 0.35, 0.28, 1.0)
    obj.data.materials.append(mat)
    collection.objects.link(obj)
    return obj


# ---------------------------------------------------------------------------
# Real model placement (Prototype -> ProtoDB -> .srm -> collection instance)
# ---------------------------------------------------------------------------

def _resolve_data_root(filepath, data_root):
    """Find the extracted-data root that holds ProtoDB.bin + model folders.

    Uses the user-supplied dir if valid, else walks up from the .map looking for
    a directory that contains ProtoDB.bin (the extraction root).
    """
    if data_root:
        data_root = bpy.path.abspath(data_root)
        if os.path.isdir(data_root):
            return data_root
    d = os.path.dirname(os.path.abspath(filepath))
    for _ in range(6):
        if os.path.isfile(os.path.join(d, 'ProtoDB.bin')):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            break
        d = parent
    return None


def _import_model_collection(context, srm_path, data_root, import_textures):
    """Import one .srm via the SRM add-on's operator; return its collection.

    Returns None if the SRM importer is unavailable or the import fails.
    """
    if not hasattr(bpy.ops.import_scene, 'cpcw_srm'):
        return None
    before = set(bpy.data.collections)
    try:
        bpy.ops.import_scene.cpcw_srm(
            filepath=srm_path, import_textures=import_textures,
            texture_dir=data_root or "", apply_skin='NONE')
    except Exception as e:
        print("  model import failed for %s: %s" % (srm_path, e))
        return None
    new_cols = [c for c in bpy.data.collections if c not in before]
    return new_cols[0] if new_cols else None


def _place_models(context, mf, root, entities, data_root, import_textures,
                  models_max):
    """Resolve each entity's Prototype to a model and place a collection instance.

    Returns the number of instances created. Falls back silently (0) if ProtoDB
    or the SRM importer is missing; the caller still placed entity Empties.
    """
    data_root = _resolve_data_root(mf.filepath, data_root)
    if not data_root:
        print("  no ProtoDB.bin found (set 'Data Root'); skipping model placement")
        return 0
    proto_path = os.path.join(data_root, 'ProtoDB.bin')
    if not os.path.isfile(proto_path):
        print("  ProtoDB.bin not in data root; skipping model placement")
        return 0
    try:
        model_index = protodb.build_model_index(proto_path)
    except Exception as e:
        print("  ProtoDB parse failed: %s" % e)
        return 0

    # Template collections live under a hidden holder so only instances show.
    templates = bpy.data.collections.new("_MapModelTemplates")
    root.children.link(templates)
    context.view_layer.layer_collection.children[root.name].children[
        templates.name].exclude = True

    inst_col = bpy.data.collections.new("Models")
    root.children.link(inst_col)

    cache = {}          # model_path -> collection (or None if failed)
    placed = 0
    for e in entities:
        if models_max and placed >= models_max:
            break
        guid = (e.get('Prototype') or '').lower()
        model = model_index.get(guid)
        pos = e.get('Pos')
        if not model or not isinstance(pos, (list, tuple)) or len(pos) < 3:
            continue
        if model not in cache:
            srm_path = os.path.normpath(os.path.join(data_root, model))
            col = (_import_model_collection(context, srm_path, data_root,
                                            import_textures)
                   if os.path.isfile(srm_path) else None)
            if col is not None:
                # move template out of the scene view; keep as instance source
                try:
                    context.scene.collection.children.unlink(col)
                except Exception:
                    pass
                templates.children.link(col)
            cache[model] = col
        col = cache[model]
        if col is None:
            continue
        inst = bpy.data.objects.new("%s.%s" % (
            os.path.splitext(os.path.basename(model))[0], e.get('ID', '')), None)
        inst.instance_type = 'COLLECTION'
        inst.instance_collection = col
        inst.location = (float(pos[0]), float(pos[1]), float(pos[2]))
        d = e.get('Dir')
        if isinstance(d, (list, tuple)) and d:
            inst.rotation_euler = (0.0, 0.0, math.radians(float(d[0])))
        sc = e.get('Scale')
        if isinstance(sc, (int, float)) and sc:
            inst.scale = (float(sc), float(sc), float(sc))
        inst["cpcw_prototype"] = e.get('Prototype', '')
        inst["cpcw_model"] = model
        inst_col.objects.link(inst)
        placed += 1
    print("  placed %d model instances (%d unique models)" %
          (placed, sum(1 for v in cache.values() if v)))
    return placed


def load_map(context, filepath, place_entities=True, build_terrain=True,
             tint_passability=True, max_entities=0, place_models=False,
             data_root="", import_textures=True, models_max=0,
             use_heightmap=True, height_res=256, paint_splatmap=True):
    """Import a .map file: terrain + entity Empties. Returns the root collection."""
    mf = map_format.MapFile(filepath)
    world_w, world_h = _get_world_size(mf)
    resolved_root = _resolve_data_root(filepath, data_root)

    map_name = os.path.splitext(os.path.basename(filepath))[0]
    root = bpy.data.collections.new(f"Map_{map_name}")
    context.scene.collection.children.link(root)

    if build_terrain:
        terr_col = bpy.data.collections.new("Terrain")
        root.children.link(terr_col)
        _build_terrain(mf, terr_col, world_w, world_h, tint_passability,
                       use_heightmap=use_heightmap, height_res=height_res,
                       data_root=resolved_root, paint_splatmap=paint_splatmap)

    n_placed = 0
    if place_entities:
        entities = mf.get_entities()
        if max_entities and max_entities > 0:
            entities = entities[:max_entities]

        # sub-collections created lazily per descriptor type
        sub_cols = {}

        def col_for(name):
            if name not in sub_cols:
                c = bpy.data.collections.new(name)
                root.children.link(c)
                sub_cols[name] = c
            return sub_cols[name]

        for e in entities:
            pos = e.get('Pos')
            if not isinstance(pos, (list, tuple)) or len(pos) < 3:
                continue
            etype = e.get('_type', '?')
            col_name, display, size = _TYPE_STYLE.get(etype, _DEFAULT_STYLE)

            proto = e.get('Prototype', '')
            name = f"{etype}:{e.get('ID', '')}"

            obj = bpy.data.objects.new(name, None)
            obj.empty_display_type = display
            obj.empty_display_size = size
            obj.location = (float(pos[0]), float(pos[1]), float(pos[2]))

            direction = e.get('Dir')
            if isinstance(direction, (list, tuple)) and direction:
                obj.rotation_euler = (0.0, 0.0, math.radians(float(direction[0])))

            # game metadata as custom properties for browsing / future resolve
            obj["cpcw_type"] = etype
            if proto:
                obj["cpcw_prototype"] = proto
            if e.get('ID', '') != '':
                obj["cpcw_id"] = e.get('ID')
            if e.get('Player', '') != '':
                obj["cpcw_player"] = e.get('Player')

            col_for(col_name).objects.link(obj)
            n_placed += 1

    n_models = 0
    if place_models:
        entities = mf.get_entities()
        if max_entities and max_entities > 0:
            entities = entities[:max_entities]
        n_models = _place_models(context, mf, root, entities, data_root,
                                 import_textures, models_max)

    print(f"Imported map {filepath}: world {world_w:.0f}x{world_h:.0f}, "
          f"{n_placed} entities placed, {n_models} models")
    return root, n_placed


class IMPORT_OT_cpcw_map(bpy.types.Operator, ImportHelper):
    """Import a Codename: Panzers Cold War map (terrain extent + placed entities)"""
    bl_idname = "import_scene.cpcw_map"
    bl_label = "Import CPCW Map (.map)"
    bl_options = {'REGISTER', 'UNDO'}

    filename_ext = ".map"
    filter_glob: StringProperty(default="*.map", options={'HIDDEN'})

    build_terrain: BoolProperty(
        name="Build Terrain", default=True,
        description="Create a ground mesh over the world extent")
    use_heightmap: BoolProperty(
        name="Real Heightmap", default=True,
        description="Displace the terrain by the decoded GTRD elevation grid "
                    "(matches the in-game hills). If off, a flat plane is built")
    height_res: IntProperty(
        name="Terrain Resolution", default=256, min=16, max=1024,
        description="Max terrain subdivisions per axis when using the heightmap")
    paint_splatmap: BoolProperty(
        name="Paint Terrain", default=True,
        description="Tint the terrain per-vertex from the GTRD paint layers "
                    "(real .dds ground colours when the data root is found, "
                    "else a colour by ground type). Needs the real heightmap")
    tint_passability: BoolProperty(
        name="Tint Passability", default=False,
        description="Colour a flat terrain grid by the BLCK passability data "
                    "(green = passable, red = blocked). Ignored when the real "
                    "heightmap is used")
    place_entities: BoolProperty(
        name="Place Entity Markers", default=True,
        description="Add an Empty for each placed entity (unit/building/doodad)")
    max_entities: IntProperty(
        name="Max Entities", default=0, min=0,
        description="Limit number of entities placed (0 = all)")
    place_models: BoolProperty(
        name="Place Real Models", default=False,
        description="Resolve each entity's Prototype via ProtoDB and instance "
                    "its actual .srm model (needs the SRM add-on enabled and the "
                    "extracted data root). Slower, but matches the in-game look")
    data_root: StringProperty(
        name="Data Root", default="", subtype='DIR_PATH',
        description="Extracted game-data folder containing ProtoDB.bin and the "
                    "model folders (auto-detected from the .map path if blank)")
    import_textures: BoolProperty(
        name="Model Textures", default=True,
        description="Load DDS textures on the placed models")
    models_max: IntProperty(
        name="Max Models", default=0, min=0,
        description="Limit number of model instances placed (0 = all)")

    def execute(self, context):
        try:
            root, n = load_map(context, self.filepath,
                               place_entities=self.place_entities,
                               build_terrain=self.build_terrain,
                               tint_passability=self.tint_passability,
                               max_entities=self.max_entities,
                               place_models=self.place_models,
                               data_root=self.data_root,
                               import_textures=self.import_textures,
                               models_max=self.models_max,
                               use_heightmap=self.use_heightmap,
                               height_res=self.height_res,
                               paint_splatmap=self.paint_splatmap)
        except Exception as e:
            self.report({'ERROR'}, f"Map import failed: {e}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"Imported map: {n} entities")
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "build_terrain")
        if self.build_terrain:
            layout.prop(self, "use_heightmap")
            if self.use_heightmap:
                layout.prop(self, "height_res")
                layout.prop(self, "paint_splatmap")
            else:
                layout.prop(self, "tint_passability")
        layout.prop(self, "place_entities")
        if self.place_entities:
            layout.prop(self, "max_entities")
        layout.prop(self, "place_models")
        if self.place_models:
            layout.prop(self, "data_root")
            layout.prop(self, "import_textures")
            layout.prop(self, "models_max")


def menu_func_import(self, context):
    self.layout.operator(IMPORT_OT_cpcw_map.bl_idname, text="CPCW Map (.map)")


def register():
    bpy.utils.register_class(IMPORT_OT_cpcw_map)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.utils.unregister_class(IMPORT_OT_cpcw_map)
