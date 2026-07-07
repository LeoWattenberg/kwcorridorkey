bl_info = {
    "name": "CorridorKey",
    "author": "CorridorKey integration contributors",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "Compositor > Add > CorridorKey",
    "description": "CorridorKey compositor node using a Color Key hint and shared worker",
    "category": "Compositing",
}

from . import nodes, operators, prefs, properties
import bpy


classes = (
    prefs.CorridorKeyPreferences,
    properties.CorridorKeySceneSettings,
    nodes.CORRIDORKEY_ND_compositor_node,
    operators.CORRIDORKEY_OT_from_selected_strips,
    operators.CORRIDORKEY_OT_render_sequence,
    operators.CORRIDORKEY_OT_render_selected_node,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    properties.register()
    nodes.register()


def unregister():
    nodes.unregister()
    properties.unregister()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
