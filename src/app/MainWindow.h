#pragma once

#include "Settings.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace devildj::app
{

// Resizable top-level window. Restores its size and position from the settings file and saves them on close.
class MainWindow final : public juce::DocumentWindow
{
public:
    MainWindow(const juce::String& name, Settings& settings, std::unique_ptr<juce::Component> content);
    ~MainWindow() override;

    void closeButtonPressed() override;

private:
    void saveWindowState();

    Settings& settings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace devildj::app
