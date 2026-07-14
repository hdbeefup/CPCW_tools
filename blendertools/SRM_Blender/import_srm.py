"""CPCW .srm model importer for Blender.

Builds native Blender meshes from a parsed SRM (see srm_format.py) and applies
materials with DDS textures (decoded by dds.py).

CPCW models are rigidly skinned: each mesh vertex is stored in the local space
of one skeleton bone (the bone-palette index is byte 3 of the normal stream) and
the skeleton is a node hierarchy (each node's parent is its `unk3`). Assembly is
two steps: (1) transform each vertex by its bone's composed world matrix -- the
game's proven rule is exactly `v_world = BoneWorld[node] @ v_stored`, no
inverse-bind, where `node` comes from the bone-palette compaction (see
`srm_format.bone_node_list`); then
(2) convert SRM's DirectX LEFT-handed Y-up frame to Blender's right-handed Z-up
by BAKING a reflection into the geometry -- swap (x,y,z)->(x,z,y) and reverse
triangle winding (see `_HAND`). The old importer used a pure rotation Rx(90) for
step 2, which is determinant +1 and therefore left every model MIRRORED.
"""

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

_TEX_INDEX = {}  # dir -> {basename_lower: path}, recursive, cached


def _tex_index(d):
    """Recursive basename -> path index of a directory tree (cached)."""
    if d in _TEX_INDEX:
        return _TEX_INDEX[d]
    idx = {}
    try:
        for dp, _dirs, fns in os.walk(d):
            for fn in fns:
                stem, ext = os.path.splitext(fn)
                if ext.lower() in ('.dds', '.tga', '.png'):
                    idx.setdefault(stem.lower(), os.path.join(dp, fn))
    except OSError:
        pass
    _TEX_INDEX[d] = idx
    return idx


def _find_texture(tex_name, search_dirs):
    """Locate a texture file by extension-less basename in the search dirs.

    Checks each dir directly first (a texture next to the .srm wins), then falls
    back to a recursive basename index of each dir -- game models routinely
    reference a shared texture that lives in a sibling folder (e.g. SU_T-54.dds
    under Vehicles/SovietAdditional while the .srm is in Vehicles/Soviet).
    """
    base = os.path.splitext(tex_name)[0]
    exts = ('.dds', '.DDS', '.tga', '.TGA', '.png', '.PNG')
    for d in search_dirs:
        if not d or not os.path.isdir(d):
            continue
        for ext in exts:
            p = os.path.join(d, base + ext)
            if os.path.isfile(p):
                return p
        try:
            low = base.lower()
            for fn in os.listdir(d):
                stem, ext = os.path.splitext(fn)
                if stem.lower() == low and ext.lower() in ('.dds', '.tga', '.png'):
                    return os.path.join(d, fn)
        except OSError:
            pass
    # recursive fallback: shared texture in a sibling folder under the data root
    base_low = os.path.splitext(tex_name)[0].lower()
    for d in search_dirs:
        if not d or not os.path.isdir(d):
            continue
        p = _tex_index(d).get(base_low)
        if p:
            return p
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
# Handedness
# ---------------------------------------------------------------------------
#
# SRM uses DirectX's LEFT-handed, Y-up frame; Blender is RIGHT-handed, Z-up.
# Converting between them needs a REFLECTION (determinant -1), not merely a
# rotation.  The old importer used Matrix.Rotation(90,'X') (determinant +1) as
# the root transform: that maps Y-up->Z-up but PRESERVES the left-handedness,
# so every model came in MIRRORED (the train's "DEUTSCHE" read "EHCSTUED" from
# outside, symmetric hulls hid it, and the ka_15's one-sided canopy exposed it).
#
# The correct map is the axis swap (x, y, z) -> (x, z, y): it carries Y-up->Z-up
# AND flips handedness (it is a single transposition, determinant -1).  We BAKE
# it into geometry (swap every vertex/normal component and REVERSE triangle
# winding so faces stay outward) rather than hang a negative-determinant matrix
# on the root Empty -- baked geometry keeps each object at positive scale so
# Blender shades it right-side-out.  Node/bone matrices are conjugated
# (P @ M @ P) so the skeleton still assembles and stays a proper rotation.
_HAND = Matrix(((1, 0, 0, 0),
                (0, 0, 1, 0),
                (0, 1, 0, 0),
                (0, 0, 0, 1)))  # swap Y/Z, determinant -1


def _swap_pt(p):
    """Bake the LH->RH axis swap into a point: (x, y, z) -> (x, z, y)."""
    return (p[0], p[2], p[1])


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


# Upgrade-variant convention (baked into node names by the authoring tool):
# vehicles pack every loadout in one .srm. A part's variant is read from the
# name of the bone it is skinned to -- the ``_std`` / ``_upg`` SUFFIX marks the
# standard vs upgraded loadout. The upgrade's camouflage-net parts are named
# ``camo_..._upg`` (they always carry ``_upg``), so they fall under UPGRADED
# without a separate rule -- and, crucially, so a model whose *own name* contains
# "camo" (e.g. ``camo_tent``, not an upgrade) is NOT mis-filtered. Untagged bones
# are the always-present base hull.
_VARIANT_KEEP = {
    'STANDARD': frozenset(('BASE', 'STD')),
    'UPGRADED': frozenset(('BASE', 'UPG')),
}


def _variant_tag(name):
    n = name.lower()
    if '_upg' in n:
        return 'UPG'
    if '_std' in n:
        return 'STD'
    return 'BASE'


def _variant_vertex_keep(mesh, nodes, variant):
    """Per-vertex keep mask for the selected upgrade variant, or None.

    Returns None when the mesh is unskinned or carries no variant parts (the
    common case -- most models are entirely 'BASE', so filtering is a no-op).
    """
    keep_tags = _VARIANT_KEEP.get(variant)
    if keep_tags is None or nodes is None:
        return None
    bone_idx = mesh.vertex_bone_indices()
    if not bone_idx:
        return None
    palette = mesh.bones
    bnl = srm_format.bone_node_list(nodes)
    tag_by_bi = {}
    for bi in set(bone_idx):
        v = palette[bi] if 0 <= bi < len(palette) else -1
        ni = bnl[v] if 0 <= v < len(bnl) else -1
        nm = nodes[ni].name if 0 <= ni < len(nodes) else ''
        tag_by_bi[bi] = _variant_tag(nm)
    if all(t == 'BASE' for t in tag_by_bi.values()):
        return None  # nothing to filter
    return [tag_by_bi[bi] in keep_tags for bi in bone_idx]


def _mesh_is_smooth(mesh):
    """True if the mesh is smooth-skinned (carries a BLENDINDICES stream).

    Smooth meshes (characters, animals) store their vertices in MODEL space and
    must be rendered in the bind pose; rigid meshes instead carry a single
    bone-palette index in NORMAL byte3 and are assembled by skinning.
    """
    return mesh.stream_by_usage(srm_format.USAGE_BLENDINDICES) is not None


def _build_mesh(node, name, nodes=None, world_mats=None, apply_skin='FULL',
                variant='ALL'):
    """Build a Blender mesh datablock from an SrmNode's mesh.

    CPCW meshes are rigidly skinned: each vertex carries a bone-palette index
    (byte 3 of the normal stream).  We (1) assemble in SRM space by transforming
    each vertex by its bone's world matrix -- the exact engine rule
    ``v_world = BoneWorld[node] @ v`` where ``node`` comes from the palette
    compaction (:func:`srm_format.bone_node_list`) -- then (2) BAKE the LH->RH
    handedness swap into the result: swap each position/normal component
    (x,y,z)->(x,z,y) and REVERSE triangle winding so faces stay outward (see
    :data:`_HAND`).

    ``apply_skin``:  'FULL' = skin every rigid vertex (default);  'NONE' = raw
    bind pose, no skinning (mesh is placed by its node's world matrix in
    :func:`load_srm`).
    """
    mesh = node.mesh
    pos_stream = mesh.stream_by_usage(srm_format.USAGE_POSITION)
    if pos_stream is None:
        return None

    verts = list(pos_stream.positions())
    idx = mesh.indices
    # Winding REVERSED (v0, v2, v1): the handedness swap is a reflection, which
    # flips triangle orientation; reversing restores outward-facing normals.
    faces = [(idx[i], idx[i + 2], idx[i + 1]) for i in range(0, len(idx) - 2, 3)]

    # Upgrade-variant filter: drop faces whose vertices belong to an excluded
    # loadout (e.g. show the STANDARD tank without the upgraded gun / camo net).
    # A variant part is a separate geometry island, so all 3 verts share a tag.
    if variant != 'ALL':
        keep = _variant_vertex_keep(mesh, nodes, variant)
        if keep is not None:
            faces = [f for f in faces if keep[f[0]] and keep[f[1]] and keep[f[2]]]

    norm_stream = mesh.stream_by_usage(srm_format.USAGE_NORMAL)
    normals = list(norm_stream.normals()) if norm_stream is not None else None

    # (1) Assemble in SRM space (skip in NONE; the object matrix places it).
    # SMOOTH-skinned meshes (characters/animals -- a BLENDINDICES stream) store
    # their vertices in MODEL space; the engine's per-bone skin matrix is
    # boneWorld @ inverseBind, which at the static rest pose is IDENTITY, so the
    # faithful result is the BIND POSE. (Applying a rigid bone transform mangles
    # them: their NORMAL byte3 is 0, so the old code skinned every vertex to
    # palette[0].)  We treat them exactly like NONE mode -- leave the verts in
    # model space here and place the object by its node's world matrix in
    # :func:`load_srm` (see ``_mesh_is_smooth``).  Rigid meshes (bone-local
    # verts, no BLENDINDICES) still skin normally.
    is_smooth = _mesh_is_smooth(mesh)
    bone_idx = None
    if apply_skin != 'NONE' and not is_smooth and nodes is not None and world_mats is not None:
        bone_idx = mesh.vertex_bone_indices()
    if bone_idx is not None:
        palette = mesh.bones
        # Remap each palette value to its real node (the compaction) then skin
        # every rigid vertex by that node's world matrix -- the exact engine rule.
        bnl = srm_format.bone_node_list(nodes)
        node_of_local = [bnl[v] if 0 <= v < len(bnl) else -1 for v in palette]
        rot3 = [wm.to_3x3() for wm in world_mats]
        out_v = []
        out_n = [] if normals is not None else None
        for vi, p in enumerate(verts):
            bi = bone_idx[vi]
            bn = node_of_local[bi] if 0 <= bi < len(node_of_local) else -1
            if 0 <= bn < len(world_mats):
                v = world_mats[bn] @ Vector(p)
                out_v.append((v.x, v.y, v.z))
                if out_n is not None and vi < len(normals):
                    nn = rot3[bn] @ Vector(normals[vi])
                    out_n.append((nn.x, nn.y, nn.z))
                elif out_n is not None:
                    out_n.append((0.0, 0.0, 1.0))
            else:
                out_v.append(p)
                if out_n is not None:
                    out_n.append(normals[vi] if vi < len(normals) else (0.0, 0.0, 1.0))
        verts = out_v
        normals = out_n if out_n is not None else normals

    # (2) Bake the LH->RH handedness swap into positions and normals.
    verts = [_swap_pt(p) for p in verts]
    if normals is not None:
        normals = [_swap_pt(n) for n in normals]

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

    # Custom split normals (already assembled + swapped above)
    if normals is not None and bl_mesh.polygons and len(normals) >= len(verts):
        loop_normals = []
        for loop in bl_mesh.loops:
            vi = loop.vertex_index
            loop_normals.append(normals[vi] if vi < len(normals) else (0.0, 0.0, 1.0))
        try:
            bl_mesh.normals_split_custom_set(loop_normals)
        except (AttributeError, RuntimeError):
            pass  # Blender 4.1+ removed this; auto normals are fine for preview

    return bl_mesh


def _assign_bone_groups(obj, srm_mesh, nodes):
    """Expose the mesh's rigid skinning as Blender vertex groups.

    Each vertex is rigidly bound to one bone (palette index = NORMAL byte 3);
    create one vertex group per palette entry, named after its node, and assign
    each vertex (weight 1.0). The Blender vertex order matches the SRM order
    (``from_pydata`` preserves it), so group membership round-trips. A
    ``cpcw_bone_map`` custom prop records group-name -> node-index so the exporter
    can turn edited/new groups back into a bone palette. Enables authoring: a
    modder assigns new geometry to a bone's group and exports it.
    """
    bidx = srm_mesh.vertex_bone_indices()
    if not bidx:
        return
    palette = srm_mesh.bones
    if len(bidx) != len(obj.data.vertices):
        return  # vertex counts must line up to map groups safely
    # Groups are NAMED after the real (remapped) node, but cpcw_bone_map records
    # the RAW palette value V so the exporter reconstructs mesh.bones verbatim
    # (byte-faithful round-trip). Only the display name changes.
    bnl = srm_format.bone_node_list(nodes)
    group_of_pi = {}
    name_map = {}
    used = set()
    for pi, V in enumerate(palette):
        nidx = bnl[V] if 0 <= V < len(bnl) else -1
        base = (nodes[nidx].name if 0 <= nidx < len(nodes) and nodes[nidx].name
                else "bone_%d" % pi)
        nm = base
        k = 1
        while nm in used:
            nm = "%s.%03d" % (base, k); k += 1
        used.add(nm)
        group_of_pi[pi] = obj.vertex_groups.new(name=nm)
        name_map[nm] = V
    buckets = {}
    for vi, bi in enumerate(bidx):
        if 0 <= bi < len(palette):
            buckets.setdefault(bi, []).append(vi)
    for pi, verts in buckets.items():
        group_of_pi[pi].add(verts, 1.0, 'REPLACE')
    obj["cpcw_bone_map"] = ";".join("%s=%d" % (k, v) for k, v in name_map.items())


def load_srm(context, filepath, scale=1.0, import_textures=True, texture_dir="",
             apply_skin='FULL', show_skeleton=False, variant='STANDARD',
             make_vertex_groups=True):
    """Import an SRM file. apply_skin is 'FULL' or 'NONE'.

    ``variant`` selects the upgrade loadout for vehicles that pack several into
    one .srm: 'STANDARD' (base + std parts, the clean default), 'UPGRADED'
    (base + upgraded parts + camo net), or 'ALL' (every variant, may overlap).
    Filtering is display-only; export round-trips from the pristine source.
    """
    nodes = srm_format.parse_srm(filepath)
    world_mats = _build_world_matrices(nodes)
    # Node/bone world matrices expressed in Blender space: conjugate by the
    # handedness swap (P @ M @ P, P == P**-1) so Empties and NONE-mode meshes
    # sit consistently with the baked geometry and stay proper rotations.
    world_mats_bl = [_HAND @ wm @ _HAND for wm in world_mats]

    model_dir = os.path.dirname(os.path.abspath(filepath))
    search_dirs = [model_dir]
    if texture_dir:
        search_dirs.append(bpy.path.abspath(texture_dir))

    model_name = os.path.splitext(os.path.basename(filepath))[0]
    collection = bpy.data.collections.new(model_name)
    context.scene.collection.children.link(collection)

    # Root Empty: handedness (Y-up LH -> Z-up RH) is baked into the geometry, so
    # the root only carries the user's uniform scale -- no rotation, no negative
    # determinant (which would flip child normals inside-out).
    root = bpy.data.objects.new(model_name, None)
    root.empty_display_type = 'PLAIN_AXES'
    root.empty_display_size = 0.5
    root.matrix_basis = Matrix.Scale(scale, 4)
    # Stash the source path so the exporter can round-trip from the pristine
    # file (see export_srm.py) and record the assemble mode used.
    root["cpcw_srm_source"] = os.path.abspath(filepath)
    root["cpcw_assemble"] = apply_skin
    root["cpcw_variant"] = variant
    collection.objects.link(root)

    img_cache = {}
    mat_cache = {}
    created = []

    for i, node in enumerate(nodes):
        obj_name = node.name or f"node_{i}"

        if node.mesh:
            bl_mesh = _build_mesh(node, obj_name, nodes, world_mats, apply_skin,
                                  variant)
            # Whole mesh filtered out by the variant selection (e.g. an all-camo
            # merged mesh under STANDARD): drop it rather than leave loose verts.
            if bl_mesh is not None and variant != 'ALL' and not bl_mesh.polygons:
                bpy.data.meshes.remove(bl_mesh)
                continue
            obj = bpy.data.objects.new(obj_name, bl_mesh)
            for sm in node.mesh.submeshes:
                obj.data.materials.append(
                    _make_material(sm, search_dirs, import_textures,
                                   img_cache, mat_cache))
            # Place by the node's (Blender-space) world transform when the mesh
            # is NOT assembled here: NONE mode (nothing skinned) OR a smooth mesh
            # (model-space verts left in bind pose by _build_mesh). Skinned
            # (rigid) meshes already bake world+swap into the geometry, so they
            # stay at identity.
            if apply_skin == 'NONE' or _mesh_is_smooth(node.mesh):
                obj.matrix_basis = world_mats_bl[i]
            if make_vertex_groups:
                _assign_bone_groups(obj, node.mesh, nodes)
        else:
            if not show_skeleton:
                continue
            obj = bpy.data.objects.new(obj_name, None)
            obj.empty_display_type = 'ARROWS'
            obj.empty_display_size = 0.1
            # place skeleton bone at its (Blender-space) world transform
            obj.matrix_basis = world_mats_bl[i]

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
            ('FULL', "Full (game's exact rule)",
             "Apply the engine's proven transform to every vertex: "
             "world = BoneWorld[node] @ v, where node comes from the bone-palette "
             "compaction (no inverse-bind; verified in the binary). Exact for "
             "non-animated bones (car bodies/wheels, gun, turret, window frames); "
             "genuinely animated-bone-bound parts (tank road wheels, suspension "
             "travel) render in their static, un-settled pose"),
            ('NONE', "Off (raw bind pose)",
             "No skinning; each mesh placed by its node's world matrix only"),
        ],
        default='FULL',
    )
    variant: EnumProperty(
        name="Variant",
        description="Which upgrade loadout to show for vehicles that pack "
                    "several into one file (chosen per-vertex by the bone each "
                    "part is skinned to). Non-variant models are unaffected",
        items=[
            ('STANDARD', "Standard",
             "Base hull plus the standard parts; hides the upgraded gun/parts "
             "and the camo net (the clean default)"),
            ('UPGRADED', "Upgraded",
             "Base hull plus the upgraded parts and camo net; hides the "
             "standard parts"),
            ('ALL', "All (raw)",
             "Every variant at once (standard + upgraded + camo may overlap "
             "at attach points). Faithful to the file's full contents"),
        ],
        default='STANDARD',
    )
    show_skeleton: BoolProperty(
        name="Show Skeleton",
        description="Also create an Empty at each skeleton bone (attach points)",
        default=False,
    )
    make_vertex_groups: BoolProperty(
        name="Bone Vertex Groups",
        description="Add a vertex group per bone (named after its node) holding "
                    "each vertex's rigid binding. Lets you re-bind or author new "
                    "geometry and export it back (see the exporter's geometry "
                    "write-back)",
        default=True,
    )

    def execute(self, context):
        try:
            root = load_srm(context, self.filepath, self.scale,
                            self.import_textures, self.texture_dir,
                            self.apply_skin, self.show_skeleton, self.variant,
                            self.make_vertex_groups)
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
        layout.prop(self, "variant")
        layout.prop(self, "show_skeleton")
        layout.prop(self, "make_vertex_groups")
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
