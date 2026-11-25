cmake_minimum_required(VERSION 3.25)

# Link the MSVC C++ runtime statically (/MT, /MTd) so Devil-DJ.exe runs on machines without the
# Visual C++ Redistributable. Must match the vcpkg triplet (x64-windows-static) and be set before project().
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(DevilDJ VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(DEVILDJ_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (on in CI)" OFF)

# Compiler flags for Devil-DJ's own targets. Link this into every target we write.
add_library(devildj_compile_options INTERFACE)
add_library(devildj::compile_options ALIAS devildj_compile_options)
if(MSVC)
    target_compile_options(devildj_compile_options INTERFACE /W4 /permissive- /utf-8 /MP)
    if(DEVILDJ_WARNINGS_AS_ERRORS)
        target_compile_options(devildj_compile_options INTERFACE /WX)
    endif()
else()
    target_compile_options(devildj_compile_options INTERFACE -Wall -Wextra -Wpedantic)
    if(DEVILDJ_WARNINGS_AS_ERRORS)
        target_compile_options(devildj_compile_options INTERFACE -Werror)
    endif()
endif()

include(cmake/Dependencies.cmake)

add_subdirectory(src/util)
add_subdirectory(src/dsp)
add_subdirectory(src/analysis)
add_subdirectory(src/library)
add_subdirectory(src/matcher)
add_subdirectory(src/engine)
add_subdirectory(src/beatmaker)
add_subdirectory(src/codebeat)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(tools/matcher_eval)

enable_testing()
add_subdirectory(tests)
