# Third-party dependencies fetched at configure time.
# Every entry here must also be listed in docs/LICENSES.md (checked by tools/check_licenses.py).

include(FetchContent)

# JUCE 8: app framework, audio I/O, UI. Licence: JUCE Starter EULA (docs/LICENSES.md, decision D6).
set(DEVILDJ_JUCE_TAG 8.0.15)
FetchContent_Declare(
    juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ${DEVILDJ_JUCE_TAG}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE)
FetchContent_MakeAvailable(juce)

# Signalsmith Stretch + Signalsmith Linear (its FFT/STFT helper): key lock / transpose (P1-08). Both MIT,
# header-only. Linear is declared here first so Stretch's own nested FetchContent of it is skipped and the
# licence checker sees it. SYSTEM: their headers don't count towards our /W4 /WX.
FetchContent_Declare(
    signalsmith-linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG 0.6.6
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_Declare(
    signalsmith-stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.4.0
    GIT_SHALLOW TRUE
    SYSTEM)
FetchContent_MakeAvailable(signalsmith-linear signalsmith-stretch)

# quickjs-ng: the JavaScript engine for Code Beat (P5-07). MIT. Only its `qjs` library is used: no quickjs-libc
# (no std / os modules, no file or network access), none of its executables (EXCLUDE_FROM_ALL).
# Its CMakeLists asks for CMake 3.10, which would leave the MSVC runtime choice (CMP0091) at the old default (/MD)
# and clash with our static runtime; the policy default makes it honour CMAKE_MSVC_RUNTIME_LIBRARY.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    quickjs-ng
    GIT_REPOSITORY https://github.com/quickjs-ng/quickjs.git
    GIT_TAG v0.18.0
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(quickjs-ng)
if(MSVC)
    # Its own warnings are its upstream's business: drop the -Wall (= /Wall) it sets and silence the rest.
    get_target_property(devildj_qjs_options qjs COMPILE_OPTIONS)
    list(REMOVE_ITEM devildj_qjs_options -Wall)
    list(APPEND devildj_qjs_options /w)
    set_target_properties(qjs PROPERTIES COMPILE_OPTIONS "${devildj_qjs_options}")
endif()
unset(CMAKE_POLICY_DEFAULT_CMP0091)

# ONNX Runtime with DirectML (P6-01; owner approved 2026-11-24): prebuilt from NuGet, hashes pinned. ONNX Runtime is
# MIT; DirectML.dll is under Microsoft's DirectML licence (redistributable with an app, Windows only). Imported as
# `devildj::onnxruntime`; devildj_copy_inference_dlls(<target>) puts the three DLLs next to an executable.
FetchContent_Declare(
    onnxruntime-directml
    URL https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime.DirectML/1.24.4
    URL_HASH SHA256=57E9F11B73437BEF7A309496135D4C1F96B1A8E9DDBA60013FA27BFC1D788681
    DOWNLOAD_NAME onnxruntime-directml-1.24.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(
    directml
    URL https://www.nuget.org/api/v2/package/Microsoft.AI.DirectML/1.15.4
    URL_HASH SHA256=4E7CB7DDCE8CF837A7A75DC029209B520CA0101470FCDF275C1F49736A3615B9
    DOWNLOAD_NAME directml-1.15.4.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(onnxruntime-directml directml) # no CMake in them: just unpacked
set(DEVILDJ_ORT_DIR "${onnxruntime-directml_SOURCE_DIR}")
set(DEVILDJ_DML_DIR "${directml_SOURCE_DIR}")
add_library(devildj_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(devildj_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${DEVILDJ_ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll"
    IMPORTED_IMPLIB "${DEVILDJ_ORT_DIR}/runtimes/win-x64/native/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${DEVILDJ_ORT_DIR}/build/native/include")
add_library(devildj::onnxruntime ALIAS devildj_onnxruntime)
function(devildj_copy_inference_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DEVILDJ_ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll"
            "${DEVILDJ_ORT_DIR}/runtimes/win-x64/native/onnxruntime_providers_shared.dll"
            "${DEVILDJ_DML_DIR}/bin/x64-win/DirectML.dll"
            "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()

# vcpkg manifest packages (vcpkg.json). Triplet x64-windows-static: static libraries and static CRT.
find_package(unofficial-sqlite3 CONFIG REQUIRED) # SQLite: public domain
find_package(TagLib CONFIG REQUIRED)             # TagLib: MPL-1.1 (used unmodified); pulls in zlib + utfcpp
find_package(xxHash CONFIG REQUIRED)             # xxHash: BSD-2-Clause
find_package(Catch2 3 CONFIG REQUIRED)           # Catch2 v3: BSL-1.0, tests only
