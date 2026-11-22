# dsp: pure signal processing, no JUCE, no knowledge of decks or tracks (ARCHITECTURE §2). Normal static library.
add_library(devildj_dsp STATIC
    DriftBuffer.cpp
    Limiter.cpp
    fx/BeatFx.cpp
    fx/FxSlot.cpp
    fx/FxUnit.cpp
    Resampler.cpp
    SignalsmithStretcher.cpp
    SincResampler.cpp)
add_library(devildj::dsp ALIAS devildj_dsp)

target_sources(devildj_dsp PUBLIC
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/src"
    FILES
        Biquad.h
        DriftBuffer.h
        fx/BeatFx.h
        fx/Effect.h
        fx/FxSlot.h
        fx/FxUnit.h
        LevelMeter.h
        Limiter.h
        LinearRamp.h
        Resampler.h
        SincResampler.h
        StateVariableFilter.h
        Stretcher.h)

target_include_directories(devildj_dsp PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_compile_features(devildj_dsp PUBLIC cxx_std_20)
target_link_libraries(devildj_dsp
    PUBLIC devildj::util
    PRIVATE devildj::compile_options signalsmith-stretch)
if(MSVC)
    target_compile_options(devildj_dsp PRIVATE /bigobj) # Signalsmith's templates
endif()
