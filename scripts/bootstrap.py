#!/usr/bin/env python3
"""Fetch exact upstream revisions. Never reset or overwrite existing vendor work."""
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def git(*args, cwd=ROOT):
    return subprocess.check_output(["git", *args], cwd=cwd, text=True).strip()

def main():
    lock = json.loads((ROOT / "vendor.lock.json").read_text())
    for repo in lock["repositories"]:
        path = ROOT / repo["path"]
        if path.exists():
            if git("rev-parse", "HEAD", cwd=path) != repo["commit"] or git("status", "--porcelain", cwd=path):
                raise SystemExit(f"{path}: revision mismatch or local changes; refusing to modify")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            git("clone", "--no-checkout", "--filter=blob:none", repo["url"], str(path))
            git("fetch", "--depth", "1", "origin", repo["commit"], cwd=path)
            git("checkout", "--detach", repo["commit"], cwd=path)
        print(repo["path"], repo["commit"])

if __name__ == "__main__":
    main()
