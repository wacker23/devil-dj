# One Catch2 test executable; sources mirror the src/ module folders.
# Catch2WithMain supplies main(). Tests are registered with ctest per TEST_CASE.

include(Catch)

juce_add_console_app(DevilDJTests
    PRODUCT_NAME "DevilDJTests")

target_sources(DevilDJTests PRIVATE
    analysis/AnalysisSmokeTests.cpp
    app/AppSmokeTests.cpp
    beatmaker/BeatMakerSmokeTests.cpp
    codebeat/CodeBeatSmokeTests.cpp
    dsp/DspSmokeTests.cpp
    engine/EngineSmokeTests.cpp
    library/LibrarySmokeTests.cpp
    matcher/MatcherSmokeTests.cpp
    ui/UiSmokeTests.cpp
    util/GarbageQueueTests.cpp
    util/SpscQueueTests.cpp
    util/TripleBufferTests.cpp
    util/UtilSmokeTests.cpp)

target_compile_definitions(DevilDJTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(DevilDJTests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_core
    unofficial::sqlite3::sqlite3
    xxHash::xxhash
    devildj::util
    devildj::compile_options
    juce::juce_recommended_config_flags)

catch_discover_tests(DevilDJTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)

# Python tool tests (P0-07): the licence check itself, and the checker's own unit tests.
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)

set(devildj_license_args --repo "${PROJECT_SOURCE_DIR}")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND devildj_license_args --vcpkg-installed "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

add_test(NAME tools.check_licenses
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_licenses.py" ${devildj_license_args})

add_test(NAME tools.check_licenses.unittests
    COMMAND Python3::Interpreter -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -v)
