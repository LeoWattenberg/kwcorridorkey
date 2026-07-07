import os
import subprocess
import sys

import numpy as np

from corridorkey_worker.buffers import FrameBufferSpec, read_frame, write_frame
from corridorkey_worker.protocol import read_message, write_message


def request(proc, command, payload=None, request_id=1):
    write_message(proc.stdin, {"id": request_id, "command": command, "payload": payload or {}})
    response = read_message(proc.stdout)
    assert response["ok"], response
    return response["result"]


def test_worker_process_command_with_fake_engine(tmp_path):
    env = os.environ.copy()
    env["CORRIDORKEY_WORKER_FAKE_ENGINE"] = "1"
    proc = subprocess.Popen(
        [sys.executable, "-m", "corridorkey_worker", "--stdio"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
    )
    try:
        hello = request(proc, "hello", request_id=1)
        assert hello["fake_engine"] is True

        source_spec = FrameBufferSpec(str(tmp_path / "source.raw"), 3, 2, 4)
        alpha_spec = FrameBufferSpec(str(tmp_path / "alpha.raw"), 3, 2, 1)
        output_spec = FrameBufferSpec(str(tmp_path / "output.raw"), 3, 2, 4)
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
        output = read_frame(output_spec)
        assert output.shape == (2, 3, 4)
        assert np.all(output[:, :, 3] == 0.5)
        request(proc, "shutdown", request_id=3)
    finally:
        proc.terminate()
        proc.wait(timeout=5)

