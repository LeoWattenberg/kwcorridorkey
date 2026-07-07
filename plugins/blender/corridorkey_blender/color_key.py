from __future__ import annotations

import array
import math
from pathlib import Path

import bpy

from .worker_client import FrameSpec


def clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return max(low, min(high, value))


def color_key_alpha(pixel: tuple[float, float, float], key_color: tuple[float, float, float], acceptance: float, cutoff: float, falloff: float) -> float:
    distance = math.sqrt(
        (pixel[0] - key_color[0]) ** 2
        + (pixel[1] - key_color[1]) ** 2
        + (pixel[2] - key_color[2]) ** 2
    )
    acceptance = max(0.0001, acceptance)
    cutoff = clamp(cutoff)
    falloff = max(0.0001, falloff)

    matte = clamp((distance - cutoff) / acceptance)
    if matte < 1.0:
        matte = matte ** (1.0 / falloff)
    return clamp(matte)


def write_color_key_hint(source_image_path: str, raw_path: str, key_color, acceptance: float, cutoff: float, falloff: float) -> FrameSpec:
    image = bpy.data.images.load(source_image_path, check_existing=False)
    try:
        width, height = image.size
        source_pixels = array.array("f", image.pixels[:])
        alpha = array.array("f")
        key = (float(key_color[0]), float(key_color[1]), float(key_color[2]))
        for index in range(0, len(source_pixels), 4):
            rgb = (source_pixels[index], source_pixels[index + 1], source_pixels[index + 2])
            alpha.append(color_key_alpha(rgb, key, acceptance, cutoff, falloff))

        Path(raw_path).parent.mkdir(parents=True, exist_ok=True)
        with open(raw_path, "wb") as handle:
            alpha.tofile(handle)
        return FrameSpec(raw_path, width=width, height=height, channels=1)
    finally:
        bpy.data.images.remove(image)


def write_color_key_preview(source_image_path: str, image_path: str, key_color, acceptance: float, cutoff: float, falloff: float) -> None:
    image = bpy.data.images.load(source_image_path, check_existing=False)
    try:
        width, height = image.size
        source_pixels = array.array("f", image.pixels[:])
        preview = array.array("f")
        key = (float(key_color[0]), float(key_color[1]), float(key_color[2]))
        for index in range(0, len(source_pixels), 4):
            rgb = (source_pixels[index], source_pixels[index + 1], source_pixels[index + 2])
            alpha = color_key_alpha(rgb, key, acceptance, cutoff, falloff)
            preview.extend((rgb[0] * alpha, rgb[1] * alpha, rgb[2] * alpha, alpha))

        output = bpy.data.images.new("CorridorKey Preview", width=width, height=height, alpha=True, float_buffer=True)
        try:
            output.pixels.foreach_set(preview)
            output.filepath_raw = image_path
            output.file_format = "OPEN_EXR" if image_path.lower().endswith(".exr") else "PNG"
            output.save()
        finally:
            bpy.data.images.remove(output)
    finally:
        bpy.data.images.remove(image)

