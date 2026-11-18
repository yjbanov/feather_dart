#include <jni.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include "feather_dart.h"

namespace {

JavaVM* g_jvm = nullptr;
std::mutex g_log_jni_mutex;
jclass g_engine_class = nullptr;
jmethodID g_on_native_log_method = nullptr;

class ScopedJniEnv {
 public:
  ScopedJniEnv() {
    if (g_jvm == nullptr) {
      return;
    }
    const jint status =
        g_jvm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
    if (status == JNI_EDETACHED) {
#if defined(__ANDROID__)
      if (g_jvm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
        attached_ = true;
      }
#else
      if (g_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env_),
                                     nullptr) == JNI_OK) {
        attached_ = true;
      }
#endif
    }
  }

  ~ScopedJniEnv() {
    if (attached_ && g_jvm != nullptr) {
      g_jvm->DetachCurrentThread();
    }
  }

  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

void ThrowFeatherDartException(JNIEnv* env, const char* message) {
  jclass ex_cls = env->FindClass("dev/featherdart/FeatherDartException");
  if (ex_cls == nullptr) {
    env->ExceptionClear();
    ex_cls = env->FindClass("java/lang/RuntimeException");
  }
  if (ex_cls != nullptr) {
    env->ThrowNew(ex_cls, message != nullptr ? message : "FeatherDart error");
  }
}

void ConsumeAndThrowError(JNIEnv* env, char* err_ptr) {
  const std::string msg =
      err_ptr != nullptr ? err_ptr : "Unknown FeatherDart error";
  feather_dart_free_string(err_ptr);
  ThrowFeatherDartException(env, msg.c_str());
}

void JniLogCallback(const char* line, void* /*user_data*/) {
  if (line == nullptr) {
    return;
  }
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, "FeatherDart", "%s", line);
#endif
  jclass cls = nullptr;
  jmethodID method = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_log_jni_mutex);
    cls = g_engine_class;
    method = g_on_native_log_method;
  }
  if (cls == nullptr || method == nullptr) {
    return;
  }
  ScopedJniEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) {
    return;
  }
  jstring jline = env->NewStringUTF(line);
  if (jline != nullptr) {
    env->CallStaticVoidMethod(cls, method, jline);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(jline);
  }
}

struct JniSchedulerContext {
  jobject callback_ref = nullptr;
  jmethodID on_schedule_message = nullptr;
  jmethodID on_schedule_timer = nullptr;
  jmethodID on_unhandled_error = nullptr;
};

void JniScheduleMessage(FeatherDartIsolate* /*isolate*/, void* raw_ctx) {
  auto* ctx = static_cast<JniSchedulerContext*>(raw_ctx);
  if (ctx == nullptr || ctx->callback_ref == nullptr) {
    return;
  }
  ScopedJniEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) {
    return;
  }
  env->CallVoidMethod(ctx->callback_ref, ctx->on_schedule_message);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
}

void JniScheduleTimer(FeatherDartIsolate* /*isolate*/,
                      int64_t port_id,
                      int64_t delay_ms,
                      void* raw_ctx) {
  auto* ctx = static_cast<JniSchedulerContext*>(raw_ctx);
  if (ctx == nullptr || ctx->callback_ref == nullptr) {
    return;
  }
  ScopedJniEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) {
    return;
  }
  env->CallVoidMethod(ctx->callback_ref, ctx->on_schedule_timer,
                      static_cast<jlong>(port_id),
                      static_cast<jlong>(delay_ms));
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
}

void JniUnhandledError(const char* error_message, void* raw_ctx) {
  auto* ctx = static_cast<JniSchedulerContext*>(raw_ctx);
  if (ctx == nullptr || ctx->callback_ref == nullptr ||
      error_message == nullptr) {
    return;
  }
  ScopedJniEnv scoped;
  JNIEnv* env = scoped.get();
  if (env == nullptr) {
    return;
  }
  jstring jmsg = env->NewStringUTF(error_message);
  if (jmsg != nullptr) {
    env->CallVoidMethod(ctx->callback_ref, ctx->on_unhandled_error, jmsg);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(jmsg);
  }
}

struct JniIsolateHolder {
  FeatherDartIsolate* isolate = nullptr;
  JniSchedulerContext* scheduler_ctx = nullptr;
};

}  // namespace

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_jvm = vm;
  return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeEngineInit(JNIEnv* env,
                                                        jclass /*clazz*/,
                                                        jobjectArray vm_flags) {
  {
    std::lock_guard<std::mutex> lock(g_log_jni_mutex);
    if (g_engine_class == nullptr) {
      jclass local_cls = env->FindClass("dev/featherdart/FeatherDartEngine");
      if (local_cls != nullptr) {
        g_engine_class = static_cast<jclass>(env->NewGlobalRef(local_cls));
        g_on_native_log_method = env->GetStaticMethodID(
            g_engine_class, "onNativeLog", "(Ljava/lang/String;)V");
        env->DeleteLocalRef(local_cls);
      } else {
        env->ExceptionClear();
      }
    }
  }

  std::vector<std::string> owned_flags;
  std::vector<const char*> c_flags;
  if (vm_flags != nullptr) {
    const jsize count = env->GetArrayLength(vm_flags);
    owned_flags.reserve(static_cast<size_t>(count));
    for (jsize i = 0; i < count; ++i) {
      auto jflag = static_cast<jstring>(env->GetObjectArrayElement(vm_flags, i));
      if (jflag != nullptr) {
        const char* chars = env->GetStringUTFChars(jflag, nullptr);
        if (chars != nullptr) {
          owned_flags.emplace_back(chars);
          env->ReleaseStringUTFChars(jflag, chars);
        }
        env->DeleteLocalRef(jflag);
      }
    }
    for (const auto& s : owned_flags) {
      c_flags.push_back(s.c_str());
    }
  }

  FeatherDartEngineConfig config{};
  config.vm_flags = c_flags.empty() ? nullptr : c_flags.data();
  config.vm_flags_count = static_cast<int>(c_flags.size());
  config.log_callback = &JniLogCallback;
  config.log_user_data = nullptr;

  char* err = nullptr;
  if (!feather_dart_engine_init(&config, &err)) {
    ConsumeAndThrowError(env, err);
  }
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeEngineShutdown(JNIEnv* /*env*/,
                                                            jclass /*clazz*/) {
  feather_dart_engine_shutdown();
}

JNIEXPORT jboolean JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsAotRuntime(JNIEnv* /*env*/,
                                                          jclass /*clazz*/) {
  return feather_dart_is_aot_runtime() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeRegisterGlobalSymbol(
    JNIEnv* env,
    jclass /*clazz*/,
    jstring symbol,
    jlong fn_ptr) {
  if (symbol == nullptr) {
    return;
  }
  const char* c_sym = env->GetStringUTFChars(symbol, nullptr);
  if (c_sym != nullptr) {
    feather_dart_register_global_symbol(
        c_sym, reinterpret_cast<void*>(static_cast<uintptr_t>(fn_ptr)));
    env->ReleaseStringUTFChars(symbol, c_sym);
  }
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeSnapshotLoadAotFile(JNIEnv* env,
                                                                 jclass /*clazz*/,
                                                                 jstring path) {
  const char* c_path = env->GetStringUTFChars(path, nullptr);
  char* err = nullptr;
  FeatherDartSnapshot* snap = feather_dart_snapshot_load_aot_file(c_path, &err);
  env->ReleaseStringUTFChars(path, c_path);
  if (snap == nullptr) {
    ConsumeAndThrowError(env, err);
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(snap));
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeSnapshotLoadKernelFile(
    JNIEnv* env,
    jclass /*clazz*/,
    jstring path) {
  const char* c_path = env->GetStringUTFChars(path, nullptr);
  char* err = nullptr;
  FeatherDartSnapshot* snap =
      feather_dart_snapshot_load_kernel_file(c_path, &err);
  env->ReleaseStringUTFChars(path, c_path);
  if (snap == nullptr) {
    ConsumeAndThrowError(env, err);
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(snap));
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeSnapshotLoadKernelBytes(
    JNIEnv* env,
    jclass /*clazz*/,
    jbyteArray bytes) {
  const jsize len = env->GetArrayLength(bytes);
  jbyte* ptr = env->GetByteArrayElements(bytes, nullptr);
  char* err = nullptr;
  FeatherDartSnapshot* snap = feather_dart_snapshot_load_kernel_buffer(
      reinterpret_cast<const uint8_t*>(ptr), static_cast<size_t>(len),
      /*copy_bytes=*/true, &err);
  env->ReleaseByteArrayElements(bytes, ptr, JNI_ABORT);
  if (snap == nullptr) {
    ConsumeAndThrowError(env, err);
    return 0;
  }
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(snap));
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeSnapshotDestroy(JNIEnv* /*env*/,
                                                             jclass /*clazz*/,
                                                             jlong handle) {
  if (handle != 0) {
    feather_dart_snapshot_destroy(
        reinterpret_cast<FeatherDartSnapshot*>(static_cast<uintptr_t>(handle)));
  }
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateCreate(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong snapshot_handle,
    jstring debug_name,
    jobject callbacks) {
  auto* snapshot = reinterpret_cast<const FeatherDartSnapshot*>(
      static_cast<uintptr_t>(snapshot_handle));
  auto* sched_ctx = new JniSchedulerContext();
  if (callbacks != nullptr) {
    sched_ctx->callback_ref = env->NewGlobalRef(callbacks);
    jclass cb_cls = env->GetObjectClass(callbacks);
    sched_ctx->on_schedule_message =
        env->GetMethodID(cb_cls, "onScheduleMessage", "()V");
    sched_ctx->on_schedule_timer =
        env->GetMethodID(cb_cls, "onScheduleTimer", "(JJ)V");
    sched_ctx->on_unhandled_error =
        env->GetMethodID(cb_cls, "onUnhandledError", "(Ljava/lang/String;)V");
    env->DeleteLocalRef(cb_cls);
  }

  FeatherDartScheduler scheduler{};
  scheduler.schedule_message = &JniScheduleMessage;
  scheduler.schedule_timer = &JniScheduleTimer;
  scheduler.on_unhandled_error = &JniUnhandledError;
  scheduler.context = sched_ctx;

  const char* c_name = debug_name != nullptr
                           ? env->GetStringUTFChars(debug_name, nullptr)
                           : nullptr;
  char* err = nullptr;
  FeatherDartIsolate* iso =
      feather_dart_isolate_create(snapshot, c_name, &scheduler, &err);
  if (c_name != nullptr) {
    env->ReleaseStringUTFChars(debug_name, c_name);
  }

  if (iso == nullptr) {
    if (sched_ctx->callback_ref != nullptr) {
      env->DeleteGlobalRef(sched_ctx->callback_ref);
    }
    delete sched_ctx;
    ConsumeAndThrowError(env, err);
    return 0;
  }

  auto* holder = new JniIsolateHolder{iso, sched_ctx};
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(holder));
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateRawPointer(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong holder_handle) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  return holder != nullptr
             ? static_cast<jlong>(reinterpret_cast<uintptr_t>(holder->isolate))
             : 0;
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateDestroy(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong holder_handle) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr) {
    return;
  }
  feather_dart_isolate_destroy(holder->isolate);
  if (holder->scheduler_ctx != nullptr) {
    if (holder->scheduler_ctx->callback_ref != nullptr) {
      env->DeleteGlobalRef(holder->scheduler_ctx->callback_ref);
    }
    delete holder->scheduler_ctx;
  }
  delete holder;
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateRegisterSymbol(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong holder_handle,
    jstring symbol,
    jlong fn_ptr) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr || symbol == nullptr) {
    return;
  }
  const char* c_sym = env->GetStringUTFChars(symbol, nullptr);
  feather_dart_isolate_register_symbol(
      holder->isolate, c_sym,
      reinterpret_cast<void*>(static_cast<uintptr_t>(fn_ptr)));
  env->ReleaseStringUTFChars(symbol, c_sym);
}

JNIEXPORT jboolean JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateEnter(JNIEnv* /*env*/,
                                                          jclass /*clazz*/,
                                                          jlong holder_handle) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr) {
    return JNI_FALSE;
  }
  return feather_dart_isolate_enter(holder->isolate) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateExit(JNIEnv* env,
                                                         jclass /*clazz*/,
                                                         jlong holder_handle) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr) {
    return;
  }
  char* err = nullptr;
  if (!feather_dart_isolate_exit(holder->isolate, &err)) {
    ConsumeAndThrowError(env, err);
  }
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateInvokeEntrypoint(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong holder_handle,
    jstring library_uri,
    jstring function_name) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr || function_name == nullptr) {
    return;
  }
  const char* c_lib = library_uri != nullptr
                          ? env->GetStringUTFChars(library_uri, nullptr)
                          : nullptr;
  const char* c_fn = env->GetStringUTFChars(function_name, nullptr);
  char* err = nullptr;
  const bool ok =
      feather_dart_isolate_invoke_entrypoint(holder->isolate, c_lib, c_fn, &err);
  if (c_lib != nullptr) {
    env->ReleaseStringUTFChars(library_uri, c_lib);
  }
  env->ReleaseStringUTFChars(function_name, c_fn);
  if (!ok) {
    ConsumeAndThrowError(env, err);
  }
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateLookupExport(
    JNIEnv* env,
    jclass /*clazz*/,
    jlong holder_handle,
    jstring export_name) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder == nullptr || export_name == nullptr) {
    return 0;
  }
  const char* c_name = env->GetStringUTFChars(export_name, nullptr);
  void* ptr = feather_dart_isolate_lookup_export(holder->isolate, c_name);
  env->ReleaseStringUTFChars(export_name, c_name);
  return static_cast<jlong>(reinterpret_cast<uintptr_t>(ptr));
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateHandleMessage(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong holder_handle) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder != nullptr) {
    feather_dart_isolate_handle_message(holder->isolate);
  }
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeIsolateNotifyTimer(
    JNIEnv* /*env*/,
    jclass /*clazz*/,
    jlong holder_handle,
    jlong port_id) {
  auto* holder = reinterpret_cast<JniIsolateHolder*>(
      static_cast<uintptr_t>(holder_handle));
  if (holder != nullptr) {
    feather_dart_isolate_notify_timer(holder->isolate,
                                      static_cast<int64_t>(port_id));
  }
}

JNIEXPORT void JNICALL
Java_dev_featherdart_FeatherDartNative_nativeCallVoid(JNIEnv* /*env*/,
                                                      jclass /*clazz*/,
                                                      jlong fn_ptr) {
  auto fn = reinterpret_cast<void (*)()>(static_cast<uintptr_t>(fn_ptr));
  fn();
}

JNIEXPORT jlong JNICALL
Java_dev_featherdart_FeatherDartNative_nativeCallInt64(JNIEnv* /*env*/,
                                                       jclass /*clazz*/,
                                                       jlong fn_ptr,
                                                       jlong arg0,
                                                       jlong arg1) {
  auto fn = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(
      static_cast<uintptr_t>(fn_ptr));
  return static_cast<jlong>(
      fn(static_cast<int64_t>(arg0), static_cast<int64_t>(arg1)));
}

JNIEXPORT jint JNICALL
Java_dev_featherdart_FeatherDartNative_nativeCallDirectBuffer(JNIEnv* env,
                                                              jclass /*clazz*/,
                                                              jlong fn_ptr,
                                                              jobject in_buf,
                                                              jint in_len,
                                                              jobject out_buf,
                                                              jint out_cap) {
  const auto* in_ptr =
      in_buf != nullptr
          ? static_cast<const uint8_t*>(env->GetDirectBufferAddress(in_buf))
          : nullptr;
  auto* out_ptr =
      out_buf != nullptr
          ? static_cast<uint8_t*>(env->GetDirectBufferAddress(out_buf))
          : nullptr;
  auto fn = reinterpret_cast<int32_t (*)(const uint8_t*, int32_t, uint8_t*,
                                         int32_t)>(
      static_cast<uintptr_t>(fn_ptr));
  return static_cast<jint>(fn(in_ptr, static_cast<int32_t>(in_len), out_ptr,
                              static_cast<int32_t>(out_cap)));
}

}  // extern "C"
