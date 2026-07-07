from __future__ import annotations

import io
import json
import struct
from typing import Any, BinaryIO


MAX_MESSAGE_BYTES = 16 * 1024 * 1024


class ProtocolError(RuntimeError):
    pass


def _read_exact(stream: BinaryIO, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise EOFError("unexpected EOF while reading framed message")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def encode_message(message: dict[str, Any]) -> bytes:
    payload = json.dumps(message, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    if len(payload) > MAX_MESSAGE_BYTES:
        raise ProtocolError(f"message too large: {len(payload)} bytes")
    return struct.pack("<I", len(payload)) + payload


def decode_message(data: bytes) -> dict[str, Any]:
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ProtocolError(f"invalid JSON message: {exc}") from exc
    if not isinstance(value, dict):
        raise ProtocolError("framed message must be a JSON object")
    return value


def read_message(stream: BinaryIO) -> dict[str, Any]:
    header = _read_exact(stream, 4)
    (length,) = struct.unpack("<I", header)
    if length > MAX_MESSAGE_BYTES:
        raise ProtocolError(f"message too large: {length} bytes")
    return decode_message(_read_exact(stream, length))


def write_message(stream: BinaryIO, message: dict[str, Any]) -> None:
    stream.write(encode_message(message))
    if hasattr(stream, "flush"):
        stream.flush()


def roundtrip_for_tests(message: dict[str, Any]) -> dict[str, Any]:
    stream = io.BytesIO(encode_message(message))
    return read_message(stream)

