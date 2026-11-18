# engine: decks, mixer, audio device, clock (see ARCHITECTURE §3). Must not use JUCE GUI modules.
#
# Built as an INTERFACE library: its .cpp files are compiled into each executable that links it (the app
# and the tests). JUCE modules add their own sources to every target that links them, so a normal static
# library that uses JUCE would end up with a second copy of JUCE in the final executable.
add_library(devildj_engine INTERFACE)
add_library(devildj::engine ALIAS devildj_engine)

target_sources(devildj_engine INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioDeviceService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioDeviceService.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioEngine.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioEngine.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/BufferRegistry.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineCommand.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineIds.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/EngineState.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Mp3Gapless.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Mp3Gapless.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/OutputRouter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/OutputRouter.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackBuffer.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackDecoder.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackDecoder.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackLoader.h")

target_include_directories(devildj_engine INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_engine INTERFACE
    juce::juce_audio_devices
    juce::juce_audio_formats
    devildj::dsp
    xxHash::xxhash
    devildj::util
    # Windows Media Foundation (MP3/AAC/ALAC decoding): part of Windows.
    $<$<PLATFORM_ID:Windows>:mfplat mfreadwrite mfuuid propsys>)
