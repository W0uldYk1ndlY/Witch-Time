cmake_minimum_required(VERSION 3.22)
project(witch_time_android C)
if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "This launcher requires Android arm64-v8a")
endif()
add_library(main SHARED main.c diag_bootstrap.c)
add_library(save_export SHARED save_export.c save_export_jni.c diag_bootstrap.c)
target_link_options(save_export PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
# jniLibs is the sole packaging owner for SDL; an IMPORTED SHARED target
# would also be copied by AGP and cause duplicate native-library entries.
target_include_directories(main PRIVATE "${WT_ANDROID_DEPS}/include" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../runtime")
target_link_directories(main PRIVATE "${WT_ANDROID_DEPS}/lib")
target_link_libraries(main PRIVATE SDL3 dl log)
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384" "-Wl,-z,common-page-size=16384")
