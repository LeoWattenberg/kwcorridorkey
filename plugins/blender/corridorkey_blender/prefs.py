from __future__ import annotations

import bpy


class CorridorKeyPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    worker_python: bpy.props.StringProperty(
        name="Worker Python",
        subtype="FILE_PATH",
        description="Python executable from scripts/install_corridorkey.py",
    )
    timeout_seconds: bpy.props.IntProperty(name="Timeout", default=0, min=0)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "worker_python")
        layout.prop(self, "timeout_seconds")

