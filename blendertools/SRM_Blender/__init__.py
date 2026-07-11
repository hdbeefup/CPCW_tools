"""CPCW SRM model importer — Blender add-on.

Imports Codename: Panzers Cold War (.srm) models into Blender with geometry,
UVs, normals and DDS-textured materials. File > Import > CPCW Model (.srm).
"""

bl_info = {
    "name": "CPCW Model (.srm)",
    "author": "CPCW_tools",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > CPCW Model (.srm)",
    "description": "Import Codename: Panzers Cold War .srm models",
    "category": "Import-Export",
}


def register():
    from . import import_srm
    import_srm.register()


def unregister():
    from . import import_srm
    import_srm.unregister()


if __name__ == "__main__":
    register()
