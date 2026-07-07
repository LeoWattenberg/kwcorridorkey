from __future__ import annotations

import os

import bpy


class CORRIDORKEY_ND_compositor_node(bpy.types.Node):
    bl_idname = "CORRIDORKEY_ND_compositor_node"
    bl_label = "CorridorKey"
    bl_icon = "IMAGE_ALPHA"

    output_dir: bpy.props.StringProperty(name="Output Directory", subtype="DIR_PATH", default="//corridorkey_output")
    start_frame: bpy.props.IntProperty(name="Start", default=1, min=0)
    end_frame: bpy.props.IntProperty(name="End", default=1, min=0)
    input_colorspace: bpy.props.EnumProperty(
        name="Input",
        items=(("srgb", "sRGB", ""), ("linear", "Linear", "")),
        default="srgb",
    )
    inference_size: bpy.props.EnumProperty(
        name="Inference Size",
        items=(("512", "512", ""), ("1024", "1024", ""), ("2048", "2048", "")),
        default="2048",
    )
    auto_despeckle: bpy.props.BoolProperty(name="Auto Despeckle", default=True)

    @classmethod
    def poll(cls, tree):
        return tree.bl_idname == "CompositorNodeTree"

    def init(self, context):
        self.inputs.new("NodeSocketColor", "Image")
        self.inputs.new("NodeSocketColor", "Key Color")
        self.inputs["Key Color"].default_value = (0.0, 1.0, 0.0, 1.0)
        self.inputs.new("NodeSocketFloat", "Acceptance")
        self.inputs["Acceptance"].default_value = 0.25
        self.inputs.new("NodeSocketFloat", "Cutoff")
        self.inputs["Cutoff"].default_value = 0.05
        self.inputs.new("NodeSocketFloat", "Falloff")
        self.inputs["Falloff"].default_value = 1.0
        self.inputs.new("NodeSocketFloat", "Despill")
        self.inputs["Despill"].default_value = 5.0
        self.inputs.new("NodeSocketFloat", "Despeckle Size")
        self.inputs["Despeckle Size"].default_value = 400.0
        self.inputs.new("NodeSocketFloat", "Refiner")
        self.inputs["Refiner"].default_value = 1.0
        self.outputs.new("NodeSocketColor", "Image")
        self.outputs.new("NodeSocketColor", "Image Preview")
        self.outputs.new("NodeSocketFloat", "Matte")

    def draw_buttons(self, context, layout):
        layout.prop(self, "output_dir")
        row = layout.row(align=True)
        row.prop(self, "start_frame")
        row.prop(self, "end_frame")
        layout.prop(self, "input_colorspace")
        layout.prop(self, "inference_size")
        layout.prop(self, "auto_despeckle")
        layout.operator("corridorkey.render_selected_node", text="Render CorridorKey")

    def draw_label(self):
        return "CorridorKey"


def get_socket_value(node, name):
    socket = node.inputs[name]
    return getattr(socket, "default_value", None)


def get_float_socket_value(node, name) -> float:
    value = get_socket_value(node, name)
    if value is None:
        return 0.0
    return float(value)


def get_color_socket_value(node, name):
    value = get_socket_value(node, name)
    if value is None:
        return (0.0, 1.0, 0.0, 1.0)
    return tuple(value)


def linked_image_path(node) -> str:
    image_socket = node.inputs["Image"]
    if not image_socket.is_linked:
        raise ValueError("CorridorKey node Image input must be linked to an Image node")

    source_node = image_socket.links[0].from_node
    image = getattr(source_node, "image", None)
    if image is None:
        raise ValueError("CorridorKey Image input must be linked from a Blender Image node")

    filepath = bpy.path.abspath(image.filepath)
    if not filepath:
        raise ValueError("Linked Image node has no file path")
    return filepath


def selected_corridorkey_node(context):
    node = getattr(context, "node", None)
    if isinstance(node, CORRIDORKEY_ND_compositor_node):
        return node
    space = getattr(context, "space_data", None)
    tree = getattr(space, "edit_tree", None) or getattr(space, "node_tree", None)
    if tree is None:
        tree = getattr(context.scene, "node_tree", None) or getattr(context.scene, "compositing_node_group", None)
    if tree is None:
        return None
    for candidate in tree.nodes:
        if isinstance(candidate, CORRIDORKEY_ND_compositor_node) and candidate.select:
            return candidate
    return None


def output_frame_path(output_dir: str, frame: int, stem: str, suffix: str = "exr") -> str:
    return os.path.join(output_dir, f"corridorkey_{stem}_{frame:04d}.{suffix}")


def add_menu_entry(self, context):
    if context.space_data.tree_type == "CompositorNodeTree":
        self.layout.operator("node.add_node", text="CorridorKey", icon="IMAGE_ALPHA").type = CORRIDORKEY_ND_compositor_node.bl_idname


def register():
    bpy.types.NODE_MT_add.append(add_menu_entry)


def unregister():
    bpy.types.NODE_MT_add.remove(add_menu_entry)
