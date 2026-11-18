# ui: JUCE components (ARCHITECTURE §9). INTERFACE library like engine: compiled into each executable.
add_library(devildj_ui INTERFACE)
add_library(devildj::ui ALIAS devildj_ui)

target_sources(devildj_ui INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerControls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerControls.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformColours.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.h")

target_include_directories(devildj_ui INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_ui INTERFACE
    juce::juce_gui_basics
    devildj::engine)
