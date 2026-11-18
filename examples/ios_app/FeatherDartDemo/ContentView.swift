import SwiftUI

struct ContentView: View {
    @StateObject private var client = SharedLogicClient()
    @State private var pathCount: Double = 10_000

    var body: some View {
        NavigationStack {
            List {
                if let err = client.startupError {
                    Section("VM Status") {
                        Text(err)
                            .foregroundStyle(.red)
                            .font(.footnote)
                    }
                }

                Section("Portfolio Summary (Shared Dart State)") {
                    LabeledContent(
                        "Market Value",
                        value: client.totalValue.formatted(.currency(code: "USD"))
                    )
                    LabeledContent(
                        "Auto-Saved Revisions (Dart Timer -> UserDefaults)",
                        value: "\(client.persistedRevision)"
                    )
                }

                Section("In-Memory Monte Carlo Risk (10-Day Horizon)") {
                    VStack(alignment: .leading, spacing: 8) {
                        Text("Paths: \(Int(pathCount))")
                            .font(.subheadline)
                        Slider(value: $pathCount, in: 1_000...50_000, step: 1_000)
                        Button("Run Direct-FFI VaR Simulation") {
                            client.runMonteCarlo(paths: Int(pathCount), horizonDays: 10)
                        }
                        .buttonStyle(.borderedProminent)
                    }

                    if let risk = client.latestRisk {
                        LabeledContent("95% VaR", value: risk.var95.formatted(.currency(code: "USD")))
                        LabeledContent("95% CVaR (Expected Shortfall)", value: risk.cvar95.formatted(.currency(code: "USD")))
                        LabeledContent("99% VaR", value: risk.var99.formatted(.currency(code: "USD")))
                        LabeledContent(
                            "Compute Time",
                            value: String(
                                format: "%.2f ms (%@)",
                                risk.elapsedMilliseconds,
                                risk.fromCache ? "LRU cache hit" : "computed"
                            )
                        )
                    }
                }

                Section("Holdings (Direct FFI + Async Host Quote Feed)") {
                    ForEach(client.holdings) { item in
                        VStack(alignment: .leading, spacing: 6) {
                            HStack {
                                Text(item.symbol).font(.headline)
                                Spacer()
                                Text(
                                    (item.quantity * item.spotPrice)
                                        .formatted(.currency(code: "USD"))
                                )
                            }
                            Text(
                                "\(Int(item.quantity)) sh @ \(item.spotPrice.formatted(.currency(code: "USD"))) • σ=\(Int(item.annualVolatility * 100))%"
                            )
                            .font(.caption)
                            .foregroundStyle(.secondary)

                            HStack {
                                Button("+5 Shares") {
                                    client.adjustShares(for: item, delta: 5)
                                }
                                .buttonStyle(.bordered)

                                Button("Refresh Live Quote") {
                                    client.requestLiveQuote(symbol: item.symbol)
                                }
                                .buttonStyle(.bordered)
                            }
                        }
                        .padding(.vertical, 4)
                    }
                }

                if !client.logs.isEmpty {
                    Section("Dart Console Output") {
                        ForEach(Array(client.logs.suffix(8).enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.system(.caption, design: .monospaced))
                        }
                    }
                }
            }
            .navigationTitle("FeatherDart iOS")
        }
    }
}
