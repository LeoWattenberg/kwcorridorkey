from __future__ import annotations

import argparse
import json
import subprocess
from datetime import date
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Resolve and update corridorkey.lock")
    parser.add_argument("--lock", default="corridorkey.lock")
    args = parser.parse_args()

    lock_path = Path(args.lock)
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    repo = lock["repo"]
    ref = lock.get("ref", "main")
    output = subprocess.check_output(["git", "ls-remote", repo, f"refs/heads/{ref}"], text=True)
    commit = output.split()[0]
    lock["commit"] = commit
    lock["resolved_at"] = date.today().isoformat()
    lock_path.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")
    print(commit)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

