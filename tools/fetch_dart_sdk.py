#!/usr/bin/env python3
"""Fetches a pinned Dart SDK checkout and applies the FeatherDart VM overlay.

Usage:
  python3 tools/fetch_dart_sdk.py [--ref=3.7.2] [--android] [--overlay-only]
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY_DIR = REPO_ROOT / "third_party"
DART_SDK_DIR = THIRD_PARTY_DIR / "sdk"
DEPOT_TOOLS_DIR = REPO_ROOT / ".depot_tools"
VM_DIR = REPO_ROOT / "vm"

DEFAULT_DART_REF = "3.7.2"
DEPOT_TOOLS_URL = "https://chromium.googlesource.com/chromium/tools/depot_tools.git"


def run_cmd(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
  print(f"+ (cd {cwd} && {' '.join(cmd)})")
  subprocess.run(cmd, cwd=str(cwd), env=env, check=True)


def ensure_depot_tools(env: dict[str, str]) -> tuple[str, dict[str, str]]:
  updated = dict(env)
  existing = shutil.which("gclient", path=updated.get("PATH"))
  if existing:
    return existing, updated

  gclient_bin = DEPOT_TOOLS_DIR / "gclient"
  if not gclient_bin.exists():
    if DEPOT_TOOLS_DIR.is_symlink() or DEPOT_TOOLS_DIR.exists():
      if DEPOT_TOOLS_DIR.is_symlink():
        target = DEPOT_TOOLS_DIR.resolve()
        print(f"Cloning depot_tools into {target}...")
        run_cmd(["git", "clone", "--depth=1", DEPOT_TOOLS_URL, str(target)], REPO_ROOT)
      else:
        shutil.rmtree(DEPOT_TOOLS_DIR)
        print(f"Cloning depot_tools into {DEPOT_TOOLS_DIR}...")
        run_cmd(["git", "clone", "--depth=1", DEPOT_TOOLS_URL, str(DEPOT_TOOLS_DIR)], REPO_ROOT)
    else:
      print(f"Cloning depot_tools into {DEPOT_TOOLS_DIR}...")
      run_cmd(["git", "clone", "--depth=1", DEPOT_TOOLS_URL, str(DEPOT_TOOLS_DIR)], REPO_ROOT)

  updated["PATH"] = f"{DEPOT_TOOLS_DIR.resolve()}{os.pathsep}{updated.get('PATH', '')}"
  return str(gclient_bin.resolve()), updated


def write_gclient_config(ref: str, download_android: bool) -> None:
  THIRD_PARTY_DIR.mkdir(parents=True, exist_ok=True)
  custom_vars = []
  if download_android:
    custom_vars.append('      "download_android_deps": True,')
  custom_vars_block = "\n".join(custom_vars)
  gclient_spec = f"""solutions = [
  {{
    "name": "sdk",
    "url": "https://dart.googlesource.com/sdk.git@{ref}",
    "deps_file": "DEPS",
    "managed": False,
    "custom_vars": {{
{custom_vars_block}
    }},
  }},
]
"""
  if download_android:
    gclient_spec += 'target_os = ["android"]\n'
  (THIRD_PARTY_DIR / ".gclient").write_text(gclient_spec, encoding="utf-8")


def copy_or_replace(src: Path, dst: Path) -> None:
  if dst.is_symlink() or dst.exists():
    if dst.is_symlink() or dst.is_file():
      dst.unlink()
    else:
      shutil.rmtree(dst)
  dst.parent.mkdir(parents=True, exist_ok=True)
  if src.is_dir():
    shutil.copytree(src, dst)
  else:
    shutil.copy2(src, dst)


def apply_overlay(sdk_dir: Path) -> None:
  if not sdk_dir.exists():
    raise FileNotFoundError(f"Dart SDK directory not found at {sdk_dir}")

  print("Mounting FeatherDart overlay into Dart SDK checkout...")
  copy_or_replace(VM_DIR, sdk_dir / "feather_dart")
  copy_or_replace(
      VM_DIR / "sdk" / "libraries_feather_dart.json",
      sdk_dir / "sdk" / "lib" / "libraries_feather_dart.json",
  )
  copy_or_replace(
      VM_DIR / "sdk" / "lib" / "_feather_dart",
      sdk_dir / "sdk" / "lib" / "_feather_dart",
  )
  copy_or_replace(
      VM_DIR / "sdk" / "compiler" / "feather_dart_target.dart",
      sdk_dir / "pkg" / "vm" / "lib" / "modular" / "target" / "feather_dart.dart",
  )

  # Register `feather_dart_runtime` GN group in top-level BUILD.gn idempotently.
  root_build_gn = sdk_dir / "BUILD.gn"
  if root_build_gn.exists():
    content = root_build_gn.read_text(encoding="utf-8")
    if 'group("feather_dart_runtime")' not in content:
      snippet = (
          '\ngroup("feather_dart_runtime") {\n'
          '  deps = [ "//feather_dart:feather_dart_aot" ]\n'
          "  if (!is_ios) {\n"
          '    deps += [ "//feather_dart:feather_dart_jit" ]\n'
          "  }\n"
          "}\n"
      )
      root_build_gn.write_text(content + snippet, encoding="utf-8")

  # Register `feather_dart` CFE target in `pkg/vm/lib/modular/target/install.dart`.
  install_dart = sdk_dir / "pkg" / "vm" / "lib" / "modular" / "target" / "install.dart"
  if install_dart.exists():
    text = install_dart.read_text(encoding="utf-8")
    if "FeatherDartTarget" not in text:
      text = text.replace(
          "import 'package:vm/modular/target/vm.dart' show VmTarget;",
          "import 'package:vm/modular/target/feather_dart.dart' show FeatherDartTarget;\n"
          "import 'package:vm/modular/target/vm.dart' show VmTarget;",
      )
    if "feather_dart" not in text.split("void installAdditionalTargets()")[1]:
      for quote in ('"', "'"):
        needle = f"targets[{quote}vm{quote}] = (TargetFlags flags) => VmTarget(flags);"
        if needle in text:
          text = text.replace(
              needle,
              f"targets[{quote}feather_dart{quote}] = (TargetFlags flags) => FeatherDartTarget(flags);\n    {needle}",
          )
          break
    install_dart.write_text(text, encoding="utf-8")

  # Allow `--target=feather_dart` in `pkg/vm/lib/kernel_front_end.dart`.
  kfe_dart = sdk_dir / "pkg" / "vm" / "lib" / "kernel_front_end.dart"
  if kfe_dart.exists():
    kfe_text = kfe_dart.read_text(encoding="utf-8")
    if "'feather_dart'" not in kfe_text and '"feather_dart"' not in kfe_text:
      for quote in ("'", '"'):
        needle = f"allowed: <String>[{quote}vm{quote},"
        if needle in kfe_text:
          kfe_text = kfe_text.replace(
              needle,
              f"allowed: <String>[{quote}vm{quote}, {quote}feather_dart{quote},",
          )
          break
      kfe_dart.write_text(kfe_text, encoding="utf-8")

  # Shallow gclient clones may omit `.git/logs/HEAD`, which Dart's GN rules
  # list in `build.ninja.d` when git version info is enabled.
  git_logs_head = sdk_dir / ".git" / "logs" / "HEAD"
  if (sdk_dir / ".git").exists() and not git_logs_head.exists():
    git_logs_head.parent.mkdir(parents=True, exist_ok=True)
    git_logs_head.touch()

  print("FeatherDart SDK overlay applied successfully.")


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      "--ref",
      default=DEFAULT_DART_REF,
      help=f"Git tag, branch, or commit of dart-lang/sdk (default: {DEFAULT_DART_REF})",
  )
  parser.add_argument(
      "--android",
      action="store_true",
      help="Also download Android NDK/SDK sysroot dependencies via gclient",
  )
  parser.add_argument(
      "--sdk-dir",
      type=Path,
      default=DART_SDK_DIR,
      help="Path to Dart SDK checkout (default: third_party/dart-sdk)",
  )
  parser.add_argument(
      "--overlay-only",
      action="store_true",
      help="Apply the FeatherDart overlay to an existing --sdk-dir without running gclient sync",
  )
  args = parser.parse_args()

  if not args.overlay_only:
    gclient_exe, env = ensure_depot_tools(os.environ)
    write_gclient_config(args.ref, args.android)
    run_cmd([gclient_exe, "sync", "--no-history", "--shallow"], THIRD_PARTY_DIR.resolve(), env=env)

  apply_overlay(args.sdk_dir.resolve())
  return 0


if __name__ == "__main__":
  sys.exit(main())
