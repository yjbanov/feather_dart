import XCTest
@testable import FeatherDart

final class FeatherDartTests: XCTestCase {
    func testBufferReadsWithinInitialCapacity() throws {
        let expected = #"{"portfolioValue":12540.50,"currency":"USD"}"#
        let actual = try FeatherDartBuffer.readUtf8(initialCapacity: 128) { outPtr, cap in
            let utf8 = Array(expected.utf8)
            guard let outPtr, Int(cap) >= utf8.count else {
                return -Int32(utf8.count)
            }
            outPtr.update(from: utf8, count: utf8.count)
            return Int32(utf8.count)
        }
        XCTAssertEqual(actual, expected)
    }

    func testBufferRetriesWithExactCapacityWhenInitialTooSmall() throws {
        let expected = String(repeating: "Δ-signal-", count: 64)
        var attempts = 0
        let actual = try FeatherDartBuffer.readUtf8(initialCapacity: 8) { outPtr, cap in
            attempts += 1
            let utf8 = Array(expected.utf8)
            guard let outPtr, Int(cap) >= utf8.count else {
                return -Int32(utf8.count)
            }
            outPtr.update(from: utf8, count: utf8.count)
            return Int32(utf8.count)
        }
        XCTAssertEqual(attempts, 2)
        XCTAssertEqual(actual, expected)
    }
}
