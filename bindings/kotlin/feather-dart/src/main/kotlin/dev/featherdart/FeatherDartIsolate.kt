package dev.featherdart

import java.io.Closeable
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.util.concurrent.atomic.AtomicLong

/**
 * Lock-guarded Dart isolate integrated with Android's [Handler] or Kotlin
 * Coroutines, supporting re-entrant synchronous calls and Direct FFI exports.
 */
class FeatherDartIsolate(
    @Suppress("unused") private val snapshot: FeatherDartSnapshot,
    debugName: String = "feather_dart",
    private val schedulerBridge: FeatherDartSchedulerBridge =
        FeatherDartSchedulerBridge.androidHandler(),
    private val onUnhandledAsyncError: ((String) -> Unit)? = null,
) : Closeable {

    private val holderAtomic = AtomicLong(0L)

    init {
        val callbacks = InternalIsolateCallbacks(
            onMessage = {
                schedulerBridge.scheduleMessage {
                    val h = holderAtomic.get()
                    if (h != 0L) {
                        FeatherDartNative.nativeIsolateHandleMessage(h)
                    }
                }
            },
            onTimer = { portId, delayMs ->
                schedulerBridge.scheduleTimer(portId, delayMs) { firedPort ->
                    val h = holderAtomic.get()
                    if (h != 0L) {
                        FeatherDartNative.nativeIsolateNotifyTimer(h, firedPort)
                    }
                }
            },
            onError = { message ->
                onUnhandledAsyncError?.invoke(message)
            },
        )
        val created = FeatherDartNative.nativeIsolateCreate(
            snapshot.nativeHandle,
            debugName,
            callbacks,
        )
        holderAtomic.set(created)
    }

    /**
     * Raw `FeatherDartIsolate*` C pointer for consumer NDK/JNI libraries calling
     * functions from `feather_dart.h` directly.
     */
    val rawIsolatePointer: Long
        get() = FeatherDartNative.nativeIsolateRawPointer(requireActiveHolder())

    /** Registers an isolate-local native C function pointer for `@Native` lookup. */
    fun registerSymbol(symbol: String, functionPointer: Long) {
        require(functionPointer != 0L) { "functionPointer must be non-zero" }
        FeatherDartNative.nativeIsolateRegisterSymbol(
            requireActiveHolder(),
            symbol,
            functionPointer,
        )
    }

    /** Invokes a zero-argument Dart entrypoint (default `"main"`) and drains microtasks. */
    fun invokeEntrypoint(functionName: String = "main", libraryUri: String? = null) {
        FeatherDartNative.nativeIsolateInvokeEntrypoint(
            requireActiveHolder(),
            libraryUri,
            functionName,
        )
    }

    /**
     * Looks up a raw C function pointer (`void*`) exported from Dart via
     * `FeatherDartExports.register(name, pointer)`.
     */
    fun lookupExportPointer(exportName: String): Long {
        val ptr = FeatherDartNative.nativeIsolateLookupExport(
            requireActiveHolder(),
            exportName,
        )
        if (ptr == 0L) {
            throw FeatherDartException(
                "Dart FFI export '$exportName' was not registered via FeatherDartExports.register.",
            )
        }
        return ptr
    }

    /**
     * Acquires the isolate's re-entrant lock, runs [block] inside an active
     * Dart handle scope, and drains pending microtasks before releasing the lock.
     */
    inline fun <R> withIsolate(block: FeatherDartIsolateScope.() -> R): R {
        val holder = enterInternal()
        var blockError: Throwable? = null
        val result: R = try {
            FeatherDartIsolateScope(this).block()
        } catch (t: Throwable) {
            blockError = t
            @Suppress("UNCHECKED_CAST")
            null as R
        } finally {
            try {
                exitInternal(holder)
            } catch (exitEx: Throwable) {
                if (blockError == null) {
                    throw exitEx
                }
            }
        }
        if (blockError != null) {
            throw blockError
        }
        return result
    }

    @PublishedApi
    internal fun enterInternal(): Long {
        val holder = requireActiveHolder()
        if (!FeatherDartNative.nativeIsolateEnter(holder)) {
            throw FeatherDartException(
                "Failed to enter FeatherDartIsolate (isolate may be closed).",
            )
        }
        return holder
    }

    @PublishedApi
    internal fun exitInternal(holder: Long) {
        FeatherDartNative.nativeIsolateExit(holder)
    }

    override fun close() {
        val holder = holderAtomic.getAndSet(0L)
        if (holder != 0L) {
            schedulerBridge.cancelAll()
            FeatherDartNative.nativeIsolateDestroy(holder)
        }
    }

    private fun requireActiveHolder(): Long {
        val h = holderAtomic.get()
        if (h == 0L) {
            throw FeatherDartException("FeatherDartIsolate has already been closed.")
        }
        return h
    }
}

/**
 * Scope receiver active inside [FeatherDartIsolate.withIsolate], providing
 * zero-copy Direct FFI trampolines for calling exported Dart C functions.
 */
@JvmInline
value class FeatherDartIsolateScope(val isolate: FeatherDartIsolate) {
    /** Calls a `void (*)(void)` Dart FFI export. */
    fun callVoid(fnPtr: Long) {
        FeatherDartNative.nativeCallVoid(fnPtr)
    }

    /** Calls an `int64_t (*)(int64_t, int64_t)` Dart FFI export. */
    fun callInt64(fnPtr: Long, arg0: Long = 0L, arg1: Long = 0L): Long {
        return FeatherDartNative.nativeCallInt64(fnPtr, arg0, arg1)
    }

    /**
     * Calls an `int32_t (*)(const uint8_t*, int32_t, uint8_t*, int32_t)` Dart
     * FFI export using zero-copy direct [ByteBuffer]s.
     */
    fun callDirectBuffer(
        fnPtr: Long,
        request: ByteBuffer?,
        requestLength: Int,
        response: ByteBuffer?,
        responseCapacity: Int,
    ): Int {
        require(request == null || request.isDirect) { "request ByteBuffer must be direct" }
        require(response == null || response.isDirect) { "response ByteBuffer must be direct" }
        return FeatherDartNative.nativeCallDirectBuffer(
            fnPtr,
            request,
            requestLength,
            response,
            responseCapacity,
        )
    }

    /**
     * Invokes a UTF-8 request/response Dart FFI export following the two-pass
     * `FfiUtf8.writeInto` buffer capacity protocol.
     */
    fun callUtf8(
        fnPtr: Long,
        requestJson: String = "",
        initialResponseCapacity: Int = 1024,
    ): String {
        val reqBytes = requestJson.toByteArray(StandardCharsets.UTF_8)
        val reqBuf = if (reqBytes.isNotEmpty()) {
            ByteBuffer.allocateDirect(reqBytes.size).apply {
                put(reqBytes)
                flip()
            }
        } else {
            null
        }

        var respBuf = ByteBuffer.allocateDirect(initialResponseCapacity.coerceAtLeast(16))
        var written = callDirectBuffer(fnPtr, reqBuf, reqBytes.size, respBuf, respBuf.capacity())
        if (written < 0) {
            val exactNeeded = -written
            respBuf = ByteBuffer.allocateDirect(exactNeeded + 1)
            written = callDirectBuffer(fnPtr, reqBuf, reqBytes.size, respBuf, respBuf.capacity())
        }
        if (written < 0) {
            throw FeatherDartException("Dart export returned error or negative length ($written)")
        }
        val out = ByteArray(written)
        respBuf.position(0)
        respBuf.get(out, 0, written)
        return String(out, StandardCharsets.UTF_8)
    }
}
