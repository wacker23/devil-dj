# analysis: offline analysis of decoded audio - the analysis service (P2-12), waveforms; beat tracker, key,
# loudness to come. No JUCE (the app hands it a decoder).
add_library(devildj_analysis STATIC
    AnalysisService.cpp
    BeatTracker.cpp
    KeyDetector.cpp
    WaveformBuilder.cpp)
add_library(devildj::analysis ALIAS devildj_analysis)

target_sources(devildj_analysis PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        AnalysisService.h
        BeatTracker.h
        Fft.h
        KeyDetector.h
        WaveformBuilder.h)

target_include_directories(devildj_analysis PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_analysis PUBLIC cxx_std_20)
target_link_libraries(devildj_analysis
    PUBLIC devildj::util devildj::library
    PRIVATE devildj::dsp
    PRIVATE devildj::compile_options)
