from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import install_corridorkey


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUNDLE = REPO_ROOT / "build" / "resolve" / "CorridorKeyResolve.ofx.bundle"


def default_ofx_root() -> Path:
    if os.name == "nt":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        return Path(program_files) / "Common Files" / "OFX" / "Plugins"
    if sys.platform == "darwin":
        return Path("/Library/OFX/Plugins")
    return Path("/usr/OFX/Plugins")


def runtime_python(runtime_root: Path) -> Path:
    return runtime_root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")


def runtime_worker_python(runtime_root: Path) -> Path:
    names = ["python.exe"] if os.name == "nt" else ["python3", "python"]
    python_root = runtime_root / "python"
    if python_root.exists():
        for child in python_root.iterdir():
            if child.is_dir():
                candidate = child / ("python.exe" if os.name == "nt" else "bin/python")
                if candidate.is_file():
                    return candidate
        for name in names:
            for candidate in python_root.rglob(name):
                parts = {part.lower() for part in candidate.parts}
                if ".venv" not in parts and "venv" not in parts and candidate.is_file():
                    return candidate
    return runtime_python(runtime_root)


def runtime_environment(runtime_root: Path) -> dict[str, str]:
    cache_root = runtime_root / "cache"
    env = os.environ.copy()
    env.setdefault("CORRIDORKEY_CACHE_DIR", str(cache_root))
    env.setdefault("XDG_CACHE_HOME", str(cache_root / "xdg"))
    env.setdefault("HF_HOME", str(cache_root / "huggingface"))
    env.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")
    env.setdefault("TORCH_HOME", str(cache_root / "torch"))
    env.setdefault("NUMBA_CACHE_DIR", str(cache_root / "numba"))
    return env


def copy_bundle(source: Path, target: Path, force: bool) -> None:
    if not source.exists():
        raise FileNotFoundError(f"bundle not found: {source}")
    if source.resolve() == target.resolve():
        return
    if target.exists():
        if not force:
            raise FileExistsError(f"{target} already exists; rerun with --force to replace it")
        shutil.rmtree(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, target)


def copy_metadata(resources_dir: Path, lock: dict[str, str], extra: str) -> None:
    resources_dir.mkdir(parents=True, exist_ok=True)
    for name in ("THIRD_PARTY_NOTICES.md", "corridorkey.lock"):
        source = REPO_ROOT / name
        if source.exists():
            shutil.copy2(source, resources_dir / name)
    manifest = {
        "name": "CorridorKeyResolve",
        "runtime": "corridorkey-runtime",
        "corridorkey": lock,
        "dependency_extra": extra,
        "worker_python": str(runtime_worker_python(resources_dir / "corridorkey-runtime")),
        "venv_python": str(runtime_python(resources_dir / "corridorkey-runtime")),
    }
    (resources_dir / "corridorkey-runtime-manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
    )


def verify_runtime(runtime_root: Path) -> None:
    python = runtime_python(runtime_root)
    code = (
        "import importlib;"
        "import corridorkey_worker;"
        "importlib.import_module('CorridorKeyModule.backend');"
        "print('CorridorKey worker runtime import check passed')"
    )
    install_corridorkey.run([str(python), "-c", code], cwd=runtime_root, env=runtime_environment(runtime_root))


def preflight_models(runtime_root: Path, backend: str, device: str, inference_size: int) -> None:
    python = runtime_python(runtime_root)
    code = (
        "from corridorkey_worker.server import configure_runtime_cache;"
        "from corridorkey_worker.engine import CorridorKeyAdapter;"
        "from corridorkey_worker.settings import WorkerSettings;"
        "configure_runtime_cache();"
        f"settings = WorkerSettings(backend={backend!r}, device={device!r}, inference_size={inference_size!r});"
        "print(CorridorKeyAdapter().preflight(settings))"
    )
    install_corridorkey.run([str(python), "-c", code], cwd=runtime_root, env=runtime_environment(runtime_root))


def install_runtime(runtime_root: Path, lock_path: Path, extra: str, python_version: str) -> dict[str, str]:
    runtime_root.mkdir(parents=True, exist_ok=True)
    lock = install_corridorkey.load_lock(lock_path)
    source = install_corridorkey.ensure_checkout(runtime_root, lock)
    install_corridorkey.install_worker(
        runtime_root,
        editable=False,
        managed_python=True,
        python_version=python_version,
        relocatable=True,
    )
    install_corridorkey.install_corridorkey(runtime_root, source, extra, editable=False)
    return lock


def main() -> int:
    parser = argparse.ArgumentParser(description="Install CorridorKey Resolve OFX bundle with a bundle-local runtime")
    parser.add_argument("--bundle", default=str(DEFAULT_BUNDLE), help="Built CorridorKeyResolve.ofx.bundle to install")
    parser.add_argument("--install-root", default=str(default_ofx_root()), help="OFX plugin directory")
    parser.add_argument("--target", help="Full destination .ofx.bundle path; overrides --install-root")
    parser.add_argument("--in-place", action="store_true", help="Provision the runtime inside --bundle without copying it")
    parser.add_argument("--lock", default=str(REPO_ROOT / "corridorkey.lock"))
    parser.add_argument("--with", dest="extra", choices=["cpu", "cuda", "mlx", "rocm"], default="cpu")
    parser.add_argument("--python-version", default="3.10", help="uv-managed Python version to install into the bundle")
    parser.add_argument("--force", action="store_true", help="Replace an existing destination bundle")
    parser.add_argument("--skip-import-check", action="store_true")
    parser.add_argument("--preflight-models", action="store_true", help="Load CorridorKey engines once so model files are cached")
    parser.add_argument("--backend", choices=["auto", "torch", "mlx"], default="auto")
    parser.add_argument("--device", choices=["auto", "cuda", "mps", "cpu", "rocm"], default="auto")
    parser.add_argument("--inference-size", type=int, choices=[512, 1024, 2048], default=512)
    args = parser.parse_args()

    source_bundle = Path(args.bundle).resolve()
    if args.in_place:
        target_bundle = source_bundle
    elif args.target:
        target_bundle = Path(args.target).resolve()
    else:
        target_bundle = Path(args.install_root).resolve() / source_bundle.name

    copy_bundle(source_bundle, target_bundle, args.force)
    resources_dir = target_bundle / "Contents" / "Resources"
    runtime_root = resources_dir / "corridorkey-runtime"
    lock = install_runtime(runtime_root, Path(args.lock), args.extra, args.python_version)
    copy_metadata(resources_dir, lock, args.extra)

    python = runtime_worker_python(runtime_root)
    if not args.skip_import_check:
        verify_runtime(runtime_root)
    if args.preflight_models:
        preflight_models(runtime_root, args.backend, args.device, args.inference_size)

    print("\nInstalled CorridorKey Resolve bundle:")
    print(target_bundle)
    print("\nBundle-local worker Python:")
    print(python)
    print("\nThe OFX plugin will use this bundle-local runtime unless CORRIDORKEY_WORKER_PYTHON is set.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
