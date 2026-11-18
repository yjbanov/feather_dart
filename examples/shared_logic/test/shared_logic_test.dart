import 'dart:convert';
import 'dart:ffi';

import 'package:feather_dart_runtime/feather_dart_runtime.dart';
import 'package:shared_logic/shared_logic.dart';
import 'package:test/test.dart';

final class FakeHostPlatform implements HostPlatform {
  final Map<String, String> storage = <String, String>{};
  final List<(int, String)> quoteRequests = <(int, String)>[];
  final List<String> notifications = <String>[];

  @override
  String? kvRead(String key) => storage[key];

  @override
  void kvWrite(String key, String value) {
    storage[key] = value;
  }

  @override
  void fetchQuoteAsync(int requestId, String symbol) {
    quoteRequests.add((requestId, symbol));
  }

  @override
  void notifyPortfolioUpdated(String summaryJson) {
    notifications.add(summaryJson);
  }
}

void main() {
  group('PortfolioEngine domain, Monte Carlo VaR, & async Timer', () {
    late FakeHostPlatform host;
    late PortfolioEngine engine;

    setUp(() {
      host = FakeHostPlatform();
      engine = PortfolioEngine(
        host: host,
        autoSaveDebounce: const Duration(milliseconds: 30),
      );
    });

    tearDown(() {
      engine.dispose();
    });

    test('seeds default portfolio, runs Monte Carlo VaR, and hits LRU cache', () {
      final count = engine.loadOrSeedDefaults();
      expect(count, equals(3));
      expect(
        engine.totalMarketValue,
        closeTo(25 * 228.5 + 30 * 186.2 + 15 * 142.0, 1e-6),
      );

      final first = engine.simulateVaR(paths: 5000, horizonDays: 10, seed: 7);
      expect(first.fromCache, isFalse);
      expect(first.var95, greaterThan(0));
      expect(first.var99, greaterThanOrEqualTo(first.var95));
      expect(first.cvar95, greaterThanOrEqualTo(first.var95));

      final second = engine.simulateVaR(paths: 5000, horizonDays: 10, seed: 7);
      expect(second.fromCache, isTrue);
      expect(second.var95, equals(first.var95));
    });

    test('debounces persistence via Timer and completes async host quotes', () async {
      engine.loadOrSeedDefaults();

      final quoteFuture = engine.refreshQuoteFromHost('NVDA');
      expect(host.quoteRequests, hasLength(1));
      final (reqId, symbol) = host.quoteRequests.single;
      expect(symbol, equals('NVDA'));

      final completed = engine.completeQuoteRequest(reqId, 155250000);
      expect(completed, isTrue);
      expect(await quoteFuture, closeTo(155.25, 1e-6));

      await Future<void>.delayed(const Duration(milliseconds: 80));
      expect(engine.persistedRevision, equals(1));
      expect(host.storage[PortfolioEngine.storageKey], isNotNull);
      expect(host.notifications, hasLength(1));

      final restored = PortfolioEngine(host: host);
      expect(restored.loadOrSeedDefaults(), equals(3));
      final nvda = restored.positions.firstWhere((p) => p.symbol == 'NVDA');
      expect(nvda.spotPrice, closeTo(155.25, 1e-6));
      restored.dispose();
    });
  });

  group('Direct FFI C-ABI exports end-to-end trampoline invocation', () {
    tearDown(() {
      FeatherDartExports.testRegistrar = null;
    });

    test('invokes registered Pointer.fromFunction C trampolines natively', () {
      final exports = <String, int>{};
      FeatherDartExports.testRegistrar = (name, address) {
        exports[name] = address;
      };

      final host = FakeHostPlatform();
      initializeSharedLogicExports(customHost: host);

      expect(
        exports.keys,
        containsAll(<String>[
          'fd_load_persisted_state',
          'fd_get_summary_json',
          'fd_upsert_position',
          'fd_run_monte_carlo',
          'fd_simulate_var_fast',
          'fd_refresh_quote',
          'fd_complete_quote_request',
        ]),
      );

      final loadFn = Pointer<
        NativeFunction<Int64 Function(Int64, Int64)>
      >.fromAddress(exports['fd_load_persisted_state']!)
          .asFunction<int Function(int, int)>();
      expect(loadFn(0, 0), equals(3));

      final fastVarFn = Pointer<
        NativeFunction<Int32 Function(Int32, Int32, Pointer<Double>)>
      >.fromAddress(exports['fd_simulate_var_fast']!)
          .asFunction<int Function(int, int, Pointer<Double>)>();

      NativeArena.using((arena) {
        final metrics = arena.allocateFloat64(5);
        final cacheHit0 = fastVarFn(2000, 10, metrics);
        expect(cacheHit0, equals(0));
        final out = metrics.asTypedList(5);
        expect(out[0], greaterThan(10000.0));
        expect(out[2], greaterThan(0.0));
        expect(out[4], greaterThanOrEqualTo(out[2]));

        final cacheHit1 = fastVarFn(2000, 10, metrics);
        expect(cacheHit1, equals(1));

        final getSummaryFn = Pointer<
          NativeFunction<
            Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
          >
        >.fromAddress(exports['fd_get_summary_json']!)
            .asFunction<
              int Function(Pointer<Uint8>, int, Pointer<Uint8>, int)
            >();
        final jsonBuf = arena.allocateBytes(2048);
        final len = getSummaryFn(nullptr, 0, jsonBuf, 2048);
        expect(len, greaterThan(0));
        final parsed =
            jsonDecode(FfiUtf8.fromPointer(jsonBuf, byteLength: len))
                as Map<String, Object?>;
        expect(parsed['positions'], hasLength(3));
      });
    });
  });
}
