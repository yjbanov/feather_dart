import CFeatherDart
import Dispatch
import Foundation

/// Lock-guarded Dart isolate with Grand Central Dispatch event-loop & `Timer`
/// integration and zero-overhead Direct FFI symbol binding.
public final class FeatherDartIsolate: @unchecked Sendable {
    private final class SchedulerContext: @unchecked Sendable {
        private let lock = NSLock()
        private let queue: DispatchQueue
        private let errorHandler: (@Sendable (String) -> Void)?
        private var rawIsolate: OpaquePointer?
        private var timers: [Int64: DispatchSourceTimer] = [:]
        private var closed = false

        init(
            queue: DispatchQueue,
            errorHandler: (@Sendable (String) -> Void)?
        ) {
            self.queue = queue
            self.errorHandler = errorHandler
        }

        func attach(rawIsolate: OpaquePointer) {
            lock.lock()
            self.rawIsolate = rawIsolate
            lock.unlock()
        }

        func invalidate() {
            lock.lock()
            closed = true
            rawIsolate = nil
            let activeTimers = Array(timers.values)
            timers.removeAll()
            lock.unlock()

            for timer in activeTimers {
                timer.setEventHandler {}
                timer.cancel()
            }
        }

        func scheduleMessage() {
            queue.async { [weak self] in
                guard let self else { return }
                self.lock.lock()
                let handle = self.closed ? nil : self.rawIsolate
                self.lock.unlock()
                if let handle {
                    _ = feather_dart_isolate_handle_message(handle)
                }
            }
        }

        func scheduleTimer(portId: Int64, delayMs: Int64) {
            lock.lock()
            if closed {
                lock.unlock()
                return
            }
            if let existing = timers.removeValue(forKey: portId) {
                existing.setEventHandler {}
                existing.cancel()
            }
            guard delayMs >= 0 else {
                lock.unlock()
                return
            }

            let timer = DispatchSource.makeTimerSource(queue: queue)
            let clampedMs = Int(clamping: delayMs)
            timer.schedule(
                deadline: .now() + .milliseconds(clampedMs),
                leeway: .milliseconds(1)
            )
            timer.setEventHandler { [weak self] in
                guard let self else { return }
                self.lock.lock()
                if let finished = self.timers.removeValue(forKey: portId) {
                    finished.cancel()
                }
                let handle = self.closed ? nil : self.rawIsolate
                self.lock.unlock()
                if let handle {
                    _ = feather_dart_isolate_notify_timer(handle, portId)
                }
            }
            timers[portId] = timer
            lock.unlock()
            timer.resume()
        }

        func reportUnhandledError(_ message: String) {
            if let errorHandler {
                errorHandler(message)
            } else {
                Swift.print("[FeatherDart Unhandled Async Error] \(message)")
            }
        }
    }

    private let snapshot: FeatherDartSnapshot
    private let contextPtr: UnsafeMutableRawPointer
    private let context: SchedulerContext
    private let stateLock = NSLock()
    private var rawIsolate: OpaquePointer?

    /// Creates a new isolate from `snapshot` whose asynchronous port messages
    /// and `dart:async` `Timer` callbacks execute on `schedulerQueue`.
    public init(
        snapshot: FeatherDartSnapshot,
        debugName: String = "feather_dart",
        schedulerQueue: DispatchQueue = .main,
        onUnhandledAsyncError: (@Sendable (String) -> Void)? = nil
    ) throws {
        self.snapshot = snapshot
        let ctx = SchedulerContext(
            queue: schedulerQueue,
            errorHandler: onUnhandledAsyncError
        )
        self.context = ctx
        let retainedPtr = Unmanaged.passRetained(ctx).toOpaque()
        self.contextPtr = retainedPtr

        var cScheduler = FeatherDartScheduler(
            schedule_message: { _, rawCtx in
                guard let rawCtx else { return }
                let scheduler = Unmanaged<SchedulerContext>
                    .fromOpaque(rawCtx)
                    .takeUnretainedValue()
                scheduler.scheduleMessage()
            },
            schedule_timer: { _, portId, delayMs, rawCtx in
                guard let rawCtx else { return }
                let scheduler = Unmanaged<SchedulerContext>
                    .fromOpaque(rawCtx)
                    .takeUnretainedValue()
                scheduler.scheduleTimer(portId: portId, delayMs: delayMs)
            },
            on_unhandled_error: { cMsg, rawCtx in
                guard let rawCtx, let cMsg else { return }
                let scheduler = Unmanaged<SchedulerContext>
                    .fromOpaque(rawCtx)
                    .takeUnretainedValue()
                scheduler.reportUnhandledError(String(cString: cMsg))
            },
            context: retainedPtr
        )

        var errPtr: UnsafeMutablePointer<CChar>?
        let created = debugName.withCString { cName in
            feather_dart_isolate_create(snapshot.rawHandle, cName, &cScheduler, &errPtr)
        }
        guard let created else {
            ctx.invalidate()
            Unmanaged<SchedulerContext>.fromOpaque(retainedPtr).release()
            throw FeatherDartError.isolateCreationFailed(consumeFeatherDartError(errPtr))
        }

        self.rawIsolate = created
        ctx.attach(rawIsolate: created)
    }

    deinit {
        close()
    }

    /// Deterministically shuts down the isolate and cancels pending GCD timers.
    public func close() {
        stateLock.lock()
        guard let handle = rawIsolate else {
            stateLock.unlock()
            return
        }
        rawIsolate = nil
        stateLock.unlock()

        context.invalidate()
        feather_dart_isolate_destroy(handle)
        Unmanaged<SchedulerContext>.fromOpaque(contextPtr).release()
    }

    /// Registers an isolate-local raw C function pointer for `@Native` lookup.
    public func registerSymbol(_ name: String, pointer: UnsafeMutableRawPointer) {
        guard let handle = currentHandle() else { return }
        name.withCString { cName in
            feather_dart_isolate_register_symbol(handle, cName, pointer)
        }
    }

    /// Registers an isolate-local `@convention(c)` Swift function for `@Native` lookup.
    public func registerFunction<F>(_ name: String, function: F) {
        precondition(
            MemoryLayout<F>.size == MemoryLayout<UnsafeMutableRawPointer>.size,
            "Only @convention(c) function types can be registered as FFI symbols."
        )
        let ptr = unsafeBitCast(function, to: UnsafeMutableRawPointer.self)
        registerSymbol(name, pointer: ptr)
    }

    /// Invokes a zero-argument Dart entrypoint (default `"main"`) and drains microtasks.
    public func invokeEntrypoint(
        _ functionName: String = "main",
        libraryURI: String? = nil
    ) throws {
        guard let handle = currentHandle() else {
            throw FeatherDartError.isolateEnterFailed
        }
        var errPtr: UnsafeMutablePointer<CChar>?
        let ok: Bool = functionName.withCString { cFn in
            if let libraryURI {
                return libraryURI.withCString { cLib in
                    feather_dart_isolate_invoke_entrypoint(handle, cLib, cFn, &errPtr)
                }
            } else {
                return feather_dart_isolate_invoke_entrypoint(handle, nil, cFn, &errPtr)
            }
        }
        if !ok {
            throw FeatherDartError.runtimeError(consumeFeatherDartError(errPtr))
        }
    }

    /// Acquires the isolate's re-entrant lock, executes `body` inside an active
    /// Dart scope (enabling direct calls to exported C function pointers), and
    /// automatically drains microtasks before releasing the lock.
    public func withIsolate<R>(_ body: () throws -> R) throws -> R {
        guard let handle = currentHandle(), feather_dart_isolate_enter(handle) else {
            throw FeatherDartError.isolateEnterFailed
        }

        var bodyResult: Result<R, Error>
        do {
            bodyResult = .success(try body())
        } catch {
            bodyResult = .failure(error)
        }

        var exitErr: UnsafeMutablePointer<CChar>?
        let drainOk = feather_dart_isolate_exit(handle, &exitErr)
        let drainErrorMsg = drainOk ? nil : consumeFeatherDartError(exitErr)

        let value = try bodyResult.get()
        if let drainErrorMsg {
            throw FeatherDartError.runtimeError(drainErrorMsg)
        }
        return value
    }

    /// Retrieves a `@convention(c)` function pointer exported from Dart via
    /// `FeatherDartExports.register(name, pointer)`.
    public func lookupExport<F>(
        _ name: String,
        as type: F.Type = F.self
    ) throws -> F {
        precondition(
            MemoryLayout<F>.size == MemoryLayout<UnsafeMutableRawPointer>.size,
            "lookupExport target type must be a @convention(c) function pointer."
        )
        guard let handle = currentHandle() else {
            throw FeatherDartError.isolateEnterFailed
        }
        let rawPtr: UnsafeMutableRawPointer? = name.withCString { cName in
            feather_dart_isolate_lookup_export(handle, cName)
        }
        guard let rawPtr else {
            throw FeatherDartError.exportNotFound(name)
        }
        return unsafeBitCast(rawPtr, to: F.self)
    }

    private func currentHandle() -> OpaquePointer? {
        stateLock.lock()
        defer { stateLock.unlock() }
        return rawIsolate
    }
}
