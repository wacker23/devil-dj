# app_core: app code that tests also use. INTERFACE library = compiled into each executable that links it
# (JUCE modules can only be compiled once per executable; see src/engine/CMakeLists.txt).
add_library(devildj_app_core INTERFACE)
add_library(devildj::app_core ALIAS devildj_app_core)
target_sources(devildj_app_core INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioSettingsWindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/AudioSettingsWindow.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/LibraryBackup.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/HotCueStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadFxPresets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecordingSheets.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecordingSheets.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SequenceStore.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SequenceStore.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PadFxPresets.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/HotCueStore.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/LibraryBackup.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/LoopExport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/LoopExport.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/MemoryCues.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/MemoryCues.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlayTracker.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/PlayTracker.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Settings.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SoakTest.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/SoakTest.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Settings.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/SettingsKeys.h")
target_include_directories(devildj_app_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(devildj_app_core INTERFACE juce::juce_data_structures juce::juce_gui_basics devildj::engine
                                                devildj::library)

juce_add_gui_app(DevilDJ
    PRODUCT_NAME "Devil-DJ"
    COMPANY_NAME "Devil-DJ"
    VERSION ${PROJECT_VERSION})

target_sources(DevilDJ PRIVATE
    AppMenu.cpp
    AppMenu.h
    Main.cpp
    MainComponent.cpp
    MainComponent.h
    MainWindow.cpp
    MainWindow.h)

target_compile_definitions(DevilDJ PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DevilDJ,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DevilDJ,JUCE_VERSION>")

target_link_libraries(DevilDJ
    PRIVATE
        devildj::app_core
        devildj::engine
        devildj::ui
        juce::juce_gui_basics
        juce::juce_audio_utils
        TagLib::tag
        devildj::util
        devildj::compile_options
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
