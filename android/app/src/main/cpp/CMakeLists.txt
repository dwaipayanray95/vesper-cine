cmake_minimum_required(VERSION 3.22.1)

project("rcamera_engine" LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan linking will use standard NDK prototypes

# Core native camera and compute engine library
add_library(rcamera_engine SHARED
    native_bridge.cpp
    camera_engine.cpp
    vulkan_compute.cpp
)

target_include_directories(rcamera_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_library(CAMERA2NDK_LIB camera2ndk)
find_library(MEDIANDK_LIB mediandk)
find_library(ANDROID_LIB android)
find_library(VULKAN_LIB vulkan)
find_library(LOG_LIB log)

target_link_libraries(rcamera_engine
    ${CAMERA2NDK_LIB}
    ${MEDIANDK_LIB}
    ${ANDROID_LIB}
    ${VULKAN_LIB}
    ${LOG_LIB}
)
