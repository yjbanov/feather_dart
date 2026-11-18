/*
 * Host CLI harness exercising `feather_dart.h` end-to-end against
 * `examples/shared_logic` (compiled to either `.dill` or AOT `.so`/`.dylib`).
 *
 * Run via `python3 tools/run_host_demo.py` after building `--target=host`.
 */

#include "feather_dart.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

std::unordered_map<std::string, std::string> g_kv_store;

extern "C" int32_t host_kv_read(const uint8_t* key,
                                uint8_t* out_buf,
                                int32_t out_cap) {
  auto it = g_kv_store.find(reinterpret_cast<const char*>(key));
  if (it == g_kv_store.end()) {
    return -1;
  }
  const std::string& val = it->second;
  if (out_buf == nullptr || static_cast<size_t>(out_cap) < val.size()) {
    return -static_cast<int32_t>(val.size()) - 1;
  }
  std::memcpy(out_buf, val.data(), val.size());
  return static_cast<int32_t>(val.size());
}

extern "C" int32_t host_kv_write(const uint8_t* key,
                                 const uint8_t* val,
                                 int32_t len) {
  g_kv_store[reinterpret_cast<const char*>(key)] =
      std::string(reinterpret_cast<const char*>(val), static_cast<size_t>(len));
  return 0;
}

extern "C" void host_fetch_quote_async(int64_t request_id,
                                       const uint8_t* symbol) {
  std::printf("[Host] Async quote requested (id=%lld, sym=%s)\n",
              static_cast<long long>(request_id),
              reinterpret_cast<const char*>(symbol));
}

extern "C" void host_on_portfolio_updated(const uint8_t* json, int32_t len) {
  std::printf("[Host] Persisted portfolio (%d bytes)\n", len);
  (void)json;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "Usage: %s <app_logic.dill | libapp_logic.so>\n",
                 argv[0]);
    return 64;
  }

  char* err = nullptr;
  if (!feather_dart_engine_init(nullptr, &err)) {
    std::fprintf(stderr, "Engine init failed: %s\n", err);
    feather_dart_free_string(err);
    return 1;
  }

  feather_dart_register_global_symbol("host_kv_read",
                                      reinterpret_cast<void*>(&host_kv_read));
  feather_dart_register_global_symbol("host_kv_write",
                                      reinterpret_cast<void*>(&host_kv_write));
  feather_dart_register_global_symbol(
      "host_fetch_quote_async",
      reinterpret_cast<void*>(&host_fetch_quote_async));
  feather_dart_register_global_symbol(
      "host_on_portfolio_updated",
      reinterpret_cast<void*>(&host_on_portfolio_updated));

  const char* snapshot_path = argv[1];
  FeatherDartSnapshot* snapshot =
      feather_dart_is_aot_runtime()
          ? feather_dart_snapshot_load_aot_file(snapshot_path, &err)
          : feather_dart_snapshot_load_kernel_file(snapshot_path, &err);
  if (snapshot == nullptr) {
    std::fprintf(stderr, "Snapshot load failed: %s\n", err);
    feather_dart_free_string(err);
    return 1;
  }

  FeatherDartIsolate* isolate =
      feather_dart_isolate_create(snapshot, "cli_demo", nullptr, &err);
  if (isolate == nullptr) {
    std::fprintf(stderr, "Isolate create failed: %s\n", err);
    feather_dart_free_string(err);
    feather_dart_snapshot_destroy(snapshot);
    return 1;
  }

  if (!feather_dart_isolate_invoke_entrypoint(isolate, nullptr, "main", &err)) {
    std::fprintf(stderr, "main() failed: %s\n", err);
    feather_dart_free_string(err);
    return 1;
  }

  auto load_fn = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(
      feather_dart_isolate_lookup_export(isolate, "fd_load_persisted_state"));
  auto sim_fn = reinterpret_cast<int32_t (*)(int32_t, int32_t, double*)>(
      feather_dart_isolate_lookup_export(isolate, "fd_simulate_var_fast"));

  double metrics[5] = {};
  if (feather_dart_isolate_enter(isolate)) {
    load_fn(0, 0);
    sim_fn(10000, 10, metrics);
    feather_dart_isolate_exit(isolate, nullptr);
  }

  std::printf(
      "Portfolio Value: $%.2f | 10d 95%% VaR: $%.2f | 95%% CVaR: $%.2f | 99%% VaR: $%.2f\n",
      metrics[0], metrics[2], metrics[3], metrics[4]);

  feather_dart_isolate_destroy(isolate);
  feather_dart_snapshot_destroy(snapshot);
  feather_dart_engine_shutdown();
  return 0;
}
