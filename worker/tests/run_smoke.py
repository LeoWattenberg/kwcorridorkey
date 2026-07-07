from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "worker" / "src"))

from corridorkey_worker.buffers import FrameBufferSpec, read_frame, write_frame  # noqa: E402
from corridorkey_worker.protocol import read_message, roundtrip_for_tests, write_message  # noqa: E402
from corridorkey_worker.settings import WorkerSettings  # noqa: E402


def request(proc, command, payload=None, request_id=1):
    write_message(proc.stdin, {"id": request_id, "command": command, "payload": payload or {}})
    response = read_message(proc.stdout)
    if not response["ok"]:
        raise RuntimeError(response)
    return response["result"]


def main() -> int:
    assert roundtrip_for_tests({"id": 1, "command": "hello"})["command"] == "hello"
    WorkerSettings.from_payload({"output_mode": "matte"}).validate()

    with tempfile.TemporaryDirectory() as temp:
        temp_path = Path(temp)
        frame = np.arange(2 * 3 * 4, dtype=np.float32).reshape(2, 3, 4)
        spec = FrameBufferSpec(str(temp_path / "frame.raw"), 3, 2, 4)
        write_frame(spec, frame)
        np.testing.assert_array_equal(read_frame(spec), frame)

        env = os.environ.copy()
        env["PYTHONPATH"] = str(ROOT / "worker" / "src")
        env["CORRIDORKEY_WORKER_FAKE_ENGINE"] = "1"
        proc = subprocess.Popen(
            [sys.executable, "-m", "corridorkey_worker", "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            assert request(proc, "hello", request_id=1)["fake_engine"] is True
            source_spec = FrameBufferSpec(str(temp_path / "source.raw"), 3, 2, 4)
            alpha_spec = FrameBufferSpec(str(temp_path / "alpha.raw"), 3, 2, 1)
            output_spec = FrameBufferSpec(str(temp_path / "output.raw"), 3, 2, 4)
            write_frame(source_spec, np.ones((2, 3, 4), dtype=np.float32))
            write_frame(alpha_spec, np.full((2, 3, 1), 0.5, dtype=np.float32))
            result = request(
                proc,
                "process",
                {
                    "settings": {"screen_color": "green", "output_mode": "processed_rgba"},
                    "source": source_spec.asdict(),
                    "alpha_hint": alpha_spec.asdict(),
                    "output": output_spec.asdict(),
                },
                request_id=2,
            )
            assert result["screen_color"] == "green"
            assert read_frame(output_spec).shape == (2, 3, 4)
            request(proc, "shutdown", request_id=3)
        finally:
            proc.terminate()
            proc.wait(timeout=5)

    print("worker smoke passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

