"""Dump the transform of every part object to JSON (for srm_split_import scenes).

Paste into Blender's Scripting tab and Run. Records each object's position in
SRM-native coordinates (matrix_basis, i.e. local to the Y-up root) plus its
srm_mesh / srm_bone / bone-node metadata, so the corrected layout can be
compared against the file's bone transforms.

Change OUT for a different file (e.g. one for the as-imported layout and one for
your corrected layout).
"""

import json

import bpy

OUT = r"N:\ProjectsCODE\CPCW_tools\blendertools\tools\srm_layout.json"

data = []
for o in bpy.data.objects:
    if o.type != 'MESH':
        continue
    b = o.matrix_basis           # SRM-native (local to the Y-up root)
    t = b.translation
    data.append({
        "name": o.name,
        "srm_mesh": o.get("srm_mesh"),
        "srm_bone": o.get("srm_bone"),
        "srm_bone_node": o.get("srm_bone_node"),
        "srm_bone_name": o.get("srm_bone_name"),
        "pos": [round(t.x, 4), round(t.y, 4), round(t.z, 4)],
        "matrix_basis": [[round(v, 5) for v in row] for row in b],
    })

with open(OUT, "w") as f:
    json.dump(data, f, indent=2)

print(f"Wrote {OUT} ({len(data)} objects)")
