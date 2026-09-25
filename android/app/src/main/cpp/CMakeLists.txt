cmake_minimum_required(VERSION 3.22.1)

project("vesper_engine" LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vesper_engine SHARED
    native_bridge.cpp
    camera_engine.cpp
    color_science.cpp
    vulkan_engine.cpp
    recorder.cpp
)

target_include_directories(vesper_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(vesper_engine PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)

target_link_libraries(vesper_engine
    camera2ndk
    mediandk
    aaudio
    android
    vulkan
    log
)
