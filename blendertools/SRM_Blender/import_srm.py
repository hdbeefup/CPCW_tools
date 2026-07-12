"""CPCW .srm model importer for Blender.

Builds native Blender meshes from a parsed SRM (see srm_format.py) and applies
materials with DDS textures (decoded by dds.py).

CPCW models are rigidly skinned: each mesh vertex is stored in the local space
of one skeleton bone (the bone-palette index is byte 3 of the normal stream) and
the skeleton is a node hierarchy (each node's parent is its `unk3`). To assemble
a model we compose each bone's world matrix up the parent chain and transform
every vertex by its bone's world matrix. The assembled model is then parented
under one root Empty that converts SRM's Y-up space to Blender's Z-up.
"""

import math
import os

import bpy
from bpy.props import StringProperty, FloatProperty, BoolProperty, EnumProperty
from bpy_extras.io_utils import ImportHelper
from mathutils import Matrix, Euler, Vector

from . import srm_format
from . import dds


# ---------------------------------------------------------------------------
# Textures / materials
# ---------------------------------------------------------------------------

def _find_texture(tex_name, search_dirs):
    """Locate a texture file by extension-less basename in the search dirs."""
    base = os.path.splitext(tex_name)[0]
    exts = ('.dds', '.DDS', '.tga', '.TGA', '.png', '.PNG')
    for d in search_dirs:
        if not d or not os.path.isdir(d):
            continue
        for ext in exts:
            p = os.path.join(d, base + ext)
            if os.path.isfile(p):
                return p
        # case-insensitive fallback
        try:
            low = base.lower()
            for fn in os.listdir(d):
                stem, ext = os.path.splitext(fn)
                if stem.lower() == low and ext.lower() in ('.dds', '.tga', '.png'):
                    return os.path.join(d, fn)
        except OSError:
            pass
    return None


def _load_image(path, cache):
    """Load an image file into a Blender image (cached). DDS via dds.py."""
    if path in cache:
        return cache[path]
    img = None
    ext = os.path.splitext(path)[1].lower()
    name = os.path.basename(path)
    try:
        if ext == '.dds':
            tex = dds.DDS.read(path)
            img = bpy.data.images.new(name, width=tex.width, height=tex.height,
                                      alpha=True)
            w, h, src = tex.width, tex.height, tex.rgba
            flat = [0.0] * (w * h * 4)
            inv = 1.0 / 255.0
            for y in range(h):
                dst_row = (h - 1 - y) * w        # flip vertically for Blender
                src_row = y * w
                for x in range(w):
                    s = (src_row + x) * 4
                    d = (dst_row + x) * 4
                    flat[d] = src[s] * inv
                    flat[d + 1] = src[s + 1] * inv
                    flat[d + 2] = src[s + 2] * inv
                    flat[d + 3] = src[s + 3] * inv
            img.pixels = flat
            img.update()
            img.pack()
        else:
            img = bpy.data.images.load(path)
            img.pack()
    except Exception as e:
        print(f"  WARNING: failed to load texture {path}: {e}")
        img = None
    cache[path] = img
    return img


def _pick_texture(textures, *slot_names):
    """Return the first non-empty texture among the given slot names."""
    for name in slot_names:
        val = textures.get(name)
        if val:
            return val
    return ''


def _make_material(submesh, search_dirs, import_textures, img_cache, mat_cache):
    """Create (or reuse) a Blender material for a submesh."""
    # Base color lives in DiffuseTexture or the DiffuseSpec variant.
    diffuse = _pick_texture(submesh.textures, 'DiffuseTexture', 'DiffuseSpecTexture')
    normal = _pick_texture(submesh.textures, 'NormalTexture')
    key = (submesh.material_name, diffuse, normal)
    if key in mat_cache:
        return mat_cache[key]

    mat_name = os.path.splitext(os.path.basename(diffuse))[0] or submesh.material_name \
        or 'cpcw_material'
    mat = bpy.data.materials.new(name=mat_name)
    mat.use_nodes = True
    if submesh.material_name:
        mat["cpcw_material"] = submesh.material_name
    for slot, tname in submesh.textures.items():
        mat[f"cpcw_tex_{slot}"] = tname

    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    principled = next((n for n in nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if principled is None:
        nodes.clear()
        principled = nodes.new('ShaderNodeBsdfPrincipled')
        out = nodes.new('ShaderNodeOutputMaterial')
        links.new(principled.outputs['BSDF'], out.inputs['Surface'])
    principled.inputs['Metallic'].default_value = 0.0
    principled.inputs['Roughness'].default_value = 0.7

    if import_textures:
        if diffuse:
            p = _find_texture(diffuse, search_dirs)
            if p:
                img = _load_image(p, img_cache)
                if img:
                    tex = nodes.new('ShaderNodeTexImage')
                    tex.image = img
                    tex.location = (-500, 300)
                    links.new(tex.outputs['Color'], principled.inputs['Base Color'])
                    # NOTE: the diffuse alpha channel holds specular/team-mask
                    # data, not opacity, so it is deliberately NOT wired to
                    # Principled Alpha (doing so makes surfaces see-through).
        if normal:
            p = _find_texture(normal, search_dirs)
            if p:
                img = _load_image(p, img_cache)
                if img:
                    img.colorspace_settings.name = 'Non-Color'
                    ntex = nodes.new('ShaderNodeTexImage')
                    ntex.image = img
                    ntex.location = (-500, -100)
                    nmap = nodes.new('ShaderNodeNormalMap')
                    nmap.location = (-200, -100)
                    links.new(ntex.outputs['Color'], nmap.inputs['Color'])
                    links.new(nmap.outputs['Normal'], principled.inputs['Normal'])

    mat_cache[key] = mat
    return mat


# ---------------------------------------------------------------------------
# Mesh building
# ---------------------------------------------------------------------------

def _node_local_matrix(node):
    """Local transform of a node (position / euler radians / scale).

    Rotation is composed Rx @ Ry @ Rz (this is what assembles the models;
    it is NOT the same as mathutils Euler('XYZ'), which is Rz @ Ry @ Rx).
    """
    rx, ry, rz = node.rotation
    R = (Matrix.Rotation(rx, 4, 'X') @
         Matrix.Rotation(ry, 4, 'Y') @
         Matrix.Rotation(rz, 4, 'Z'))
    T = Matrix.Translation(node.position)
    S = Matrix.Diagonal((node.scale[0], node.scale[1], node.scale[2], 1.0))
    return T @ R @ S


def _build_world_matrices(nodes):
    """World matrix per node, composed up the parent_idx (unk3) chain."""
    cache = {}

    def world(i):
        if i in cache:
            return cache[i]
        nd = nodes[i]
        lm = _node_local_matrix(nd)
        p = nd.parent_idx
        wm = (world(p) @ lm) if (0 <= p < len(nodes) and p != i) else lm
        cache[i] = wm
        return wm

    return [world(i) for i in range(len(nodes))]


def _compact_bone_groups(verts, bone_idx, ratio=0.85):
    """Decide which bone groups are compact enough to be skinned.

    Returns {bone_index: bool}. A group is skinned unless it spans almost the
    whole mesh: the *static body* of a model (a car body, a building shell) is
    one bone group that covers the entire mesh and is authored already-posed in
    model space, so skinning it would shift/explode it. Articulated parts
    (wheels, turret, running gear) each cover only part of the mesh and are
    stored bone-local, so they are skinned into place.
    """
    groups = {}
    for vi, bi in enumerate(bone_idx):
        groups.setdefault(bi, []).append(verts[vi])
    # whole-mesh diagonal
    xs = [p[0] for p in verts]; ys = [p[1] for p in verts]; zs = [p[2] for p in verts]
    mesh_diag = ((max(xs) - min(xs)) ** 2 + (max(ys) - min(ys)) ** 2 +
                 (max(zs) - min(zs)) ** 2) ** 0.5 or 1.0
    out = {}
    for bi, pts in groups.items():
        gx = [p[0] for p in pts]; gy = [p[1] for p in pts]; gz = [p[2] for p in pts]
        gdiag = ((max(gx) - min(gx)) ** 2 + (max(gy) - min(gy)) ** 2 +
                 (max(gz) - min(gz)) ** 2) ** 0.5
        out[bi] = (gdiag / mesh_diag) < ratio
    return out


def _build_mesh(node, name, nodes=None, world_mats=None, apply_skin=True):
    """Build a Blender mesh datablock from an SrmNode's mesh.

    CPCW meshes are rigidly skinned: each vertex carries a bone-palette index
    (byte 3 of the normal stream) and is stored in that bone's local space. To
    assemble the model we transform each vertex by its bone's world matrix
    (composed via the node parent hierarchy). Without this, parts collapse to
    the skeleton origin (wheels stacked at centre, etc.).
    """
    mesh = node.mesh
    pos_stream = mesh.stream_by_usage(srm_format.USAGE_POSITION)
    if pos_stream is None:
        return None

    verts = list(pos_stream.positions())
    idx = mesh.indices
    faces = [(idx[i], idx[i + 1], idx[i + 2]) for i in range(0, len(idx) - 2, 3)]

    # Per-vertex bone (palette index) for rigid skinning.
    bone_idx = None
    rot3 = None  # per-node 3x3 rotation for skinning normals
    skin_group = None
    if apply_skin != 'NONE' and nodes is not None and world_mats is not None:
        bone_idx = mesh.vertex_bone_indices()
    if bone_idx is not None:
        palette = mesh.bones
        rot3 = [wm.to_3x3() for wm in world_mats]
        # A CPCW mesh may MIX model-space geometry (a static car body, already
        # posed) with bone-local parts (wheels stored at the origin, positioned
        # by their bone). In 'PARTS' mode we skin only the compact parts and
        # leave the one whole-mesh-spanning body group in place; in 'FULL' mode
        # every group is skinned (correct for fully-articulated models where
        # even the body is bone-local). The two cases are geometrically
        # indistinguishable, hence the user-selectable mode.
        if apply_skin == 'PARTS':
            skin_group = _compact_bone_groups(verts, bone_idx)
        else:
            skin_group = {bi: True for bi in set(bone_idx)}
        skinned = []
        for vi, p in enumerate(verts):
            bi = bone_idx[vi]
            if (skin_group.get(bi) and 0 <= bi < len(palette)
                    and 0 <= palette[bi] < len(world_mats)):
                v = world_mats[palette[bi]] @ Vector(p)
                skinned.append((v.x, v.y, v.z))
            else:
                skinned.append(p)
        verts = skinned

    bl_mesh = bpy.data.meshes.new(name)
    bl_mesh.from_pydata(verts, [], faces)
    bl_mesh.update()

    # UVs (per-loop, V flipped)
    uv_stream = mesh.stream_by_usage(srm_format.USAGE_TEXCOORD)
    if uv_stream is not None and bl_mesh.polygons:
        uvs = list(uv_stream.uvs())
        uv_layer = bl_mesh.uv_layers.new(name="UVMap")
        for loop in bl_mesh.loops:
            vi = loop.vertex_index
            if vi < len(uvs):
                u, v = uvs[vi]
                uv_layer.data[loop.index].uv = (u, 1.0 - v)

    # Custom split normals (rotated by the bone if skinned)
    norm_stream = mesh.stream_by_usage(srm_format.USAGE_NORMAL)
    if norm_stream is not None and bl_mesh.polygons:
        normals = list(norm_stream.normals())
        if bone_idx is not None and rot3 is not None:
            palette = mesh.bones
            for vi in range(min(len(normals), len(bone_idx))):
                bi = bone_idx[vi]
                if (skin_group and skin_group.get(bi)
                        and 0 <= bi < len(palette) and 0 <= palette[bi] < len(rot3)):
                    nrm = rot3[palette[bi]] @ Vector(normals[vi])
                    normals[vi] = (nrm.x, nrm.y, nrm.z)
        if len(normals) >= len(verts):
            loop_normals = []
            for loop in bl_mesh.loops:
                vi = loop.vertex_index
                loop_normals.append(normals[vi] if vi < len(normals) else (0.0, 0.0, 1.0))
            try:
                bl_mesh.normals_split_custom_set(loop_normals)
            except (AttributeError, RuntimeError):
                pass  # Blender 4.1+ removed this; auto normals are fine for preview

    return bl_mesh


def load_srm(context, filepath, scale=1.0, import_textures=True, texture_dir="",
             apply_skin='FULL', show_skeleton=False):
    """Import an SRM file. apply_skin is 'FULL', 'PARTS' or 'NONE'."""
    nodes = srm_format.parse_srm(filepath)
    world_mats = _build_world_matrices(nodes)

    model_dir = os.path.dirname(os.path.abspath(filepath))
    search_dirs = [model_dir]
    if texture_dir:
        search_dirs.append(bpy.path.abspath(texture_dir))

    model_name = os.path.splitext(os.path.basename(filepath))[0]
    collection = bpy.data.collections.new(model_name)
    context.scene.collection.children.link(collection)

    # Root Empty: SRM (Y-up) -> Blender (Z-up) + uniform scale.
    root = bpy.data.objects.new(model_name, None)
    root.empty_display_type = 'PLAIN_AXES'
    root.empty_display_size = 0.5
    root.matrix_basis = Matrix.Rotation(math.radians(90.0), 4, 'X') @ Matrix.Scale(scale, 4)
    # Stash the source path so the exporter can round-trip from the pristine
    # file (see export_srm.py) and record the assemble mode used.
    root["cpcw_srm_source"] = os.path.abspath(filepath)
    root["cpcw_assemble"] = apply_skin
    collection.objects.link(root)

    img_cache = {}
    mat_cache = {}
    created = []

    for i, node in enumerate(nodes):
        obj_name = node.name or f"node_{i}"

        if node.mesh:
            bl_mesh = _build_mesh(node, obj_name, nodes, world_mats, apply_skin)
            obj = bpy.data.objects.new(obj_name, bl_mesh)
            for sm in node.mesh.submeshes:
                obj.data.materials.append(
                    _make_material(sm, search_dirs, import_textures,
                                   img_cache, mat_cache))
            # With no skinning, place the mesh at its node's world transform;
            # skinned vertices are already baked into model space (identity).
            if apply_skin == 'NONE':
                obj.matrix_basis = world_mats[i]
        else:
            if not show_skeleton:
                continue
            obj = bpy.data.objects.new(obj_name, None)
            obj.empty_display_type = 'ARROWS'
            obj.empty_display_size = 0.1
            # place skeleton bone at its world transform (for reference)
            obj.matrix_basis = world_mats[i]

        obj["cpcw_node_index"] = i
        obj["cpcw_parent"] = node.parent_idx
        collection.objects.link(obj)
        obj.parent = root
        obj.matrix_parent_inverse = Matrix.Identity(4)
        created.append(obj)

    print(f"Imported {filepath}: {len(nodes)} nodes, "
          f"{sum(1 for o in created if o.type == 'MESH')} meshes, skin={apply_skin}")
    return root


# ---------------------------------------------------------------------------
# Operator
# ---------------------------------------------------------------------------

class IMPORT_OT_cpcw_srm(bpy.types.Operator, ImportHelper):
    """Import a Codename: Panzers Cold War model"""
    bl_idname = "import_scene.cpcw_srm"
    bl_label = "Import CPCW Model (.srm)"
    bl_options = {'REGISTER', 'UNDO', 'PRESET'}

    filename_ext = ".srm"
    filter_glob: StringProperty(default="*.srm", options={'HIDDEN'})

    scale: FloatProperty(
        name="Scale",
        description="Uniform scale applied to the imported model",
        default=1.0, min=0.0001, max=10000.0, soft_min=0.001, soft_max=100.0,
    )
    import_textures: BoolProperty(
        name="Import Textures",
        description="Decode and load DDS textures referenced by the model",
        default=True,
    )
    texture_dir: StringProperty(
        name="Extra Texture Dir",
        description="Additional directory to search for textures "
                    "(e.g. the extracted game data root)",
        default="", subtype='DIR_PATH',
    )
    apply_skin: EnumProperty(
        name="Assemble",
        description="How to assemble the model from its skeleton",
        items=[
            ('FULL', "Full (articulated)",
             "Skin every part by its bone. Correct for fully-articulated models "
             "(tanks, tracked vehicles) where the whole model is bone-local"),
            ('PARTS', "Parts only (static body)",
             "Skin only the small parts (wheels, etc.) and leave the one large "
             "body group in place. Use for cars/models whose body shifts or "
             "whose wheels float in Full mode"),
            ('NONE', "Off (raw bind pose)",
             "No skinning. Correct for static models (buildings) which are "
             "authored already-posed"),
        ],
        default='FULL',
    )
    show_skeleton: BoolProperty(
        name="Show Skeleton",
        description="Also create an Empty at each skeleton bone (attach points)",
        default=False,
    )

    def execute(self, context):
        try:
            root = load_srm(context, self.filepath, self.scale,
                            self.import_textures, self.texture_dir,
                            self.apply_skin, self.show_skeleton)
        except Exception as e:
            self.report({'ERROR'}, f"SRM import failed: {e}")
            return {'CANCELLED'}
        bpy.ops.object.select_all(action='DESELECT')
        root.select_set(True)
        context.view_layer.objects.active = root
        _set_material_shading(context)
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "scale")
        layout.prop(self, "apply_skin")
        layout.prop(self, "show_skeleton")
        layout.prop(self, "import_textures")
        if self.import_textures:
            layout.prop(self, "texture_dir")


def _set_material_shading(context):
    try:
        for area in context.screen.areas:
            if area.type == 'VIEW_3D':
                for space in area.spaces:
                    if space.type == 'VIEW_3D':
                        space.shading.type = 'MATERIAL'
                        return
    except Exception:
        pass


def menu_func_import(self, context):
    self.layout.operator(IMPORT_OT_cpcw_srm.bl_idname, text="CPCW Model (.srm)")


def register():
    bpy.utils.register_class(IMPORT_OT_cpcw_srm)
    bpy.types.TOPBAR_MT_file_import.append(menu_func_import)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_func_import)
    bpy.utils.unregister_class(IMPORT_OT_cpcw_srm)
