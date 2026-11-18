# FeatherDart

**FeatherDart** is a featherweight, WebAssembly-inspired Dart VM embedding for **Android (Kotlin)** and **iOS (Swift)** designed to share pure mobile application logic, state machines, and numerical computation across native mobile apps—**without** bundling Flutter's UI framework, rendering engine (Skia/Impeller), platform channels, or `dart:io` (BoringSSL, zlib, sockets, filesystem).

## Core Design Principles

1. **Minimal 7-Library Standard Surface**:
   User Dart code is restricted at compile time (`FeatherDartTarget`) to strictly:
   - `dart:async`
   - `dart:collection`
   - `dart:convert`
   - `dart:core`
   - `dart:ffi`
   - `dart:math`
   - `dart:typed_data`

   Importing `dart:io`, `dart:isolate`, `dart:mirrors`, `dart:developer`, `dart:cli`, or `dart:ui` is rejected by the compiler, and `//runtime/bin` (`dart:io`) is never linked into `libfeather_dart`.
2. **Host-Supplied Capabilities via Direct FFI**:
   Persistence, networking, logging, sensors, and OS services are supplied by the Swift or Kotlin host app via `@Native` (`Dart -> Host`), while Dart publishes `Pointer.fromFunction` / `NativeCallable` entrypoints via `FeatherDartExports.register` (`Host -> Dart`) for zero-reflection native C-ABI calls.
3. **AOT in Production + Kernel (`.dill`) in Development**:
   - **Mobile Release (iOS & Android)**: Links against `libdart_aotruntime_product` (`PRODUCT` mode, no JIT, no profiler/VM service, iOS App Store compliant) and executes tree-shaken AOT snapshots (`AppLogicSnapshot.xcframework` on iOS, `libapp_logic.so` on Android).
   - **Local Dev & Host Testing**: Supports loading portable `.dill` Kernel binaries via `libfeather_dart_jit`.
4. **Lock-Guarded Re-Entrant Isolates + Host Event-Loop Bridge**:
   Host threads enter an isolate synchronously (`isolate.withIsolate { ... }`), invoke C function pointers directly, and automatically drain the Dart microtask queue on exit. Asynchronous port messages and `dart:async` `Timer` wakeups bridge directly onto **Grand Central Dispatch (`DispatchQueue` / `DispatchSourceTimer`)** on iOS and **`Handler`/`Looper` or Kotlin `CoroutineDispatcher`** on Android.

---

## Repository Structure

```text
feather_dart/
├── vm/                                 # Custom Dart VM variant, C11 embedder API, & CFE target
│   ├── include/feather_dart.h          # Pure C11 embedding ABI used by Swift & Android JNI
│   ├── src/feather_dart.cc             # Lock-guarded isolate manager, @Native resolver, & microtask/Timer bridge
│   ├── src/timer_fallback.cc           # Condition-variable fallback timer when host omits custom timer hook
│   ├── sdk/libraries_feather_dart.json # Platform SDK spec for the `feather_dart` target
│   ├── sdk/lib/_feather_dart/          # Internal `dart:_feather_dart` bootstrap (print, monotonic clock, Timer port)
│   ├── sdk/compiler/                   # `FeatherDartTarget` enforcing the 7-library allowlist
│   ├── BUILD.gn & CMakeLists.txt       # Build definitions for iOS (.a/.xcframework), Android (.so), & host
│   ├── patches/                        # Minimal Dart SDK integration patch
│   └── test/                           # C++ unit test & standalone host CLI runner
├── tools/
│   ├── fetch_dart_sdk.py               # Fetches pinned `dart-lang/sdk` into `third_party/sdk` & mounts overlay
│   ├── build_vm.py                     # Builds platform `.dill`, host runtime, Android `.so`s, & iOS `.xcframework`
│   ├── compile_dart.py                 # Compiles shared Dart packages -> `.dill`, Android AOT `.so`, or iOS `.xcframework`
│   └── run_host_demo.py                # One-command macOS & Linux end-to-end Kernel + AOT demo runner
├── packages/
│   └── feather_dart_runtime/           # Zero-dependency Dart FFI helpers (`FeatherDartExports`, `NativeArena`, `FfiUtf8`)
├── bindings/
│   ├── swift/                          # `FeatherDart` Swift Package (`FeatherDartEngine`, `FeatherDartSnapshot`, `FeatherDartIsolate`)
│   └── kotlin/                         # `dev.featherdart:feather-dart` Android library + NDK JNI bridge
└── examples/
    ├── shared_logic/                   # Shared Dart portfolio & Monte Carlo VaR engine using all 7 core libraries
    ├── ios_app/                        # Sample SwiftUI iOS app (`FeatherDartDemo`)
    └── android_app/                    # Sample Jetpack Compose Android app (`FeatherDartAndroidDemo`)
```

---

## Quick Start on macOS & Linux

### Prerequisites

- **macOS or Linux** (`x86_64` or `arm64`)
- `git`, `python3` (3.10+), and a C++17 compiler (`clang++` via Xcode Command Line Tools on macOS, or `clang++`/`g++` on Linux)
- *(Optional for pure-Dart unit tests prior to fetching the VM)*: `dart` SDK 3.3+
- *(For iOS builds)*: macOS with Xcode 15+ and [XcodeGen](https://github.com/yonaskolb/XcodeGen) (`brew install xcodegen`)
- *(For Android builds)*: JDK 17+, Android Studio / Gradle 8.7+, Android SDK 35, and NDK 26+

### 1. Run Pure-Dart, Swift, & C++ Unit Tests (Instant, No VM Checkout Needed)

```bash
# Pure-Dart runtime & shared logic tests (including live FFI trampoline tests):
(cd packages/feather_dart_runtime && dart pub get && dart analyze && dart test)
(cd examples/shared_logic && dart pub get && dart analyze && dart test)

# Swift package build & UTF-8 buffer tests (macOS or Linux with Swift 5.9+):
(cd bindings/swift && swift test)
```

### 2. Fetch Dart SDK, Build Host FeatherDart VM, & Run End-to-End Demo

`fetch_dart_sdk.py` automatically bootstraps `depot_tools` (into `.depot_tools/` if `gclient` is not already in `PATH`), fetches the pinned `dart-lang/sdk` tree (including its prebuilt bootstrap `dart` binary), and mounts the `FeatherDart` overlay:

```bash
# 1. Fetch Dart SDK + apply FeatherDart overlay:
python3 tools/fetch_dart_sdk.py

# 2. Build feather_dart_platform_strong.dill + host AOT & Kernel runtimes:
python3 tools/build_vm.py --target=host

# 3. Compile examples/shared_logic and execute in BOTH Kernel (.dill) and AOT modes:
python3 tools/run_host_demo.py
```

---

## Building for iOS & Android

### iOS (`FeatherDartVM.xcframework` + Sample SwiftUI App)

```bash
# Build device (arm64) + simulator (arm64/x86_64) VM XCFramework:
python3 tools/build_vm.py --target=ios

# Compile shared Dart logic to a Mach-O AOT XCFramework:
python3 tools/compile_dart.py \
    --format=aot-ios \
    --entrypoint=examples/shared_logic/lib/shared_logic.dart \
    --output=dist/ios/AppLogicSnapshot.xcframework

# Generate and open the Xcode project:
cd examples/ios_app
xcodegen generate
open FeatherDartDemo.xcodeproj
```

### Android (`libfeather_dart.so` + Sample Jetpack Compose App)

```bash
# Fetch Android NDK/sysroot deps in Dart SDK and build arm64-v8a + x86_64 runtimes:
python3 tools/fetch_dart_sdk.py --android
python3 tools/build_vm.py --target=android

# Compile shared Dart logic to stripped Android ELF AOT snapshots:
python3 tools/compile_dart.py \
    --format=aot-android \
    --entrypoint=examples/shared_logic/lib/shared_logic.dart \
    --output=examples/android_app/app/src/main/jniLibs

# Open examples/android_app in Android Studio, or build from CLI with Gradle 8.7+:
cd examples/android_app
gradle :app:assembleDebug
```

---

## License

Released under the [BSD 3-Clause License](LICENSE).
