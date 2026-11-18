import 'dart:ffi';

import 'package:feather_dart_runtime/feather_dart_runtime.dart';

// ============================================================================
// Host-supplied @Native functions (implemented in Swift on iOS & Kotlin/JNI on Android)
// ============================================================================

@Native<Int32 Function(Pointer<Uint8>, Pointer<Uint8>, Int32)>(
  symbol: 'host_kv_read',
)
external int _nativeHostKvRead(
  Pointer<Uint8> key,
  Pointer<Uint8> outBuf,
  int outCapacity,
);

@Native<Int32 Function(Pointer<Uint8>, Pointer<Uint8>, Int32)>(
  symbol: 'host_kv_write',
)
external int _nativeHostKvWrite(
  Pointer<Uint8> key,
  Pointer<Uint8> valuePtr,
  int valueLength,
);

@Native<Void Function(Int64, Pointer<Uint8>)>(
  symbol: 'host_fetch_quote_async',
)
external void _nativeHostFetchQuoteAsync(
  int requestId,
  Pointer<Uint8> tickerSymbol,
);

@Native<Void Function(Pointer<Uint8>, Int32)>(
  symbol: 'host_on_portfolio_updated',
)
external void _nativeHostOnPortfolioUpdated(
  Pointer<Uint8> summaryJsonPtr,
  int summaryLength,
);

/// Abstraction over host-provided capabilities (`@Native` in production,
/// injectable in pure-Dart unit tests).
abstract interface class HostPlatform {
  String? kvRead(String key);
  void kvWrite(String key, String value);
  void fetchQuoteAsync(int requestId, String symbol);
  void notifyPortfolioUpdated(String summaryJson);
}

/// Default production implementation backed directly by `@Native` FFI symbols
/// supplied by the Swift or Kotlin host app.
final class FfiHostPlatform implements HostPlatform {
  const FfiHostPlatform();

  @override
  String? kvRead(String key) {
    return NativeArena.using((arena) {
      final cKey = arena.toCString(key);
      var capacity = 1024;
      var buf = arena.allocateBytes(capacity);
      var len = _nativeHostKvRead(cKey, buf, capacity);
      if (len == -1) {
        return null;
      }
      if (len < -1) {
        capacity = (-len) + 1;
        buf = arena.allocateBytes(capacity);
        len = _nativeHostKvRead(cKey, buf, capacity);
      }
      if (len < 0) {
        return null;
      }
      return FfiUtf8.fromPointer(buf, byteLength: len);
    });
  }

  @override
  void kvWrite(String key, String value) {
    NativeArena.using((arena) {
      final cKey = arena.toCString(key);
      final cVal = arena.toCString(value);
      int byteLen = 0;
      while (Pointer<Uint8>.fromAddress(cVal.address + byteLen).value != 0) {
        byteLen++;
      }
      _nativeHostKvWrite(cKey, cVal, byteLen);
    });
  }

  @override
  void fetchQuoteAsync(int requestId, String symbol) {
    NativeArena.using((arena) {
      final cSym = arena.toCString(symbol);
      _nativeHostFetchQuoteAsync(requestId, cSym);
    });
  }

  @override
  void notifyPortfolioUpdated(String summaryJson) {
    NativeArena.using((arena) {
      final cJson = arena.toCString(summaryJson);
      int byteLen = 0;
      while (Pointer<Uint8>.fromAddress(cJson.address + byteLen).value != 0) {
        byteLen++;
      }
      _nativeHostOnPortfolioUpdated(cJson, byteLen);
    });
  }
}
