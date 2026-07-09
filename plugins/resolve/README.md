# CorridorKey Resolve OpenFX Plugin

This target builds `CorridorKeyResolve` as an OpenFX image effect for DaVinci Resolve and other OFX hosts.

Build with:

```powershell
cmake --preset resolve
cmake --build --preset resolve
```

The plugin bundle is emitted at `build/resolve/CorridorKeyResolve.ofx.bundle`.

Install it with a bundle-local CorridorKey runtime:

```powershell
python scripts/install_resolve_ofx_bundle.py --with cuda --force
```

The installer copies the bundle to the OFX plugin directory and provisions a uv-managed Python, CorridorKey, the worker package, dependencies, notices, and cache directory under `Contents/Resources/corridorkey-runtime`. The plugin will use that runtime automatically unless `CORRIDORKEY_WORKER_PYTHON` is set. On Windows, the plugin launches the bundle-local uv-managed `python.exe` directly and injects the bundled venv's `site-packages`, so the installed bundle remains relocatable after copying.

The effect defines:

- `Source` clip input
- `AlphaHint` clip input
- `Output` clip
- CorridorKey settings matching the shared worker protocol

The plugin expects float RGBA buffers from the host. It writes temporary raw float32 frame files, calls `corridorkey-worker`, and copies the selected output mode back into the OFX output image.

For development-only external runtimes, set `CORRIDORKEY_WORKER_PYTHON` to the Python executable printed by `scripts/install_corridorkey.py`.
