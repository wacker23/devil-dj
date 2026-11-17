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

# vcpkg manifest packages (vcpkg.json). Triplet x64-windows-static: static libraries and static CRT.
find_package(unofficial-sqlite3 CONFIG REQUIRED) # SQLite: public domain
find_package(TagLib CONFIG REQUIRED)             # TagLib: MPL-1.1 (used unmodified); pulls in zlib + utfcpp
find_package(xxHash CONFIG REQUIRED)             # xxHash: BSD-2-Clause
find_package(Catch2 3 CONFIG REQUIRED)           # Catch2 v3: BSL-1.0, tests only
