# dsp: pure signal processing, no JUCE, no knowledge of decks or tracks (ARCHITECTURE §2). Normal static library.
add_library(devildj_dsp STATIC
    Resampler.cpp
    SincResampler.cpp)
add_library(devildj::dsp ALIAS devildj_dsp)

target_sources(devildj_dsp PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        Resampler.h
        SincResampler.h)

target_include_directories(devildj_dsp PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_dsp PUBLIC cxx_std_20)
target_link_libraries(devildj_dsp
    PUBLIC devildj::util
    PRIVATE devildj::compile_options)
