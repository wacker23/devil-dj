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
    util/UtilSmokeTests.cpp)

target_compile_definitions(DevilDJTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(DevilDJTests PRIVATE
    Catch2::Catch2WithMain
    juce::juce_core
    unofficial::sqlite3::sqlite3
    xxHash::xxhash
    devildj::compile_options
    juce::juce_recommended_config_flags)

catch_discover_tests(DevilDJTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
