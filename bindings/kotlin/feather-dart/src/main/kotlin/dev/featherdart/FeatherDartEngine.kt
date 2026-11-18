package dev.featherdart

import java.util.concurrent.atomic.AtomicReference

/**
 * Process-wide lifecycle and global `@Native` symbol registry for the embedded
 * FeatherDart VM.
 */
object FeatherDartEngine {
    private val logListenerRef = AtomicReference<((String) -> Unit)?>(null)

    /** True if linked against the AOT precompiled runtime (`libfeather_dart.so`). */
    val isAotRuntime: Boolean
        get() = FeatherDartNative.nativeIsAotRuntime()

    /**
     * Initializes the FeatherDart VM process state. Safe to invoke multiple times.
     */
    fun initialize(
        vmFlags: List<String> = emptyList(),
        onLog: ((String) -> Unit)? = null,
    ) {
        if (onLog != null) {
            logListenerRef.set(onLog)
        }
        FeatherDartNative.nativeEngineInit(vmFlags.toTypedArray())
    }

    /** Updates the listener receiving `print()` lines from Dart isolates. */
    fun setLogListener(listener: ((String) -> Unit)?) {
        logListenerRef.set(listener)
    }

    /**
     * Registers a native C function address (`void*`) under [symbol] so Dart
     * `@Native<...>(symbol: ...)` declarations resolve across all isolates.
     */
    fun registerGlobalSymbol(symbol: String, functionPointer: Long) {
        require(functionPointer != 0L) { "functionPointer must be non-zero" }
        FeatherDartNative.nativeRegisterGlobalSymbol(symbol, functionPointer)
    }

    /** Shuts down all remaining isolates and cleans up the Dart VM. */
    fun shutdown() {
        FeatherDartNative.nativeEngineShutdown()
    }

    // Invoked via JNI from feather_dart_jni.cc
    @JvmStatic
    private fun onNativeLog(line: String) {
        logListenerRef.get()?.invoke(line)
    }
}
