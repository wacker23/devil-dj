# beatmaker: the Beat Maker groovebox (BEATMAKER_SPEC) - its model and file format (P4-01); the sequencer, voices
# and synths come in later tasks. The sequencer (P4-02) makes events for the engine's shared Scheduler.
#
# An INTERFACE library like engine: it uses juce_core (JSON, files), and JUCE modules add their sources to every
# target that links them, so its .cpp files are compiled into each executable that links it.
add_library(devildj_beatmaker INTERFACE)
add_library(devildj::beatmaker ALIAS devildj_beatmaker)

target_sources(devildj_beatmaker INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/Model.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Model.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ProjectFile.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/ProjectFile.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sequencer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/Sequencer.h")

target_include_directories(devildj_beatmaker INTERFACE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(devildj_beatmaker INTERFACE
    juce::juce_core
    devildj::engine
    devildj::util)
