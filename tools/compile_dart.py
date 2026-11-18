#!/usr/bin/env python3
"""Compiles shared Dart logic into a FeatherDart Kernel (.dill) or AOT snapshot.

Enforces the 7-library FeatherDart allowlist (`async`, `collection`, `convert`,
`core`, `ffi`, `math`, `typed_data`) via `gen_kernel.dart --target=feather_dart`.

Examples:
  # Compile Kernel (.dill) for dev / host testing:
  python3 tools/compile_dart.py --format=kernel \
      --entrypoint=examples/shared_logic/lib/shared_logic.dart \
      --output=dist/app_logic.dill

  # Compile Android AOT ELF snapshots (libapp_logic.so per ABI):
  python3 tools/compile_dart.py --format=aot-android \
      --entrypoint=examples/shared_logic/lib/shared_logic.dart \
      --output=examples/android_app/app/src/main/jniLibs

  # Compile iOS AOT Mach-O XCFramework:
  python3 tools/compile_dart.py --format=aot-ios \
      --entrypoint=examples/shared_logic/lib/shared_logic.dart \
      --output=dist/ios/AppLogicSnapshot.xcframework
"""

import argparse
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = Path(__file__).resolve().parent.parent
DIST_DIR = REPO_ROOT / "dist"
DEFAULT_PLATFORM_DILL = DIST_DIR / "sdk" / "feather_dart_platform_strong.dill"
DEFAULT_SDK_DIR = REPO_ROOT / "third_party" / "dart-sdk"

ANDROID_ABI_FOR_ARCH = {
    "arm64": "arm64-v8a",
    "x64": "x86_64",
}


def run_cmd(cmd: list[str], cwd: Path = REPO_ROOT) -> None:
  print(f"+ {' '.join(cmd)}")
  subprocess.run(cmd, cwd=str(cwd), check=True)


def find_dart_bin() -> str:
  sdk_prebuilt = DEFAULT_SDK_DIR / "tools" / "sdks" / "dart-sdk" / "bin" / "dart"
  if sdk_prebuilt.exists():
    return str(sdk_prebuilt)
  found = shutil.which("dart")
  if found:
    return found
  raise RuntimeError("`dart` executable not found in third_party/dart-sdk or PATH.")


def ensure_package_config(entrypoint: Path, dart_bin: str) -> Path | None:
  cur = entrypoint.resolve().parent
  while True:
    pubspec = cur / "pubspec.yaml"
    pkg_cfg = cur / ".dart_tool" / "package_config.json"
    if pkg_cfg.exists():
      return pkg_cfg
    if pubspec.exists():
      run_cmd([dart_bin, "pub", "get"], cwd=cur)
      return pkg_cfg if pkg_cfg.exists() else None
    if cur.parent == cur:
      return None
    cur = cur.parent


def compile_kernel(
    entrypoint: Path,
    platform_dill: Path,
    output_dill: Path,
    aot: bool,
    product: bool,
) -> None:
  dart_bin = find_dart_bin()
  gen_kernel = DEFAULT_SDK_DIR / "pkg" / "vm" / "bin" / "gen_kernel.dart"
  if not gen_kernel.exists():
    raise FileNotFoundError(
        f"gen_kernel.dart not found at {gen_kernel}. Run `python3 tools/fetch_dart_sdk.py`."
    )

  sdk_pkg_cfg = DEFAULT_SDK_DIR / ".dart_tool" / "package_config.json"
  app_pkg_cfg = ensure_package_config(entrypoint, dart_bin)
  output_dill.parent.mkdir(parents=True, exist_ok=True)

  cmd = [dart_bin]
  if sdk_pkg_cfg.exists():
    cmd.append(f"--packages={sdk_pkg_cfg}")
  cmd.extend([
      str(gen_kernel),
      "--target=feather_dart",
      f"--platform={platform_dill.resolve()}",
      f"--output={output_dill.resolve()}",
  ])
  if app_pkg_cfg is not None:
    cmd.append(f"--packages={app_pkg_cfg.resolve()}")
  if aot:
    cmd.extend(["--aot", "--tfa"])
  if product:
    cmd.append("-Ddart.vm.product=true")
  cmd.append(str(entrypoint.resolve()))
  run_cmd(cmd, cwd=DEFAULT_SDK_DIR)


def compile_aot_host(entrypoint: Path, platform_dill: Path, output_so: Path) -> None:
  gen_snapshot = DIST_DIR / "host" / "gen_snapshot"
  if not gen_snapshot.exists():
    raise FileNotFoundError(
        f"Host gen_snapshot not found at {gen_snapshot}. "
        "Run `python3 tools/build_vm.py --target=host`."
    )
  output_so.parent.mkdir(parents=True, exist_ok=True)
  with tempfile.TemporaryDirectory() as tmp:
    aot_dill = Path(tmp) / "app_aot.dill"
    compile_kernel(entrypoint, platform_dill, aot_dill, aot=True, product=True)
    if platform.system().lower() == "darwin":
      asm_file = Path(tmp) / "app_aot.S"
      run_cmd([
          str(gen_snapshot),
          "--deterministic",
          "--snapshot-kind=app-aot-assembly",
          f"--assembly={asm_file}",
          "--strip",
          str(aot_dill),
      ])
      run_cmd([
          "clang",
          "-dynamiclib",
          str(asm_file),
          "-o",
          str(output_so.resolve()),
      ])
    else:
      run_cmd([
          str(gen_snapshot),
          "--deterministic",
          "--snapshot-kind=app-aot-elf",
          f"--elf={output_so.resolve()}",
          "--strip",
          str(aot_dill),
      ])


def compile_aot_android(
    entrypoint: Path,
    platform_dill: Path,
    jni_libs_dir: Path,
    lib_name: str,
    archs: list[str],
) -> None:
  with tempfile.TemporaryDirectory() as tmp:
    aot_dill = Path(tmp) / "app_aot.dill"
    compile_kernel(entrypoint, platform_dill, aot_dill, aot=True, product=True)

    for arch in archs:
      abi = ANDROID_ABI_FOR_ARCH[arch]
      gen_snapshot = DIST_DIR / "android" / "compilers" / f"gen_snapshot_android_{arch}"
      if not gen_snapshot.exists():
        raise FileNotFoundError(
            f"Android gen_snapshot not found at {gen_snapshot}. "
            "Run `python3 tools/build_vm.py --target=android`."
        )
      out_elf = jni_libs_dir.resolve() / abi / f"lib{lib_name}.so"
      out_elf.parent.mkdir(parents=True, exist_ok=True)
      run_cmd([
          str(gen_snapshot),
          "--deterministic",
          "--snapshot-kind=app-aot-elf",
          f"--elf={out_elf}",
          "--strip",
          str(aot_dill),
      ])
      print(f"Wrote Android AOT snapshot -> {out_elf}")


def compile_aot_ios(
    entrypoint: Path,
    platform_dill: Path,
    xcframework_out: Path,
    framework_name: str,
) -> None:
  if platform.system().lower() != "darwin":
    raise RuntimeError("Compiling Mach-O iOS XCFrameworks requires macOS with Xcode.")

  compilers_dir = DIST_DIR / "ios" / "compilers"
  with tempfile.TemporaryDirectory() as tmp_str:
    tmp = Path(tmp_str)
    aot_dill = tmp / "app_aot.dill"
    compile_kernel(entrypoint, platform_dill, aot_dill, aot=True, product=True)

    def build_macho_slice(
        gen_snap: Path, sdk_name: str, clang_arch: str, min_flag: str, out_file: Path
    ) -> None:
      asm_file = out_file.with_suffix(".S")
      run_cmd([
          str(gen_snap),
          "--deterministic",
          "--snapshot-kind=app-aot-assembly",
          f"--assembly={asm_file}",
          "--strip",
          str(aot_dill),
      ])
      sdk_path = subprocess.check_output(
          ["xcrun", "--sdk", sdk_name, "--show-sdk-path"], text=True
      ).strip()
      run_cmd([
          "xcrun",
          "-sdk",
          sdk_name,
          "clang",
          "-arch",
          clang_arch,
          "-dynamiclib",
          "-isysroot",
          sdk_path,
          min_flag,
          "-Wl,-install_name,@rpath/" + framework_name + ".framework/" + framework_name,
          str(asm_file),
          "-o",
          str(out_file),
      ])

    dev_fw = tmp / "ios-arm64" / f"{framework_name}.framework"
    dev_fw.mkdir(parents=True)
    build_macho_slice(
        compilers_dir / "gen_snapshot_ios_arm64",
        "iphoneos",
        "arm64",
        "-miphoneos-version-min=15.0",
        dev_fw / framework_name,
    )
    _write_framework_plist(dev_fw / "Info.plist", framework_name)

    sim_arm64 = tmp / "sim_arm64.dylib"
    sim_x64 = tmp / "sim_x64.dylib"
    build_macho_slice(
        compilers_dir / "gen_snapshot_ios_sim_arm64",
        "iphonesimulator",
        "arm64",
        "-mios-simulator-version-min=15.0",
        sim_arm64,
    )
    build_macho_slice(
        compilers_dir / "gen_snapshot_ios_sim_x64",
        "iphonesimulator",
        "x86_64",
        "-mios-simulator-version-min=15.0",
        sim_x64,
    )
    sim_fw = tmp / "ios-simulator" / f"{framework_name}.framework"
    sim_fw.mkdir(parents=True)
    run_cmd([
        "lipo",
        "-create",
        str(sim_arm64),
        str(sim_x64),
        "-output",
        str(sim_fw / framework_name),
    ])
    _write_framework_plist(sim_fw / "Info.plist", framework_name)

    if xcframework_out.exists():
      shutil.rmtree(xcframework_out)
    xcframework_out.parent.mkdir(parents=True, exist_ok=True)
    run_cmd([
        "xcodebuild",
        "-create-xcframework",
        "-framework",
        str(dev_fw),
        "-framework",
        str(sim_fw),
        "-output",
        str(xcframework_out.resolve()),
    ])
    print(f"Wrote iOS AOT XCFramework -> {xcframework_out}")


def _write_framework_plist(plist_path: Path, name: str) -> None:
  plist_path.write_text(
      f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>{name}</string>
  <key>CFBundleIdentifier</key>
  <string>dev.featherdart.snapshot.{name}</string>
  <key>CFBundleName</key>
  <string>{name}</string>
  <key>CFBundlePackageType</key>
  <string>FMWK</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
</dict>
</plist>
""",
      encoding="utf-8",
  )


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument(
      "--format",
      choices=["kernel", "aot-host", "aot-android", "aot-ios"],
      required=True,
      help="Output snapshot format",
  )
  parser.add_argument(
      "--entrypoint",
      type=Path,
      required=True,
      help="Dart entrypoint file (e.g. lib/shared_logic.dart)",
  )
  parser.add_argument(
      "--output",
      type=Path,
      required=True,
      help="Output file (.dill / .so / .dylib), Android jniLibs dir, or .xcframework path",
  )
  parser.add_argument(
      "--platform-dill",
      type=Path,
      default=DEFAULT_PLATFORM_DILL,
      help="Path to feather_dart_platform_strong.dill",
  )
  parser.add_argument(
      "--lib-name",
      default="app_logic",
      help="Shared library / framework base name for AOT snapshots (default: app_logic)",
  )
  parser.add_argument(
      "--android-archs",
      default="arm64,x64",
      help="Comma-separated Android architectures for aot-android (default: arm64,x64)",
  )
  args = parser.parse_args()

  if not args.platform_dill.exists():
    print(
        f"Error: Platform dill not found at {args.platform_dill}.\n"
        "Run `python3 tools/build_vm.py --target=platform-only` first.",
        file=sys.stderr,
    )
    return 1

  if args.format == "kernel":
    compile_kernel(args.entrypoint, args.platform_dill, args.output, aot=False, product=False)
  elif args.format == "aot-host":
    compile_aot_host(args.entrypoint, args.platform_dill, args.output)
  elif args.format == "aot-android":
    archs = [a.strip() for a in args.android_archs.split(",") if a.strip()]
    compile_aot_android(args.entrypoint, args.platform_dill, args.output, args.lib_name, archs)
  elif args.format == "aot-ios":
    compile_aot_ios(args.entrypoint, args.platform_dill, args.output, "AppLogicSnapshot")
  return 0


if __name__ == "__main__":
  sys.exit(main())
