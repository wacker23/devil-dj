cmake_minimum_required(VERSION 3.25)

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

add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)
