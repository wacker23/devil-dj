# ui: JUCE components (ARCHITECTURE §9). INTERFACE library like engine: compiled into each executable.
add_library(devildj_ui INTERFACE)
add_library(devildj::ui ALIAS devildj_ui)

target_sources(devildj_ui INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Actions.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Actions.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AutomixPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AutomixPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StemStrip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StemStrip.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/FxPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/FxPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeChannelStrip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeEditorPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeHelp.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeHelp.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeEditorPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeTokeniser.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeTokeniser.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/CodeChannelStrip.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/BeatChannelStrip.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/BeatChannelStrip.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/BeatMakerPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/BeatMakerPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PianoRoll.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PianoRoll.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/TransitionEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/TransitionEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/StepGrid.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/StepGrid.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/NextTrackPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/NextTrackPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadModes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecorderPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecorderPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SamplerPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SamplerPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadModes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/DeckView.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/FileBrowserPanel.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/FileBrowserPanel.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeyboardController.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeyboardController.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeymapEditor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/KeymapEditor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Keymaps.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Keymaps.h"
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
    juce::juce_gui_extra
    devildj_codebeat_help
    devildj::engine
    devildj::beatmaker
    devildj::library)
