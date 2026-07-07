from __future__ import annotations

import array
import os
from pathlib import Path

import bpy

from .worker_client import FrameSpec


def load_image_to_raw(path: str, raw_path: str) -> FrameSpec:
    image = bpy.data.images.load(path, check_existing=False)
    try:
        width, height = image.size
        pixels = array.array("f", image.pixels[:])
        Path(raw_path).parent.mkdir(parents=True, exist_ok=True)
        with open(raw_path, "wb") as handle:
            pixels.tofile(handle)
        return FrameSpec(raw_path, width=width, height=height, channels=4)
    finally:
        bpy.data.images.remove(image)


def save_raw_to_image(raw_path: str, image_path: str, width: int, height: int, channels: int) -> None:
    with open(raw_path, "rb") as handle:
        values = array.array("f")
        values.fromfile(handle, width * height * channels)
    if channels == 1:
        rgba = array.array("f")
        for value in values:
            rgba.extend((value, value, value, 1.0))
    elif channels == 3:
        rgba = array.array("f")
        for index in range(0, len(values), 3):
            rgba.extend((values[index], values[index + 1], values[index + 2], 1.0))
    else:
        rgba = values

    image = bpy.data.images.new("CorridorKey Output", width=width, height=height, alpha=True, float_buffer=True)
    try:
        image.pixels.foreach_set(rgba)
        image.filepath_raw = image_path
        image.file_format = "OPEN_EXR" if image_path.lower().endswith(".exr") else "PNG"
        image.save()
    finally:
        bpy.data.images.remove(image)


def resolve_frame_path(template: str, frame: int) -> str:
    if "#" in template:
        count = template.count("#")
        return template.replace("#" * count, str(frame).zfill(count))
    if "{" in template:
        return template.format(frame=frame)
    return template


def output_frame_path(output_dir: str, frame: int, mode: str) -> str:
    suffix = "exr" if mode in {"processed_rgba", "matte", "straight_fg"} else "png"
    return os.path.join(output_dir, f"corridorkey_{mode}_{frame:04d}.{suffix}")

