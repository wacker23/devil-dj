#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::app
{

// Temporary main-window menu bar until the real UI and ui/Actions.h exist (P1-16):
//   Decks: load / unload a track on deck A or B (until the file browser, P1-18), status overlay
//   Options: Audio settings...
//   Debug (optional): debug overlay, test tone, open log folder, crash tests. Shown in Debug
//   builds, with --debug-menu, or with the showDebugMenu setting.
class AppMenu final : public juce::MenuBarModel
{
public:
    AppMenu(juce::File logDirectory, bool showDebugMenu);

    std::function<void()> onAudioSettings;
    std::function<void()> onToggleDebugOverlay;
    std::function<void()> onToggleTestTone;
    std::function<void(int deck)> onLoadTrack;
    std::function<void(int deck)> onUnloadTrack;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    enum ItemId
    {
        AudioSettings = 1,
        DebugOverlay,
        TestTone,
        LoadDeckA,
        LoadDeckB,
        UnloadDeckA,
        UnloadDeckB,
        CrashUiThread,
        CrashWorkerThread,
        OpenLogFolder,
    };

    juce::File logDirectory;
    bool showDebugMenu;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace devildj::app
