import FeatherDart
import Foundation

/// Supplies native iOS host services (`UserDefaults` persistence, async quote
/// feed, and reactive state notifications) to Dart `@Native` declarations.
enum HostBridge {
    nonisolated(unsafe) static var onQuoteResolved: (@Sendable (Int64, Int64) -> Void)?
    nonisolated(unsafe) static var onPortfolioPersisted: (@Sendable (String) -> Void)?

    static func registerAll() {
        FeatherDartEngine.registerGlobalFunction("host_kv_read", function: hostKvRead)
        FeatherDartEngine.registerGlobalFunction("host_kv_write", function: hostKvWrite)
        FeatherDartEngine.registerGlobalFunction("host_fetch_quote_async", function: hostFetchQuoteAsync)
        FeatherDartEngine.registerGlobalFunction("host_on_portfolio_updated", function: hostOnPortfolioUpdated)
    }
}

private let hostKvRead: @convention(c) (
    UnsafePointer<UInt8>?,
    UnsafeMutablePointer<UInt8>?,
    Int32
) -> Int32 = { keyPtr, outBuf, outCap in
    guard let keyPtr else { return -1 }
    let key = String(cString: keyPtr)
    guard let stored = UserDefaults.standard.string(forKey: key) else {
        return -1
    }
    let utf8 = Array(stored.utf8)
    guard let outBuf, Int(outCap) >= utf8.count else {
        return -Int32(utf8.count) - 1
    }
    outBuf.update(from: utf8, count: utf8.count)
    return Int32(utf8.count)
}

private let hostKvWrite: @convention(c) (
    UnsafePointer<UInt8>?,
    UnsafePointer<UInt8>?,
    Int32
) -> Int32 = { keyPtr, valPtr, valLen in
    guard let keyPtr, let valPtr, valLen >= 0 else { return -1 }
    let key = String(cString: keyPtr)
    let bytes = UnsafeBufferPointer(start: valPtr, count: Int(valLen))
    guard let value = String(bytes: bytes, encoding: .utf8) else { return -1 }
    UserDefaults.standard.set(value, forKey: key)
    return 0
}

private let hostFetchQuoteAsync: @convention(c) (
    Int64,
    UnsafePointer<UInt8>?
) -> Void = { requestId, symbolPtr in
    guard let symbolPtr else { return }
    let symbol = String(cString: symbolPtr)

    Task.detached {
        try? await Task.sleep(nanoseconds: 180_000_000)
        let basePrices: [String: Double] = [
            "AAPL": 231.40,
            "GOOGL": 189.75,
            "NVDA": 146.80,
            "MSFT": 428.10,
        ]
        let base = basePrices[symbol] ?? 100.0
        let jitter = Double.random(in: -3.50...3.50)
        let priceMicros = Int64(((base + jitter) * 1_000_000.0).rounded())
        HostBridge.onQuoteResolved?(requestId, priceMicros)
    }
}

private let hostOnPortfolioUpdated: @convention(c) (
    UnsafePointer<UInt8>?,
    Int32
) -> Void = { jsonPtr, jsonLen in
    guard let jsonPtr, jsonLen > 0 else { return }
    let slice = UnsafeBufferPointer(start: jsonPtr, count: Int(jsonLen))
    if let summary = String(bytes: slice, encoding: .utf8) {
        HostBridge.onPortfolioPersisted?(summary)
    }
}
