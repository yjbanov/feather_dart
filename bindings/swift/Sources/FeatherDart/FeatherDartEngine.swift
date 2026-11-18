import CFeatherDart
import Foundation

/// Errors thrown by the FeatherDart Swift embedding layer.
public enum FeatherDartError: LocalizedError, Equatable {
    case engineInitializationFailed(String)
    case snapshotLoadFailed(String)
    case isolateCreationFailed(String)
    case isolateEnterFailed
    case runtimeError(String)
    case exportNotFound(String)
    case invalidUtf8Payload

    public var errorDescription: String? {
        switch self {
        case .engineInitializationFailed(let msg):
            return "FeatherDart engine initialization failed: \(msg)"
        case .snapshotLoadFailed(let msg):
            return "FeatherDart snapshot load failed: \(msg)"
        case .isolateCreationFailed(let msg):
            return "FeatherDart isolate creation failed: \(msg)"
        case .isolateEnterFailed:
            return "Failed to acquire or enter FeatherDart isolate (isolate may be closed)."
        case .runtimeError(let msg):
            return "FeatherDart runtime error: \(msg)"
        case .exportNotFound(let name):
            return "Dart FFI export '\(name)' was not registered via FeatherDartExports.register."
        case .invalidUtf8Payload:
            return "Dart FFI buffer did not contain valid UTF-8 data."
        }
    }
}

func consumeFeatherDartError(_ errorPtr: UnsafeMutablePointer<CChar>?) -> String {
    guard let errorPtr else {
        return "Unknown error"
    }
    let text = String(cString: errorPtr)
    feather_dart_free_string(errorPtr)
    return text
}

/// Process-wide lifecycle and global `@Native` symbol registry for FeatherDart.
public enum FeatherDartEngine {
    private static let lock = NSLock()
    private static var logHandler: (@Sendable (String) -> Void)?

    /// Whether the linked `libfeather_dart` binary is the AOT precompiled runtime.
    public static var isAotRuntime: Bool {
        feather_dart_is_aot_runtime()
    }

    /// Initializes the FeatherDart VM process state. Safe to invoke multiple times.
    public static func initialize(
        vmFlags: [String] = [],
        onLog: (@Sendable (String) -> Void)? = nil
    ) throws {
        lock.lock()
        if let onLog {
            logHandler = onLog
        }
        lock.unlock()

        let cFlags: [UnsafeMutablePointer<CChar>?] = vmFlags.map { strdup($0) }
        defer {
            for ptr in cFlags where ptr != nil {
                free(ptr)
            }
        }

        var rawErr: UnsafeMutablePointer<CChar>?
        let ok = cFlags.withUnsafeBufferPointer { buffer in
            var config = FeatherDartEngineConfig(
                vm_flags: buffer.baseAddress?.withMemoryRebound(
                    to: UnsafePointer<CChar>?.self,
                    capacity: buffer.count
                ) { $0 },
                vm_flags_count: Int32(buffer.count),
                log_callback: featherDartSwiftLogTrampoline,
                log_user_data: nil
            )
            return feather_dart_engine_init(&config, &rawErr)
        }

        if !ok {
            throw FeatherDartError.engineInitializationFailed(consumeFeatherDartError(rawErr))
        }
    }

    /// Updates the process-wide handler receiving `print()` lines from Dart.
    public static func setLogHandler(_ handler: (@Sendable (String) -> Void)?) {
        lock.lock()
        logHandler = handler
        lock.unlock()
        feather_dart_set_log_callback(handler == nil ? nil : featherDartSwiftLogTrampoline, nil)
    }

    /// Registers a raw native pointer under `symbol` for `@Native` resolution across all isolates.
    public static func registerGlobalSymbol(_ symbol: String, pointer: UnsafeMutableRawPointer) {
        symbol.withCString { cName in
            feather_dart_register_global_symbol(cName, pointer)
        }
    }

    /// Registers a `@convention(c)` Swift function under `symbol` for `@Native` resolution.
    public static func registerGlobalFunction<F>(_ symbol: String, function: F) {
        precondition(
            MemoryLayout<F>.size == MemoryLayout<UnsafeMutableRawPointer>.size,
            "Only @convention(c) function types can be registered as FFI symbols."
        )
        let rawPtr = unsafeBitCast(function, to: UnsafeMutableRawPointer.self)
        registerGlobalSymbol(symbol, pointer: rawPtr)
    }

    /// Shuts down all active isolates and cleans up the Dart VM.
    public static func shutdown() {
        feather_dart_engine_shutdown()
    }

    fileprivate static func dispatchLog(_ line: String) {
        lock.lock()
        let handler = logHandler
        lock.unlock()
        if let handler {
            handler(line)
        } else {
            Swift.print("[FeatherDart] \(line)")
        }
    }
}

private let featherDartSwiftLogTrampoline: FeatherDartLogCallback = { cLine, _ in
    guard let cLine else { return }
    FeatherDartEngine.dispatchLog(String(cString: cLine))
}
