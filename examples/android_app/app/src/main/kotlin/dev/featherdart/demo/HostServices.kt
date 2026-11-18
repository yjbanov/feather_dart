package dev.featherdart.demo

import android.content.Context
import dev.featherdart.FeatherDartEngine
import java.nio.charset.StandardCharsets
import kotlin.random.Random
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

fun interface NativeSymbolRegistrar {
    fun register(name: String, pointer: Long)
}

/**
 * Supplies native Android capabilities (`SharedPreferences` persistence and
 * Kotlin Coroutine quote fetching) to Dart `@Native` functions via C++/JNI.
 */
class HostServices(
    context: Context,
    private val ioScope: CoroutineScope,
    private val onQuoteFetched: (requestId: Long, priceMicros: Long) -> Unit,
    private val onSummaryPersisted: (summaryJson: String) -> Unit,
) {
    private val prefs = context.getSharedPreferences("feather_dart_demo", Context.MODE_PRIVATE)

    fun installIntoFeatherDart() {
        nativeBindHostServices { name, ptr ->
            FeatherDartEngine.registerGlobalSymbol(name, ptr)
        }
    }

    @Suppress("unused")
    fun onKvRead(key: String): ByteArray? {
        val value = prefs.getString(key, null) ?: return null
        return value.toByteArray(StandardCharsets.UTF_8)
    }

    @Suppress("unused")
    fun onKvWrite(key: String, utf8Bytes: ByteArray) {
        val str = String(utf8Bytes, StandardCharsets.UTF_8)
        prefs.edit().putString(key, str).apply()
    }

    @Suppress("unused")
    fun onFetchQuoteAsync(requestId: Long, symbol: String) {
        ioScope.launch(Dispatchers.IO) {
            delay(180L)
            val base = when (symbol) {
                "AAPL" -> 231.40
                "GOOGL" -> 189.75
                "NVDA" -> 146.80
                "MSFT" -> 428.10
                else -> 100.0
            }
            val jitter = Random.nextDouble(-3.50, 3.50)
            val micros = kotlin.math.round((base + jitter) * 1_000_000.0).toLong()
            onQuoteFetched(requestId, micros)
        }
    }

    @Suppress("unused")
    fun onPortfolioUpdated(utf8Bytes: ByteArray) {
        val json = String(utf8Bytes, StandardCharsets.UTF_8)
        onSummaryPersisted(json)
    }

    private external fun nativeBindHostServices(registrar: NativeSymbolRegistrar)

    companion object {
        init {
            System.loadLibrary("app_bridge_jni")
        }
    }
}
