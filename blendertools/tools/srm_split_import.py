"""Import a CPCW .srm with each skinning part as a SEPARATE, movable object.

Paste into Blender's Scripting tab and press Run (the SRM_Blender add-on must be
installed so its parser/materials are importable). Every bone group becomes its
own textured object named `m<mesh>_b<bone>_<bonename>`, positioned at its
current (Full-skinning) place. Move the misplaced parts (wheels, speaker...) to
where they belong, then run dump_scene.py to record the corrected positions.

Edit SRM and DATA below for a different model.
"""

import math
from collections import defaultdict

import bpy
from mathutils import Matrix, Vector

from SRM_Blender import srm_format, import_srm

SRM = r"N:\gamePAKdata\CPCWPak\Vehicles\Civilian\moskvitch401_speaker.srm"
DATA = r"N:\gamePAKdata\CPCWPak"           # extra texture search dir
PLACE_SKINNED = True                        # True: start at Full-skinning pose
IMPORT_TEXTURES = True

nodes = srm_format.parse_srm(SRM)
world_mats = import_srm._build_world_matrices(nodes)

col = bpy.data.collections.new("srm_split")
bpy.context.scene.collection.children.link(col)

# Upright root: SRM Y-up -> Blender Z-up. Children keep SRM-native coords in
# their matrix_basis, so a dump of matrix_basis maps straight back to the file.
root = bpy.data.objects.new("root_Yup_to_Zup", None)
root.matrix_basis = Matrix.Rotation(math.radians(90.0), 4, 'X')
col.objects.link(root)

img_cache, mat_cache = {}, {}
search_dirs = [DATA]

for mi, node in enumerate(nodes):
    if not node.mesh:
        continue
    mesh = node.mesh
    pos = list(mesh.stream(srm_format.SEM_POSITION).positions())
    idx = mesh.indices
    pal = mesh.bones
    bidx = mesh.vertex_bone_indices()
    if bidx is None:
        bidx = [0] * len(pos)
        pal = [mi]

    # group triangles by the bone of their first vertex
    groups = defaultdict(lambda: {"vmap": {}, "faces": []})
    for t in range(0, len(idx) - 2, 3):
        a, b, c = idx[t], idx[t + 1], idx[t + 2]
        g = groups[bidx[a]]
        tri = []
        for v in (a, b, c):
            if v not in g["vmap"]:
                g["vmap"][v] = len(g["vmap"])
            tri.append(g["vmap"][v])
        g["faces"].append(tuple(tri))

    mat = None
    if mesh.submeshes:
        mat = import_srm._make_material(mesh.submeshes[0], search_dirs,
                                        IMPORT_TEXTURES, img_cache, mat_cache)

    for bi, g in groups.items():
        inv = sorted(g["vmap"], key=lambda k: g["vmap"][k])
        node_i = pal[bi] if bi < len(pal) else -1
        wm = (world_mats[node_i] if (PLACE_SKINNED and 0 <= node_i < len(world_mats))
              else Matrix.Identity(4))
        wverts = [wm @ Vector(pos[v]) for v in inv]
        cen = sum(wverts, Vector((0, 0, 0))) / len(wverts)
        local = [tuple(v - cen) for v in wverts]

        me = bpy.data.meshes.new(f"m{mi}_b{bi}")
        me.from_pydata(local, [], g["faces"])
        me.update()
        # UVs
        uvs = list(mesh.stream(srm_format.SEM_TEXCOORD).uvs()) \
            if mesh.stream(srm_format.SEM_TEXCOORD) else None
        if uvs and me.polygons:
            uvl = me.uv_layers.new(name="UVMap")
            for loop in me.loops:
                ov = inv[loop.vertex_index]
                if ov < len(uvs):
                    u, v = uvs[ov]
                    uvl.data[loop.index].uv = (u, 1.0 - v)

        bonename = nodes[node_i].name if 0 <= node_i < len(nodes) else "?"
        ob = bpy.data.objects.new(f"m{mi}_b{bi}_{bonename}", me)
        ob.location = cen
        ob["srm_mesh"] = mi
        ob["srm_bone"] = bi
        ob["srm_bone_node"] = node_i
        ob["srm_bone_name"] = bonename
        if mat:
            ob.data.materials.append(mat)
        col.objects.link(ob)
        ob.parent = root
        ob.matrix_parent_inverse = Matrix.Identity(4)

# material-preview shading so textures show
for area in bpy.context.screen.areas:
    if area.type == 'VIEW_3D':
        for sp in area.spaces:
            if sp.type == 'VIEW_3D':
                sp.shading.type = 'MATERIAL'

print(f"Split-imported {SRM}: "
      f"{sum(1 for o in col.objects if o.type == 'MESH')} part objects")
