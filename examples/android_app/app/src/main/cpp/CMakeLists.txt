cmake_minimum_required(VERSION 3.22.1)
project(feather_dart_demo_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FEATHER_DART_INCLUDE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../vm/include")

add_library(app_bridge_jni SHARED
    app_bridge_jni.cc
)

target_include_directories(app_bridge_jni PRIVATE
    "${FEATHER_DART_INCLUDE}"
)

find_library(log-lib log)
target_link_libraries(app_bridge_jni PRIVATE
    ${log-lib}
)
