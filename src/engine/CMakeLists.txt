# engine: decks, mixer, audio device, clock (see ARCHITECTURE §3). Must not use JUCE GUI modules.
#
# Built as an INTERFACE library: its .cpp files are compiled into each executable that links it (the app
# and the tests). JUCE modules add their own sources to every target that links them, so a normal static
# library that uses JUCE would end up with a second copy of JUCE in the final executable.
add_library(devildj_engine INTERFACE)
add_library(devildj::engine ALIAS devildj_engine)

target_sources(devildj_engine INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioDeviceService.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioDeviceService.h")

target_include_directories(devildj_engine INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_engine INTERFACE
    juce::juce_audio_devices
    devildj::util)
