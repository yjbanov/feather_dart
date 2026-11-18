#include <jni.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "feather_dart.h"

namespace {

JavaVM* g_app_jvm = nullptr;
std::mutex g_host_mutex;
jobject g_host_services_ref = nullptr;
jmethodID g_kv_read_method = nullptr;
jmethodID g_kv_write_method = nullptr;
jmethodID g_fetch_quote_method = nullptr;
jmethodID g_on_updated_method = nullptr;

class AttachedJniEnv {
 public:
  AttachedJniEnv() {
    if (g_app_jvm == nullptr) {
      return;
    }
    const jint res =
        g_app_jvm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
#if defined(__ANDROID__)
      if (g_app_jvm->AttachCurrentThread(&env_, nullptr) == JNI_OK) {
        attached_ = true;
      }
#else
      if (g_app_jvm->AttachCurrentThread(reinterpret_cast<void**>(&env_),
                                         nullptr) == JNI_OK) {
        attached_ = true;
      }
#endif
    }
  }

  ~AttachedJniEnv() {
    if (attached_ && g_app_jvm != nullptr) {
      g_app_jvm->DetachCurrentThread();
    }
  }

  JNIEnv* get() const { return env_; }

 private:
  JNIEnv* env_ = nullptr;
  bool attached_ = false;
};

extern "C" int32_t host_kv_read(const uint8_t* key_ptr,
                                uint8_t* out_buf,
                                int32_t out_cap) {
  if (key_ptr == nullptr) {
    return -1;
  }
  AttachedJniEnv scoped;
  JNIEnv* env = scoped.get();
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (env == nullptr || g_host_services_ref == nullptr) {
    return -1;
  }

  jstring jkey = env->NewStringUTF(reinterpret_cast<const char*>(key_ptr));
  auto jbytes = static_cast<jbyteArray>(
      env->CallObjectMethod(g_host_services_ref, g_kv_read_method, jkey));
  env->DeleteLocalRef(jkey);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    return -1;
  }
  if (jbytes == nullptr) {
    return -1;
  }

  const jsize byte_len = env->GetArrayLength(jbytes);
  if (out_buf == nullptr || out_cap < byte_len) {
    env->DeleteLocalRef(jbytes);
    return -static_cast<int32_t>(byte_len) - 1;
  }
  env->GetByteArrayRegion(jbytes, 0, byte_len,
                          reinterpret_cast<jbyte*>(out_buf));
  env->DeleteLocalRef(jbytes);
  return static_cast<int32_t>(byte_len);
}

extern "C" int32_t host_kv_write(const uint8_t* key_ptr,
                                 const uint8_t* val_ptr,
                                 int32_t val_len) {
  if (key_ptr == nullptr || val_ptr == nullptr || val_len < 0) {
    return -1;
  }
  AttachedJniEnv scoped;
  JNIEnv* env = scoped.get();
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (env == nullptr || g_host_services_ref == nullptr) {
    return -1;
  }

  jstring jkey = env->NewStringUTF(reinterpret_cast<const char*>(key_ptr));
  jbyteArray jval = env->NewByteArray(val_len);
  env->SetByteArrayRegion(jval, 0, val_len,
                          reinterpret_cast<const jbyte*>(val_ptr));
  env->CallVoidMethod(g_host_services_ref, g_kv_write_method, jkey, jval);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(jval);
  env->DeleteLocalRef(jkey);
  return 0;
}

extern "C" void host_fetch_quote_async(int64_t request_id,
                                       const uint8_t* symbol_ptr) {
  if (symbol_ptr == nullptr) {
    return;
  }
  AttachedJniEnv scoped;
  JNIEnv* env = scoped.get();
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (env == nullptr || g_host_services_ref == nullptr) {
    return;
  }

  jstring jsym = env->NewStringUTF(reinterpret_cast<const char*>(symbol_ptr));
  env->CallVoidMethod(g_host_services_ref, g_fetch_quote_method,
                      static_cast<jlong>(request_id), jsym);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(jsym);
}

extern "C" void host_on_portfolio_updated(const uint8_t* json_ptr,
                                          int32_t json_len) {
  if (json_ptr == nullptr || json_len <= 0) {
    return;
  }
  AttachedJniEnv scoped;
  JNIEnv* env = scoped.get();
  std::lock_guard<std::mutex> lock(g_host_mutex);
  if (env == nullptr || g_host_services_ref == nullptr) {
    return;
  }

  jbyteArray jjson = env->NewByteArray(json_len);
  env->SetByteArrayRegion(jjson, 0, json_len,
                          reinterpret_cast<const jbyte*>(json_ptr));
  env->CallVoidMethod(g_host_services_ref, g_on_updated_method, jjson);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(jjson);
}

}  // namespace

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* /*reserved*/) {
  g_app_jvm = vm;
  return JNI_VERSION_1_6;
}

extern "C" {

JNIEXPORT void JNICALL
Java_dev_featherdart_demo_HostServices_nativeBindHostServices(
    JNIEnv* env,
    jobject thiz,
    jobject symbol_registrar) {
  {
    std::lock_guard<std::mutex> lock(g_host_mutex);
    if (g_host_services_ref != nullptr) {
      env->DeleteGlobalRef(g_host_services_ref);
    }
    g_host_services_ref = env->NewGlobalRef(thiz);
    jclass cls = env->GetObjectClass(thiz);
    g_kv_read_method =
        env->GetMethodID(cls, "onKvRead", "(Ljava/lang/String;)[B");
    g_kv_write_method =
        env->GetMethodID(cls, "onKvWrite", "(Ljava/lang/String;[B)V");
    g_fetch_quote_method =
        env->GetMethodID(cls, "onFetchQuoteAsync", "(JLjava/lang/String;)V");
    g_on_updated_method =
        env->GetMethodID(cls, "onPortfolioUpdated", "([B)V");
    env->DeleteLocalRef(cls);
  }

  jclass reg_cls = env->GetObjectClass(symbol_registrar);
  jmethodID accept_sym =
      env->GetMethodID(reg_cls, "register", "(Ljava/lang/String;J)V");

  struct Entry {
    const char* name;
    void* ptr;
  };
  const Entry entries[] = {
      {"host_kv_read", reinterpret_cast<void*>(&host_kv_read)},
      {"host_kv_write", reinterpret_cast<void*>(&host_kv_write)},
      {"host_fetch_quote_async",
       reinterpret_cast<void*>(&host_fetch_quote_async)},
      {"host_on_portfolio_updated",
       reinterpret_cast<void*>(&host_on_portfolio_updated)},
  };

  for (const auto& entry : entries) {
    jstring jname = env->NewStringUTF(entry.name);
    env->CallVoidMethod(
        symbol_registrar, accept_sym, jname,
        static_cast<jlong>(reinterpret_cast<uintptr_t>(entry.ptr)));
    env->DeleteLocalRef(jname);
  }
  env->DeleteLocalRef(reg_cls);
}

JNIEXPORT jint JNICALL
Java_dev_featherdart_demo_SharedLogicClient_nativeSimulateVarFast(
    JNIEnv* env,
    jobject /*thiz*/,
    jlong fn_ptr,
    jint paths,
    jint horizon_days,
    jdoubleArray out_metrics5) {
  double metrics[5] = {0.0, 0.0, 0.0, 0.0, 0.0};
  auto fn = reinterpret_cast<int32_t (*)(int32_t, int32_t, double*)>(
      static_cast<uintptr_t>(fn_ptr));
  const int32_t cache_hit =
      fn(static_cast<int32_t>(paths), static_cast<int32_t>(horizon_days),
         metrics);
  env->SetDoubleArrayRegion(out_metrics5, 0, 5, metrics);
  return static_cast<jint>(cache_hit);
}

}  // extern "C"
