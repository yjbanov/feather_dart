cmake_minimum_required(VERSION 3.22.1)
project(feather_dart_jni LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FEATHER_DART_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..")
set(FEATHER_DART_INCLUDE "${FEATHER_DART_ROOT}/vm/include")
set(FEATHER_DART_DIST_JNILIBS
    "${FEATHER_DART_ROOT}/dist/android/jniLibs/${ANDROID_ABI}")

add_library(feather_dart_headers INTERFACE)
target_include_directories(feather_dart_headers INTERFACE "${FEATHER_DART_INCLUDE}")

add_library(feather_dart_vm SHARED IMPORTED)
set_target_properties(feather_dart_vm PROPERTIES
    IMPORTED_LOCATION "${FEATHER_DART_DIST_JNILIBS}/libfeather_dart.so"
)

add_library(feather_dart_jni SHARED
    feather_dart_jni.cc
)

target_include_directories(feather_dart_jni PRIVATE "${FEATHER_DART_INCLUDE}")

find_library(log-lib log)
target_link_libraries(feather_dart_jni PRIVATE
    feather_dart_vm
    ${log-lib}
    dl
)
