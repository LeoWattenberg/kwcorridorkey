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
os.environ.setdefault(
    "CORRIDORKEY_WORKER_PYTHON",
    str(ROOT / ".deps" / "corridorkey-runtime-check" / ".venv" / "Scripts" / "python.exe"),
)

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
            output = temp_path / "out"
            make_image(source, (0.0, 1.0, 0.0, 1.0))

            scene = bpy.context.scene
            tree = getattr(scene, "node_tree", None)
            if tree is None:
                tree = bpy.data.node_groups.new("CorridorKey Smoke Compositor", "CompositorNodeTree")
                scene.compositing_node_group = tree
            else:
                scene.use_nodes = True
            tree.nodes.clear()
            image_node = tree.nodes.new("CompositorNodeImage")
            image_node.image = bpy.data.images.load(str(source), check_existing=False)
            corridor_node = tree.nodes.new("CORRIDORKEY_ND_compositor_node")
            corridor_node.output_dir = str(output)
            corridor_node.start_frame = 1
            corridor_node.end_frame = 1
            corridor_node.select = True
            tree.nodes.active = corridor_node
            tree.links.new(image_node.outputs["Image"], corridor_node.inputs["Image"])

            result = bpy.ops.corridorkey.render_selected_node()
            if result != {"FINISHED"}:
                raise RuntimeError(f"unexpected operator result: {result}")
            for name in ("image", "preview", "matte"):
                expected = output / f"corridorkey_{name}_0001.exr"
                if not expected.exists():
                    raise RuntimeError(f"missing output: {expected}")
    finally:
        corridorkey_blender.unregister()


if __name__ == "__main__":
    main()
