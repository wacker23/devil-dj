#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace devildj::app
{

// Temporary "Debug" menu bar for developer tools (crash tests, log folder). Shown in Debug builds, or in
// Release when started with --debug-menu. These become CommandIds once ui/Actions.h exists (P1-16).
class DebugMenu final : public juce::MenuBarModel
{
public:
    explicit DebugMenu(juce::File logDirectory);

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    enum ItemId
    {
        CrashUiThread = 1,
        CrashWorkerThread,
        OpenLogFolder,
    };

    juce::File logDirectory;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DebugMenu)
};

} // namespace devildj::app
