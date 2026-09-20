#!/usr/bin/env python3
"""Build the omle_server C++ binary and install it into the Python package.

Usage
-----
    python scripts/build.py [--build-type Release|Debug] [--install]

Steps
-----
1. Run CMake configure + build in ``build/`` (Release by default).
2. Copy the compiled ``omle_server`` binary into
   ``python/omle_server/bin/``.
3. Optionally ``pip install -e python/`` so the package is immediately usable.
"""
from __future__ import annotations

import argparse
import platform
import shutil
import subprocess
import sys
from pathlib import Path

ROOT      = Path(__file__).parent.parent.resolve()
BUILD_DIR = ROOT / "build"
BIN_DIR   = ROOT / "python" / "omle_server" / "bin"

BINARY_NAME = "omle_server.exe" if sys.platform == "win32" else "omle_server"


def run(cmd: list[str], **kwargs) -> None:
    print(f"  $ {' '.join(str(c) for c in cmd)}")
    subprocess.run(cmd, check=True, **kwargs)


def cmake_path() -> str:
    p = shutil.which("cmake")
    if not p:
        raise SystemExit("cmake not found on PATH. Please install CMake 3.21+.")
    return p


def main() -> None:
    parser = argparse.ArgumentParser(description="Build omle-server")
    parser.add_argument(
        "--build-type", default="Release",
        choices=["Release", "Debug", "RelWithDebInfo"],
        help="CMake build type (default: Release)",
    )
    parser.add_argument(
        "--install", action="store_true",
        help="Run `pip install -e python/` after copying the binary",
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=None,
        help="Parallel build jobs (default: number of CPUs)",
    )
    args = parser.parse_args()

    cmake = cmake_path()
    jobs  = args.jobs or 0  # 0 → cmake picks cpu count

    print(f"\n── Configure ({'already done' if (BUILD_DIR / 'CMakeCache.txt').exists() else 'fresh'}) ──")
    BUILD_DIR.mkdir(exist_ok=True)
    run([cmake, str(ROOT),
         f"-DCMAKE_BUILD_TYPE={args.build_type}",
         "-B", str(BUILD_DIR)])

    print(f"\n── Build ({args.build_type}) ──")
    build_cmd = [cmake, "--build", str(BUILD_DIR), "--target", "omle_server"]
    if jobs:
        build_cmd += ["--parallel", str(jobs)]
    else:
        build_cmd += ["--parallel"]
    run(build_cmd)

    binary_src = BUILD_DIR / BINARY_NAME
    if not binary_src.exists():
        raise SystemExit(f"Build succeeded but binary not found at {binary_src}")

    print(f"\n── Install binary → {BIN_DIR / BINARY_NAME} ──")
    BIN_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binary_src, BIN_DIR / BINARY_NAME)
    if sys.platform != "win32":
        (BIN_DIR / BINARY_NAME).chmod(0o755)
    print(f"  Copied {binary_src.stat().st_size // 1024:,} KB")

    if args.install:
        print(f"\n── pip install -e python/ ──")
        run([sys.executable, "-m", "pip", "install", "-e", str(ROOT / "python")])

    print(f"\n✓  Done.  Run:  omle-server [config.json]")


if __name__ == "__main__":
    main()
