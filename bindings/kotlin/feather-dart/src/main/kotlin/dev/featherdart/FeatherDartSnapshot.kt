package dev.featherdart

import android.content.Context
import java.io.Closeable
import java.io.File
import java.util.concurrent.atomic.AtomicLong

/**
 * Loaded FeatherDart AOT ELF shared library (`lib<name>.so`) or Kernel (`.dill`)
 * snapshot. Multiple [FeatherDartIsolate] instances can share one snapshot.
 */
class FeatherDartSnapshot private constructor(rawHandle: Long) : Closeable {
    private val handleAtomic = AtomicLong(rawHandle)

    internal val nativeHandle: Long
        get() {
            val h = handleAtomic.get()
            if (h == 0L) {
                throw FeatherDartException("FeatherDartSnapshot has already been closed.")
            }
            return h
        }

    override fun close() {
        val h = handleAtomic.getAndSet(0L)
        if (h != 0L) {
            FeatherDartNative.nativeSnapshotDestroy(h)
        }
    }

    companion object {
        /**
         * Loads an AOT ELF snapshot packaged in the APK's `jniLibs/<abi>/lib[libraryName].so`.
         */
        fun fromAotLibrary(
            context: Context,
            libraryName: String = "app_logic",
        ): FeatherDartSnapshot {
            val nativeDir = context.applicationInfo.nativeLibraryDir
            val soFile = File(nativeDir, "lib$libraryName.so")
            return fromAotFile(soFile.absolutePath)
        }

        /** Loads an AOT ELF `.so` snapshot from an explicit filesystem path. */
        fun fromAotFile(path: String): FeatherDartSnapshot {
            val handle = FeatherDartNative.nativeSnapshotLoadAotFile(path)
            return FeatherDartSnapshot(handle)
        }

        /** Loads a compiled Kernel `.dill` asset from `assets/[assetName]` (dev mode). */
        fun fromKernelAsset(context: Context, assetName: String): FeatherDartSnapshot {
            val bytes = context.assets.open(assetName).use { it.readBytes() }
            return fromKernelBytes(bytes)
        }

        /** Loads a compiled Kernel `.dill` snapshot from a filesystem path (dev mode). */
        fun fromKernelFile(path: String): FeatherDartSnapshot {
            val handle = FeatherDartNative.nativeSnapshotLoadKernelFile(path)
            return FeatherDartSnapshot(handle)
        }

        /** Loads a compiled Kernel `.dill` snapshot from in-memory bytes (dev mode). */
        fun fromKernelBytes(bytes: ByteArray): FeatherDartSnapshot {
            require(bytes.isNotEmpty()) { "Kernel snapshot bytes must not be empty." }
            val handle = FeatherDartNative.nativeSnapshotLoadKernelBytes(bytes)
            return FeatherDartSnapshot(handle)
        }
    }
}
