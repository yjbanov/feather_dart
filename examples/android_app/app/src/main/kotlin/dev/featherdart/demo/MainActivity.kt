package dev.featherdart.demo

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Slider
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import androidx.lifecycle.lifecycleScope
import java.util.Locale

class MainActivity : ComponentActivity() {
    private lateinit var logicClient: SharedLogicClient

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        logicClient = SharedLogicClient(applicationContext, lifecycleScope)

        setContent {
            MaterialTheme {
                val uiState by logicClient.state.collectAsState()
                FeatherDartDemoScreen(
                    state = uiState,
                    onRunSimulation = { paths ->
                        logicClient.runMonteCarlo(paths = paths, horizonDays = 10)
                    },
                    onAddShares = { item ->
                        logicClient.adjustShares(item, delta = 5.0)
                    },
                    onRefreshQuote = { symbol ->
                        logicClient.requestLiveQuote(symbol)
                    },
                )
            }
        }
    }

    override fun onDestroy() {
        logicClient.close()
        super.onDestroy()
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun FeatherDartDemoScreen(
    state: DemoUiState,
    onRunSimulation: (Int) -> Unit,
    onAddShares: (HoldingUiModel) -> Unit,
    onRefreshQuote: (String) -> Unit,
) {
    var pathSlider by remember { mutableFloatStateOf(10_000f) }

    Scaffold(
        topBar = {
            TopAppBar(title = { Text("FeatherDart Android") })
        },
    ) { innerPadding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(innerPadding)
                .padding(horizontal = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            state.error?.let { err ->
                item {
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Text(
                            text = "VM Status: $err",
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.padding(12.dp),
                        )
                    }
                }
            }

            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text("Portfolio Summary (Shared Dart State)", style = MaterialTheme.typography.titleMedium)
                        Spacer(Modifier.height(8.dp))
                        Text(String.format(Locale.US, "Market Value: $%,.2f", state.totalValue))
                        Text("Auto-Saved Revisions (Dart Timer -> SharedPreferences): ${state.persistedRevision}")
                    }
                }
            }

            item {
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Text("In-Memory Monte Carlo Risk (10-Day)", style = MaterialTheme.typography.titleMedium)
                        Spacer(Modifier.height(6.dp))
                        Text("Paths: ${pathSlider.toInt()}")
                        Slider(
                            value = pathSlider,
                            onValueChange = { pathSlider = it },
                            valueRange = 1_000f..50_000f,
                            steps = 48,
                        )
                        Button(onClick = { onRunSimulation(pathSlider.toInt()) }) {
                            Text("Run Direct-FFI VaR Simulation")
                        }
                        state.latestRisk?.let { risk ->
                            Spacer(Modifier.height(8.dp))
                            Text(String.format(Locale.US, "95%% VaR: $%,.2f", risk.var95))
                            Text(String.format(Locale.US, "95%% CVaR: $%,.2f", risk.cvar95))
                            Text(String.format(Locale.US, "99%% VaR: $%,.2f", risk.var99))
                            val modeTag = if (risk.fromCache) "LRU cache hit" else "computed"
                            Text(String.format(Locale.US, "Time: %.2f ms (%s)", risk.elapsedMs, modeTag))
                        }
                    }
                }
            }

            items(state.holdings, key = { it.symbol }) { item ->
                Card(modifier = Modifier.fillMaxWidth()) {
                    Column(modifier = Modifier.padding(16.dp)) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                        ) {
                            Text(item.symbol, style = MaterialTheme.typography.titleMedium)
                            Text(String.format(Locale.US, "$%,.2f", item.quantity * item.spotPrice))
                        }
                        Text(
                            String.format(
                                Locale.US,
                                "%.0f sh @ $%.2f • σ=%.0f%%",
                                item.quantity,
                                item.spotPrice,
                                item.annualVolatility * 100.0,
                            ),
                            style = MaterialTheme.typography.bodySmall,
                        )
                        Spacer(Modifier.height(8.dp))
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton(onClick = { onAddShares(item) }) {
                                Text("+5 Shares")
                            }
                            OutlinedButton(onClick = { onRefreshQuote(item.symbol) }) {
                                Text("Refresh Live Quote")
                            }
                        }
                    }
                }
            }

            if (state.logs.isNotEmpty()) {
                item {
                    Card(modifier = Modifier.fillMaxWidth()) {
                        Column(modifier = Modifier.padding(16.dp)) {
                            Text("Dart Console Output", style = MaterialTheme.typography.titleSmall)
                            Spacer(Modifier.height(4.dp))
                            state.logs.forEach { line ->
                                Text(
                                    text = line,
                                    fontFamily = FontFamily.Monospace,
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}
