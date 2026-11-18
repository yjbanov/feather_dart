package dev.featherdart

import java.nio.ByteBuffer

internal class InternalIsolateCallbacks(
    private val onMessage: () -> Unit,
    private val onTimer: (portId: Long, delayMs: Long) -> Unit,
    private val onError: (String) -> Unit,
) {
    fun onScheduleMessage() = onMessage()

    fun onScheduleTimer(portId: Long, delayMs: Long) = onTimer(portId, delayMs)

    fun onUnhandledError(message: String) = onError(message)
}

internal object FeatherDartNative {
    init {
        System.loadLibrary("feather_dart")
        System.loadLibrary("feather_dart_jni")
    }

    @JvmStatic external fun nativeEngineInit(vmFlags: Array<String>?)

    @JvmStatic external fun nativeEngineShutdown()

    @JvmStatic external fun nativeIsAotRuntime(): Boolean

    @JvmStatic external fun nativeRegisterGlobalSymbol(symbol: String, fnPtr: Long)

    @JvmStatic external fun nativeSnapshotLoadAotFile(path: String): Long

    @JvmStatic external fun nativeSnapshotLoadKernelFile(path: String): Long

    @JvmStatic external fun nativeSnapshotLoadKernelBytes(bytes: ByteArray): Long

    @JvmStatic external fun nativeSnapshotDestroy(handle: Long)

    @JvmStatic
    external fun nativeIsolateCreate(
        snapshotHandle: Long,
        debugName: String,
        callbacks: InternalIsolateCallbacks,
    ): Long

    @JvmStatic external fun nativeIsolateRawPointer(holderHandle: Long): Long

    @JvmStatic external fun nativeIsolateDestroy(holderHandle: Long)

    @JvmStatic
    external fun nativeIsolateRegisterSymbol(
        holderHandle: Long,
        symbol: String,
        fnPtr: Long,
    )

    @JvmStatic external fun nativeIsolateEnter(holderHandle: Long): Boolean

    @JvmStatic external fun nativeIsolateExit(holderHandle: Long)

    @JvmStatic
    external fun nativeIsolateInvokeEntrypoint(
        holderHandle: Long,
        libraryUri: String?,
        functionName: String,
    )

    @JvmStatic
    external fun nativeIsolateLookupExport(
        holderHandle: Long,
        exportName: String,
    ): Long

    @JvmStatic external fun nativeIsolateHandleMessage(holderHandle: Long)

    @JvmStatic external fun nativeIsolateNotifyTimer(holderHandle: Long, portId: Long)

    @JvmStatic external fun nativeCallVoid(fnPtr: Long)

    @JvmStatic external fun nativeCallInt64(fnPtr: Long, arg0: Long, arg1: Long): Long

    @JvmStatic
    external fun nativeCallDirectBuffer(
        fnPtr: Long,
        inBuf: ByteBuffer?,
        inLen: Int,
        outBuf: ByteBuffer?,
        outCap: Int,
    ): Int
}
