#include "feather_dart.h"

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dart_api.h"
#include "dart_native_api.h"
#include "timer_fallback.h"

namespace {

char* AllocFormatted(const char* fmt, ...) {
  va_list args_len;
  va_start(args_len, fmt);
  const int len = std::vsnprintf(nullptr, 0, fmt, args_len);
  va_end(args_len);
  if (len < 0) {
    return nullptr;
  }
  char* buf = static_cast<char*>(std::malloc(static_cast<size_t>(len) + 1));
  if (buf == nullptr) {
    return nullptr;
  }
  va_list args_out;
  va_start(args_out, fmt);
  std::vsnprintf(buf, static_cast<size_t>(len) + 1, fmt, args_out);
  va_end(args_out);
  return buf;
}

char* CopyString(const char* str) {
  if (str == nullptr) {
    return nullptr;
  }
  const size_t len = std::strlen(str);
  char* copy = static_cast<char*>(std::malloc(len + 1));
  if (copy != nullptr) {
    std::memcpy(copy, str, len + 1);
  }
  return copy;
}

void SetOutError(char** out_error, const char* msg) {
  if (out_error != nullptr) {
    *out_error = CopyString(msg != nullptr ? msg : "Unknown FeatherDart error");
  }
}

int64_t MonotonicMillis() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void* LookupDlsymWithUnderscoreFallback(void* lib_handle, const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  dlerror();
  void* sym = dlsym(lib_handle, name);
  if (sym != nullptr) {
    return sym;
  }
  if (name[0] == '_') {
    return dlsym(lib_handle, name + 1);
  }
  std::string prefixed("_");
  prefixed += name;
  return dlsym(lib_handle, prefixed.c_str());
}

enum class SnapshotKind {
  kKernel,
  kAot,
};

}  // namespace

struct FeatherDartSnapshot {
  SnapshotKind kind = SnapshotKind::kKernel;
  void* dlopen_handle = nullptr;

  // AOT snapshot pointers.
  const uint8_t* vm_data = nullptr;
  const uint8_t* vm_instructions = nullptr;
  const uint8_t* isolate_data = nullptr;
  const uint8_t* isolate_instructions = nullptr;

  // Kernel (.dill) buffer.
  const uint8_t* kernel_bytes = nullptr;
  size_t kernel_length = 0;
  std::vector<uint8_t> owned_kernel;
};

struct FeatherDartIsolate {
  std::recursive_mutex mutex;
  Dart_Isolate dart_isolate = nullptr;
  int enter_depth = 0;
  std::atomic<bool> shutting_down{false};

  FeatherDartScheduler scheduler{};
  std::unique_ptr<feather_dart::FallbackTimerService> fallback_timer;

  Dart_PersistentHandle root_library = nullptr;
  Dart_PersistentHandle isolate_library = nullptr;
  Dart_PersistentHandle drain_microtasks_name = nullptr;

  std::mutex symbols_mutex;
  std::unordered_map<std::string, void*> host_symbols;
  std::unordered_map<std::string, void*> dart_exports;
};

namespace {

struct EngineState {
  std::mutex lifecycle_mutex;
  bool flags_configured = false;
  bool vm_initialized = false;
  std::atomic<bool> running{false};

  std::mutex log_mutex;
  FeatherDartLogCallback log_callback = nullptr;
  void* log_user_data = nullptr;

  std::mutex symbols_mutex;
  std::unordered_map<std::string, void*> global_symbols;

  std::mutex isolates_mutex;
  std::unordered_map<Dart_Isolate, FeatherDartIsolate*> by_dart_isolate;
};

EngineState& GetEngine() {
  static EngineState* state = new EngineState();
  return *state;
}

thread_local FeatherDartIsolate* g_entered_isolate = nullptr;

class EnteredIsolateTlsGuard {
 public:
  explicit EnteredIsolateTlsGuard(FeatherDartIsolate* isolate)
      : prev_(g_entered_isolate) {
    g_entered_isolate = isolate;
  }
  ~EnteredIsolateTlsGuard() { g_entered_isolate = prev_; }

 private:
  FeatherDartIsolate* prev_;
};

FeatherDartIsolate* CurrentFeatherDartIsolate() {
  if (g_entered_isolate != nullptr) {
    return g_entered_isolate;
  }
  Dart_Isolate current = Dart_CurrentIsolate();
  if (current == nullptr) {
    return nullptr;
  }
  EngineState& engine = GetEngine();
  std::lock_guard<std::mutex> lock(engine.isolates_mutex);
  auto it = engine.by_dart_isolate.find(current);
  return it != engine.by_dart_isolate.end() ? it->second : nullptr;
}

extern "C" void feather_dart_builtin_print(const char* line) {
  if (line == nullptr) {
    return;
  }
  EngineState& engine = GetEngine();
  FeatherDartLogCallback cb = nullptr;
  void* user_data = nullptr;
  {
    std::lock_guard<std::mutex> lock(engine.log_mutex);
    cb = engine.log_callback;
    user_data = engine.log_user_data;
  }
  if (cb != nullptr) {
    cb(line, user_data);
  } else {
    std::fprintf(stdout, "%s\n", line);
    std::fflush(stdout);
  }
}

extern "C" void feather_dart_builtin_print_handle(Dart_Handle line_handle) {
  Dart_EnterScope();
  const char* cstr = nullptr;
  if (!Dart_IsError(Dart_StringToCString(line_handle, &cstr)) &&
      cstr != nullptr) {
    feather_dart_builtin_print(cstr);
  }
  Dart_ExitScope();
}

extern "C" int64_t feather_dart_builtin_monotonic_ms() {
  return MonotonicMillis();
}

// Called by `dart:_feather_dart` `VMLibraryHooks.eventHandlerSendData`.
// `wakeup_time_ms == -1` cancels pending wakeups; otherwise `wakeup_time_ms`
// is an absolute monotonic timestamp in milliseconds.
extern "C" void feather_dart_builtin_schedule_wakeup(int64_t port_id,
                                                     int64_t wakeup_time_ms) {
  FeatherDartIsolate* fd_isolate = CurrentFeatherDartIsolate();
  if (fd_isolate == nullptr || fd_isolate->shutting_down.load()) {
    return;
  }

  int64_t delay_ms = -1;
  if (wakeup_time_ms >= 0) {
    const int64_t now_ms = MonotonicMillis();
    delay_ms = wakeup_time_ms > now_ms ? (wakeup_time_ms - now_ms) : 0;
  }

  if (fd_isolate->scheduler.schedule_timer != nullptr) {
    fd_isolate->scheduler.schedule_timer(fd_isolate, port_id, delay_ms,
                                         fd_isolate->scheduler.context);
  } else if (fd_isolate->fallback_timer != nullptr) {
    fd_isolate->fallback_timer->ScheduleOrCancel(port_id, delay_ms);
  }
}

void RegisterBuiltinSymbolsLocked(
    std::unordered_map<std::string, void*>& table) {
  table["feather_dart_print"] =
      reinterpret_cast<void*>(&feather_dart_builtin_print);
  table["feather_dart_print_handle"] =
      reinterpret_cast<void*>(&feather_dart_builtin_print_handle);
  table["feather_dart_monotonic_ms"] =
      reinterpret_cast<void*>(&feather_dart_builtin_monotonic_ms);
  table["feather_dart_schedule_wakeup"] =
      reinterpret_cast<void*>(&feather_dart_builtin_schedule_wakeup);
  table["feather_dart_export"] = reinterpret_cast<void*>(&feather_dart_export);
}

void* ResolveHostSymbol(FeatherDartIsolate* fd_isolate, const char* symbol) {
  if (symbol == nullptr) {
    return nullptr;
  }
  if (fd_isolate != nullptr) {
    std::lock_guard<std::mutex> lock(fd_isolate->symbols_mutex);
    auto it = fd_isolate->host_symbols.find(symbol);
    if (it != fd_isolate->host_symbols.end()) {
      return it->second;
    }
  }
  {
    EngineState& engine = GetEngine();
    std::lock_guard<std::mutex> lock(engine.symbols_mutex);
    auto it = engine.global_symbols.find(symbol);
    if (it != engine.global_symbols.end()) {
      return it->second;
    }
  }
  return LookupDlsymWithUnderscoreFallback(RTLD_DEFAULT, symbol);
}

void* FeatherDartFfiNativeResolver(const char* name, uintptr_t /*args_n*/) {
  return ResolveHostSymbol(CurrentFeatherDartIsolate(), name);
}

#if DART_INITIALIZE_PARAMS_CURRENT_VERSION >= 0x0000000B
bool NativeAssetsContainsAsset(const char* /*asset_id*/) {
  return true;
}

void* NativeAssetsDlopenAsset(const char* /*asset_id*/, char** error) {
  if (error != nullptr) {
    *error = nullptr;
  }
  return reinterpret_cast<void*>(0x1);
}

char* NativeAssetsAvailableAssets() {
  return CopyString("feather_dart:host (and process symbols)");
}

void* NativeAssetsDlsym(void* /*handle*/, const char* symbol, char** error) {
  if (error != nullptr) {
    *error = nullptr;
  }
  void* resolved = ResolveHostSymbol(CurrentFeatherDartIsolate(), symbol);
  if (resolved == nullptr && error != nullptr) {
    *error = AllocFormatted(
        "Symbol '%s' not found in FeatherDart host registry",
        symbol != nullptr ? symbol : "<null>");
  }
  return resolved;
}

void NativeAssetsDlclose(void* /*handle*/, char** error) {
  if (error != nullptr) {
    *error = nullptr;
  }
}
#endif

void MessageNotifyCallback(Dart_Isolate dart_isolate) {
  EngineState& engine = GetEngine();
  if (!engine.running.load(std::memory_order_acquire)) {
    return;
  }
  FeatherDartIsolate* fd_isolate = nullptr;
  {
    std::lock_guard<std::mutex> lock(engine.isolates_mutex);
    auto it = engine.by_dart_isolate.find(dart_isolate);
    if (it != engine.by_dart_isolate.end()) {
      fd_isolate = it->second;
    }
  }
  if (fd_isolate == nullptr ||
      fd_isolate->shutting_down.load(std::memory_order_acquire)) {
    return;
  }
  if (fd_isolate->scheduler.schedule_message != nullptr) {
    fd_isolate->scheduler.schedule_message(fd_isolate,
                                           fd_isolate->scheduler.context);
  }
}

bool DrainMicrotasksLocked(FeatherDartIsolate* isolate, char** out_error) {
  if (isolate->isolate_library == nullptr ||
      isolate->drain_microtasks_name == nullptr) {
    return true;
  }
  EnteredIsolateTlsGuard tls_guard(isolate);
  Dart_Handle lib = Dart_HandleFromPersistent(isolate->isolate_library);
  Dart_Handle fn = Dart_HandleFromPersistent(isolate->drain_microtasks_name);
  Dart_Handle res = Dart_Invoke(lib, fn, 0, nullptr);
  if (Dart_IsError(res)) {
    const char* err_msg = Dart_GetError(res);
    SetOutError(out_error, err_msg);
    if (isolate->scheduler.on_unhandled_error != nullptr) {
      isolate->scheduler.on_unhandled_error(err_msg,
                                            isolate->scheduler.context);
    }
    return false;
  }
  return true;
}

bool InitializeFeatherDartCoreLibraries(FeatherDartIsolate* isolate,
                                        char** out_error) {
  EnteredIsolateTlsGuard tls_guard(isolate);

  Dart_Handle finalize_res = Dart_FinalizeLoading(false);
  if (Dart_IsError(finalize_res)) {
    SetOutError(out_error, Dart_GetError(finalize_res));
    return false;
  }

  Dart_Handle libs = Dart_GetLoadedLibraries();
  if (!Dart_IsError(libs)) {
    intptr_t num_libs = 0;
    if (!Dart_IsError(Dart_ListLength(libs, &num_libs))) {
      for (intptr_t i = 0; i < num_libs; ++i) {
        Dart_Handle lib = Dart_ListGetAt(libs, i);
        if (!Dart_IsError(lib)) {
          Dart_Handle set_res =
              Dart_SetFfiNativeResolver(lib, &FeatherDartFfiNativeResolver);
          (void)set_res;
        }
      }
    }
  }

  Dart_Handle async_lib =
      Dart_LookupLibrary(Dart_NewStringFromCString("dart:async"));
  Dart_Handle isolate_lib =
      Dart_LookupLibrary(Dart_NewStringFromCString("dart:isolate"));
  Dart_Handle internal_lib =
      Dart_LookupLibrary(Dart_NewStringFromCString("dart:_internal"));
  if (Dart_IsError(async_lib) || Dart_IsError(isolate_lib) ||
      Dart_IsError(internal_lib)) {
    SetOutError(out_error, "Required bootstrap libraries missing in snapshot");
    return false;
  }

  Dart_Handle sched_closure = Dart_Invoke(
      isolate_lib,
      Dart_NewStringFromCString("_getIsolateScheduleImmediateClosure"), 0,
      nullptr);
  if (Dart_IsError(sched_closure)) {
    SetOutError(out_error, Dart_GetError(sched_closure));
    return false;
  }
  Dart_Handle set_sched_args[1] = {sched_closure};
  Dart_Handle set_sched_res = Dart_Invoke(
      async_lib, Dart_NewStringFromCString("_setScheduleImmediateClosure"), 1,
      set_sched_args);
  if (Dart_IsError(set_sched_res)) {
    SetOutError(out_error, Dart_GetError(set_sched_res));
    return false;
  }

  Dart_Handle timer_hooks_res = Dart_Invoke(
      isolate_lib, Dart_NewStringFromCString("_setupHooks"), 0, nullptr);
  if (Dart_IsError(timer_hooks_res)) {
    SetOutError(out_error, Dart_GetError(timer_hooks_res));
    return false;
  }

  Dart_Handle feather_lib =
      Dart_LookupLibrary(Dart_NewStringFromCString("dart:_feather_dart"));
  if (!Dart_IsError(feather_lib)) {
    Dart_Handle hook_res = Dart_Invoke(
        feather_lib, Dart_NewStringFromCString("_setupFeatherDartHooks"), 0,
        nullptr);
    if (Dart_IsError(hook_res)) {
      SetOutError(out_error, Dart_GetError(hook_res));
      return false;
    }

    Dart_Handle print_closure = Dart_Invoke(
        feather_lib, Dart_NewStringFromCString("_getPrintClosure"), 0, nullptr);
    if (Dart_IsError(print_closure)) {
      SetOutError(out_error, Dart_GetError(print_closure));
      return false;
    }
    Dart_Handle set_print_res = Dart_SetField(
        internal_lib, Dart_NewStringFromCString("_printClosure"),
        print_closure);
    if (Dart_IsError(set_print_res)) {
      SetOutError(out_error, Dart_GetError(set_print_res));
      return false;
    }
  }

  isolate->isolate_library = Dart_NewPersistentHandle(isolate_lib);
  isolate->drain_microtasks_name = Dart_NewPersistentHandle(
      Dart_NewStringFromCString("_runPendingImmediateCallback"));
  return true;
}

bool EnsureVmInitialized(const FeatherDartSnapshot* snapshot,
                         char** out_error) {
  EngineState& engine = GetEngine();
  std::lock_guard<std::mutex> lock(engine.lifecycle_mutex);
  if (engine.vm_initialized) {
    return true;
  }
  if (!engine.flags_configured) {
    std::vector<const char*> default_flags;
    if (Dart_IsPrecompiledRuntime()) {
      default_flags.push_back("--precompilation");
    }
    char* flag_err = Dart_SetVMFlags(static_cast<int>(default_flags.size()),
                                     default_flags.data());
    if (flag_err != nullptr) {
      if (out_error != nullptr) {
        *out_error = flag_err;
      } else {
        std::free(flag_err);
      }
      return false;
    }
    engine.flags_configured = true;
  }

  Dart_InitializeParams params;
  std::memset(&params, 0, sizeof(params));
  params.version = DART_INITIALIZE_PARAMS_CURRENT_VERSION;
  params.start_kernel_isolate = false;
#if DART_INITIALIZE_PARAMS_CURRENT_VERSION <= 0x0000000A
  if (snapshot != nullptr && snapshot->kind == SnapshotKind::kAot) {
    params.vm_snapshot_data = snapshot->vm_data;
    params.vm_snapshot_instructions = snapshot->vm_instructions;
  }
#else
  (void)snapshot;
#endif

  char* init_err = Dart_Initialize(&params);
  if (init_err != nullptr) {
    if (out_error != nullptr) {
      *out_error = init_err;
    } else {
      std::free(init_err);
    }
    return false;
  }

  engine.vm_initialized = true;
  engine.running.store(true, std::memory_order_release);
  return true;
}

bool PopulateAotSymbolsFromLibraryHandle(void* handle,
                                         FeatherDartSnapshot* out_snapshot,
                                         char** out_error) {
#if defined(kSnapshotDataCSymbol)
  const auto* data = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle, kSnapshotDataCSymbol));
  const auto* text = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle, kSnapshotTextCSymbol));
  if (data != nullptr && text != nullptr) {
    out_snapshot->isolate_data = data;
    out_snapshot->isolate_instructions = text;
    return true;
  }
#endif
  const auto* vm_data = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle, "_kDartVmSnapshotData"));
  const auto* vm_instr = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle,
                                        "_kDartVmSnapshotInstructions"));
  const auto* iso_data = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle, "_kDartIsolateSnapshotData"));
  const auto* iso_instr = static_cast<const uint8_t*>(
      LookupDlsymWithUnderscoreFallback(handle,
                                        "_kDartIsolateSnapshotInstructions"));

  if (iso_data != nullptr && iso_instr != nullptr) {
    out_snapshot->vm_data = vm_data;
    out_snapshot->vm_instructions = vm_instr;
    out_snapshot->isolate_data = iso_data;
    out_snapshot->isolate_instructions = iso_instr;
    return true;
  }

  SetOutError(out_error,
              "Failed to locate Dart AOT snapshot symbols in library");
  return false;
}

}  // namespace

extern "C" {

bool feather_dart_engine_init(const FeatherDartEngineConfig* config,
                              char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  EngineState& engine = GetEngine();
  {
    std::lock_guard<std::mutex> sym_lock(engine.symbols_mutex);
    RegisterBuiltinSymbolsLocked(engine.global_symbols);
  }
  if (config != nullptr && config->log_callback != nullptr) {
    std::lock_guard<std::mutex> log_lock(engine.log_mutex);
    engine.log_callback = config->log_callback;
    engine.log_user_data = config->log_user_data;
  }

  std::lock_guard<std::mutex> lock(engine.lifecycle_mutex);
  if (engine.flags_configured) {
    return true;
  }

  std::vector<const char*> flags;
  if (Dart_IsPrecompiledRuntime()) {
    flags.push_back("--precompilation");
  }
  if (config != nullptr && config->vm_flags != nullptr) {
    for (int i = 0; i < config->vm_flags_count; ++i) {
      if (config->vm_flags[i] != nullptr) {
        flags.push_back(config->vm_flags[i]);
      }
    }
  }

  char* flag_err =
      Dart_SetVMFlags(static_cast<int>(flags.size()), flags.data());
  if (flag_err != nullptr) {
    if (out_error != nullptr) {
      *out_error = flag_err;
    } else {
      std::free(flag_err);
    }
    return false;
  }

  engine.flags_configured = true;
  return true;
}

void feather_dart_engine_shutdown(void) {
  EngineState& engine = GetEngine();
  engine.running.store(false, std::memory_order_release);

  std::vector<FeatherDartIsolate*> remaining;
  {
    std::lock_guard<std::mutex> lock(engine.isolates_mutex);
    for (const auto& kv : engine.by_dart_isolate) {
      remaining.push_back(kv.second);
    }
  }
  for (FeatherDartIsolate* iso : remaining) {
    feather_dart_isolate_destroy(iso);
  }

  std::lock_guard<std::mutex> lock(engine.lifecycle_mutex);
  if (engine.vm_initialized) {
    char* err = Dart_Cleanup();
    if (err != nullptr) {
      std::free(err);
    }
    engine.vm_initialized = false;
  }
}

bool feather_dart_is_aot_runtime(void) {
  return Dart_IsPrecompiledRuntime();
}

void feather_dart_set_log_callback(FeatherDartLogCallback callback,
                                   void* user_data) {
  EngineState& engine = GetEngine();
  std::lock_guard<std::mutex> lock(engine.log_mutex);
  engine.log_callback = callback;
  engine.log_user_data = user_data;
}

void feather_dart_register_global_symbol(const char* symbol_name,
                                         void* fn_ptr) {
  if (symbol_name == nullptr) {
    return;
  }
  EngineState& engine = GetEngine();
  std::lock_guard<std::mutex> lock(engine.symbols_mutex);
  RegisterBuiltinSymbolsLocked(engine.global_symbols);
  engine.global_symbols[symbol_name] = fn_ptr;
}

void feather_dart_free_string(char* str) {
  std::free(str);
}

FeatherDartSnapshot* feather_dart_snapshot_load_aot_file(
    const char* library_path,
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (library_path == nullptr) {
    SetOutError(out_error, "library_path must not be NULL");
    return nullptr;
  }

  dlerror();
  void* handle = dlopen(library_path, RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* dl_err = dlerror();
    if (out_error != nullptr) {
      *out_error = AllocFormatted("dlopen('%s') failed: %s", library_path,
                                  dl_err != nullptr ? dl_err : "unknown");
    }
    return nullptr;
  }

  auto snapshot = std::make_unique<FeatherDartSnapshot>();
  snapshot->kind = SnapshotKind::kAot;
  snapshot->dlopen_handle = handle;
  if (!PopulateAotSymbolsFromLibraryHandle(handle, snapshot.get(), out_error)) {
    dlclose(handle);
    return nullptr;
  }
  return snapshot.release();
}

FeatherDartSnapshot* feather_dart_snapshot_from_aot_symbols(
    const uint8_t* vm_data,
    const uint8_t* vm_instructions,
    const uint8_t* isolate_data,
    const uint8_t* isolate_instructions,
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (isolate_data == nullptr || isolate_instructions == nullptr) {
    SetOutError(out_error,
                "isolate_data and isolate_instructions must not be NULL");
    return nullptr;
  }
  auto snapshot = std::make_unique<FeatherDartSnapshot>();
  snapshot->kind = SnapshotKind::kAot;
  snapshot->vm_data = vm_data;
  snapshot->vm_instructions = vm_instructions;
  snapshot->isolate_data = isolate_data;
  snapshot->isolate_instructions = isolate_instructions;
  return snapshot.release();
}

FeatherDartSnapshot* feather_dart_snapshot_from_process_symbols(
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  auto snapshot = std::make_unique<FeatherDartSnapshot>();
  snapshot->kind = SnapshotKind::kAot;
  if (!PopulateAotSymbolsFromLibraryHandle(RTLD_DEFAULT, snapshot.get(),
                                           out_error)) {
    return nullptr;
  }
  return snapshot.release();
}

FeatherDartSnapshot* feather_dart_snapshot_load_kernel_file(
    const char* dill_path,
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (dill_path == nullptr) {
    SetOutError(out_error, "dill_path must not be NULL");
    return nullptr;
  }

  std::FILE* file = std::fopen(dill_path, "rb");
  if (file == nullptr) {
    if (out_error != nullptr) {
      *out_error = AllocFormatted("Failed to open kernel file '%s'", dill_path);
    }
    return nullptr;
  }

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::rewind(file);
  if (size <= 0) {
    std::fclose(file);
    SetOutError(out_error, "Kernel file is empty or unreadable");
    return nullptr;
  }

  auto snapshot = std::make_unique<FeatherDartSnapshot>();
  snapshot->kind = SnapshotKind::kKernel;
  snapshot->owned_kernel.resize(static_cast<size_t>(size));
  const size_t read_bytes = std::fread(snapshot->owned_kernel.data(), 1,
                                       static_cast<size_t>(size), file);
  std::fclose(file);
  if (read_bytes != static_cast<size_t>(size)) {
    SetOutError(out_error, "Incomplete read of kernel file");
    return nullptr;
  }
  snapshot->kernel_bytes = snapshot->owned_kernel.data();
  snapshot->kernel_length = snapshot->owned_kernel.size();
  return snapshot.release();
}

FeatherDartSnapshot* feather_dart_snapshot_load_kernel_buffer(
    const uint8_t* bytes,
    size_t length,
    bool copy_bytes,
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (bytes == nullptr || length == 0) {
    SetOutError(out_error, "Kernel buffer must be non-empty");
    return nullptr;
  }
  auto snapshot = std::make_unique<FeatherDartSnapshot>();
  snapshot->kind = SnapshotKind::kKernel;
  if (copy_bytes) {
    snapshot->owned_kernel.assign(bytes, bytes + length);
    snapshot->kernel_bytes = snapshot->owned_kernel.data();
  } else {
    snapshot->kernel_bytes = bytes;
  }
  snapshot->kernel_length = length;
  return snapshot.release();
}

void feather_dart_snapshot_destroy(FeatherDartSnapshot* snapshot) {
  if (snapshot == nullptr) {
    return;
  }
  if (snapshot->dlopen_handle != nullptr) {
    dlclose(snapshot->dlopen_handle);
  }
  delete snapshot;
}

FeatherDartIsolate* feather_dart_isolate_create(
    const FeatherDartSnapshot* snapshot,
    const char* debug_name,
    const FeatherDartScheduler* scheduler,
    char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (snapshot == nullptr) {
    SetOutError(out_error, "snapshot must not be NULL");
    return nullptr;
  }
  if (Dart_IsPrecompiledRuntime() && snapshot->kind != SnapshotKind::kAot) {
    SetOutError(
        out_error,
        "AOT FeatherDart runtime requires an AOT snapshot (got Kernel)");
    return nullptr;
  }
  if (!Dart_IsPrecompiledRuntime() && snapshot->kind != SnapshotKind::kKernel) {
    SetOutError(
        out_error,
        "JIT/Kernel FeatherDart runtime requires a Kernel .dill snapshot");
    return nullptr;
  }

  {
    EngineState& engine = GetEngine();
    std::lock_guard<std::mutex> sym_lock(engine.symbols_mutex);
    RegisterBuiltinSymbolsLocked(engine.global_symbols);
  }

  if (!EnsureVmInitialized(snapshot, out_error)) {
    return nullptr;
  }

  auto fd_isolate = std::make_unique<FeatherDartIsolate>();
  if (scheduler != nullptr) {
    fd_isolate->scheduler = *scheduler;
  }
  if (fd_isolate->scheduler.schedule_timer == nullptr) {
    FeatherDartIsolate* raw_ptr = fd_isolate.get();
    fd_isolate->fallback_timer =
        std::make_unique<feather_dart::FallbackTimerService>(
            [raw_ptr](int64_t port_id) {
              feather_dart_isolate_notify_timer(raw_ptr, port_id);
            });
  }

  Dart_IsolateFlags flags;
  Dart_IsolateFlagsInitialize(&flags);
  flags.load_vmservice_library = false;

  const char* name = debug_name != nullptr ? debug_name : "feather_dart";
  char* create_err = nullptr;
  Dart_Isolate dart_isolate = nullptr;

  if (snapshot->kind == SnapshotKind::kAot) {
    dart_isolate = Dart_CreateIsolateGroup(
        name, name, snapshot->isolate_data, snapshot->isolate_instructions,
        &flags, fd_isolate.get(), fd_isolate.get(), &create_err);
  } else {
    dart_isolate = Dart_CreateIsolateGroupFromKernel(
        name, name, snapshot->kernel_bytes,
        static_cast<intptr_t>(snapshot->kernel_length), &flags,
        fd_isolate.get(), fd_isolate.get(), &create_err);
  }

  if (dart_isolate == nullptr) {
    if (out_error != nullptr) {
      *out_error = create_err != nullptr
                       ? create_err
                       : CopyString("Failed to create Dart isolate");
    } else if (create_err != nullptr) {
      std::free(create_err);
    }
    return nullptr;
  }

  fd_isolate->dart_isolate = dart_isolate;
  {
    EngineState& engine = GetEngine();
    std::lock_guard<std::mutex> lock(engine.isolates_mutex);
    engine.by_dart_isolate[dart_isolate] = fd_isolate.get();
  }

#if DART_INITIALIZE_PARAMS_CURRENT_VERSION >= 0x0000000B
  NativeAssetsApi native_assets{};
  std::memset(&native_assets, 0, sizeof(native_assets));
  native_assets.contains_asset = &NativeAssetsContainsAsset;
  native_assets.dlopen_asset = &NativeAssetsDlopenAsset;
  native_assets.available_assets = &NativeAssetsAvailableAssets;
  native_assets.dlsym = &NativeAssetsDlsym;
  native_assets.dlclose = &NativeAssetsDlclose;
  Dart_InitializeNativeAssetsResolver(&native_assets);
#endif

  Dart_SetMessageNotifyCallback(&MessageNotifyCallback);

  Dart_EnterScope();
  if (snapshot->kind == SnapshotKind::kKernel) {
    Dart_Handle root_lib = Dart_LoadScriptFromKernel(
        snapshot->kernel_bytes, static_cast<intptr_t>(snapshot->kernel_length));
    if (Dart_IsError(root_lib)) {
      SetOutError(out_error, Dart_GetError(root_lib));
      Dart_ExitScope();
      Dart_ShutdownIsolate();
      EngineState& engine = GetEngine();
      std::lock_guard<std::mutex> lock(engine.isolates_mutex);
      engine.by_dart_isolate.erase(dart_isolate);
      return nullptr;
    }
    fd_isolate->root_library = Dart_NewPersistentHandle(root_lib);
  } else {
    Dart_Handle root_lib = Dart_RootLibrary();
    if (!Dart_IsError(root_lib) && !Dart_IsNull(root_lib)) {
      fd_isolate->root_library = Dart_NewPersistentHandle(root_lib);
    }
  }

  if (!InitializeFeatherDartCoreLibraries(fd_isolate.get(), out_error)) {
    if (fd_isolate->root_library != nullptr) {
      Dart_DeletePersistentHandle(fd_isolate->root_library);
    }
    Dart_ExitScope();
    Dart_ShutdownIsolate();
    EngineState& engine = GetEngine();
    std::lock_guard<std::mutex> lock(engine.isolates_mutex);
    engine.by_dart_isolate.erase(dart_isolate);
    return nullptr;
  }

  Dart_ExitScope();
  Dart_ExitIsolate();
  return fd_isolate.release();
}

void feather_dart_isolate_destroy(FeatherDartIsolate* isolate) {
  if (isolate == nullptr) {
    return;
  }
  isolate->shutting_down.store(true, std::memory_order_release);
  if (isolate->fallback_timer != nullptr) {
    isolate->fallback_timer->Shutdown();
  }

  {
    std::lock_guard<std::recursive_mutex> lock(isolate->mutex);
    if (isolate->dart_isolate != nullptr) {
      {
        EngineState& engine = GetEngine();
        std::lock_guard<std::mutex> map_lock(engine.isolates_mutex);
        engine.by_dart_isolate.erase(isolate->dart_isolate);
      }
      Dart_EnterIsolate(isolate->dart_isolate);
      Dart_SetMessageNotifyCallback(nullptr);
      if (isolate->root_library != nullptr) {
        Dart_DeletePersistentHandle(isolate->root_library);
      }
      if (isolate->isolate_library != nullptr) {
        Dart_DeletePersistentHandle(isolate->isolate_library);
      }
      if (isolate->drain_microtasks_name != nullptr) {
        Dart_DeletePersistentHandle(isolate->drain_microtasks_name);
      }
      Dart_ShutdownIsolate();
      isolate->dart_isolate = nullptr;
    }
  }

  delete isolate;
}

void feather_dart_isolate_register_symbol(FeatherDartIsolate* isolate,
                                          const char* symbol_name,
                                          void* fn_ptr) {
  if (isolate == nullptr || symbol_name == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(isolate->symbols_mutex);
  isolate->host_symbols[symbol_name] = fn_ptr;
}

bool feather_dart_isolate_enter(FeatherDartIsolate* isolate) {
  if (isolate == nullptr ||
      isolate->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  isolate->mutex.lock();
  if (isolate->dart_isolate == nullptr) {
    isolate->mutex.unlock();
    return false;
  }
  if (isolate->enter_depth == 0) {
    if (Dart_CurrentIsolate() != isolate->dart_isolate) {
      Dart_EnterIsolate(isolate->dart_isolate);
    }
  }
  isolate->enter_depth++;
  g_entered_isolate = isolate;
  Dart_EnterScope();
  return true;
}

bool feather_dart_isolate_exit(FeatherDartIsolate* isolate, char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (isolate == nullptr) {
    SetOutError(out_error, "isolate must not be NULL");
    return false;
  }

  const bool drained_ok = DrainMicrotasksLocked(isolate, out_error);
  Dart_ExitScope();
  isolate->enter_depth--;
  if (isolate->enter_depth == 0) {
    g_entered_isolate = nullptr;
    Dart_ExitIsolate();
  }
  isolate->mutex.unlock();
  return drained_ok;
}

bool feather_dart_isolate_invoke_entrypoint(FeatherDartIsolate* isolate,
                                            const char* library_uri,
                                            const char* function_name,
                                            char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (isolate == nullptr || function_name == nullptr) {
    SetOutError(out_error, "isolate and function_name must not be NULL");
    return false;
  }
  if (!feather_dart_isolate_enter(isolate)) {
    SetOutError(out_error, "Failed to enter isolate");
    return false;
  }

  bool ok = true;
  {
    EnteredIsolateTlsGuard tls_guard(isolate);
    Dart_Handle target_lib = Dart_Null();
    if (library_uri != nullptr) {
      target_lib = Dart_LookupLibrary(Dart_NewStringFromCString(library_uri));
    } else if (isolate->root_library != nullptr) {
      target_lib = Dart_HandleFromPersistent(isolate->root_library);
    } else {
      target_lib = Dart_RootLibrary();
    }

    if (Dart_IsError(target_lib) || Dart_IsNull(target_lib)) {
      SetOutError(out_error, Dart_IsError(target_lib)
                                 ? Dart_GetError(target_lib)
                                 : "Target Dart library not found");
      ok = false;
    } else {
      Dart_Handle result = Dart_Invoke(
          target_lib, Dart_NewStringFromCString(function_name), 0, nullptr);
      if (Dart_IsError(result)) {
        SetOutError(out_error, Dart_GetError(result));
        ok = false;
      }
    }
  }

  char* exit_err = nullptr;
  if (!feather_dart_isolate_exit(isolate, &exit_err)) {
    if (ok && out_error != nullptr) {
      *out_error = exit_err;
    } else {
      std::free(exit_err);
    }
    ok = false;
  }
  return ok;
}

void* feather_dart_isolate_lookup_export(FeatherDartIsolate* isolate,
                                         const char* export_name) {
  if (isolate == nullptr || export_name == nullptr) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(isolate->symbols_mutex);
  auto it = isolate->dart_exports.find(export_name);
  return it != isolate->dart_exports.end() ? it->second : nullptr;
}

bool feather_dart_isolate_drain_microtasks(FeatherDartIsolate* isolate,
                                           char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (!feather_dart_isolate_enter(isolate)) {
    SetOutError(out_error, "Failed to enter isolate");
    return false;
  }
  return feather_dart_isolate_exit(isolate, out_error);
}

bool feather_dart_isolate_handle_message(FeatherDartIsolate* isolate) {
  if (isolate == nullptr ||
      isolate->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  if (!feather_dart_isolate_enter(isolate)) {
    return false;
  }

  bool ok = true;
  {
    EnteredIsolateTlsGuard tls_guard(isolate);
    Dart_Handle res = Dart_HandleMessage();
    if (Dart_IsError(res)) {
      ok = false;
      if (isolate->scheduler.on_unhandled_error != nullptr) {
        isolate->scheduler.on_unhandled_error(Dart_GetError(res),
                                              isolate->scheduler.context);
      }
    }
  }

  char* exit_err = nullptr;
  if (!feather_dart_isolate_exit(isolate, &exit_err)) {
    std::free(exit_err);
    ok = false;
  }
  return ok;
}

bool feather_dart_isolate_notify_timer(FeatherDartIsolate* isolate,
                                       int64_t port_id) {
  if (isolate == nullptr ||
      isolate->shutting_down.load(std::memory_order_acquire)) {
    return false;
  }
  Dart_CObject msg;
  msg.type = Dart_CObject_kNull;
  return Dart_PostCObject(static_cast<Dart_Port>(port_id), &msg);
}

void feather_dart_export(const char* name, void* fn_ptr) {
  if (name == nullptr) {
    return;
  }
  FeatherDartIsolate* fd_isolate = CurrentFeatherDartIsolate();
  if (fd_isolate == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(fd_isolate->symbols_mutex);
  fd_isolate->dart_exports[name] = fn_ptr;
}

}  // extern "C"
