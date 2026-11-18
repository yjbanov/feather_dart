#!/usr/bin/env python3
"""Builds the stripped FeatherDart VM runtime, platform .dill, and AOT compilers.

Targets:
  - `host`    : Builds host `libfeather_dart` (AOT), `libfeather_dart_jit`
                (Kernel), `gen_snapshot`, and `feather_dart_platform_strong.dill`.
  - `android` : Builds `libfeather_dart.so` for `arm64-v8a` and `x86_64`, plus
                cross-target `gen_snapshot` binaries for Android AOT builds.
  - `ios`     : (macOS host) Builds `FeatherDartVM.xcframework` combining device
                (`ios-arm64`) and simulator (`ios-arm64_x86_64-simulator`)
                static AOT runtimes.
"""

import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SDK_DIR = REPO_ROOT / "third_party" / "sdk"
DIST_DIR = REPO_ROOT / "dist"
VM_INCLUDE_DIR = REPO_ROOT / "vm" / "include"

ANDROID_ABI_FOR_ARCH = {
    "arm64": "arm64-v8a",
    "x64": "x86_64",
    "arm": "armeabi-v7a",
}


def host_arch() -> str:
  machine = platform.machine().lower()
  if machine in ("x86_64", "amd64"):
    return "x64"
  if machine in ("aarch64", "arm64"):
    return "arm64"
  raise RuntimeError(f"Unsupported host CPU architecture: {machine}")


def host_os_name() -> str:
  sys_name = platform.system().lower()
  if sys_name == "darwin":
    return "macos"
  if sys_name == "linux":
    return "linux"
  raise RuntimeError(f"Unsupported host OS: {sys_name}")


def dart_out_dir(sdk_dir: Path, mode: str, arch: str, target_os: str = "host") -> Path:
  prefix = "xcodebuild" if host_os_name() == "macos" else "out"
  cap_mode = mode.capitalize()
  os_tag = "" if target_os == "host" else target_os.capitalize()
  return sdk_dir / prefix / f"{cap_mode}{os_tag}{arch.upper()}"


def run_cmd(cmd: list[str], cwd: Path) -> None:
  print(f"+ (cd {cwd} && {' '.join(cmd)})")
  env = dict(os.environ)
  depot_tools = REPO_ROOT / ".depot_tools"
  if depot_tools.exists():
    env["PATH"] = f"{depot_tools}{os.pathsep}{env.get('PATH', '')}"
  subprocess.run(cmd, cwd=str(cwd), env=env, check=True)


def find_sdk_prebuilt_dart(sdk_dir: Path) -> tuple[Path, Path]:
  """Returns `(dart_executable, host_vm_platform_dill)` from third_party/dart-sdk."""
  prebuilt_root = sdk_dir / "tools" / "sdks" / "dart-sdk"
  prebuilt_dart = prebuilt_root / "bin" / "dart"
  prebuilt_platform = prebuilt_root / "lib" / "_internal" / "vm_platform_strong.dill"
  if prebuilt_dart.exists() and prebuilt_platform.exists():
    return prebuilt_dart, prebuilt_platform

  env_dart = shutil.which("dart")
  if env_dart:
    dart_path = Path(env_dart).resolve()
    inferred_root = dart_path.parent.parent
    inferred_platform = inferred_root / "lib" / "_internal" / "vm_platform_strong.dill"
    if inferred_platform.exists():
      return dart_path, inferred_platform

  raise RuntimeError(
      "Could not locate prebuilt Dart SDK in `third_party/dart-sdk/tools/sdks/dart-sdk` "
      "or `dart` in PATH."
  )


def copy_built_platform_dill(out_dir: Path) -> None:
  built_dill = out_dir / "feather_dart_platform_strong.dill"
  if built_dill.exists():
    dst = DIST_DIR / "sdk" / "feather_dart_platform_strong.dill"
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(built_dill, dst)
    print(f"Staged FeatherDart platform kernel -> {dst}")


def gn_and_build(
    sdk_dir: Path,
    target_os: str,
    mode: str,
    arch: str,
    ninja_targets: list[str],
    extra_gn_args: list[str] | None = None,
) -> Path:
  gn_args = [
      "dart_support_perfetto=false",
      "dart_version_git_info=false",
  ]
  if extra_gn_args:
    gn_args.extend(extra_gn_args)

  gn_cmd = [
      sys.executable,
      "tools/gn.py",
      f"--os={target_os}",
      f"--mode={mode}",
      f"--arch={arch}",
      "--exclude-kernel-service",
      f"--gn-args={' '.join(gn_args)}",
  ]
  run_cmd(gn_cmd, sdk_dir)

  if target_os == "ios_simulator":
    cap_mode = mode.capitalize()
    prefix = "xcodebuild" if host_os_name() == "macos" else "out"
    out_dir = sdk_dir / prefix / f"{cap_mode}IOSSimulator{arch.upper()}"
  elif target_os == "ios":
    cap_mode = mode.capitalize()
    prefix = "xcodebuild" if host_os_name() == "macos" else "out"
    out_dir = sdk_dir / prefix / f"{cap_mode}IOS{arch.upper()}"
  else:
    out_dir = dart_out_dir(sdk_dir, mode, arch, target_os)

  # Invoke ninja directly rather than `tools/build.py` so `tools/build.py`
  # does not re-run `gn.py` with default args and overwrite `--gn-args`.
  ninja_bin = sdk_dir / "buildtools" / "ninja" / "ninja"
  if not ninja_bin.exists():
    ninja_bin = Path(shutil.which("ninja") or "ninja")
  run_cmd([str(ninja_bin), "-C", str(out_dir), *ninja_targets], sdk_dir)
  return out_dir


def build_host(sdk_dir: Path) -> None:
  arch = host_arch()
  ext = "dylib" if host_os_name() == "macos" else "so"
  host_dist = DIST_DIR / "host"
  host_dist.mkdir(parents=True, exist_ok=True)

  prod_out = gn_and_build(
      sdk_dir,
      target_os="host",
      mode="product",
      arch=arch,
      ninja_targets=[
          "feather_dart:feather_dart_platform",
          "feather_dart:feather_dart_aot",
          "gen_snapshot",
      ],
  )
  copy_built_platform_dill(prod_out)
  shutil.copy2(prod_out / f"libfeather_dart.{ext}", host_dist / f"libfeather_dart.{ext}")
  shutil.copy2(prod_out / "gen_snapshot", host_dist / "gen_snapshot")

  rel_out = gn_and_build(
      sdk_dir,
      target_os="host",
      mode="release",
      arch=arch,
      ninja_targets=["feather_dart:feather_dart_jit"],
  )
  shutil.copy2(rel_out / f"libfeather_dart_jit.{ext}", host_dist / f"libfeather_dart_jit.{ext}")
  shutil.copy2(VM_INCLUDE_DIR / "feather_dart.h", host_dist / "feather_dart.h")
  print(f"Host artifacts staged in {host_dist}")


def build_android(sdk_dir: Path, archs: list[str], include_jit: bool) -> None:
  jni_dist = DIST_DIR / "android" / "jniLibs"
  compiler_dist = DIST_DIR / "android" / "compilers"
  compiler_dist.mkdir(parents=True, exist_ok=True)

  for arch in archs:
    abi = ANDROID_ABI_FOR_ARCH[arch]
    abi_dir = jni_dist / abi
    abi_dir.mkdir(parents=True, exist_ok=True)

    host_gen_snapshot_target = f"clang_{host_arch()}/gen_snapshot"
    prod_out = gn_and_build(
        sdk_dir,
        target_os="android",
        mode="product",
        arch=arch,
        ninja_targets=[
            "feather_dart:feather_dart_platform",
            "feather_dart:feather_dart_aot",
            host_gen_snapshot_target,
        ],
    )
    copy_built_platform_dill(prod_out)
    staged_so = abi_dir / "libfeather_dart.so"
    shutil.copy2(prod_out / "libfeather_dart.so", staged_so)
    ndk_strip = (
        sdk_dir
        / "third_party/android_tools/ndk/toolchains/llvm/prebuilt"
        / f"{host_os_name()}-x86_64/bin/llvm-strip"
    )
    if ndk_strip.exists():
      run_cmd([str(ndk_strip), "--strip-all", str(staged_so)], sdk_dir)
    for candidate in (
        prod_out / f"clang_{host_arch()}" / "exe.stripped" / "gen_snapshot",
        prod_out / f"clang_{host_arch()}" / "gen_snapshot",
        prod_out / "gen_snapshot",
    ):
      if candidate.exists():
        shutil.copy2(candidate, compiler_dist / f"gen_snapshot_android_{arch}")
        break

    if include_jit:
      rel_out = gn_and_build(
          sdk_dir,
          target_os="android",
          mode="release",
          arch=arch,
          ninja_targets=["feather_dart:feather_dart_jit"],
      )
      shutil.copy2(rel_out / "libfeather_dart_jit.so", abi_dir / "libfeather_dart_jit.so")

  print(f"Android JNI libraries staged in {jni_dist}")


def build_ios(sdk_dir: Path) -> None:
  if host_os_name() != "macos":
    raise RuntimeError("Building iOS XCFrameworks requires a macOS host with Xcode installed.")

  ios_dist = DIST_DIR / "ios"
  headers_stage = ios_dist / "_headers"
  headers_stage.mkdir(parents=True, exist_ok=True)
  shutil.copy2(VM_INCLUDE_DIR / "feather_dart.h", headers_stage / "feather_dart.h")
  (headers_stage / "module.modulemap").write_text(
      'module CFeatherDart {\n  header "feather_dart.h"\n  export *\n}\n',
      encoding="utf-8",
  )

  host_gen_snapshot_target = f"clang_{host_arch()}/gen_snapshot"
  device_out = gn_and_build(
      sdk_dir,
      target_os="ios",
      mode="product",
      arch="arm64",
      ninja_targets=[
          "feather_dart:feather_dart_platform",
          "feather_dart:feather_dart_aot",
          host_gen_snapshot_target,
      ],
  )
  copy_built_platform_dill(device_out)
  device_lib = device_out / "obj" / "feather_dart" / "libfeather_dart_aot.a"

  compiler_dist = ios_dist / "compilers"
  compiler_dist.mkdir(parents=True, exist_ok=True)
  for candidate in (
      device_out / f"clang_{host_arch()}" / "gen_snapshot",
      device_out / "gen_snapshot",
  ):
    if candidate.exists():
      shutil.copy2(candidate, compiler_dist / "gen_snapshot_ios_arm64")
      break

  sim_arm64_out = gn_and_build(
      sdk_dir,
      target_os="ios_simulator",
      mode="product",
      arch="arm64",
      ninja_targets=["feather_dart:feather_dart_aot", host_gen_snapshot_target],
  )
  sim_x64_out = gn_and_build(
      sdk_dir,
      target_os="ios_simulator",
      mode="product",
      arch="x64",
      ninja_targets=["feather_dart:feather_dart_aot", host_gen_snapshot_target],
  )
  for arch_tag, out_dir in (("arm64", sim_arm64_out), ("x64", sim_x64_out)):
    for candidate in (
        out_dir / f"clang_{host_arch()}" / "gen_snapshot",
        out_dir / "gen_snapshot",
    ):
      if candidate.exists():
        shutil.copy2(candidate, compiler_dist / f"gen_snapshot_ios_sim_{arch_tag}")
        break

  sim_fat_lib = ios_dist / "_sim_universal" / "libfeather_dart_aot.a"
  sim_fat_lib.parent.mkdir(parents=True, exist_ok=True)
  run_cmd(
      [
          "lipo",
          "-create",
          str(sim_arm64_out / "obj" / "feather_dart" / "libfeather_dart_aot.a"),
          str(sim_x64_out / "obj" / "feather_dart" / "libfeather_dart_aot.a"),
          "-output",
          str(sim_fat_lib),
      ],
      REPO_ROOT,
  )

  xcframework_out = ios_dist / "FeatherDartVM.xcframework"
  if xcframework_out.exists():
    shutil.rmtree(xcframework_out)
  run_cmd(
      [
          "xcodebuild",
          "-create-xcframework",
          "-library",
          str(device_lib),
          "-headers",
          str(headers_stage),
          "-library",
          str(sim_fat_lib),
          "-headers",
          str(headers_stage),
          "-output",
          str(xcframework_out),
      ],
      REPO_ROOT,
  )
  print(f"Built iOS XCFramework -> {xcframework_out}")


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      "--target",
      choices=["host", "android", "ios", "platform-only", "all"],
      default="host",
      help="Target platform to build (default: host)",
  )
  parser.add_argument(
      "--sdk-dir",
      type=Path,
      default=DEFAULT_SDK_DIR,
      help="Path to patched third_party/dart-sdk checkout",
  )
  parser.add_argument(
      "--android-archs",
      default="arm64,x64",
      help="Comma-separated Android architectures (default: arm64,x64)",
  )
  parser.add_argument(
      "--include-android-jit",
      action="store_true",
      help="Also build libfeather_dart_jit.so for Android debug .dill iteration",
  )
  args = parser.parse_args()

  sdk_dir = args.sdk_dir.resolve()
  if not sdk_dir.exists():
    print(
        f"Error: Dart SDK checkout not found at {sdk_dir}.\n"
        "Run `python3 tools/fetch_dart_sdk.py` first.",
        file=sys.stderr,
    )
    return 1

  if args.target == "platform-only":
    prod_out = gn_and_build(
        sdk_dir,
        target_os="host",
        mode="product",
        arch=host_arch(),
        ninja_targets=["feather_dart:feather_dart_platform"],
    )
    copy_built_platform_dill(prod_out)
    return 0
  if args.target in ("host", "all"):
    build_host(sdk_dir)
  if args.target in ("android", "all"):
    archs = [a.strip() for a in args.android_archs.split(",") if a.strip()]
    build_android(sdk_dir, archs, args.include_android_jit)
  if args.target == "ios" or (args.target == "all" and host_os_name() == "macos"):
    build_ios(sdk_dir)

  return 0


if __name__ == "__main__":
  sys.exit(main())
