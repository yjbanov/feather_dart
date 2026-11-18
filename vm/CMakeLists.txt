cmake_minimum_required(VERSION 3.22)
project(feather_dart_vm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(DART_SDK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/dart-sdk"
    CACHE PATH "Path to dart-lang/sdk checkout")
set(DART_PREBUILT_LIB ""
    CACHE FILEPATH "Path to prebuilt libdart_aotruntime_product.a or libdart_jit.a")

add_library(feather_dart SHARED
    src/feather_dart.cc
    src/timer_fallback.cc
)

target_include_directories(feather_dart
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${DART_SDK_SRC_DIR}/runtime/include"
)

if(DART_PREBUILT_LIB)
    target_link_libraries(feather_dart PRIVATE "${DART_PREBUILT_LIB}")
endif()

if(ANDROID)
    target_link_libraries(feather_dart PRIVATE dl log)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(feather_dart PRIVATE dl pthread)
endif()
