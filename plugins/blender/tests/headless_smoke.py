"""Run with:

blender --background --python plugins/blender/tests/headless_smoke.py
"""

from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path

import bpy

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "plugins" / "blender"))
sys.path.insert(0, str(ROOT / "worker" / "src"))
os.environ["CORRIDORKEY_WORKER_FAKE_ENGINE"] = "1"

import corridorkey_blender  # noqa: E402


def make_image(path: Path, color: tuple[float, float, float, float]) -> None:
    image = bpy.data.images.new(path.stem, width=2, height=2, alpha=True, float_buffer=True)
    image.pixels.foreach_set(color * 4)
    image.filepath_raw = str(path)
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def main() -> None:
    corridorkey_blender.register()
    try:
        with tempfile.TemporaryDirectory() as temp:
            temp_path = Path(temp)
            source = temp_path / "source_0001.png"
            alpha = temp_path / "alpha_0001.png"
            output = temp_path / "out"
            make_image(source, (0.0, 1.0, 0.0, 1.0))
            make_image(alpha, (0.5, 0.5, 0.5, 1.0))

            settings = bpy.context.scene.corridorkey
            settings.source_path = str(source)
            settings.alpha_path = str(alpha)
            settings.output_dir = str(output)
            settings.start_frame = 1
            settings.end_frame = 1
            result = bpy.ops.corridorkey.render_sequence()
            if result != {"FINISHED"}:
                raise RuntimeError(f"unexpected operator result: {result}")
            expected = output / "corridorkey_processed_rgba_0001.exr"
            if not expected.exists():
                raise RuntimeError(f"missing output: {expected}")
    finally:
        corridorkey_blender.unregister()


if __name__ == "__main__":
    main()

