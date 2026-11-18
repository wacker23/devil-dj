#include "MainWindow.h"

#include "SettingsKeys.h"

#include <memory>

namespace devildj::app
{

namespace
{
constexpr int kMinWidth = 960;
constexpr int kMinHeight = 600;
} // namespace

MainWindow::MainWindow(const juce::String& name, Settings& settingsStore, std::unique_ptr<juce::Component> content)
    : DocumentWindow(
          name,
          juce::Desktop::getInstance().getDefaultLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId),
          DocumentWindow::allButtons)
    , settings(settingsStore)
{
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    setResizable(true, true);
    setResizeLimits(kMinWidth, kMinHeight, 16384, 16384);

    const auto state = settings.get(keys::mainWindowState);
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
    settings.set(keys::mainWindowState, getWindowStateAsString());
    settings.save();
}

} // namespace devildj::app
