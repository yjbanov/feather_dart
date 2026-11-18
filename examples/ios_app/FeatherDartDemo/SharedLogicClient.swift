import Combine
import FeatherDart
import Foundation

struct HoldingItem: Identifiable, Decodable {
    var id: String { symbol }
    let symbol: String
    let quantity: Double
    let spotPrice: Double
    let annualDrift: Double
    let annualVolatility: Double
}

struct PortfolioSummary: Decodable {
    let portfolioValue: Double
    let persistedRevision: Int
    let positions: [HoldingItem]
}

struct VaRMetrics {
    let portfolioValue: Double
    let meanPnl: Double
    let var95: Double
    let cvar95: Double
    let var99: Double
    let fromCache: Bool
    let elapsedMilliseconds: Double
}

/// Typed Swift client wrapping the Direct FFI C function pointers exported by
/// `examples/shared_logic` inside a lock-guarded `FeatherDartIsolate`.
@MainActor
final class SharedLogicClient: ObservableObject {
    private typealias Int64PairFn = @convention(c) (Int64, Int64) -> Int64
    private typealias Utf8SliceFn = @convention(c) (
        UnsafePointer<UInt8>?,
        Int32,
        UnsafeMutablePointer<UInt8>?,
        Int32
    ) -> Int32
    private typealias SimulateVarFastFn = @convention(c) (
        Int32,
        Int32,
        UnsafeMutablePointer<Double>?
    ) -> Int32

    @Published private(set) var holdings: [HoldingItem] = []
    @Published private(set) var totalValue: Double = 0
    @Published private(set) var persistedRevision: Int = 0
    @Published private(set) var latestRisk: VaRMetrics?
    @Published private(set) var logs: [String] = []
    @Published private(set) var startupError: String?

    private var isolate: FeatherDartIsolate?
    private var fnLoadState: Int64PairFn?
    private var fnGetSummary: Utf8SliceFn?
    private var fnUpsertPosition: Utf8SliceFn?
    private var fnSimulateFast: SimulateVarFastFn?
    private var fnRefreshQuote: Utf8SliceFn?
    private var fnCompleteQuote: Int64PairFn?

    init() {
        bootRuntime()
    }

    private func bootRuntime() {
        do {
            HostBridge.registerAll()
            try FeatherDartEngine.initialize { [weak self] line in
                Task { @MainActor in
                    self?.logs.append(line)
                }
            }

            let snapshot: FeatherDartSnapshot
            if FeatherDartEngine.isAotRuntime {
                snapshot = try FeatherDartSnapshot.aotFromProcessSymbols()
            } else if let dillURL = Bundle.main.url(forResource: "app_logic", withExtension: "dill") {
                snapshot = try FeatherDartSnapshot.kernelFile(at: dillURL.path)
            } else {
                throw FeatherDartError.snapshotLoadFailed(
                    "Missing AOT AppLogicSnapshot.xcframework or bundled app_logic.dill"
                )
            }

            let iso = try FeatherDartIsolate(
                snapshot: snapshot,
                debugName: "ios_pulse_risk",
                schedulerQueue: .main
            )
            try iso.invokeEntrypoint("main")

            self.isolate = iso
            self.fnLoadState = try iso.lookupExport("fd_load_persisted_state")
            self.fnGetSummary = try iso.lookupExport("fd_get_summary_json")
            self.fnUpsertPosition = try iso.lookupExport("fd_upsert_position")
            self.fnSimulateFast = try iso.lookupExport("fd_simulate_var_fast")
            self.fnRefreshQuote = try iso.lookupExport("fd_refresh_quote")
            self.fnCompleteQuote = try iso.lookupExport("fd_complete_quote_request")

            HostBridge.onQuoteResolved = { [weak self] reqId, priceMicros in
                Task { @MainActor in
                    self?.completeHostQuote(requestId: reqId, priceMicros: priceMicros)
                }
            }
            HostBridge.onPortfolioPersisted = { [weak self] summaryJson in
                Task { @MainActor in
                    self?.applySummaryJson(summaryJson)
                }
            }

            _ = try iso.withIsolate {
                self.fnLoadState?(0, 0) ?? 0
            }
            refreshSummary()
            runMonteCarlo(paths: 10_000, horizonDays: 10)
        } catch {
            startupError = error.localizedDescription
        }
    }

    func refreshSummary() {
        guard let isolate, let fnGetSummary else { return }
        do {
            let json = try isolate.withIsolate {
                try FeatherDartBuffer.readUtf8 { outPtr, cap in
                    fnGetSummary(nil, 0, outPtr, cap)
                }
            }
            applySummaryJson(json)
        } catch {
            logs.append("Summary error: \(error.localizedDescription)")
        }
    }

    func adjustShares(for item: HoldingItem, delta: Double) {
        guard let isolate, let fnUpsertPosition else { return }
        let payload = """
        {"symbol":"\(item.symbol)","quantity":\(max(0, item.quantity + delta)),\
        "spotPrice":\(item.spotPrice),"annualDrift":\(item.annualDrift),\
        "annualVolatility":\(item.annualVolatility)}
        """
        let reqBytes = Array(payload.utf8)
        do {
            let updatedJson = try isolate.withIsolate {
                try reqBytes.withUnsafeBufferPointer { reqPtr in
                    try FeatherDartBuffer.readUtf8 { outPtr, cap in
                        fnUpsertPosition(reqPtr.baseAddress, Int32(reqPtr.count), outPtr, cap)
                    }
                }
            }
            applySummaryJson(updatedJson)
            runMonteCarlo(paths: 10_000, horizonDays: 10)
        } catch {
            logs.append("Upsert error: \(error.localizedDescription)")
        }
    }

    func requestLiveQuote(symbol: String) {
        guard let isolate, let fnRefreshQuote else { return }
        let symBytes = Array(symbol.utf8)
        do {
            _ = try isolate.withIsolate {
                try symBytes.withUnsafeBufferPointer { symPtr in
                    try FeatherDartBuffer.readUtf8(initialCapacity: 64) { outPtr, cap in
                        fnRefreshQuote(symPtr.baseAddress, Int32(symPtr.count), outPtr, cap)
                    }
                }
            }
        } catch {
            logs.append("Quote request error: \(error.localizedDescription)")
        }
    }

    func runMonteCarlo(paths: Int, horizonDays: Int) {
        guard let isolate, let fnSimulateFast else { return }
        let t0 = CFAbsoluteTimeGetCurrent()
        do {
            var metrics = [Double](repeating: 0.0, count: 5)
            let cacheFlag = try isolate.withIsolate {
                metrics.withUnsafeMutableBufferPointer { buf in
                    fnSimulateFast(Int32(paths), Int32(horizonDays), buf.baseAddress)
                }
            }
            let elapsedMs = (CFAbsoluteTimeGetCurrent() - t0) * 1000.0
            latestRisk = VaRMetrics(
                portfolioValue: metrics[0],
                meanPnl: metrics[1],
                var95: metrics[2],
                cvar95: metrics[3],
                var99: metrics[4],
                fromCache: cacheFlag == 1,
                elapsedMilliseconds: elapsedMs
            )
        } catch {
            logs.append("Simulation error: \(error.localizedDescription)")
        }
    }

    private func completeHostQuote(requestId: Int64, priceMicros: Int64) {
        guard let isolate, let fnCompleteQuote else { return }
        do {
            _ = try isolate.withIsolate {
                fnCompleteQuote(requestId, priceMicros)
            }
            refreshSummary()
            runMonteCarlo(paths: 10_000, horizonDays: 10)
        } catch {
            logs.append("Quote completion error: \(error.localizedDescription)")
        }
    }

    private func applySummaryJson(_ json: String) {
        guard let data = json.data(using: .utf8),
              let summary = try? JSONDecoder().decode(PortfolioSummary.self, from: data)
        else {
            return
        }
        holdings = summary.positions
        totalValue = summary.portfolioValue
        persistedRevision = summary.persistedRevision
    }
}
