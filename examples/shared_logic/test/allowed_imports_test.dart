import 'dart:io';

import 'package:test/test.dart';

/// Verifies that every production `.dart` source file in `feather_dart_runtime`
/// and `shared_logic` imports ONLY the 7 allowed `dart:*` libraries:
/// `async`, `collection`, `convert`, `core`, `ffi`, `math`, `typed_data`.
void main() {
  const allowedDartLibraries = <String>{
    'async',
    'collection',
    'convert',
    'core',
    'ffi',
    'math',
    'typed_data',
  };

  final dartImportPattern = RegExp(
    r'''(?:import|export)\s+['"]dart:([a-zA-Z0-9_]+)['"]''',
  );

  test('production Dart sources only import the 7 allowed dart:* libraries', () {
    final dirsToAudit = <Directory>[
      Directory('../../packages/feather_dart_runtime/lib'),
      Directory('lib'),
    ];

    var scannedFiles = 0;
    final violations = <String>[];

    for (final dir in dirsToAudit) {
      expect(dir.existsSync(), isTrue, reason: 'Missing ${dir.path}');
      for (final entity in dir.listSync(recursive: true)) {
        if (entity is! File || !entity.path.endsWith('.dart')) {
          continue;
        }
        scannedFiles++;
        final content = entity.readAsStringSync();
        for (final match in dartImportPattern.allMatches(content)) {
          final libName = match.group(1)!;
          if (!allowedDartLibraries.contains(libName)) {
            violations.add('${entity.path}: disallowed import dart:$libName');
          }
        }
      }
    }

    expect(scannedFiles, greaterThanOrEqualTo(5));
    expect(violations, isEmpty);
  });
}
