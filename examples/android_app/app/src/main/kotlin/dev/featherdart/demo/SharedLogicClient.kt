package dev.featherdart.demo

import android.content.Context
import android.os.SystemClock
import dev.featherdart.FeatherDartEngine
import dev.featherdart.FeatherDartIsolate
import dev.featherdart.FeatherDartSchedulerBridge
import dev.featherdart.FeatherDartSnapshot
import java.io.Closeable
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.json.JSONObject

data class HoldingUiModel(
    val symbol: String,
    val quantity: Double,
    val spotPrice: Double,
    val annualDrift: Double,
    val annualVolatility: Double,
)

data class RiskUiModel(
    val portfolioValue: Double,
    val meanPnl: Double,
    val var95: Double,
    val cvar95: Double,
    val var99: Double,
    val fromCache: Boolean,
    val elapsedMs: Double,
)

data class DemoUiState(
    val holdings: List<HoldingUiModel> = emptyList(),
    val totalValue: Double = 0.0,
    val persistedRevision: Int = 0,
    val latestRisk: RiskUiModel? = null,
    val logs: List<String> = emptyList(),
    val error: String? = null,
)

/**
 * Typed Kotlin client managing the embedded `FeatherDartIsolate` and invoking
 * shared Dart C-ABI exports inside lock-guarded `withIsolate` blocks.
 */
class SharedLogicClient(
    context: Context,
    private val mainScope: CoroutineScope,
) : Closeable {
    private val _state = MutableStateFlow(DemoUiState())
    val state: StateFlow<DemoUiState> = _state.asStateFlow()

    private var snapshot: FeatherDartSnapshot? = null
    private var isolate: FeatherDartIsolate? = null

    private var fnLoadState: Long = 0L
    private var fnGetSummary: Long = 0L
    private var fnUpsertPosition: Long = 0L
    private var fnSimulateFast: Long = 0L
    private var fnRefreshQuote: Long = 0L
    private var fnCompleteQuote: Long = 0L

    private val hostServices = HostServices(
        context = context.applicationContext,
        ioScope = mainScope,
        onQuoteFetched = { reqId, priceMicros ->
            mainScope.launch(Dispatchers.Main) {
                completeHostQuote(reqId, priceMicros)
            }
        },
        onSummaryPersisted = { summaryJson ->
            mainScope.launch(Dispatchers.Main) {
                applySummaryJson(summaryJson)
            }
        },
    )

    init {
        bootRuntime(context.applicationContext)
    }

    private fun bootRuntime(appContext: Context) {
        try {
            FeatherDartEngine.initialize(
                onLog = { line ->
                    mainScope.launch(Dispatchers.Main) {
                        _state.update { it.copy(logs = (it.logs + line).takeLast(8)) }
                    }
                },
            )
            hostServices.installIntoFeatherDart()

            val loadedSnapshot = if (FeatherDartEngine.isAotRuntime) {
                FeatherDartSnapshot.fromAotLibrary(appContext, libraryName = "app_logic")
            } else {
                FeatherDartSnapshot.fromKernelAsset(appContext, assetName = "app_logic.dill")
            }
            snapshot = loadedSnapshot

            val iso = FeatherDartIsolate(
                snapshot = loadedSnapshot,
                debugName = "android_pulse_risk",
                schedulerBridge = FeatherDartSchedulerBridge.coroutines(
                    scope = mainScope,
                    dispatcher = Dispatchers.Main,
                ),
            )
            iso.invokeEntrypoint("main")

            fnLoadState = iso.lookupExportPointer("fd_load_persisted_state")
            fnGetSummary = iso.lookupExportPointer("fd_get_summary_json")
            fnUpsertPosition = iso.lookupExportPointer("fd_upsert_position")
            fnSimulateFast = iso.lookupExportPointer("fd_simulate_var_fast")
            fnRefreshQuote = iso.lookupExportPointer("fd_refresh_quote")
            fnCompleteQuote = iso.lookupExportPointer("fd_complete_quote_request")
            isolate = iso

            iso.withIsolate {
                callInt64(fnLoadState)
            }
            refreshSummary()
            runMonteCarlo(paths = 10_000, horizonDays = 10)
        } catch (t: Throwable) {
            _state.update { it.copy(error = t.message ?: t.toString()) }
        }
    }

    fun refreshSummary() {
        val iso = isolate ?: return
        val json = iso.withIsolate {
            callUtf8(fnGetSummary)
        }
        applySummaryJson(json)
    }

    fun adjustShares(item: HoldingUiModel, delta: Double) {
        val iso = isolate ?: return
        val req = JSONObject()
            .put("symbol", item.symbol)
            .put("quantity", (item.quantity + delta).coerceAtLeast(0.0))
            .put("spotPrice", item.spotPrice)
            .put("annualDrift", item.annualDrift)
            .put("annualVolatility", item.annualVolatility)
            .toString()

        val updatedJson = iso.withIsolate {
            callUtf8(fnUpsertPosition, requestJson = req)
        }
        applySummaryJson(updatedJson)
        runMonteCarlo(paths = 10_000, horizonDays = 10)
    }

    fun requestLiveQuote(symbol: String) {
        val iso = isolate ?: return
        iso.withIsolate {
            callUtf8(fnRefreshQuote, requestJson = symbol, initialResponseCapacity = 64)
        }
    }

    fun runMonteCarlo(paths: Int, horizonDays: Int) {
        val iso = isolate ?: return
        val outMetrics = DoubleArray(5)
        val startNanos = SystemClock.elapsedRealtimeNanos()
        val cacheFlag = iso.withIsolate {
            nativeSimulateVarFast(fnSimulateFast, paths, horizonDays, outMetrics)
        }
        val elapsedMs = (SystemClock.elapsedRealtimeNanos() - startNanos) / 1_000_000.0

        _state.update {
            it.copy(
                latestRisk = RiskUiModel(
                    portfolioValue = outMetrics[0],
                    meanPnl = outMetrics[1],
                    var95 = outMetrics[2],
                    cvar95 = outMetrics[3],
                    var99 = outMetrics[4],
                    fromCache = cacheFlag == 1,
                    elapsedMs = elapsedMs,
                ),
            )
        }
    }

    private fun completeHostQuote(requestId: Long, priceMicros: Long) {
        val iso = isolate ?: return
        iso.withIsolate {
            callInt64(fnCompleteQuote, requestId, priceMicros)
        }
        refreshSummary()
        runMonteCarlo(paths = 10_000, horizonDays = 10)
    }

    private fun applySummaryJson(json: String) {
        val root = JSONObject(json)
        val arr = root.getJSONArray("positions")
        val items = buildList {
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                add(
                    HoldingUiModel(
                        symbol = obj.getString("symbol"),
                        quantity = obj.getDouble("quantity"),
                        spotPrice = obj.getDouble("spotPrice"),
                        annualDrift = obj.getDouble("annualDrift"),
                        annualVolatility = obj.getDouble("annualVolatility"),
                    ),
                )
            }
        }
        _state.update {
            it.copy(
                holdings = items,
                totalValue = root.getDouble("portfolioValue"),
                persistedRevision = root.getInt("persistedRevision"),
            )
        }
    }

    override fun close() {
        isolate?.close()
        isolate = null
        snapshot?.close()
        snapshot = null
    }

    private external fun nativeSimulateVarFast(
        fnPtr: Long,
        paths: Int,
        horizonDays: Int,
        outMetrics5: DoubleArray,
    ): Int
}
