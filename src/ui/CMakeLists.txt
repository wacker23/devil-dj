# ui: JUCE components (ARCHITECTURE §9). INTERFACE library like engine: compiled into each executable.
add_library(devildj_ui INTERFACE)
add_library(devildj::ui ALIAS devildj_ui)

target_sources(devildj_ui INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Actions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Actions.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/FxPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/FxPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadModes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SamplerPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SamplerPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadModes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/FileBrowserPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/FileBrowserPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeyboardController.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeyboardController.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeymapOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeymapOverlay.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/LibraryBrowser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/LibraryBrowser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerControls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerControls.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MemoryCuePanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MemoryCuePanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MixerView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SmartPlaylistEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SmartPlaylistEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackColours.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackDropTarget.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackTableModel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/TrackTableModel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformColours.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/WaveformView.h")

target_include_directories(devildj_ui INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_ui INTERFACE
    juce::juce_gui_basics
    devildj::engine
    devildj::library)
