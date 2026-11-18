import Foundation

/// Helpers for marshaling UTF-8 strings and binary buffers across `@convention(c)`
/// Direct FFI calls using the `FfiUtf8.writeInto` two-pass buffer convention.
public enum FeatherDartBuffer {
    /// Reads a UTF-8 string from a Dart FFI export following the
    /// `(UnsafeMutablePointer<UInt8>?, Int32) -> Int32` convention:
    /// - Returns `>= 0` when bytes were written into the buffer.
    /// - Returns `< 0` (`-requiredLength`) when the buffer was too small.
    public static func readUtf8(
        initialCapacity: Int = 1024,
        writer: (UnsafeMutablePointer<UInt8>?, Int32) -> Int32
    ) throws -> String {
        var buffer = [UInt8](repeating: 0, count: max(1, initialCapacity))
        let firstLen = buffer.withUnsafeMutableBufferPointer { ptr in
            writer(ptr.baseAddress, Int32(ptr.count))
        }
        if firstLen >= 0 {
            guard let str = String(bytes: buffer.prefix(Int(firstLen)), encoding: .utf8) else {
                throw FeatherDartError.invalidUtf8Payload
            }
            return str
        }

        let required = Int(-firstLen)
        buffer = [UInt8](repeating: 0, count: required + 1)
        let secondLen = buffer.withUnsafeMutableBufferPointer { ptr in
            writer(ptr.baseAddress, Int32(ptr.count))
        }
        guard secondLen >= 0,
              let str = String(bytes: buffer.prefix(Int(secondLen)), encoding: .utf8)
        else {
            throw FeatherDartError.invalidUtf8Payload
        }
        return str
    }
}
