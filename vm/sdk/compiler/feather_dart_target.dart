import 'package:kernel/target/targets.dart';
import 'package:vm/modular/target/vm.dart' show VmTarget;

/// Strict Dart Library allowlist for the `feather_dart` embedded runtime.
///
/// User code may ONLY import:
/// - `dart:async`
/// - `dart:collection`
/// - `dart:convert`
/// - `dart:core`
/// - `dart:ffi`
/// - `dart:math`
/// - `dart:typed_data`
///
/// All I/O, networking, persistence, UI, and OS capabilities are supplied by
/// the Swift or Kotlin host application via `dart:ffi`.
class FeatherDartLibrarySupport implements DartLibrarySupport {
  const FeatherDartLibrarySupport();

  static const Set<String> allowedUserLibraries = <String>{
    'async',
    'collection',
    'convert',
    'core',
    'ffi',
    'math',
    'typed_data',
  };

  @override
  bool computeDartLibrarySupport(
    String libraryName, {
    required bool isSupportedBySpec,
  }) {
    return isSupportedBySpec && allowedUserLibraries.contains(libraryName);
  }
}

/// CFE compilation target registered in `pkg/vm/lib/modular/target/install.dart`
/// as `feather_dart` for both `compile_platform.dart` and `gen_kernel.dart`.
class FeatherDartTarget extends VmTarget {
  FeatherDartTarget(TargetFlags flags) : super(flags);

  @override
  String get name => 'feather_dart';

  @override
  DartLibrarySupport get dartLibrarySupport => const FeatherDartLibrarySupport();

  @override
  List<String> get extraRequiredLibraries => const <String>[
    'dart:async',
    'dart:collection',
    'dart:_compact_hash',
    'dart:concurrent',
    'dart:convert',
    'dart:developer',
    'dart:ffi',
    'dart:_internal',
    'dart:isolate',
    'dart:math',
    'dart:mirrors',
    'dart:typed_data',
    'dart:_vmservice',
    'dart:nativewrappers',
    'dart:_feather_dart',
  ];

  @override
  List<String> get extraRequiredLibrariesPlatform => const <String>[];
}
