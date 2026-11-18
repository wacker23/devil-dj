# analysis: offline analysis of decoded audio (waveforms now; beat tracker, key, loudness later). No JUCE.
add_library(devildj_analysis STATIC
    WaveformOverview.cpp)
add_library(devildj::analysis ALIAS devildj_analysis)

target_sources(devildj_analysis PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        WaveformOverview.h)

target_include_directories(devildj_analysis PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_analysis PUBLIC cxx_std_20)
target_link_libraries(devildj_analysis
    PUBLIC devildj::util
    PRIVATE devildj::compile_options)
