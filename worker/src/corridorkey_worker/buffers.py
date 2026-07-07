from __future__ import annotations

import mmap
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np


SUPPORTED_DTYPES = {"float32": np.float32}


@dataclass(frozen=True)
class FrameBufferSpec:
    path: str
    width: int
    height: int
    channels: int
    dtype: str = "float32"
    byte_offset: int = 0

    @classmethod
    def from_payload(cls, payload: dict[str, Any]) -> "FrameBufferSpec":
        required = {"path", "width", "height", "channels"}
        missing = sorted(required - set(payload))
        if missing:
            raise ValueError(f"missing frame buffer keys: {', '.join(missing)}")
        spec = cls(
            path=str(payload["path"]),
            width=int(payload["width"]),
            height=int(payload["height"]),
            channels=int(payload["channels"]),
            dtype=str(payload.get("dtype", "float32")),
            byte_offset=int(payload.get("byte_offset", 0)),
        )
        spec.validate()
        return spec

    def validate(self) -> None:
        if self.dtype not in SUPPORTED_DTYPES:
            raise ValueError(f"unsupported dtype: {self.dtype}")
        if self.width <= 0 or self.height <= 0:
            raise ValueError("width and height must be positive")
        if self.channels not in (1, 3, 4):
            raise ValueError("channels must be 1, 3, or 4")
        if self.byte_offset < 0:
            raise ValueError("byte_offset must be non-negative")

    @property
    def byte_size(self) -> int:
        dtype = np.dtype(SUPPORTED_DTYPES[self.dtype])
        return self.width * self.height * self.channels * dtype.itemsize

    def asdict(self) -> dict[str, Any]:
        return {
            "path": self.path,
            "width": self.width,
            "height": self.height,
            "channels": self.channels,
            "dtype": self.dtype,
            "byte_offset": self.byte_offset,
        }


def read_frame(spec: FrameBufferSpec) -> np.ndarray:
    path = Path(spec.path)
    if not path.exists():
        raise FileNotFoundError(spec.path)
    if path.stat().st_size < spec.byte_offset + spec.byte_size:
        raise ValueError(f"frame buffer is too small: {spec.path}")

    dtype = SUPPORTED_DTYPES[spec.dtype]
    with path.open("rb") as file_obj:
        with mmap.mmap(file_obj.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            raw = np.ndarray(
                shape=(spec.height, spec.width, spec.channels),
                dtype=dtype,
                buffer=mm,
                offset=spec.byte_offset,
            )
            return np.array(raw, copy=True)


def write_frame(spec: FrameBufferSpec, frame: np.ndarray) -> None:
    spec.validate()
    frame = fit_channels(frame, spec.channels).astype(SUPPORTED_DTYPES[spec.dtype], copy=False)
    expected_shape = (spec.height, spec.width, spec.channels)
    if frame.shape != expected_shape:
        raise ValueError(f"output frame shape {frame.shape} does not match {expected_shape}")

    path = Path(spec.path)
    path.parent.mkdir(parents=True, exist_ok=True)
    required_size = spec.byte_offset + spec.byte_size
    with path.open("wb") as file_obj:
        file_obj.truncate(required_size)

    with path.open("r+b") as file_obj:
        with mmap.mmap(file_obj.fileno(), required_size, access=mmap.ACCESS_WRITE) as mm:
            mm[spec.byte_offset : spec.byte_offset + spec.byte_size] = np.ascontiguousarray(frame).tobytes()
            mm.flush()


def fit_channels(frame: np.ndarray, channels: int) -> np.ndarray:
    if frame.ndim == 2:
        frame = frame[:, :, np.newaxis]
    if frame.ndim != 3:
        raise ValueError("frame must be HxW or HxWxC")

    src_channels = frame.shape[2]
    if src_channels == channels:
        return frame
    if channels == 1:
        if src_channels == 1:
            return frame
        return frame[:, :, :1]
    if channels == 3:
        if src_channels == 1:
            return np.repeat(frame, 3, axis=2)
        return frame[:, :, :3]
    if channels == 4:
        if src_channels == 1:
            alpha = np.ones_like(frame)
            return np.concatenate([frame, frame, frame, alpha], axis=2)
        if src_channels == 3:
            alpha = np.ones(frame.shape[:2] + (1,), dtype=frame.dtype)
            return np.concatenate([frame, alpha], axis=2)
        return frame[:, :, :4]
    raise ValueError("channels must be 1, 3, or 4")


def remove_if_exists(path: str) -> None:
    try:
        os.remove(path)
    except FileNotFoundError:
        pass

