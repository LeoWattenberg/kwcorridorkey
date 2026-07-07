from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.check_call(cmd, cwd=str(cwd) if cwd else None, env=env)


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


def ensure_uv_command() -> list[str]:
    uv = shutil.which("uv")
    if uv:
        return [uv]

    scripts_uv = Path(sys.executable).resolve().parent / "Scripts" / "uv.exe"
    if scripts_uv.exists():
        return [str(scripts_uv)]

    try:
        subprocess.check_call([sys.executable, "-m", "uv", "--version"], stdout=subprocess.DEVNULL)
        return [sys.executable, "-m", "uv"]
    except subprocess.CalledProcessError:
        pass

    raise RuntimeError("uv is required. Install from https://docs.astral.sh/uv/ and rerun.")


def find_python_executable(root: Path) -> Path:
    names = ["python.exe"] if os.name == "nt" else ["python3", "python"]
    for name in names:
        for candidate in root.rglob(name):
            if ".venv" not in candidate.parts and candidate.is_file():
                return candidate
    raise RuntimeError(f"could not find Python executable under {root}")


def ensure_venv(
    root: Path,
    *,
    managed_python: bool = False,
    python_version: str | None = None,
    relocatable: bool = False,
) -> Path:
    uv = ensure_uv_command()
    python = root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if not python.exists():
        command = [*uv, "venv"]
        if relocatable:
            command.append("--relocatable")
        if managed_python:
            install_dir = root / "python"
            run([*uv, "python", "install", python_version or "3.10", "--install-dir", str(install_dir)])
            command.extend(["--python", str(find_python_executable(install_dir))])
        elif python_version:
            command.extend(["--python", python_version])
        command.append(str(root / ".venv"))
        run(command)
    return python


def install_worker(
    root: Path,
    editable: bool = False,
    managed_python: bool = False,
    python_version: str | None = None,
    relocatable: bool = False,
) -> None:
    uv = ensure_uv_command()
    worker_dir = REPO_ROOT / "worker"
    python = ensure_venv(
        root,
        managed_python=managed_python,
        python_version=python_version,
        relocatable=relocatable,
    )
    editable_args = ["-e"] if editable else []
    run([*uv, "pip", "install", "--python", str(python), *editable_args, str(worker_dir)])


def install_corridorkey(root: Path, source: Path, extra: str, editable: bool = False) -> None:
    uv = ensure_uv_command()
    python = root / ".venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    package = f"{source}[{extra}]" if extra != "cpu" else str(source)
    editable_args = ["-e"] if editable else []
    run([*uv, "pip", "install", "--python", str(python), *editable_args, package])


def main() -> int:
    parser = argparse.ArgumentParser(description="Install pinned external CorridorKey runtime")
    parser.add_argument("--root", default=str(REPO_ROOT / ".deps" / "corridorkey-runtime"))
    parser.add_argument("--lock", default=str(REPO_ROOT / "corridorkey.lock"))
    parser.add_argument("--with", dest="extra", choices=["cpu", "cuda", "mlx", "rocm"], default="cpu")
    parser.add_argument("--editable", action="store_true", help="Install local packages in editable mode for development")
    parser.add_argument("--managed-python", action="store_true", help="Install a uv-managed Python under the runtime root")
    parser.add_argument("--python-version")
    parser.add_argument("--relocatable-venv", action="store_true")
    args = parser.parse_args()

    root = Path(args.root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    lock = load_lock(Path(args.lock))
    source = ensure_checkout(root, lock)
    install_worker(
        root,
        editable=args.editable,
        managed_python=args.managed_python,
        python_version=args.python_version,
        relocatable=args.relocatable_venv,
    )
    install_corridorkey(root, source, args.extra, editable=args.editable)

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
