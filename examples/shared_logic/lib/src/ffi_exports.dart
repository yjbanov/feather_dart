import 'dart:convert';
import 'dart:ffi';

import 'package:feather_dart_runtime/feather_dart_runtime.dart';

import 'analytics_engine.dart';
import 'host_imports.dart';

const int _kExceptionalReturn = -1;

late PortfolioEngine _globalEngine;

/// Initializes the isolate-singleton [PortfolioEngine] and publishes all C-ABI
/// Direct FFI function pointers to the host via `FeatherDartExports.register`.
void initializeSharedLogicExports({HostPlatform? customHost}) {
  _globalEngine = PortfolioEngine(host: customHost ?? const FfiHostPlatform());

  FeatherDartExports.register<Int64 Function(Int64, Int64)>(
    'fd_load_persisted_state',
    Pointer.fromFunction<Int64 Function(Int64, Int64)>(
      _fdLoadPersistedState,
      _kExceptionalReturn,
    ),
  );

  FeatherDartExports.register<
    Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
  >(
    'fd_get_summary_json',
    Pointer.fromFunction<
      Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
    >(_fdGetSummaryJson, _kExceptionalReturn),
  );

  FeatherDartExports.register<
    Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
  >(
    'fd_upsert_position',
    Pointer.fromFunction<
      Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
    >(_fdUpsertPosition, _kExceptionalReturn),
  );

  FeatherDartExports.register<
    Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
  >(
    'fd_run_monte_carlo',
    Pointer.fromFunction<
      Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
    >(_fdRunMonteCarlo, _kExceptionalReturn),
  );

  FeatherDartExports.register<Int32 Function(Int32, Int32, Pointer<Double>)>(
    'fd_simulate_var_fast',
    Pointer.fromFunction<Int32 Function(Int32, Int32, Pointer<Double>)>(
      _fdSimulateVarFast,
      _kExceptionalReturn,
    ),
  );

  FeatherDartExports.register<
    Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
  >(
    'fd_refresh_quote',
    Pointer.fromFunction<
      Int32 Function(Pointer<Uint8>, Int32, Pointer<Uint8>, Int32)
    >(_fdRefreshQuote, _kExceptionalReturn),
  );

  FeatherDartExports.register<Int64 Function(Int64, Int64)>(
    'fd_complete_quote_request',
    Pointer.fromFunction<Int64 Function(Int64, Int64)>(
      _fdCompleteQuoteRequest,
      0,
    ),
  );
}

int _fdLoadPersistedState(int unused0, int unused1) {
  return _globalEngine.loadOrSeedDefaults();
}

int _fdGetSummaryJson(
  Pointer<Uint8> unusedReq,
  int unusedLen,
  Pointer<Uint8> outBuf,
  int outCap,
) {
  return FfiUtf8.writeInto(_globalEngine.toSummaryJson(), outBuf, outCap);
}

int _fdUpsertPosition(
  Pointer<Uint8> reqPtr,
  int reqLen,
  Pointer<Uint8> outBuf,
  int outCap,
) {
  final reqJson = FfiUtf8.fromPointer(reqPtr, byteLength: reqLen);
  final decoded = jsonDecode(reqJson) as Map<String, Object?>;
  _globalEngine.upsertPosition(Position.fromJson(decoded));
  return FfiUtf8.writeInto(_globalEngine.toSummaryJson(), outBuf, outCap);
}

int _fdRunMonteCarlo(
  Pointer<Uint8> reqPtr,
  int reqLen,
  Pointer<Uint8> outBuf,
  int outCap,
) {
  var paths = 10000;
  var horizonDays = 10;
  var seed = 42;
  if (reqPtr != nullptr && reqLen > 0) {
    final reqJson = FfiUtf8.fromPointer(reqPtr, byteLength: reqLen);
    final map = jsonDecode(reqJson) as Map<String, Object?>;
    paths = (map['paths'] as num?)?.toInt() ?? paths;
    horizonDays = (map['horizonDays'] as num?)?.toInt() ?? horizonDays;
    seed = (map['seed'] as num?)?.toInt() ?? seed;
  }
  final report = _globalEngine.simulateVaR(
    paths: paths,
    horizonDays: horizonDays,
    seed: seed,
  );
  return FfiUtf8.writeInto(jsonEncode(report.toJson()), outBuf, outCap);
}

int _fdSimulateVarFast(
  int paths,
  int horizonDays,
  Pointer<Double> outMetrics5,
) {
  if (outMetrics5 == nullptr) {
    return -1;
  }
  final report = _globalEngine.simulateVaR(
    paths: paths,
    horizonDays: horizonDays,
  );
  final out = outMetrics5.asTypedList(5);
  out[0] = report.portfolioValue;
  out[1] = report.meanPnl;
  out[2] = report.var95;
  out[3] = report.cvar95;
  out[4] = report.var99;
  return report.fromCache ? 1 : 0;
}

int _fdRefreshQuote(
  Pointer<Uint8> reqPtr,
  int reqLen,
  Pointer<Uint8> outBuf,
  int outCap,
) {
  final symbol = FfiUtf8.fromPointer(reqPtr, byteLength: reqLen).trim();
  _globalEngine.refreshQuoteFromHost(symbol).then(
    (price) => print('Updated live quote for $symbol -> $price'),
    onError: (Object err) => print('Quote refresh failed for $symbol: $err'),
  );
  return FfiUtf8.writeInto('{"queued":"$symbol"}', outBuf, outCap);
}

int _fdCompleteQuoteRequest(int requestId, int priceMicros) {
  return _globalEngine.completeQuoteRequest(requestId, priceMicros) ? 1 : 0;
}
