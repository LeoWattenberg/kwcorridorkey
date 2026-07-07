# CorridorKey Resolve OpenFX Plugin

This target builds `CorridorKeyResolve` as an OpenFX image effect for DaVinci Resolve and other OFX hosts.

Build with:

```powershell
cmake --preset resolve
cmake --build --preset resolve
```

The plugin bundle is emitted at `build/resolve/CorridorKeyResolve.ofx.bundle`.

The effect defines:

- `Source` clip input
- `AlphaHint` clip input
- `Output` clip
- CorridorKey settings matching the shared worker protocol

The plugin expects float RGBA buffers from the host. It writes temporary raw float32 frame files, calls `corridorkey-worker`, and copies the selected output mode back into the OFX output image.

Set `CORRIDORKEY_WORKER_PYTHON` to the Python executable printed by `scripts/install_corridorkey.py`.
