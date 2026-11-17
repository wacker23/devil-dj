#include "MainWindow.h"

#include "MainComponent.h"

#include <memory>

namespace devildj::app
{

namespace
{
constexpr const char* kWindowStateKey = "mainWindowState";
constexpr int kMinWidth = 960;
constexpr int kMinHeight = 600;
} // namespace

MainWindow::MainWindow(const juce::String& name, juce::PropertiesFile& settingsFile)
    : DocumentWindow(
          name,
          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
          DocumentWindow::allButtons)
    , settings(settingsFile)
{
    setUsingNativeTitleBar(true);
    setContentOwned(std::make_unique<MainComponent>().release(), true);
    setResizable(true, true);
    setResizeLimits(kMinWidth, kMinHeight, 16384, 16384);

    const auto state = settings.getValue(kWindowStateKey);
    if (state.isEmpty() || !restoreWindowStateFromString(state))
        centreWithSize(getWidth(), getHeight());

    setVisible(true);
}

MainWindow::~MainWindow()
{
    saveWindowState();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

void MainWindow::saveWindowState()
{
    settings.setValue(kWindowStateKey, getWindowStateAsString());
    settings.saveIfNeeded();
}

} // namespace devildj::app
