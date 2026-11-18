import 'dart:ffi';

import 'package:feather_dart_runtime/feather_dart_runtime.dart';
import 'package:test/test.dart';

int _sampleExportCallback(int x) => x * 2;

void main() {
  group('NativeArena & FfiUtf8', () {
    test('round-trips UTF-8 strings with ASCII and multi-byte runes', () {
      const text = 'FeatherDart λ 🪶 — 100% FFI';
      final decoded = NativeArena.using((arena) {
        final ptr = arena.toCString(text);
        return FfiUtf8.fromPointer(ptr);
      });
      expect(decoded, equals(text));
    });

    test('writeInto reports negative required byte length on small buffer', () {
      const payload = '{"status":"ok","items":[1,2,3]}';
      NativeArena.using((arena) {
        final tiny = arena.allocateBytes(4);
        final needed = FfiUtf8.writeInto(payload, tiny, 4);
        expect(needed, lessThan(0));

        final exactLen = -needed;
        final full = arena.allocateBytes(exactLen + 1);
        final written = FfiUtf8.writeInto(payload, full, exactLen + 1);
        expect(written, equals(exactLen));
        expect(FfiUtf8.fromPointer(full, byteLength: written), equals(payload));
      });
    });
  });

  group('FeatherDartExports', () {
    tearDown(() {
      FeatherDartExports.testRegistrar = null;
    });

    test('captures exported Pointer.fromFunction address via testRegistrar', () {
      final registered = <String, int>{};
      FeatherDartExports.testRegistrar = (name, address) {
        registered[name] = address;
      };

      final fnPtr =
          Pointer.fromFunction<Int64 Function(Int64)>(_sampleExportCallback, 0);
      FeatherDartExports.register('sample_double', fnPtr);

      expect(registered['sample_double'], equals(fnPtr.address));
      expect(fnPtr.address, isNonZero);
    });
  });
}
