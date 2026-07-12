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


def _build_terrain(mf, collection, world_w, world_h, tint_passability):
    """Create a flat ground plane over the world extent (Z=0)."""
    verts = [(0.0, 0.0, 0.0), (world_w, 0.0, 0.0),
             (world_w, world_h, 0.0), (0.0, world_h, 0.0)]
    faces = [(0, 1, 2, 3)]

    grid = None
    gw = gh = 0
    if tint_passability:
        grid, gw, gh = mf.get_blck_grid()

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
            attr = mesh.color_attributes.new(name="Passability", type='BYTE_COLOR',
                                             domain='CORNER')
            for poly in mesh.polygons:
                # cell index from the polygon order (row-major, matches faces)
                cell_i = poly.index % gw
                cell_j = poly.index // gw
                passable = grid[cell_j][cell_i][0] != 0 if cell_j < gh else True
                col = (0.35, 0.5, 0.3, 1.0) if passable else (0.6, 0.2, 0.2, 1.0)
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
             data_root="", import_textures=True, models_max=0):
    """Import a .map file: terrain + entity Empties. Returns the root collection."""
    mf = map_format.MapFile(filepath)
    world_w, world_h = _get_world_size(mf)

    map_name = os.path.splitext(os.path.basename(filepath))[0]
    root = bpy.data.collections.new(f"Map_{map_name}")
    context.scene.collection.children.link(root)

    if build_terrain:
        terr_col = bpy.data.collections.new("Terrain")
        root.children.link(terr_col)
        _build_terrain(mf, terr_col, world_w, world_h, tint_passability)

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
        name="Build Terrain Plane", default=True,
        description="Create a flat ground plane over the world extent")
    tint_passability: BoolProperty(
        name="Tint Passability", default=True,
        description="Colour the terrain grid by the BLCK passability data "
                    "(green = passable, red = blocked)")
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
                               models_max=self.models_max)
        except Exception as e:
            self.report({'ERROR'}, f"Map import failed: {e}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"Imported map: {n} entities")
        return {'FINISHED'}

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "build_terrain")
        if self.build_terrain:
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
