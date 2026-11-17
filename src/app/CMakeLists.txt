juce_add_gui_app(DevilDJ
    PRODUCT_NAME "Devil-DJ"
    COMPANY_NAME "Devil-DJ"
    VERSION ${PROJECT_VERSION})

target_sources(DevilDJ PRIVATE
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
        juce::juce_gui_basics
        TagLib::tag
        devildj::compile_options
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
