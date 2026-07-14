"""Provisional MOTS playback demo (Blender). Bakes the dominant animated DOF of
each animated node as rotation keyframes and renders a frame sequence.

blender --background --python mots_play.py -- <srm> <dataroot> <outdir> <motion_idx> <nframes> [axismap]
axismap optional: e.g. "1=Y-,3=X+" overrides Blender axis+sign per srm node index.
"""
import bpy, addon_utils, mathutils, math, sys, os
sys.path.insert(0, r'N:\ProjectsCODE\CPCW_tools')
import cpcw_srm, cpcw_mots

a = sys.argv[sys.argv.index('--')+1:]
SRM, ROOT, OUTDIR, MI, NF = a[0], a[1], a[2], int(a[3]), int(a[4])
AXMAP = {}
if len(a) > 5 and a[5]:
    for tok in a[5].split(','):
        k, v = tok.split('='); AXMAP[int(k)] = v   # v like "Y-" / "X+" / "Z+"
os.makedirs(OUTDIR, exist_ok=True)

nodes = cpcw_srm.parse_srm(SRM)
motions = cpcw_mots.parse_mots(SRM)
if MI < 0:
    # auto: pick the motion with the most rotation spins (the machinery loop)
    def nrot(m): return len([s for s in cpcw_mots.node_spins(m) if s.is_rotation])
    motion = max(motions, key=nrot) if motions else None
    if motion is None or nrot(motion) == 0:
        print("NO rotation motion in", SRM); sys.exit(0)
else:
    motion = motions[MI]
spins = [s for s in cpcw_mots.node_spins(motion) if s.is_rotation]
print("MOTION", motion.name, "dur", motion.duration, "rotation spins:", [(s.node_idx, s.dof, round(s.span,2)) for s in spins])

bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("SRM_Blender", default_set=True, persistent=True)
bpy.ops.import_scene.cpcw_srm(filepath=SRM, import_textures=True, texture_dir=ROOT,
                              apply_skin='AUTO', variant='ALL')

# map srm node index -> imported Blender object by node name
name_by_idx = {i: n.name for i, n in enumerate(nodes)}
obj_by_idx = {}
for i, nm in name_by_idx.items():
    o = bpy.data.objects.get(nm)
    if o: obj_by_idx[i] = o

def descendants(idx):
    kids = [i for i, n in enumerate(nodes) if n.parent_idx == idx]
    out = list(kids)
    for k in kids: out += descendants(k)
    return out

def group_objs(idx):
    out = [obj_by_idx[i] for i in [idx] + descendants(idx) if i in obj_by_idx]
    return [o for o in out if o.type == 'MESH']

# spin axis is chosen GEOMETRICALLY (provisional): a rotor disc is thinnest along
# its shaft, so we spin about the group's minimum-extent world axis through the
# group centroid. Amount & speed come from the data (NodeSpin.span / duration).
scene = bpy.context.scene
scene.frame_start = 0
scene.frame_end = NF - 1

# window the sampled time so the FASTEST rotor does ~2 visible turns over NF frames
max_span = max((abs(s.span) for s in spins), default=2*math.pi) or 2*math.pi
WINDOW = motion.duration * (2 * 2*math.pi) / max_span
print("time window", round(WINDOW,3), "s of", motion.duration)

for s in spins:
    grp = group_objs(s.node_idx)
    if not grp: continue
    # world-space verts of the group -> centroid + min-extent axis
    vs = [o.matrix_world @ v.co for o in grp for v in o.data.vertices]
    lo3 = [min(v[k] for v in vs) for k in range(3)]
    hi3 = [max(v[k] for v in vs) for k in range(3)]
    centroid = mathutils.Vector([(lo3[k]+hi3[k])/2 for k in range(3)])
    ext = [hi3[k]-lo3[k] for k in range(3)]
    over = AXMAP.get(s.node_idx)
    ai = 'XYZ'.index(over[0]) if over else min(range(3), key=lambda k: ext[k])
    sign = (-1.0 if over and over[1:] == '-' else 1.0)
    # pivot Empty at centroid; parent the group to it (keep transform)
    emp = bpy.data.objects.new("spin_%d" % s.node_idx, None)
    emp.location = centroid
    scene.collection.objects.link(emp)
    for o in grp:
        o.parent = emp
        o.matrix_parent_inverse = emp.matrix_world.inverted()
    emp.rotation_mode = 'XYZ'
    for f in range(NF):
        t = (f / max(1, NF)) * WINDOW
        e = [0.0, 0.0, 0.0]; e[ai] = sign * s.sample(t)   # provisional linear ramp
        emp.rotation_euler = e
        emp.keyframe_insert('rotation_euler', frame=f)
    o = emp
    # linear interpolation on the fcurves (API differs across Blender versions)
    def _all_fcurves(obj):
        act = obj.animation_data.action if obj.animation_data else None
        if not act: return
        try:
            for fc in act.fcurves: yield fc
            return
        except (AttributeError, TypeError):
            pass
        for layer in getattr(act, 'layers', []):
            for strip in getattr(layer, 'strips', []):
                for cbag in getattr(strip, 'channelbags', []):
                    for fc in getattr(cbag, 'fcurves', []): yield fc
    try:
        for fc in _all_fcurves(o):
            for kp in fc.keyframe_points: kp.interpolation = 'LINEAR'
    except Exception as ex:
        print("  (fcurve interp skip:", ex, ")")
    print("  baked node", s.node_idx, "grp", [g.name for g in grp],
          "world-axis", "XYZ"[ai], "ext", [round(e,2) for e in ext], "sign", sign)

# frame the model
lo=[1e9]*3; hi=[-1e9]*3
for o in bpy.data.objects:
    if o.type!='MESH': continue
    for v in o.data.vertices:
        w=o.matrix_world @ v.co
        for k in range(3): lo[k]=min(lo[k],w[k]); hi[k]=max(hi[k],w[k])
c=mathutils.Vector([(lo[i]+hi[i])/2 for i in range(3)]); sz=max(hi[i]-lo[i] for i in range(3)) or 1.0
cd=bpy.data.cameras.new('c'); cam=bpy.data.objects.new('c',cd)
scene.collection.objects.link(cam); scene.camera=cam
d=mathutils.Vector((0.75,-1.0,0.5)).normalized()
cam.location=c+d*sz*1.7
cam.rotation_euler=(c-cam.location).to_track_quat('-Z','Y').to_euler()
for ang,e in [((55,10,40),4.0),((60,0,-120),2.0)]:
    ld=bpy.data.lights.new('s','SUN'); so=bpy.data.objects.new('s',ld)
    scene.collection.objects.link(so); so.rotation_euler=tuple(math.radians(x) for x in ang); ld.energy=e
# engine: MOTS_ENGINE=WORKBENCH is light & GPU-friendly for parallel runs
if os.environ.get('MOTS_ENGINE', '').upper() == 'WORKBENCH':
    scene.render.engine = 'BLENDER_WORKBENCH'
else:
    try: scene.render.engine='BLENDER_EEVEE'
    except Exception: scene.render.engine='BLENDER_EEVEE_NEXT'
scene.render.resolution_x=480; scene.render.resolution_y=480
scene.world=bpy.data.worlds.new('w'); scene.world.use_nodes=True
scene.world.node_tree.nodes['Background'].inputs[0].default_value=(0.1,0.11,0.14,1)
scene.render.image_settings.file_format='PNG'
for f in range(NF):
    scene.frame_set(f)
    scene.render.filepath=os.path.join(OUTDIR, "f%03d.png" % f)
    bpy.ops.render.render(write_still=True)
print("RENDERED", NF, "frames ->", OUTDIR)
