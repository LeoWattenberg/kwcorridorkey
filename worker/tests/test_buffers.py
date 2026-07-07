import numpy as np

from corridorkey_worker.buffers import FrameBufferSpec, read_frame, write_frame


def test_raw_float_frame_roundtrip(tmp_path):
    path = tmp_path / "frame.raw"
    frame = np.arange(2 * 3 * 4, dtype=np.float32).reshape(2, 3, 4)
    spec = FrameBufferSpec(str(path), width=3, height=2, channels=4)
    write_frame(spec, frame)
    np.testing.assert_array_equal(read_frame(spec), frame)

