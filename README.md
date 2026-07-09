# CorridorKey Native Host Plugins

Native host integrations for [CorridorKey](https://github.com/nikopueringer/CorridorKey) using a shared worker bridge.

This repository does not vendor CorridorKey. Build and install tooling resolves the pinned upstream ref in `corridorkey.lock`, provisions a local worker environment, and keeps all downloaded sources and model files outside tracked source.

## Components

- `worker/`: Python worker package exposing the length-prefixed JSON protocol and CorridorKey engine adapter.
- `native/`: C++ client library used by host plugins.
- WORKING `plugins/resolve/`: OpenFX effect for DaVinci Resolve.
- NOT IMPLEMENTED `plugins/premiere/`: Adobe Premiere/After Effects-compatible C++ effect.  
- PARTIALLY WORKING `plugins/blender/`: Blender add-on with VSE/compositor workflow operators.
- `scripts/`: dependency resolution, worker installation, and packaging helpers.

## Build

```powershell
cmake --preset resolve
cmake --build --preset resolve
```

Standard build directories are:

- `build/native`: shared C++ client and native tests
- `build/resolve`: DaVinci Resolve OpenFX plugin
- `build/premiere`: Adobe Premiere plugin
- `build/blender`: Blender add-on package
- `build/all`: all integrations available on the current machine

The Resolve bundle is emitted at `build/resolve/CorridorKeyResolve.ofx.bundle`. The Blender package target emits `build/blender/CorridorKeyBlender.zip`.

The OpenFX SDK and C++ helper dependencies are fetched at configure/build time. The Adobe SDK is not redistributed; set `ADOBE_AE_SDK_ROOT` or `ADOBE_PREMIERE_SDK_ROOT` to build the Premiere plugin.

Useful preset commands:

```powershell
cmake --preset native
cmake --build --preset native
ctest --preset native

cmake --preset resolve
cmake --build --preset resolve
ctest --preset resolve

cmake --preset blender
cmake --build --preset blender
```

Install the Resolve OFX bundle with a bundle-local CorridorKey runtime:

```powershell
python scripts/install_resolve_ofx_bundle.py --with cuda --force
```

By default this installs to the system OFX plugin directory and provisions a uv-managed Python, CorridorKey, the worker package, dependencies, notices, and cache directory under `CorridorKeyResolve.ofx.bundle/Contents/Resources/corridorkey-runtime`. The plugin uses that bundled runtime automatically unless `CORRIDORKEY_WORKER_PYTHON` is set.

To create a self-contained build artifact without copying it to the system plugin directory:

```powershell
python scripts/install_resolve_ofx_bundle.py --in-place --with cuda --preflight-models
```

Install the external CorridorKey worker environment:

```powershell
python scripts/install_corridorkey.py --root .deps/corridorkey-runtime --with cuda
```

Use `--with cpu`, `--with cuda`, `--with mlx`, or `--with rocm` to choose the CorridorKey dependency extra.

## Worker Protocol

Messages are UTF-8 JSON framed by a 4-byte little-endian unsigned length. The worker supports:

- `hello`
- `configure`
- `preflight`
- `process`
- `shutdown`

Frame data is exchanged via raw float32 files that the worker memory maps. Inputs are HWC source RGB/RGBA and a coarse alpha hint. Outputs are selected with `processed_rgba`, `matte`, `straight_fg`, or `checker_comp`.

## License Notice

CorridorKey currently has a licensing bug, where both his custom license and CC-by-nc-sa state that the other license isn't valid. As such, the fully compiled project may not be distributed currently. See `THIRD_PARTY_NOTICES.md` and the upstream license before distributing these plugins.
