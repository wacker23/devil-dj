# ui: JUCE components (ARCHITECTURE §9). INTERFACE library like engine: compiled into each executable.
add_library(devildj_ui INTERFACE)
add_library(devildj::ui ALIAS devildj_ui)

target_sources(devildj_ui INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/DebugOverlay.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/JogPad.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/JogPad.h")

target_include_directories(devildj_ui INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_ui INTERFACE
    juce::juce_gui_basics
    devildj::engine)
