from __future__ import annotations

import os
import tempfile

import bpy

from .color_key import write_color_key_hint, write_color_key_preview
from .image_io import load_image_to_raw, output_frame_path, resolve_frame_path, save_raw_to_image
from .nodes import (
    get_color_socket_value,
    get_float_socket_value,
    linked_image_path,
    output_frame_path as node_output_frame_path,
    selected_corridorkey_node,
)
from .worker_client import FrameSpec, WorkerClient


def _backend_device_from_system(context) -> tuple[str, str]:
    compute_type = ""
    cycles = context.preferences.addons.get("cycles")
    if cycles:
        compute_type = getattr(cycles.preferences, "compute_device_type", "") or ""
    if not compute_type:
        system = getattr(context.preferences, "system", None)
        compute_type = getattr(system, "compute_device_type", "") or getattr(system, "gpu_backend", "") or ""

    compute_type = compute_type.upper()
    if compute_type in {"CUDA", "OPTIX"}:
        return "torch", "cuda"
    if compute_type == "HIP":
        return "torch", "rocm"
    if compute_type == "METAL":
        return "mlx", "mps"
    return "auto", "auto"


def _screen_color_from_key(key_color) -> str:
    return "blue" if float(key_color[2]) > float(key_color[1]) else "green"


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


def _node_settings(context, node, output_mode: str) -> dict:
    backend, device = _backend_device_from_system(context)
    key_color = get_color_socket_value(node, "Key Color")
    return {
        "screen_color": _screen_color_from_key(key_color),
        "input_colorspace": node.input_colorspace,
        "despill": int(round(get_float_socket_value(node, "Despill"))),
        "auto_despeckle": node.auto_despeckle,
        "despeckle_size": int(round(get_float_socket_value(node, "Despeckle Size"))),
        "refiner": get_float_socket_value(node, "Refiner"),
        "inference_size": int(node.inference_size),
        "backend": backend,
        "device": device,
        "output_mode": output_mode,
    }


def _preferences(context):
    addon = context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


def _selected_strips(context):
    for attr in ("selected_sequences", "selected_strips"):
        strips = getattr(context, attr, None)
        if strips is not None:
            return list(strips)

    editor = getattr(context.scene, "sequence_editor", None)
    if editor is None:
        return []
    return [strip for strip in editor.sequences_all if getattr(strip, "select", False)]


def _strip_path(strip):
    filepath = getattr(strip, "filepath", "")
    if filepath:
        return filepath

    directory = getattr(strip, "directory", "")
    elements = getattr(strip, "elements", None)
    if directory and elements:
        return os.path.join(directory, elements[0].filename)

    raise ValueError(f"Selected strip '{strip.name}' does not expose a file path")


class CORRIDORKEY_OT_from_selected_strips(bpy.types.Operator):
    bl_idname = "corridorkey.from_selected_strips"
    bl_label = "Use Selected Strips"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        strips = _selected_strips(context)
        if len(strips) < 2:
            self.report({"ERROR"}, "Select source and alpha hint strips")
            return {"CANCELLED"}
        settings = context.scene.corridorkey
        try:
            settings.source_path = bpy.path.abspath(_strip_path(strips[0]))
            settings.alpha_path = bpy.path.abspath(_strip_path(strips[1]))
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
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


class CORRIDORKEY_OT_render_selected_node(bpy.types.Operator):
    bl_idname = "corridorkey.render_selected_node"
    bl_label = "Render CorridorKey Node"
    bl_options = {"REGISTER"}

    def execute(self, context):
        node = selected_corridorkey_node(context)
        if node is None:
            self.report({"ERROR"}, "Select a CorridorKey compositor node")
            return {"CANCELLED"}
        if node.end_frame < node.start_frame:
            self.report({"ERROR"}, "End frame must be >= start frame")
            return {"CANCELLED"}

        prefs = _preferences(context)
        worker_python = prefs.worker_python if prefs and prefs.worker_python else None
        output_dir = bpy.path.abspath(node.output_dir)
        os.makedirs(output_dir, exist_ok=True)

        try:
            source_template = linked_image_path(node)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}

        key_color = get_color_socket_value(node, "Key Color")
        acceptance = get_float_socket_value(node, "Acceptance")
        cutoff = get_float_socket_value(node, "Cutoff")
        falloff = get_float_socket_value(node, "Falloff")

        client = WorkerClient(worker_python)
        try:
            client.request("hello", {})
            with tempfile.TemporaryDirectory(prefix="corridorkey_blender_node_") as temp_dir:
                for frame in range(node.start_frame, node.end_frame + 1):
                    source_file = bpy.path.abspath(resolve_frame_path(source_template, frame))
                    source_raw = os.path.join(temp_dir, f"source_{frame:04d}.raw")
                    hint_raw = os.path.join(temp_dir, f"hint_{frame:04d}.raw")
                    image_raw = os.path.join(temp_dir, f"image_{frame:04d}.raw")
                    matte_raw = os.path.join(temp_dir, f"matte_{frame:04d}.raw")

                    source_spec = load_image_to_raw(source_file, source_raw)
                    hint_spec = write_color_key_hint(source_file, hint_raw, key_color, acceptance, cutoff, falloff)

                    preview_path = node_output_frame_path(output_dir, frame, "preview", "exr")
                    write_color_key_preview(source_file, preview_path, key_color, acceptance, cutoff, falloff)

                    image_spec = FrameSpec(image_raw, width=source_spec.width, height=source_spec.height, channels=4)
                    client.process(_node_settings(context, node, "processed_rgba"), source_spec, hint_spec, image_spec)
                    image_path = node_output_frame_path(output_dir, frame, "image", "exr")
                    save_raw_to_image(image_raw, image_path, image_spec.width, image_spec.height, image_spec.channels)

                    matte_spec = FrameSpec(matte_raw, width=source_spec.width, height=source_spec.height, channels=1)
                    client.process(_node_settings(context, node, "matte"), source_spec, hint_spec, matte_spec)
                    matte_path = node_output_frame_path(output_dir, frame, "matte", "exr")
                    save_raw_to_image(matte_raw, matte_path, matte_spec.width, matte_spec.height, matte_spec.channels)
        except Exception as exc:  # noqa: BLE001
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        finally:
            client.close()

        self.report({"INFO"}, "CorridorKey node render complete")
        return {"FINISHED"}
