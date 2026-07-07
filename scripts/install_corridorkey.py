from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], cwd: Path | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None)


def load_lock(path: Path) -> dict[str, str]:
    return json.loads(path.read_text(encoding="utf-8"))


def ensure_checkout(root: Path, lock: dict[str, str]) -> Path:
    source = root / "CorridorKey"
    repo = lock["repo"]
    commit = lock["commit"]
    if not source.exists():
        run(["git", "clone", "--filter=blob:none", repo, str(source)])
    run(["git", "fetch", "--depth", "1", "origin", commit], cwd=source)
    run(["git", "checkout", "--detach", commit], cwd=source)
    actual = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    if actual != commit:
        raise RuntimeError(f"expected CorridorKey commit {commit}, got {actual}")
    return source


def ensure_uv() -> str:
    uv = shutil.which("uv")
    if uv:
        return uv
    raise RuntimeError("uv is required. Install from https://docs.astral.sh/uv/ and rerun.")


def install_worker(root: Path) -> None:
    uv = ensure_uv()
    worker_dir = REPO_ROOT / "worker"
    run([uv, "venv", str(root / ".venv")])
    python = root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    run([str(python), "-m", "pip", "install", "-e", str(worker_dir)])


def install_corridorkey(root: Path, source: Path, extra: str) -> None:
    uv = ensure_uv()
    python = root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    package = f"{source}[{extra}]" if extra != "cpu" else str(source)
    run([uv, "pip", "install", "--python", str(python), "-e", package])


def main() -> int:
    parser = argparse.ArgumentParser(description="Install pinned external CorridorKey runtime")
    parser.add_argument("--root", default=str(REPO_ROOT / ".deps" / "corridorkey-runtime"))
    parser.add_argument("--lock", default=str(REPO_ROOT / "corridorkey.lock"))
    parser.add_argument("--with", dest="extra", choices=["cpu", "cuda", "mlx", "rocm"], default="cpu")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    lock = load_lock(Path(args.lock))
    source = ensure_checkout(root, lock)
    install_worker(root)
    install_corridorkey(root, source, args.extra)

    python = root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    print("\nWorker Python:")
    print(python)
    print("\nSet CORRIDORKEY_WORKER_PYTHON to this path for host plugins.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # noqa: BLE001
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)

