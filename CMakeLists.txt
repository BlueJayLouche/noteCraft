cmake_minimum_required(VERSION 3.28)

# Read by project(), so set before it: macOS 11 and up, Apple Silicon and Intel in one binary.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")

project(NoteCraft VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX) # clap-wrapper's macOS sources
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
set(CLAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.10
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(clap)

# clap-wrapper downloads the VST3 SDK (MIT since 3.8) itself.
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
FetchContent_Declare(clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG v0.18.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(clap-wrapper)

add_library(notecraft-impl STATIC src/plugin.cpp)
target_link_libraries(notecraft-impl PUBLIC clap)
set_target_properties(notecraft-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

make_clapfirst_plugins(
    TARGET_NAME notecraft
    IMPL_TARGET notecraft-impl
    OUTPUT_NAME "NoteCraft"
    ENTRY_SOURCE src/entry.cpp
    BUNDLE_IDENTIFIER "com.notecraft.notecraft"
    BUNDLE_VERSION ${PROJECT_VERSION}
    COPY_AFTER_BUILD FALSE
    PLUGIN_FORMATS CLAP VST3
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The C++ runtime inside the plug-in, so it loads beside a host built with another one.
    foreach(format clap vst3)
        target_link_options(notecraft_${format} PRIVATE -static-libstdc++ -static-libgcc)
    endforeach()
endif()

enable_testing()
foreach(name effects timing transforms generators pairing)
    add_executable(notecraft-${name}-tests tests/${name}_tests.cpp)
    target_link_libraries(notecraft-${name}-tests PRIVATE clap)
    target_include_directories(notecraft-${name}-tests PRIVATE src)
    add_test(NAME notecraft-${name}-tests COMMAND notecraft-${name}-tests)
endforeach()
