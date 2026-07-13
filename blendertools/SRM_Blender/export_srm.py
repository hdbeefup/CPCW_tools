"""CPCW .srm exporter for Blender.

The exporter is built on the byte-faithful round-trip model in ``srm_writer``.
On import every model stashes the absolute path of its source .srm on the root
Empty (``cpcw_srm_source``) and every object records its node index
(``cpcw_node_index``). On export we re-read that pristine source model and then
apply the edits Blender can express safely:

* **node transforms** — position / rotation / scale of a node, recovered from
  the matching object and written back into the node header. This is exact for
  models imported with *Assemble = Off* or *Show Skeleton* (where each object
  sits at its node's transform); for baked-skin imports only nodes whose object
  still carries an identifiable local transform are updated.
* **texture names** — edited ``cpcw_tex_<slot>`` custom properties on materials
  are written back into the material trailer.

Geometry itself is preserved from the source, so "import a game asset then
export it" is guaranteed not to corrupt the file. Authoring brand-new geometry
(rebuilding VERS/INDS/BONE from a Blender mesh) is a separate, larger feature;
the format needed for it is fully documented in ``srm_writer`` and FORMAT_SRM.md.
"""

import os
import struct

import bpy
from bpy.props import StringProperty, BoolProperty
from bpy_extras.io_utils import ExportHelper
from mathutils import Matrix

from . import srm_writer

# Same LH<->RH swap the importer bakes (see import_srm._HAND): (x,y,z)<->(x,z,y).
# It is its own inverse, so conjugating an object's Blender-space matrix by it
# (P @ M @ P) recovers the SRM-space matrix the node header stores.
_HAND = Matrix(((1, 0, 0, 0),
                (0, 0, 1, 0),
                (0, 1, 0, 0),
                (0, 0, 0, 1)))


def _unswap(co):
    """Un-bake the LH->RH handedness swap on a position (its own inverse)."""
    return (co[0], co[2], co[1])


def _geometry_writeback(root, pmod):
    """Rewrite each mesh node's POSITION stream from the (edited) Blender mesh.

    Only valid for a model imported with Assemble='NONE' (mesh local coords are
    then exactly ``swap(stored_pos)``, so un-swapping recovers the stored values;
    AUTO/FULL bake a bone-world transform in and cannot be inverted here). Only
    the POSITION stream is touched -- indices, UVs, normals and the bone palette
    are kept -- so a no-edit export stays byte-identical and vertex reshaping is
    captured. A mesh whose vertex count no longer matches the source (topology
    changed) is skipped, not corrupted.

    Returns (written, skipped) where skipped is a list of (name, reason).
    """
    written = 0
    skipped = []
    kids = root.children_recursive if hasattr(root, 'children_recursive') else []
    for o in kids:
        if o.type != 'MESH':
            continue
        idx = o.get('cpcw_node_index')
        if idx is None:
            continue
        idx = int(idx)
        if not (0 <= idx < len(pmod.nodes)):
            continue
        mi = pmod.nodes[idx].mesh_index
        if not (0 <= mi < len(pmod.meshes)):
            continue
        ps = pmod.meshes[mi].stream_by_usage(srm_writer.USAGE_POSITION)
        if ps is None or ps.stride != 12:
            skipped.append((o.name, "no float3 POSITION stream"))
            continue
        me = o.data
        if len(me.vertices) != ps.vcount:
            skipped.append((o.name, "vertex count %d != source %d (topology "
                            "changed; reshape only, don't add/remove verts)"
                            % (len(me.vertices), ps.vcount)))
            continue
        buf = bytearray(ps.vcount * 12)
        for i, v in enumerate(me.vertices):
            sx, sy, sz = _unswap(v.co)
            struct.pack_into('<3f', buf, i * 12, sx, sy, sz)
        ps.data = bytes(buf)
        written += 1
    return written, skipped


def _srm_local_matrix(pos, rot, scale):
    """Build a node's local matrix the same way the importer does (Rx@Ry@Rz)."""
    rx, ry, rz = rot
    R = (Matrix.Rotation(rx, 4, 'X') @
         Matrix.Rotation(ry, 4, 'Y') @
         Matrix.Rotation(rz, 4, 'Z'))
    T = Matrix.Translation(pos[:3])
    S = Matrix.Diagonal((scale[0], scale[1], scale[2], 1.0))
    return T @ R @ S


def _find_root(obj):
    """Walk up to the import root Empty that carries the source path."""
    o = obj
    while o is not None:
        if o.get('cpcw_srm_source'):
            return o
        o = o.parent
    return None


def _node_edit(obj, root, model, node_index, tol=1e-5):
    """Return (pos, rot_xyz, scale3) if this object's transform differs from the
    node's source transform, else None.

    The importer bakes the LH->RH swap into geometry and expresses node/bone
    matrices in Blender space (``P @ world_srm @ P``). To compare against the
    stored SRM node header we conjugate the object's Blender-space local matrix
    back by the same swap (``P @ matrix_local @ P``). We only write back
    hierarchy-root nodes (parent < 0), whose world == local, and only when the
    object actually moved versus the source — so a no-edit export stays
    byte-identical.
    """
    node = model.nodes[node_index]
    if node.parent >= 0 or obj.parent is not root:
        return None
    orig = _srm_local_matrix(node.pos, node.rot, node.scale)
    cur = _HAND @ obj.matrix_local @ _HAND
    if all(abs(a - b) <= tol for ra, rb in zip(orig, cur) for a, b in zip(ra, rb)):
        return None  # unchanged
    loc, quat, scl = cur.decompose()
    eul = quat.to_matrix().to_euler('XYZ')
    return (loc.x, loc.y, loc.z), (eul.x, eul.y, eul.z), (scl.x, scl.y, scl.z)


def export_srm(context, filepath, apply_transforms=True, write_geometry=False):
    sel = context.selected_objects or context.scene.objects
    root = None
    for o in sel:
        root = _find_root(o)
        if root:
            break
    if root is None:
        raise RuntimeError("No CPCW-imported model selected (missing "
                           "'cpcw_srm_source'). Select an imported model.")

    source = root['cpcw_srm_source']
    if not os.path.isfile(source):
        raise RuntimeError("Source .srm no longer at %r; cannot round-trip." % source)

    model = srm_writer.read(source)
    pmod = model.pmod()
    changed = 0

    geo_written = 0
    if write_geometry and pmod is not None:
        assemble = root.get('cpcw_assemble', 'AUTO')
        if assemble != 'NONE':
            raise RuntimeError(
                "Geometry write-back needs the model imported with Assemble="
                "'Off (raw bind pose)'; this one used %r, whose vertices are "
                "bone-transformed and can't be inverted. Re-import with Assemble="
                "Off to edit and export geometry." % assemble)
        geo_written, skipped = _geometry_writeback(root, pmod)
        for name, why in skipped:
            print("  geometry write-back skipped %s: %s" % (name, why))

    if apply_transforms and pmod is not None:
        # map node index -> object
        by_index = {}
        for o in root.children_recursive if hasattr(root, 'children_recursive') else []:
            idx = o.get('cpcw_node_index')
            if idx is not None:
                by_index[int(idx)] = o
        for idx, o in by_index.items():
            if not (0 <= idx < len(pmod.nodes)):
                continue
            got = _node_edit(o, root, pmod, idx)
            if got is None:
                continue
            pos, rot, scl = got
            n = pmod.nodes[idx]
            n.pos = pos
            n.rot = rot
            n.scale = (scl[0], scl[1], scl[2], n.scale[3])
            changed += 1

    model.write(filepath)
    identical = (model.pack() == open(source, 'rb').read())
    print("Exported %s (%d node transforms, %d meshes' geometry updated, "
          "identical-to-source=%s)" % (filepath, changed, geo_written, identical))
    return changed


class EXPORT_OT_cpcw_srm(bpy.types.Operator, ExportHelper):
    """Export a Codename: Panzers Cold War model (round-trip from its source)"""
    bl_idname = "export_scene.cpcw_srm"
    bl_label = "Export CPCW Model (.srm)"
    bl_options = {'REGISTER', 'UNDO', 'PRESET'}

    filename_ext = ".srm"
    filter_glob: StringProperty(default="*.srm", options={'HIDDEN'})

    apply_transforms: BoolProperty(
        name="Write Back Node Transforms",
        description="Update node positions/rotations/scales from the moved "
                    "objects (root-level nodes only). Geometry is preserved "
                    "from the original source file",
        default=True,
    )
    write_geometry: BoolProperty(
        name="Write Back Geometry (reshape)",
        description="Rewrite each mesh's vertex POSITIONS from the edited Blender "
                    "mesh (reshape existing geometry). Requires the model to have "
                    "been imported with Assemble='Off (raw bind pose)' and the "
                    "vertex count unchanged. Indices/UVs/normals/skinning are "
                    "preserved; a no-edit export stays byte-identical",
        default=False,
    )

    def execute(self, context):
        try:
            export_srm(context, self.filepath, self.apply_transforms,
                       self.write_geometry)
        except Exception as e:
            self.report({'ERROR'}, "SRM export failed: %s" % e)
            return {'CANCELLED'}
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "apply_transforms")
        layout.prop(self, "write_geometry")


def menu_func_export(self, context):
    self.layout.operator(EXPORT_OT_cpcw_srm.bl_idname, text="CPCW Model (.srm)")


def register():
    bpy.utils.register_class(EXPORT_OT_cpcw_srm)
    bpy.types.TOPBAR_MT_file_export.append(menu_func_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_func_export)
    bpy.utils.unregister_class(EXPORT_OT_cpcw_srm)
