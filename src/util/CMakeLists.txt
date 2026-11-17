# util: lock-free primitives and helpers shared by every module.
add_library(devildj_util STATIC
    RealtimeGuard.cpp)
add_library(devildj::util ALIAS devildj_util)

target_sources(devildj_util PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        GarbageQueue.h
        RealtimeGuard.h
        SpscQueue.h
        TripleBuffer.h)

target_include_directories(devildj_util PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_util PUBLIC cxx_std_20)

# RealtimeGuard replaces global operator new/delete in Debug only; Release compiles it out.
# The library is linked before the C++ runtime, so its operator new wins for every executable that links util.
target_compile_definitions(devildj_util PUBLIC $<$<CONFIG:Debug>:DEVILDJ_REALTIME_GUARD=1>)
target_link_libraries(devildj_util
    PUBLIC $<$<AND:$<CONFIG:Debug>,$<PLATFORM_ID:Windows>>:dbghelp>
    PRIVATE devildj::compile_options)
