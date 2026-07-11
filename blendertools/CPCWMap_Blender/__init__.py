"""CPCW map viewer — Blender add-on.

Imports Codename: Panzers Cold War (.map) scenarios into Blender as a terrain
extent plane plus a labelled Empty per placed entity. File > Import > CPCW Map
(.map). Real per-entity model instancing is a planned future step.
"""

bl_info = {
    "name": "CPCW Map (.map)",
    "author": "CPCW_tools",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > CPCW Map (.map)",
    "description": "Import Codename: Panzers Cold War .map scenarios (basic viewer)",
    "category": "Import-Export",
}


def register():
    from . import import_map
    import_map.register()


def unregister():
    from . import import_map
    import_map.unregister()


if __name__ == "__main__":
    register()
