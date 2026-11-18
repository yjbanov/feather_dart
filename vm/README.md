# FeatherDart VM Variant (`vm/`)

This directory defines the stripped **FeatherDart** variant of the Dart VM and platform SDK, built for WebAssembly-style in-memory computation inside native Swift (iOS) and Kotlin (Android) mobile applications.

## What Is Included vs. Excluded

| Component | Status in `feather_dart` |
| --- | --- |
| `dart:core`, `dart:async`, `dart:collection`, `dart:convert`, `dart:ffi`, `dart:math`, `dart:typed_data` | **Included & Public** |
| `dart:io`, `dart:cli`, `dart:_http`, BoringSSL, zlib, sockets, filesystem, subprocesses | **Excluded (0 bytes linked)** |
| Flutter Engine, Skia/Impeller, `dart:ui`, platform channels | **Excluded** |
| `dart:isolate`, `dart:developer`, `dart:mirrors`, `dart:concurrent` | **Blocked at compile time** (`FeatherDartLibrarySupport`); tree-shaken in AOT |
| VM Service / Observatory / Profiler / Timeline / Perfetto | **Excluded in AOT Product builds** (`is_product=true`, `dart_support_perfetto=false`) |
| Embedded CFE (`kernel-service.dart.snapshot`) | **Excluded** (`exclude_kernel_service=true`) |

## Key Files

- `include/feather_dart.h` — Pure C11 embedding API consumed by Swift (`CFeatherDart`) and Android JNI (`feather_dart_jni.cc`).
- `src/feather_dart.cc` — Lock-guarded re-entrant isolate manager, AOT & Kernel snapshot loader, `@Native` FFI symbol resolver, automatic microtask queue drainer, and host scheduler bridge.
- `src/timer_fallback.cc` — Portable condition-variable timer service used when the host omits a custom timer hook.
- `sdk/libraries_feather_dart.json` — Platform library specification for the `feather_dart` target.
- `sdk/lib/_feather_dart/feather_dart_bootstrap.dart` — Internal `dart:_feather_dart` library wiring `print()`, monotonic clock, and `Timer` port wakeups via `@Native` calls.
- `sdk/compiler/feather_dart_target.dart` — CFE `Target` enforcing the 7-library allowlist at compile time.
- `BUILD.gn` — GN targets (`feather_dart_aot` and `feather_dart_jit`) mounted into `third_party/dart-sdk/feather_dart/`.
