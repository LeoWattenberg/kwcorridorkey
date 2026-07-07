import numpy as np

from corridorkey_worker.engine import FakeEngine, select_output
from corridorkey_worker.settings import WorkerSettings


def test_fake_engine_processes_all_output_modes():
    source = np.ones((4, 4, 4), dtype=np.float32)
    source[:, :, 1] = 0.8
    alpha = np.full((4, 4, 1), 0.5, dtype=np.float32)
    engine = FakeEngine()
    result = engine.process(source, alpha, WorkerSettings(screen_color="green"))

    assert result["processed"].shape == (4, 4, 4)
    assert result["alpha"].shape == (4, 4, 1)
    assert result["fg"].shape == (4, 4, 3)
    assert result["comp"].shape == (4, 4, 3)
    assert select_output(result, "processed_rgba").shape == (4, 4, 4)

