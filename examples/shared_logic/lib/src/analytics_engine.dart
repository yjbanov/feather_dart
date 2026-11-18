import 'dart:async';
import 'dart:collection';
import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

import 'host_imports.dart';

/// Single portfolio holding managed in shared Dart memory.
final class Position {
  final String symbol;
  final double quantity;
  final double spotPrice;
  final double annualDrift;
  final double annualVolatility;

  const Position({
    required this.symbol,
    required this.quantity,
    required this.spotPrice,
    required this.annualDrift,
    required this.annualVolatility,
  });

  double get marketValue => quantity * spotPrice;

  Position copyWith({
    double? quantity,
    double? spotPrice,
    double? annualDrift,
    double? annualVolatility,
  }) {
    return Position(
      symbol: symbol,
      quantity: quantity ?? this.quantity,
      spotPrice: spotPrice ?? this.spotPrice,
      annualDrift: annualDrift ?? this.annualDrift,
      annualVolatility: annualVolatility ?? this.annualVolatility,
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
    'symbol': symbol,
    'quantity': quantity,
    'spotPrice': spotPrice,
    'annualDrift': annualDrift,
    'annualVolatility': annualVolatility,
  };

  factory Position.fromJson(Map<String, Object?> map) {
    return Position(
      symbol: map['symbol']! as String,
      quantity: (map['quantity']! as num).toDouble(),
      spotPrice: (map['spotPrice']! as num).toDouble(),
      annualDrift: ((map['annualDrift'] as num?) ?? 0.08).toDouble(),
      annualVolatility: ((map['annualVolatility'] as num?) ?? 0.25).toDouble(),
    );
  }
}

/// Monte Carlo Value-at-Risk (VaR) and Expected Shortfall metrics computed in
/// contiguous `Float64List` memory.
final class RiskReport {
  final double portfolioValue;
  final double meanPnl;
  final double var95;
  final double cvar95;
  final double var99;
  final int paths;
  final int horizonDays;
  final bool fromCache;

  const RiskReport({
    required this.portfolioValue,
    required this.meanPnl,
    required this.var95,
    required this.cvar95,
    required this.var99,
    required this.paths,
    required this.horizonDays,
    required this.fromCache,
  });

  RiskReport asCached() => RiskReport(
    portfolioValue: portfolioValue,
    meanPnl: meanPnl,
    var95: var95,
    cvar95: cvar95,
    var99: var99,
    paths: paths,
    horizonDays: horizonDays,
    fromCache: true,
  );

  Map<String, Object?> toJson() => <String, Object?>{
    'portfolioValue': portfolioValue,
    'meanPnl': meanPnl,
    'var95': var95,
    'cvar95': cvar95,
    'var99': var99,
    'paths': paths,
    'horizonDays': horizonDays,
    'fromCache': fromCache,
  };
}

/// Bounded LRU cache built on `dart:collection` `LinkedHashMap`.
final class _LruCache<K, V> {
  final int maxEntries;
  final LinkedHashMap<K, V> _map = LinkedHashMap<K, V>();

  _LruCache(this.maxEntries);

  V? get(K key) {
    final existing = _map.remove(key);
    if (existing != null) {
      _map[key] = existing;
    }
    return existing;
  }

  void put(K key, V value) {
    _map.remove(key);
    _map[key] = value;
    while (_map.length > maxEntries) {
      _map.remove(_map.keys.first);
    }
  }

  void clear() => _map.clear();
}

/// Shared mobile portfolio & quantitative risk engine running inside FeatherDart.
///
/// Exercises all 7 allowed `dart:*` libraries (`core`, `async`, `collection`,
/// `convert`, `ffi`, `math`, `typed_data`) while delegating persistence and
/// network quote I/O to the Swift / Kotlin host via `HostPlatform`.
final class PortfolioEngine {
  static const String storageKey = 'feather_dart.portfolio.v1';

  final HostPlatform host;
  final Duration autoSaveDebounce;

  final Map<String, Position> _positions = <String, Position>{};
  final _LruCache<String, RiskReport> _simulationCache =
      _LruCache<String, RiskReport>(16);
  final Map<int, (String, Completer<double>)> _pendingQuoteRequests =
      <int, (String, Completer<double>)>{};

  int _nextRequestId = 1;
  int _persistedRevision = 0;
  Timer? _debounceSaveTimer;

  PortfolioEngine({
    required this.host,
    this.autoSaveDebounce = const Duration(milliseconds: 250),
  });

  int get persistedRevision => _persistedRevision;

  List<Position> get positions => _positions.values.toList(growable: false);

  double get totalMarketValue {
    var sum = 0.0;
    for (final p in _positions.values) {
      sum += p.marketValue;
    }
    return sum;
  }

  int loadOrSeedDefaults() {
    final raw = host.kvRead(storageKey);
    if (raw != null && raw.isNotEmpty) {
      final decoded = jsonDecode(raw) as Map<String, Object?>;
      final items = (decoded['positions'] as List<Object?>?) ?? const [];
      _positions.clear();
      for (final item in items) {
        final pos = Position.fromJson(item! as Map<String, Object?>);
        _positions[pos.symbol] = pos;
      }
      _simulationCache.clear();
      return _positions.length;
    }

    _positions
      ..clear()
      ..['AAPL'] = const Position(
        symbol: 'AAPL',
        quantity: 25,
        spotPrice: 228.50,
        annualDrift: 0.11,
        annualVolatility: 0.24,
      )
      ..['GOOGL'] = const Position(
        symbol: 'GOOGL',
        quantity: 30,
        spotPrice: 186.20,
        annualDrift: 0.12,
        annualVolatility: 0.26,
      )
      ..['NVDA'] = const Position(
        symbol: 'NVDA',
        quantity: 15,
        spotPrice: 142.00,
        annualDrift: 0.18,
        annualVolatility: 0.42,
      );
    _simulationCache.clear();
    return _positions.length;
  }

  Position upsertPosition(Position position) {
    if (position.quantity <= 0) {
      _positions.remove(position.symbol);
    } else {
      _positions[position.symbol] = position;
    }
    _simulationCache.clear();
    _scheduleDebouncedPersist();
    return position;
  }

  RiskReport simulateVaR({
    int paths = 10000,
    int horizonDays = 10,
    int seed = 42,
  }) {
    final clampedPaths = paths.clamp(100, 250000);
    final clampedHorizon = horizonDays.clamp(1, 365);

    if (_positions.isEmpty) {
      return RiskReport(
        portfolioValue: 0,
        meanPnl: 0,
        var95: 0,
        cvar95: 0,
        var99: 0,
        paths: clampedPaths,
        horizonDays: clampedHorizon,
        fromCache: false,
      );
    }

    final cacheKey = _buildCacheKey(clampedPaths, clampedHorizon, seed);
    final cached = _simulationCache.get(cacheKey);
    if (cached != null) {
      return cached.asCached();
    }

    final held = _positions.values.toList(growable: false);
    final dt = clampedHorizon / 252.0;
    final sqrtDt = math.sqrt(dt);

    final assetCount = held.length;
    final baseValues = Float64List(assetCount);
    final driftTerms = Float64List(assetCount);
    final diffTerms = Float64List(assetCount);

    var initialTotal = 0.0;
    for (var j = 0; j < assetCount; j++) {
      final p = held[j];
      final val = p.marketValue;
      initialTotal += val;
      baseValues[j] = val;
      final sigma = p.annualVolatility;
      driftTerms[j] = (p.annualDrift - 0.5 * sigma * sigma) * dt;
      diffTerms[j] = sigma * sqrtDt;
    }

    final pnlSamples = Float64List(clampedPaths);
    final rng = math.Random(seed);
    var sumPnl = 0.0;

    for (var i = 0; i < clampedPaths; i++) {
      final marketZ = _nextStandardNormal(rng);
      const rho = 0.35;
      final idioScale = math.sqrt(1.0 - rho * rho);

      var terminalTotal = 0.0;
      for (var j = 0; j < assetCount; j++) {
        final eps = _nextStandardNormal(rng);
        final z = rho * marketZ + idioScale * eps;
        terminalTotal +=
            baseValues[j] * math.exp(driftTerms[j] + diffTerms[j] * z);
      }
      final pnl = terminalTotal - initialTotal;
      pnlSamples[i] = pnl;
      sumPnl += pnl;
    }

    pnlSamples.sort();

    final idx95 = (0.05 * clampedPaths).floor().clamp(0, clampedPaths - 1);
    final idx99 = (0.01 * clampedPaths).floor().clamp(0, clampedPaths - 1);

    final var95 = math.max(0.0, -pnlSamples[idx95]);
    final var99 = math.max(0.0, -pnlSamples[idx99]);

    var tailSum = 0.0;
    final tailCount = idx95 + 1;
    for (var k = 0; k < tailCount; k++) {
      tailSum += pnlSamples[k];
    }
    final cvar95 = math.max(0.0, -(tailSum / tailCount));

    final report = RiskReport(
      portfolioValue: initialTotal,
      meanPnl: sumPnl / clampedPaths,
      var95: var95,
      cvar95: cvar95,
      var99: var99,
      paths: clampedPaths,
      horizonDays: clampedHorizon,
      fromCache: false,
    );
    _simulationCache.put(cacheKey, report);
    return report;
  }

  Future<double> refreshQuoteFromHost(String symbol) {
    final requestId = _nextRequestId++;
    final completer = Completer<double>();
    _pendingQuoteRequests[requestId] = (symbol, completer);
    host.fetchQuoteAsync(requestId, symbol);
    return completer.future;
  }

  bool completeQuoteRequest(int requestId, int priceMicros) {
    final entry = _pendingQuoteRequests.remove(requestId);
    if (entry == null) {
      return false;
    }
    final (symbol, completer) = entry;
    if (priceMicros < 0) {
      completer.completeError(
        StateError('Host failed to fetch quote for $symbol'),
      );
      return false;
    }

    final newPrice = priceMicros / 1e6;
    final existing = _positions[symbol];
    if (existing != null) {
      _positions[symbol] = existing.copyWith(spotPrice: newPrice);
      _simulationCache.clear();
      _scheduleDebouncedPersist();
    }
    completer.complete(newPrice);
    return true;
  }

  String toSummaryJson() {
    return jsonEncode(<String, Object?>{
      'portfolioValue': totalMarketValue,
      'persistedRevision': _persistedRevision,
      'positions': _positions.values.map((p) => p.toJson()).toList(),
    });
  }

  void dispose() {
    _debounceSaveTimer?.cancel();
    _debounceSaveTimer = null;
  }

  void _scheduleDebouncedPersist() {
    _debounceSaveTimer?.cancel();
    _debounceSaveTimer = Timer(autoSaveDebounce, () {
      _persistedRevision++;
      final summary = toSummaryJson();
      host.kvWrite(storageKey, summary);
      host.notifyPortfolioUpdated(summary);
    });
  }

  String _buildCacheKey(int paths, int horizonDays, int seed) {
    final sb = StringBuffer('$paths:$horizonDays:$seed');
    for (final p in _positions.values) {
      sb.write(
        '|${p.symbol}:${p.quantity.toStringAsFixed(4)}:${p.spotPrice.toStringAsFixed(4)}',
      );
    }
    return sb.toString();
  }

  static double _nextStandardNormal(math.Random rng) {
    var u1 = rng.nextDouble();
    while (u1 <= 1e-12) {
      u1 = rng.nextDouble();
    }
    final u2 = rng.nextDouble();
    return math.sqrt(-2.0 * math.log(u1)) * math.cos(2.0 * math.pi * u2);
  }
}
