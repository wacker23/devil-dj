# One Catch2 test executable; sources mirror the src/ module folders.
# Catch2WithMain supplies main(). Tests are registered with ctest per TEST_CASE.

include(Catch)

juce_add_console_app(DevilDJTests
    PRODUCT_NAME "DevilDJTests")

target_sources(DevilDJTests PRIVATE
    analysis/AnalysisServiceTests.cpp
    analysis/AnalysisSmokeTests.cpp
    analysis/BeatTrackerTests.cpp
    analysis/KeyDetectorTests.cpp
    analysis/LoudnessTests.cpp
    analysis/TestSignalTests.cpp
    analysis/WaveformBuilderTests.cpp
    analysis/WaveformCacheTests.cpp
    app/AppSmokeTests.cpp
    app/AudioSettingsTests.cpp
    app/LibraryBackupTests.cpp
    app/SettingsTests.cpp
    app/SoakTestTests.cpp
    beatmaker/BeatMakerSmokeTests.cpp
    codebeat/CodeBeatSmokeTests.cpp
    dsp/DspSmokeTests.cpp
    dsp/ResamplerTests.cpp
    dsp/SincResamplerTests.cpp
    engine/AudioDeviceServiceTests.cpp
    engine/AudioEngineDeckTests.cpp
    engine/AudioEngineRoutingTests.cpp
    engine/AudioEngineTests.cpp
    engine/BeatJumpTests.cpp
    engine/DeckKeyLockTests.cpp
    engine/DeckScratchTests.cpp
    engine/ChannelStripTests.cpp
    engine/MetronomeTests.cpp
    engine/MixerMasterTests.cpp
    engine/HeadphoneCueTests.cpp
    engine/MasterClockTests.cpp
    engine/DeckTempoTests.cpp
    engine/DeckTests.cpp
    engine/EngineSmokeTests.cpp
    engine/Mp3GaplessTests.cpp
    engine/OutputRouterTests.cpp
    engine/QuantizeTests.cpp
    engine/SyncTests.cpp
    engine/TrackDecoderTests.cpp
    engine/TrackLoaderTests.cpp
    library/DatabaseTests.cpp
    library/ForeignLibraryTests.cpp
    library/ImporterTests.cpp
    library/LibrarySmokeTests.cpp
    library/MissingFilesTests.cpp
    library/PlaylistFilesTests.cpp
    library/SmartRulesTests.cpp
    library/RepositoryTests.cpp
    library/TagReaderTests.cpp
    matcher/MatcherSmokeTests.cpp
    ui/UiSmokeTests.cpp
    ui/DeckViewTests.cpp
    ui/FileBrowserTests.cpp
    ui/KeyboardControllerTests.cpp
    ui/KeymapOverlayTests.cpp
    ui/LibraryBrowserTests.cpp
    ui/MixerViewTests.cpp
    ui/PlaylistBrowserTests.cpp
    ui/SearchFilterTests.cpp
    ui/SmartPlaylistTests.cpp
    ui/TagsRatingsTests.cpp
    ui/WaveformViewTests.cpp
    util/BeatgridTests.cpp
    util/CrashHandlerTests.cpp
    util/FractionTests.cpp
    util/GarbageQueueTests.cpp
    util/LogTests.cpp
    util/RealtimeGuardTests.cpp
    util/ResultTests.cpp
    util/SpscQueueTests.cpp
    util/ThreadPriorityTests.cpp
    util/TripleBufferTests.cpp
    util/UtilSmokeTests.cpp)

target_include_directories(DevilDJTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set(DEVILDJ_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data/generated")

target_compile_definitions(DevilDJTests PRIVATE
    DEVILDJ_TEST_DATA_DIR="${DEVILDJ_TEST_DATA_DIR}"
    DEVILDJ_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)

target_link_libraries(DevilDJTests PRIVATE
    Catch2::Catch2WithMain
    devildj::app_core
    devildj::engine
    devildj::ui
    devildj::analysis
    devildj::library
    TagLib::tag
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_formats
    unofficial::sqlite3::sqlite3
    xxHash::xxhash
    devildj::util
    devildj::dsp
    devildj::compile_options
    juce::juce_recommended_config_flags)

catch_discover_tests(DevilDJTests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120 FIXTURES_REQUIRED test_signals)

# Python tool tests (P0-07): the licence check itself, and the checker's own unit tests.
find_package(Python3 3.11 COMPONENTS Interpreter REQUIRED)

# P0-13: generate the synthetic test signals before any C++ test runs (skipped when up to date).
# Needs numpy + soundfile: python -m pip install numpy soundfile
add_test(NAME tools.gen_test_signals
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/gen_test_signals.py" --out "${DEVILDJ_TEST_DATA_DIR}")
set_tests_properties(tools.gen_test_signals PROPERTIES FIXTURES_SETUP test_signals TIMEOUT 300)

set(devildj_license_args --repo "${PROJECT_SOURCE_DIR}")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND devildj_license_args --vcpkg-installed "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

add_test(NAME tools.check_licenses
    COMMAND Python3::Interpreter "${PROJECT_SOURCE_DIR}/tools/check_licenses.py" ${devildj_license_args})

add_test(NAME tools.check_licenses.unittests
    COMMAND Python3::Interpreter -m unittest discover -s "${PROJECT_SOURCE_DIR}/tools/tests" -v)
