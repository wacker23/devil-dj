#include "DebugMenu.h"

#include "util/CrashHandler.h"
#include "util/Log.h"

#include <thread>

namespace devildj::app
{

DebugMenu::DebugMenu(juce::File logDir)
    : logDirectory(std::move(logDir))
{
}

juce::StringArray DebugMenu::getMenuBarNames()
{
    return {"Debug"};
}

juce::PopupMenu DebugMenu::getMenuForIndex(int /*topLevelMenuIndex*/, const juce::String& /*menuName*/)
{
    juce::PopupMenu menu;
    menu.addItem(OpenLogFolder, "Open log folder");
    menu.addSeparator();
    menu.addItem(CrashUiThread, "Crash test (UI thread)");
    menu.addItem(CrashWorkerThread, "Crash test (worker thread)");
    return menu;
}

void DebugMenu::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/)
{
    switch (menuItemID)
    {
        case CrashUiThread:
            util::logWarning("Crash test requested (UI thread)");
            util::Log::flush();
            util::crashForTesting();

        case CrashWorkerThread:
            util::logWarning("Crash test requested (worker thread)");
            util::Log::flush();
            std::thread([] { util::crashForTesting(); }).detach();
            break;

        case OpenLogFolder:
            logDirectory.startAsProcess();
            break;

        default:
            break;
    }
}

} // namespace devildj::app
