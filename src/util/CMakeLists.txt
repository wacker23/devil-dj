# util: lock-free primitives and helpers shared by every module. Header-only for now.
add_library(devildj_util INTERFACE)
add_library(devildj::util ALIAS devildj_util)

target_include_directories(devildj_util INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_util INTERFACE cxx_std_20)

target_sources(devildj_util INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        GarbageQueue.h
        SpscQueue.h
        TripleBuffer.h)
