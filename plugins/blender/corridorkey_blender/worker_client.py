from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
from dataclasses import dataclass
from typing import Any


@dataclass
class FrameSpec:
    path: str
    width: int
    height: int
    channels: int
    dtype: str = "float32"
    byte_offset: int = 0

    def asdict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "width": self.width,
            "height": self.height,
            "channels": self.channels,
            "dtype": self.dtype,
            "byte_offset": self.byte_offset,
        }


class WorkerClient:
    def __init__(self, python_executable: str | None = None) -> None:
        python = python_executable or os.environ.get("CORRIDORKEY_WORKER_PYTHON") or sys.executable
        self._proc = subprocess.Popen(
            [python, "-m", "corridorkey_worker", "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=None,
        )
        self._next_id = 1

    def close(self) -> None:
        if self._proc.poll() is None:
            try:
                self.request("shutdown", {})
            except OSError:
                pass
        self._proc.wait(timeout=5)

    def request(self, command: str, payload: dict[str, Any]) -> dict[str, Any]:
        if self._proc.stdin is None or self._proc.stdout is None:
            raise RuntimeError("worker pipes are closed")
        request_id = self._next_id
        self._next_id += 1
        body = json.dumps({"id": request_id, "command": command, "payload": payload}, separators=(",", ":")).encode()
        self._proc.stdin.write(struct.pack("<I", len(body)))
        self._proc.stdin.write(body)
        self._proc.stdin.flush()

        header = self._proc.stdout.read(4)
        if len(header) != 4:
            raise RuntimeError("worker exited before sending a response")
        (length,) = struct.unpack("<I", header)
        response = json.loads(self._proc.stdout.read(length).decode())
        if not response.get("ok"):
            error = response.get("error") or {}
            raise RuntimeError(error.get("message", "worker command failed"))
        return response.get("result") or {}

    def process(self, settings: dict[str, Any], source: FrameSpec, alpha: FrameSpec, output: FrameSpec) -> dict[str, Any]:
        return self.request(
            "process",
            {
                "settings": settings,
                "source": source.asdict(),
                "alpha_hint": alpha.asdict(),
                "output": output.asdict(),
            },
        )
