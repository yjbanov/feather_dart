#ifndef FEATHER_DART_H_
#define FEATHER_DART_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define FEATHER_DART_EXPORT __declspec(dllexport)
#else
#define FEATHER_DART_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FeatherDartSnapshot FeatherDartSnapshot;
typedef struct FeatherDartIsolate FeatherDartIsolate;

/**
 * Receives `print()` output from Dart isolates.
 */
typedef void (*FeatherDartLogCallback)(const char* line, void* user_data);

/**
 * Process-wide Dart VM configuration.
 */
typedef struct FeatherDartEngineConfig {
  const char* const* vm_flags;
  int vm_flags_count;
  FeatherDartLogCallback log_callback;
  void* log_user_data;
} FeatherDartEngineConfig;

/**
 * Host event-loop integration hooks for a `FeatherDartIsolate`.
 *
 * - `schedule_message`: Called from any thread when the isolate has pending
 *   port messages. The host must asynchronously invoke
 *   `feather_dart_isolate_handle_message(isolate)` on its target queue or
 *   dispatcher (e.g. `DispatchQueue`, `Handler`, or Kotlin coroutine).
 * - `schedule_timer`: Optional. Called when `dart:async` `Timer` schedules or
 *   cancels a wakeup for `port_id`. When `delay_ms >= 0`, invoke
 *   `feather_dart_isolate_notify_timer(isolate, port_id)` after `delay_ms`
 *   milliseconds (replacing any prior pending wakeup for `port_id`). When
 *   `delay_ms < 0`, cancel any pending wakeup for `port_id`. If NULL, an
 *   internal condition-variable timer thread is used automatically.
 * - `on_unhandled_error`: Optional callback invoked if an asynchronous message
 *   or microtask throws an uncaught Dart error.
 */
typedef struct FeatherDartScheduler {
  void (*schedule_message)(FeatherDartIsolate* isolate, void* context);
  void (*schedule_timer)(FeatherDartIsolate* isolate,
                         int64_t port_id,
                         int64_t delay_ms,
                         void* context);
  void (*on_unhandled_error)(const char* error_message, void* context);
  void* context;
} FeatherDartScheduler;

/* ========================================================================== *
 * Engine Lifecycle & Global Host Symbols
 * ========================================================================== */

/**
 * Initializes the process-wide FeatherDart VM runtime. Safe to call multiple
 * times; subsequent calls succeed as no-ops.
 */
FEATHER_DART_EXPORT bool feather_dart_engine_init(
    const FeatherDartEngineConfig* config,
    char** out_error);

/**
 * Shuts down all remaining isolates and cleans up the FeatherDart VM runtime.
 */
FEATHER_DART_EXPORT void feather_dart_engine_shutdown(void);

/**
 * Returns true if linked against the AOT precompiled runtime, or false if
 * linked against the Kernel/JIT development runtime.
 */
FEATHER_DART_EXPORT bool feather_dart_is_aot_runtime(void);

/**
 * Updates the global `print()` callback for Dart code.
 */
FEATHER_DART_EXPORT void feather_dart_set_log_callback(
    FeatherDartLogCallback callback,
    void* user_data);

/**
 * Registers a process-wide native function pointer resolvable by Dart
 * `@Native<...>(symbol: ...)` declarations across all isolates.
 */
FEATHER_DART_EXPORT void feather_dart_register_global_symbol(
    const char* symbol_name,
    void* fn_ptr);

/**
 * Frees an error string returned via `out_error`.
 */
FEATHER_DART_EXPORT void feather_dart_free_string(char* str);

/* ========================================================================== *
 * Snapshots (AOT & Kernel .dill)
 * ========================================================================== */

/**
 * Loads an AOT snapshot from a shared library path (`.so` on Android/Linux,
 * `.dylib` or `.framework` binary on iOS/macOS).
 */
FEATHER_DART_EXPORT FeatherDartSnapshot* feather_dart_snapshot_load_aot_file(
    const char* library_path,
    char** out_error);

/**
 * Creates an AOT snapshot reference from explicit in-memory symbols.
 * For unified Dart snapshots, pass `data` and `instructions` as `isolate_data`
 * and `isolate_instructions` (`vm_data` and `vm_instructions` may be NULL).
 */
FEATHER_DART_EXPORT FeatherDartSnapshot* feather_dart_snapshot_from_aot_symbols(
    const uint8_t* vm_data,
    const uint8_t* vm_instructions,
    const uint8_t* isolate_data,
    const uint8_t* isolate_instructions,
    char** out_error);

/**
 * Resolves AOT snapshot symbols linked into the current process/frameworks via
 * `RTLD_DEFAULT` (convenient for iOS apps embedding an AOT `.xcframework`).
 */
FEATHER_DART_EXPORT FeatherDartSnapshot*
feather_dart_snapshot_from_process_symbols(char** out_error);

/**
 * Loads a compiled Kernel `.dill` binary from a filesystem path (dev/host mode).
 */
FEATHER_DART_EXPORT FeatherDartSnapshot* feather_dart_snapshot_load_kernel_file(
    const char* dill_path,
    char** out_error);

/**
 * Loads a compiled Kernel `.dill` binary from memory (dev/host mode).
 * If `copy_bytes` is true, the snapshot copies and owns the buffer.
 */
FEATHER_DART_EXPORT FeatherDartSnapshot*
feather_dart_snapshot_load_kernel_buffer(const uint8_t* bytes,
                                         size_t length,
                                         bool copy_bytes,
                                         char** out_error);

/**
 * Releases resources owned by `snapshot`. Must only be called after all
 * isolates created from `snapshot` have been destroyed.
 */
FEATHER_DART_EXPORT void feather_dart_snapshot_destroy(
    FeatherDartSnapshot* snapshot);

/* ========================================================================== *
 * Lock-Guarded Isolates, Direct FFI Binding, & Event Loop
 * ========================================================================== */

/**
 * Creates a new Dart isolate from `snapshot`, initializes the 7 core libraries
 * (`core`, `async`, `collection`, `convert`, `ffi`, `math`, `typed_data`), and
 * attaches the `@Native` resolver and `scheduler`.
 *
 * Does NOT leave the isolate entered on the calling thread.
 */
FEATHER_DART_EXPORT FeatherDartIsolate* feather_dart_isolate_create(
    const FeatherDartSnapshot* snapshot,
    const char* debug_name,
    const FeatherDartScheduler* scheduler,
    char** out_error);

/**
 * Shuts down `isolate` and frees its handle.
 */
FEATHER_DART_EXPORT void feather_dart_isolate_destroy(
    FeatherDartIsolate* isolate);

/**
 * Registers an isolate-scoped host function pointer resolvable by Dart
 * `@Native<...>(symbol: ...)` declarations within `isolate`.
 */
FEATHER_DART_EXPORT void feather_dart_isolate_register_symbol(
    FeatherDartIsolate* isolate,
    const char* symbol_name,
    void* fn_ptr);

/**
 * Acquires the re-entrant lock for `isolate`, enters the Dart isolate if not
 * already entered on this thread, and opens a `Dart_EnterScope()`.
 *
 * Pair every call with `feather_dart_isolate_exit`. While inside an entered
 * scope, host code may directly invoke `Pointer.fromFunction` C function
 * pointers exported by Dart with zero marshaling overhead.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_enter(
    FeatherDartIsolate* isolate);

/**
 * Drains pending microtasks, closes the handle scope opened by
 * `feather_dart_isolate_enter`, exits the Dart isolate when the re-entrant
 * depth reaches zero, and releases the isolate lock.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_exit(FeatherDartIsolate* isolate,
                                                   char** out_error);

/**
 * Acquires `isolate` (re-entrant safe), invokes a zero-argument top-level Dart
 * function (such as `"main"` to register FFI exports), drains microtasks, and
 * releases `isolate`. Pass `library_uri = NULL` to target the root library.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_invoke_entrypoint(
    FeatherDartIsolate* isolate,
    const char* library_uri,
    const char* function_name,
    char** out_error);

/**
 * Looks up a C function pointer exported by Dart via
 * `feather_dart_export(name, ptr)`. Returns NULL if not registered.
 */
FEATHER_DART_EXPORT void* feather_dart_isolate_lookup_export(
    FeatherDartIsolate* isolate,
    const char* export_name);

/**
 * Manually drains the Dart microtask queue. Safe to call either inside or
 * outside an active `feather_dart_isolate_enter` scope.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_drain_microtasks(
    FeatherDartIsolate* isolate,
    char** out_error);

/**
 * Processes one pending asynchronous port message (including timer wakeups and
 * `NativeCallable.listener` callbacks) and drains resulting microtasks.
 * Thread-safe and lock-guarded.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_handle_message(
    FeatherDartIsolate* isolate);

/**
 * Posts a timer wakeup event to `port_id` when a host-scheduled timer expires.
 * Thread-safe; may be called from any host thread/queue without entering the
 * isolate first.
 */
FEATHER_DART_EXPORT bool feather_dart_isolate_notify_timer(
    FeatherDartIsolate* isolate,
    int64_t port_id);

/* ========================================================================== *
 * Built-in Symbols Callable from Dart via `@Native`
 * ========================================================================== */

/**
 * Callable from Dart via `@Native<Void Function(Pointer<Uint8>, Pointer<Void>)>(
 * symbol: 'feather_dart_export', isLeaf: true)` to publish a Dart FFI function
 * pointer (`Pointer.fromFunction` or `NativeCallable`) to the host.
 */
FEATHER_DART_EXPORT void feather_dart_export(const char* name, void* fn_ptr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FEATHER_DART_H_
