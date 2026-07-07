from __future__ import annotations

import bpy


class CorridorKeySceneSettings(bpy.types.PropertyGroup):
    source_path: bpy.props.StringProperty(name="Source", subtype="FILE_PATH")
    alpha_path: bpy.props.StringProperty(name="Alpha Hint", subtype="FILE_PATH")
    output_dir: bpy.props.StringProperty(name="Output Directory", subtype="DIR_PATH")
    start_frame: bpy.props.IntProperty(name="Start", default=1, min=0)
    end_frame: bpy.props.IntProperty(name="End", default=1, min=0)
    screen_color: bpy.props.EnumProperty(
        name="Screen",
        items=(("auto", "Auto", ""), ("green", "Green", ""), ("blue", "Blue", "")),
        default="auto",
    )
    input_colorspace: bpy.props.EnumProperty(
        name="Input",
        items=(("srgb", "sRGB", ""), ("linear", "Linear", "")),
        default="srgb",
    )
    output_mode: bpy.props.EnumProperty(
        name="Output",
        items=(
            ("processed_rgba", "Processed RGBA", ""),
            ("matte", "Matte", ""),
            ("straight_fg", "Straight FG", ""),
            ("checker_comp", "Checker Comp", ""),
        ),
        default="processed_rgba",
    )
    despill: bpy.props.IntProperty(name="Despill", default=5, min=0, max=10)
    auto_despeckle: bpy.props.BoolProperty(name="Auto Despeckle", default=True)
    despeckle_size: bpy.props.IntProperty(name="Despeckle Size", default=400, min=0)
    refiner: bpy.props.FloatProperty(name="Refiner", default=1.0, min=0.01)
    inference_size: bpy.props.EnumProperty(
        name="Inference Size",
        items=(("512", "512", ""), ("1024", "1024", ""), ("2048", "2048", "")),
        default="2048",
    )
    backend: bpy.props.EnumProperty(
        name="Backend",
        items=(("auto", "Auto", ""), ("torch", "Torch", ""), ("mlx", "MLX", "")),
        default="auto",
    )
    device: bpy.props.EnumProperty(
        name="Device",
        items=(("auto", "Auto", ""), ("cuda", "CUDA", ""), ("mps", "MPS", ""), ("cpu", "CPU", ""), ("rocm", "ROCm", "")),
        default="auto",
    )


def register():
    bpy.types.Scene.corridorkey = bpy.props.PointerProperty(type=CorridorKeySceneSettings)


def unregister():
    del bpy.types.Scene.corridorkey

