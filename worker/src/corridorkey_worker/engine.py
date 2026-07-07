from __future__ import annotations

import importlib
import logging
import os
import time
from dataclasses import dataclass
from typing import Any, Protocol

import numpy as np

from .settings import WorkerSettings

logger = logging.getLogger(__name__)


class Engine(Protocol):
    def preflight(self, settings: WorkerSettings) -> dict[str, Any]:
        ...

    def process(self, source: np.ndarray, alpha_hint: np.ndarray, settings: WorkerSettings) -> dict[str, Any]:
        ...


def create_engine() -> Engine:
    if os.environ.get("CORRIDORKEY_WORKER_FAKE_ENGINE") == "1":
        return FakeEngine()
    return CorridorKeyAdapter()


def resolve_screen_color(source: np.ndarray, alpha_hint: np.ndarray, requested: str) -> str:
    if requested in ("green", "blue"):
        return requested

    mask = alpha_hint[:, :, 0] if alpha_hint.ndim == 3 else alpha_hint
    background = source[:, :, :3][mask < 0.1]
    if background.size == 0:
        background = source[:, :, :3].reshape(-1, 3)
    means = background.mean(axis=0)
    return "blue" if means[2] > means[1] else "green"


def select_output(result: dict[str, np.ndarray], mode: str) -> np.ndarray:
    if mode == "processed_rgba":
        return result["processed"]
    if mode == "matte":
        return result["alpha"]
    if mode == "straight_fg":
        return result["fg"]
    if mode == "checker_comp":
        return result["comp"]
    raise ValueError(f"unsupported output mode: {mode}")


class FakeEngine:
    """Deterministic test engine that exercises the protocol without PyTorch."""

    def preflight(self, settings: WorkerSettings) -> dict[str, Any]:
        return {"engine": "fake", "settings": settings.asdict()}

    def process(self, source: np.ndarray, alpha_hint: np.ndarray, settings: WorkerSettings) -> dict[str, Any]:
        alpha = alpha_hint[:, :, :1] if alpha_hint.ndim == 3 else alpha_hint[:, :, np.newaxis]
        alpha = np.clip(alpha.astype(np.float32), 0.0, 1.0)
        rgb = np.clip(source[:, :, :3].astype(np.float32), 0.0, 1.0)
        screen_color = resolve_screen_color(rgb, alpha, settings.screen_color)
        spill_channel = 2 if screen_color == "blue" else 1
        fg = rgb.copy()
        other_mean = (np.sum(fg, axis=2, keepdims=True) - fg[:, :, spill_channel : spill_channel + 1]) / 2.0
        fg[:, :, spill_channel : spill_channel + 1] = (
            fg[:, :, spill_channel : spill_channel + 1] * (1.0 - settings.despill_strength)
            + other_mean * settings.despill_strength
        )
        comp = fg * alpha + (1.0 - alpha) * 0.18
        processed = np.concatenate([fg * alpha, alpha], axis=2)
        return {
            "alpha": alpha,
            "fg": fg,
            "comp": comp,
            "processed": processed,
            "screen_color": screen_color,
        }


@dataclass(frozen=True)
class _EngineKey:
    backend: str
    device: str
    inference_size: int
    tiled_inference: bool
    screen_color: str


class CorridorKeyAdapter:
    def __init__(self) -> None:
        self._engines: dict[_EngineKey, Any] = {}

    def preflight(self, settings: WorkerSettings) -> dict[str, Any]:
        colors = ["green", "blue"] if settings.screen_color == "auto" else [settings.screen_color]
        loaded: list[str] = []
        for color in colors:
            self._get_engine(settings, color)
            loaded.append(color)
        return {"engine": "corridorkey", "loaded_screen_colors": loaded}

    def process(self, source: np.ndarray, alpha_hint: np.ndarray, settings: WorkerSettings) -> dict[str, Any]:
        source_rgb = np.clip(source[:, :, :3].astype(np.float32), 0.0, 1.0)
        mask = alpha_hint[:, :, :1] if alpha_hint.ndim == 3 else alpha_hint[:, :, np.newaxis]
        mask = np.clip(mask.astype(np.float32), 0.0, 1.0)
        screen_color = resolve_screen_color(source_rgb, mask, settings.screen_color)
        engine = self._get_engine(settings, screen_color)
        screen_channel = 2 if screen_color == "blue" else 1
        result = engine.process_frame(
            source_rgb,
            mask,
            refiner_scale=settings.refiner,
            input_is_linear=settings.input_is_linear,
            fg_is_straight=True,
            despill_strength=settings.despill_strength,
            auto_despeckle=settings.auto_despeckle,
            despeckle_size=settings.despeckle_size,
            screen_channel=screen_channel,
        )
        result["screen_color"] = screen_color
        return result

    def _get_engine(self, settings: WorkerSettings, screen_color: str) -> Any:
        key = _EngineKey(
            backend=settings.backend,
            device=settings.device,
            inference_size=settings.inference_size,
            tiled_inference=settings.tiled_inference,
            screen_color=screen_color,
        )
        cached = self._engines.get(key)
        if cached is not None:
            return cached

        started = time.monotonic()
        try:
            backend_module = importlib.import_module("CorridorKeyModule.backend")
        except ImportError as exc:
            raise RuntimeError(
                "CorridorKey is not importable. Run scripts/install_corridorkey.py and launch the worker "
                "from that environment, or set PYTHONPATH to a pinned CorridorKey checkout."
            ) from exc

        backend = None if settings.backend == "auto" else settings.backend
        device = None if settings.device == "auto" else settings.device
        tile_size = 512 if settings.tiled_inference else None
        engine = backend_module.create_engine(
            backend=backend,
            device=device,
            img_size=settings.inference_size,
            tile_size=tile_size,
            screen_color=screen_color,
        )
        self._engines[key] = engine
        logger.info("Loaded CorridorKey engine %s in %.2fs", key, time.monotonic() - started)
        return engine

