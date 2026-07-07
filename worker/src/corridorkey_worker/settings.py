from __future__ import annotations

from dataclasses import asdict, dataclass
from typing import Any


SCREEN_COLORS = {"auto", "green", "blue"}
COLORSPACES = {"srgb", "linear"}
OUTPUT_MODES = {"processed_rgba", "matte", "straight_fg", "checker_comp"}
BACKENDS = {"auto", "torch", "mlx"}
DEVICES = {"auto", "cuda", "mps", "cpu", "rocm"}
INFERENCE_SIZES = {512, 1024, 2048}


@dataclass(frozen=True)
class WorkerSettings:
    screen_color: str = "auto"
    input_colorspace: str = "srgb"
    despill: int = 5
    auto_despeckle: bool = True
    despeckle_size: int = 400
    refiner: float = 1.0
    inference_size: int = 2048
    backend: str = "auto"
    device: str = "auto"
    output_mode: str = "processed_rgba"
    tiled_inference: bool = False

    @classmethod
    def from_payload(cls, payload: dict[str, Any] | None) -> "WorkerSettings":
        if payload is None:
            return cls()
        known = {field.name for field in cls.__dataclass_fields__.values()}  # type: ignore[attr-defined]
        unknown = sorted(set(payload) - known)
        if unknown:
            raise ValueError(f"Unknown settings keys: {', '.join(unknown)}")
        settings = cls(**payload)
        settings.validate()
        return settings

    def validate(self) -> None:
        if self.screen_color not in SCREEN_COLORS:
            raise ValueError(f"screen_color must be one of {sorted(SCREEN_COLORS)}")
        if self.input_colorspace not in COLORSPACES:
            raise ValueError(f"input_colorspace must be one of {sorted(COLORSPACES)}")
        if not 0 <= int(self.despill) <= 10:
            raise ValueError("despill must be an integer from 0 to 10")
        if int(self.despeckle_size) < 0:
            raise ValueError("despeckle_size must be non-negative")
        if float(self.refiner) <= 0:
            raise ValueError("refiner must be positive")
        if int(self.inference_size) not in INFERENCE_SIZES:
            raise ValueError(f"inference_size must be one of {sorted(INFERENCE_SIZES)}")
        if self.backend not in BACKENDS:
            raise ValueError(f"backend must be one of {sorted(BACKENDS)}")
        if self.device not in DEVICES:
            raise ValueError(f"device must be one of {sorted(DEVICES)}")
        if self.output_mode not in OUTPUT_MODES:
            raise ValueError(f"output_mode must be one of {sorted(OUTPUT_MODES)}")

    def asdict(self) -> dict[str, Any]:
        return asdict(self)

    @property
    def input_is_linear(self) -> bool:
        return self.input_colorspace == "linear"

    @property
    def despill_strength(self) -> float:
        return max(0.0, min(1.0, int(self.despill) / 10.0))

