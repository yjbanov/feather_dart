#include "include/shim.h"

#include <stdlib.h>
#include <string.h>

#if defined(FEATHER_DART_LINKED_HOST_VM)
typedef int cfeather_dart_linked_t;
#else

static char* stub_dup_error(void) {
  const char* msg =
      "FeatherDart native VM binary is not built yet. "
      "Run `python3 tools/fetch_dart_sdk.py && python3 tools/build_vm.py`.";
  size_t len = strlen(msg);
  char* out = (char*)malloc(len + 1);
  if (out != NULL) {
    memcpy(out, msg, len + 1);
  }
  return out;
}

bool feather_dart_engine_init(const FeatherDartEngineConfig* config,
                              char** out_error) {
  (void)config;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return false;
}

void feather_dart_engine_shutdown(void) {}

bool feather_dart_is_aot_runtime(void) {
  return false;
}

void feather_dart_set_log_callback(FeatherDartLogCallback callback,
                                   void* user_data) {
  (void)callback;
  (void)user_data;
}

void feather_dart_register_global_symbol(const char* symbol_name,
                                         void* fn_ptr) {
  (void)symbol_name;
  (void)fn_ptr;
}

void feather_dart_free_string(char* str) {
  free(str);
}

FeatherDartSnapshot* feather_dart_snapshot_load_aot_file(
    const char* library_path,
    char** out_error) {
  (void)library_path;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

FeatherDartSnapshot* feather_dart_snapshot_from_aot_symbols(
    const uint8_t* vm_data,
    const uint8_t* vm_instructions,
    const uint8_t* isolate_data,
    const uint8_t* isolate_instructions,
    char** out_error) {
  (void)vm_data;
  (void)vm_instructions;
  (void)isolate_data;
  (void)isolate_instructions;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

FeatherDartSnapshot* feather_dart_snapshot_from_process_symbols(
    char** out_error) {
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

FeatherDartSnapshot* feather_dart_snapshot_load_kernel_file(
    const char* dill_path,
    char** out_error) {
  (void)dill_path;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

FeatherDartSnapshot* feather_dart_snapshot_load_kernel_buffer(
    const uint8_t* bytes,
    size_t length,
    bool copy_bytes,
    char** out_error) {
  (void)bytes;
  (void)length;
  (void)copy_bytes;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

void feather_dart_snapshot_destroy(FeatherDartSnapshot* snapshot) {
  (void)snapshot;
}

FeatherDartIsolate* feather_dart_isolate_create(
    const FeatherDartSnapshot* snapshot,
    const char* debug_name,
    const FeatherDartScheduler* scheduler,
    char** out_error) {
  (void)snapshot;
  (void)debug_name;
  (void)scheduler;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return NULL;
}

void feather_dart_isolate_destroy(FeatherDartIsolate* isolate) {
  (void)isolate;
}

void feather_dart_isolate_register_symbol(FeatherDartIsolate* isolate,
                                          const char* symbol_name,
                                          void* fn_ptr) {
  (void)isolate;
  (void)symbol_name;
  (void)fn_ptr;
}

bool feather_dart_isolate_enter(FeatherDartIsolate* isolate) {
  (void)isolate;
  return false;
}

bool feather_dart_isolate_exit(FeatherDartIsolate* isolate, char** out_error) {
  (void)isolate;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return false;
}

bool feather_dart_isolate_invoke_entrypoint(FeatherDartIsolate* isolate,
                                            const char* library_uri,
                                            const char* function_name,
                                            char** out_error) {
  (void)isolate;
  (void)library_uri;
  (void)function_name;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return false;
}

void* feather_dart_isolate_lookup_export(FeatherDartIsolate* isolate,
                                         const char* export_name) {
  (void)isolate;
  (void)export_name;
  return NULL;
}

bool feather_dart_isolate_drain_microtasks(FeatherDartIsolate* isolate,
                                           char** out_error) {
  (void)isolate;
  if (out_error != NULL) {
    *out_error = stub_dup_error();
  }
  return false;
}

bool feather_dart_isolate_handle_message(FeatherDartIsolate* isolate) {
  (void)isolate;
  return false;
}

bool feather_dart_isolate_notify_timer(FeatherDartIsolate* isolate,
                                       int64_t port_id) {
  (void)isolate;
  (void)port_id;
  return false;
}

void feather_dart_export(const char* name, void* fn_ptr) {
  (void)name;
  (void)fn_ptr;
}

#endif
