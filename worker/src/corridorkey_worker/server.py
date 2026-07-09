from __future__ import annotations

import argparse
import io
import logging
import os
from pathlib import Path
import sys
import time
import traceback
from typing import Any, BinaryIO

from . import __version__
from .buffers import FrameBufferSpec, read_frame, write_frame
from .engine import create_engine, select_output
from .protocol import ProtocolError, read_message, write_message
from .settings import WorkerSettings


logger = logging.getLogger(__name__)


def configure_runtime_cache() -> None:
    if os.environ.get("CORRIDORKEY_CACHE_DIR"):
        cache_root = Path(os.environ["CORRIDORKEY_CACHE_DIR"]).resolve()
    else:
        executable = Path(sys.executable).resolve()
        runtime_root: Path | None = None
        for parent in executable.parents:
            if parent.name == ".venv":
                runtime_root = parent.parent
                break
        if runtime_root is None:
            return
        cache_root = runtime_root / "cache"

    os.environ.setdefault("XDG_CACHE_HOME", str(cache_root / "xdg"))
    os.environ.setdefault("HF_HOME", str(cache_root / "huggingface"))
    os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
    os.environ.setdefault("TORCH_HOME", str(cache_root / "torch"))
    os.environ.setdefault("NUMBA_CACHE_DIR", str(cache_root / "numba"))
    cache_root.mkdir(parents=True, exist_ok=True)


class WorkerServer:
    def __init__(self, input_stream: BinaryIO, output_stream: BinaryIO) -> None:
        configure_runtime_cache()
        self._input = input_stream
        self._output = output_stream
        self._settings = WorkerSettings()
        self._engine = create_engine()

    def serve_forever(self) -> None:
        while True:
            request = read_message(self._input)
            command = request.get("command")
            request_id = request.get("id")
            try:
                result = self._dispatch(str(command), request.get("payload") or {})
                write_message(self._output, {"id": request_id, "ok": True, "result": result})
                if command == "shutdown":
                    return
            except Exception as exc:  # noqa: BLE001 - protocol boundary
                logger.exception("Worker command failed: %s", command)
                write_message(
                    self._output,
                    {
                        "id": request_id,
                        "ok": False,
                        "error": {
                            "type": exc.__class__.__name__,
                            "message": str(exc),
                            "traceback": traceback.format_exc(),
                        },
                    },
                )

    def _dispatch(self, command: str, payload: dict[str, Any]) -> dict[str, Any]:
        if command == "hello":
            return {
                "worker": "corridorkey-worker",
                "version": __version__,
                "protocol": 1,
                "pid": os.getpid(),
                "fake_engine": os.environ.get("CORRIDORKEY_WORKER_FAKE_ENGINE") == "1",
                "settings": self._settings.asdict(),
            }
        if command == "configure":
            self._settings = WorkerSettings.from_payload(payload.get("settings"))
            return {"settings": self._settings.asdict()}
        if command == "preflight":
            settings = self._settings
            if "settings" in payload:
                settings = WorkerSettings.from_payload(payload.get("settings"))
            return self._engine.preflight(settings)
        if command == "process":
            return self._process(payload)
        if command == "shutdown":
            return {"shutdown": True}
        raise ProtocolError(f"unknown command: {command}")

    def _process(self, payload: dict[str, Any]) -> dict[str, Any]:
        settings = self._settings
        if "settings" in payload:
            settings = WorkerSettings.from_payload(payload.get("settings"))

        source_spec = FrameBufferSpec.from_payload(payload["source"])
        alpha_spec = FrameBufferSpec.from_payload(payload["alpha_hint"])
        output_spec = FrameBufferSpec.from_payload(payload["output"])

        started = time.monotonic()
        source = read_frame(source_spec)
        alpha_hint = read_frame(alpha_spec)
        raw_result = self._engine.process(source, alpha_hint, settings)
        output = select_output(raw_result, settings.output_mode)
        write_frame(output_spec, output)
        elapsed_ms = int((time.monotonic() - started) * 1000)

        return {
            "output": output_spec.asdict(),
            "output_mode": settings.output_mode,
            "screen_color": raw_result.get("screen_color", settings.screen_color),
            "elapsed_ms": elapsed_ms,
        }


def configure_logging(verbose: bool) -> None:
    logging.basicConfig(
        level=logging.DEBUG if verbose else logging.INFO,
        stream=sys.stderr,
        format="[%(levelname)s] %(name)s: %(message)s",
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="CorridorKey native plugin worker")
    parser.add_argument("--stdio", action="store_true", help="Use stdin/stdout framed protocol")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    configure_logging(args.verbose)
    if not args.stdio:
        parser.error("only --stdio transport is currently supported")

    input_stream = sys.stdin.buffer
    protocol_stdout_fd = os.dup(sys.stdout.fileno())
    output_stream = io.open(protocol_stdout_fd, "wb", closefd=True)
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    sys.stdout = sys.stderr
    WorkerServer(input_stream, output_stream).serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
