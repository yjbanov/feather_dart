# Keep JNI upcall entrypoints invoked from feather_dart_jni.cc
-keep class dev.featherdart.FeatherDartEngine {
    private static void onNativeLog(java.lang.String);
}
-keep class dev.featherdart.InternalIsolateCallbacks {
    void onScheduleMessage();
    void onScheduleTimer(long, long);
    void onUnhandledError(java.lang.String);
}
