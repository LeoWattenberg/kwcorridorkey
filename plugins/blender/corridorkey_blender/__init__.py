bl_info = {
    "name": "CorridorKey",
    "author": "CorridorKey integration contributors",
    "version": (0, 1, 0),
    "blender": (4, 0, 0),
    "location": "Video Sequencer > Sidebar > CorridorKey",
    "description": "Run CorridorKey through the shared native plugin worker",
    "category": "Compositing",
}

from . import operators, panels, prefs, properties
import bpy


classes = (
    prefs.CorridorKeyPreferences,
    properties.CorridorKeySceneSettings,
    operators.CORRIDORKEY_OT_from_selected_strips,
    operators.CORRIDORKEY_OT_render_sequence,
    panels.CORRIDORKEY_PT_sequence_panel,
    panels.CORRIDORKEY_PT_compositor_panel,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    properties.register()


def unregister():
    properties.unregister()
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
