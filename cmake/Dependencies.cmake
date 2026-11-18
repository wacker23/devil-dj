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

# vcpkg manifest packages (vcpkg.json). Triplet x64-windows-static: static libraries and static CRT.
find_package(unofficial-sqlite3 CONFIG REQUIRED) # SQLite: public domain
find_package(TagLib CONFIG REQUIRED)             # TagLib: MPL-1.1 (used unmodified); pulls in zlib + utfcpp
find_package(xxHash CONFIG REQUIRED)             # xxHash: BSD-2-Clause
find_package(Catch2 3 CONFIG REQUIRED)           # Catch2 v3: BSL-1.0, tests only
