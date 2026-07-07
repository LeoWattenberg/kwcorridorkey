from __future__ import annotations

import bpy


def draw_corridorkey_settings(layout, context):
    settings = context.scene.corridorkey
    layout.operator("corridorkey.from_selected_strips")
    layout.prop(settings, "source_path")
    layout.prop(settings, "alpha_path")
    layout.prop(settings, "output_dir")
    row = layout.row(align=True)
    row.prop(settings, "start_frame")
    row.prop(settings, "end_frame")
    layout.prop(settings, "screen_color")
    layout.prop(settings, "input_colorspace")
    layout.prop(settings, "output_mode")
    layout.prop(settings, "despill")
    layout.prop(settings, "auto_despeckle")
    layout.prop(settings, "despeckle_size")
    layout.prop(settings, "refiner")
    layout.prop(settings, "inference_size")
    row = layout.row(align=True)
    row.prop(settings, "backend")
    row.prop(settings, "device")
    layout.operator("corridorkey.render_sequence")


class CORRIDORKEY_PT_sequence_panel(bpy.types.Panel):
    bl_label = "CorridorKey"
    bl_space_type = "SEQUENCE_EDITOR"
    bl_region_type = "UI"
    bl_category = "CorridorKey"

    def draw(self, context):
        draw_corridorkey_settings(self.layout, context)


class CORRIDORKEY_PT_compositor_panel(bpy.types.Panel):
    bl_label = "CorridorKey"
    bl_space_type = "NODE_EDITOR"
    bl_region_type = "UI"
    bl_category = "CorridorKey"

    @classmethod
    def poll(cls, context):
        return context.space_data.tree_type == "CompositorNodeTree"

    def draw(self, context):
        draw_corridorkey_settings(self.layout, context)

