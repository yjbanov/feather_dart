import 'dart:ffi';

import 'utf8_arena.dart';

@Native<Void Function(Pointer<Uint8>, Pointer<Void>)>(
  symbol: 'feather_dart_export',
  isLeaf: true,
)
external void _nativeFeatherDartExport(
  Pointer<Uint8> name,
  Pointer<Void> fnPtr,
);

/// Publishes Dart FFI function pointers (`Pointer.fromFunction` or
/// `NativeCallable`) to the host Swift/Kotlin `FeatherDartIsolate`.
abstract final class FeatherDartExports {
  /// Optional override hook used in pure-Dart unit tests (`dart test`) when
  /// running outside the embedded `libfeather_dart` host process.
  static void Function(String name, int address)? testRegistrar;

  /// Registers [pointer] under [name] so the host can retrieve it via
  /// `feather_dart_isolate_lookup_export` (Swift `isolate.lookupExport` or
  /// Kotlin `isolate.lookupExportPointer`).
  static void register<T extends Function>(
    String name,
    Pointer<NativeFunction<T>> pointer,
  ) {
    final override = testRegistrar;
    if (override != null) {
      override(name, pointer.address);
      return;
    }
    NativeArena.using((arena) {
      final cName = arena.toCString(name);
      _nativeFeatherDartExport(cName, pointer.cast<Void>());
    });
  }
}
