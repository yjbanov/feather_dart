import CFeatherDart
import Foundation

/// Represents a loaded FeatherDart AOT or Kernel (`.dill`) snapshot.
///
/// Multiple `FeatherDartIsolate` instances can be spawned from a single snapshot.
public final class FeatherDartSnapshot: @unchecked Sendable {
    let rawHandle: OpaquePointer

    private init(rawHandle: OpaquePointer) {
        self.rawHandle = rawHandle
    }

    deinit {
        feather_dart_snapshot_destroy(rawHandle)
    }

    /// Loads a Mach-O AOT snapshot `.framework` binary or `.dylib` from disk.
    public static func aotFile(at path: String) throws -> FeatherDartSnapshot {
        var errPtr: UnsafeMutablePointer<CChar>?
        let handle = path.withCString { cPath in
            feather_dart_snapshot_load_aot_file(cPath, &errPtr)
        }
        guard let handle else {
            throw FeatherDartError.snapshotLoadFailed(consumeFeatherDartError(errPtr))
        }
        return FeatherDartSnapshot(rawHandle: handle)
    }

    /// Resolves AOT snapshot symbols linked into the current process or embedded
    /// frameworks (`_kDartSnapshotData` / `_kDartSnapshotText`).
    public static func aotFromProcessSymbols() throws -> FeatherDartSnapshot {
        var errPtr: UnsafeMutablePointer<CChar>?
        guard let handle = feather_dart_snapshot_from_process_symbols(&errPtr) else {
            throw FeatherDartError.snapshotLoadFailed(consumeFeatherDartError(errPtr))
        }
        return FeatherDartSnapshot(rawHandle: handle)
    }

    /// Loads a compiled Kernel `.dill` file from disk (development / host mode).
    public static func kernelFile(at path: String) throws -> FeatherDartSnapshot {
        var errPtr: UnsafeMutablePointer<CChar>?
        let handle = path.withCString { cPath in
            feather_dart_snapshot_load_kernel_file(cPath, &errPtr)
        }
        guard let handle else {
            throw FeatherDartError.snapshotLoadFailed(consumeFeatherDartError(errPtr))
        }
        return FeatherDartSnapshot(rawHandle: handle)
    }

    /// Loads a compiled Kernel `.dill` payload from in-memory `Data` (copies buffer).
    public static func kernelData(_ data: Data) throws -> FeatherDartSnapshot {
        var errPtr: UnsafeMutablePointer<CChar>?
        let handle: OpaquePointer? = data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.bindMemory(to: UInt8.self).baseAddress else {
                return nil
            }
            return feather_dart_snapshot_load_kernel_buffer(base, data.count, true, &errPtr)
        }
        guard let handle else {
            throw FeatherDartError.snapshotLoadFailed(consumeFeatherDartError(errPtr))
        }
        return FeatherDartSnapshot(rawHandle: handle)
    }
}
