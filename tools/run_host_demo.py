#!/usr/bin/env python3
"""Compiles and runs `examples/shared_logic` on macOS or Linux in both Kernel and AOT modes.

Prerequisites:
  1. python3 tools/fetch_dart_sdk.py
  2. python3 tools/build_vm.py --target=host
"""

import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
DIST_HOST = REPO_ROOT / "dist" / "host"
BUILD_DIR = REPO_ROOT / "build" / "host_demo"
ENTRYPOINT = REPO_ROOT / "examples" / "shared_logic" / "lib" / "shared_logic.dart"


def run_cmd(cmd: list[str]) -> None:
  print(f"+ {' '.join(cmd)}")
  subprocess.run(cmd, cwd=str(REPO_ROOT), check=True)


def find_cxx() -> str:
  for candidate in ("clang++", "g++", "c++"):
    found = shutil.which(candidate)
    if found:
      return found
  raise RuntimeError("No C++17 compiler (clang++ or g++) found in PATH.")


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      "--mode",
      choices=["both", "kernel", "aot"],
      default="both",
      help="Which execution mode(s) to compile and run (default: both)",
  )
  args = parser.parse_args()

  ext = "dylib" if platform.system().lower() == "darwin" else "so"
  if not (DIST_HOST / f"libfeather_dart.{ext}").exists():
    print(
        f"Host runtime not found in {DIST_HOST}.\n"
        "Run `python3 tools/fetch_dart_sdk.py && python3 tools/build_vm.py --target=host` first.",
        file=sys.stderr,
    )
    return 1

  BUILD_DIR.mkdir(parents=True, exist_ok=True)
  cxx = find_cxx()

  if args.mode in ("both", "kernel"):
    print("\n=== [1/2] Kernel (.dill) Execution ===")
    dill_out = BUILD_DIR / "app_logic.dill"
    runner_jit = BUILD_DIR / "host_runner_jit"
    run_cmd([
        sys.executable,
        "tools/compile_dart.py",
        "--format=kernel",
        f"--entrypoint={ENTRYPOINT}",
        f"--output={dill_out}",
    ])
    run_cmd([
        cxx,
        "-std=c++17",
        "vm/test/host_runner.cc",
        "-Ivm/include",
        f"-L{DIST_HOST}",
        "-lfeather_dart_jit",
        f"-Wl,-rpath,{DIST_HOST}",
        "-o",
        str(runner_jit),
    ])
    run_cmd([str(runner_jit), str(dill_out)])

  if args.mode in ("both", "aot"):
    print("\n=== [2/2] Precompiled AOT Execution ===")
    aot_out = BUILD_DIR / f"libapp_logic.{ext}"
    runner_aot = BUILD_DIR / "host_runner_aot"
    run_cmd([
        sys.executable,
        "tools/compile_dart.py",
        "--format=aot-host",
        f"--entrypoint={ENTRYPOINT}",
        f"--output={aot_out}",
    ])
    run_cmd([
        cxx,
        "-std=c++17",
        "vm/test/host_runner.cc",
        "-Ivm/include",
        f"-L{DIST_HOST}",
        "-lfeather_dart",
        f"-Wl,-rpath,{DIST_HOST}",
        "-o",
        str(runner_aot),
    ])
    run_cmd([str(runner_aot), str(aot_out)])

  return 0


if __name__ == "__main__":
  sys.exit(main())
