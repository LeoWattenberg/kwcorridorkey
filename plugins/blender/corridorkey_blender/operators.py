from __future__ import annotations

import os
import tempfile

import bpy

from .image_io import load_image_to_raw, output_frame_path, resolve_frame_path, save_raw_to_image
from .worker_client import FrameSpec, WorkerClient


def _settings(scene_settings) -> dict:
    return {
        "screen_color": scene_settings.screen_color,
        "input_colorspace": scene_settings.input_colorspace,
        "despill": scene_settings.despill,
        "auto_despeckle": scene_settings.auto_despeckle,
        "despeckle_size": scene_settings.despeckle_size,
        "refiner": scene_settings.refiner,
        "inference_size": int(scene_settings.inference_size),
        "backend": scene_settings.backend,
        "device": scene_settings.device,
        "output_mode": scene_settings.output_mode,
    }


def _preferences(context):
    addon = context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


class CORRIDORKEY_OT_from_selected_strips(bpy.types.Operator):
    bl_idname = "corridorkey.from_selected_strips"
    bl_label = "Use Selected Strips"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        strips = list(context.selected_sequences or [])
        if len(strips) < 2:
            self.report({"ERROR"}, "Select source and alpha hint strips")
            return {"CANCELLED"}
        settings = context.scene.corridorkey
        settings.source_path = bpy.path.abspath(strips[0].filepath)
        settings.alpha_path = bpy.path.abspath(strips[1].filepath)
        settings.start_frame = context.scene.frame_start
        settings.end_frame = context.scene.frame_end
        if not settings.output_dir:
            settings.output_dir = bpy.path.abspath("//corridorkey_output")
        return {"FINISHED"}


class CORRIDORKEY_OT_render_sequence(bpy.types.Operator):
    bl_idname = "corridorkey.render_sequence"
    bl_label = "Render CorridorKey"
    bl_options = {"REGISTER"}

    def execute(self, context):
        settings = context.scene.corridorkey
        if not settings.source_path or not settings.alpha_path or not settings.output_dir:
            self.report({"ERROR"}, "Source, alpha hint, and output directory are required")
            return {"CANCELLED"}
        if settings.end_frame < settings.start_frame:
            self.report({"ERROR"}, "End frame must be >= start frame")
            return {"CANCELLED"}

        prefs = _preferences(context)
        worker_python = prefs.worker_python if prefs and prefs.worker_python else None
        os.makedirs(bpy.path.abspath(settings.output_dir), exist_ok=True)

        client = WorkerClient(worker_python)
        try:
            client.request("hello", {})
            client.request("preflight", {"settings": _settings(settings)})
            with tempfile.TemporaryDirectory(prefix="corridorkey_blender_") as temp_dir:
                for frame in range(settings.start_frame, settings.end_frame + 1):
                    source_file = bpy.path.abspath(resolve_frame_path(settings.source_path, frame))
                    alpha_file = bpy.path.abspath(resolve_frame_path(settings.alpha_path, frame))
                    source_raw = os.path.join(temp_dir, f"source_{frame:04d}.raw")
                    alpha_raw = os.path.join(temp_dir, f"alpha_{frame:04d}.raw")
                    output_raw = os.path.join(temp_dir, f"output_{frame:04d}.raw")

                    source_spec = load_image_to_raw(source_file, source_raw)
                    alpha_spec = load_image_to_raw(alpha_file, alpha_raw)
                    output_spec = FrameSpec(
                        output_raw,
                        width=source_spec.width,
                        height=source_spec.height,
                        channels=4,
                    )
                    client.process(_settings(settings), source_spec, alpha_spec, output_spec)

                    image_path = output_frame_path(bpy.path.abspath(settings.output_dir), frame, settings.output_mode)
                    save_raw_to_image(output_raw, image_path, output_spec.width, output_spec.height, output_spec.channels)
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        finally:
            client.close()

        self.report({"INFO"}, "CorridorKey render complete")
        return {"FINISHED"}

