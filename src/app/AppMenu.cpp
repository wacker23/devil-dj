#include "AppMenu.h"

#include "util/CrashHandler.h"
#include "util/Log.h"

#include <thread>

namespace devildj::app
{

AppMenu::AppMenu(juce::File logDir, bool debugMenu)
    : logDirectory(std::move(logDir))
    , showDebugMenu(debugMenu)
{
}

juce::StringArray AppMenu::getMenuBarNames()
{
    juce::StringArray names{"Decks", "Mixer", "Options"};
    if (showDebugMenu)
        names.add("Debug");
    return names;
}

juce::PopupMenu AppMenu::getMenuForIndex(int /*topLevelMenuIndex*/, const juce::String& menuName)
{
    juce::PopupMenu menu;
    if (menuName == "Decks")
    {
        menu.addItem(LoadDeckA, "Load track to deck A...");
        menu.addItem(LoadDeckB, "Load track to deck B...");
        menu.addSeparator();
        menu.addItem(UnloadDeckA, "Unload deck A");
        menu.addItem(UnloadDeckB, "Unload deck B");
        menu.addSeparator();
        menu.addItem(KeyLockA, "Key lock deck A (Ctrl+W)");
        menu.addItem(KeyLockB, "Key lock deck B (Ctrl+I)");
        menu.addItem(ResetTempoA, "Reset tempo deck A");
        menu.addItem(ResetTempoB, "Reset tempo deck B");
        juce::PopupMenu ranges;
        ranges.addItem(TempoRange6, "+-6 %", true, tempoRange == 6.0);
        ranges.addItem(TempoRange10, "+-10 %", true, tempoRange == 10.0);
        ranges.addItem(TempoRange16, "+-16 %", true, tempoRange == 16.0);
        ranges.addItem(TempoRange50, "+-50 %", true, tempoRange == 50.0);
        menu.addSubMenu("Tempo range", ranges);
        menu.addSeparator();
        menu.addItem(DebugOverlay, "Show status overlay (F12)");
    }
    else if (menuName == "Mixer")
    {
        menu.addItem(Isolator, "Isolator EQ (both channels)", true, isolator);
        juce::PopupMenu curves;
        curves.addItem(CurveSmooth, "Smooth (constant power)", true, crossfaderCurve == 0);
        curves.addItem(CurveLinear, "Linear", true, crossfaderCurve == 1);
        curves.addItem(CurveCut, "Cut (scratch)", true, crossfaderCurve == 2);
        menu.addSubMenu("Crossfader curve", curves);
        for (int deck = 0; deck < 2; ++deck)
        {
            juce::PopupMenu sides;
            static const char* const names[] = {"A side", "THRU (ignores the crossfader)", "B side"};
            for (int a = 0; a < 3; ++a)
                sides.addItem(AssignFirst + deck * 3 + a, names[a], true, assign[deck] == a);
            menu.addSubMenu(juce::String("Crossfader assign deck ") + (deck == 0 ? "A" : "B"), sides);
        }
        menu.addSeparator();
        menu.addItem(-1, "Crossfader: hold G / H to glide, Shift+G / Shift+H snap, B centre", false);
        menu.addItem(-1, "Kill low / mid / high: Ctrl+A/S/D (A), Ctrl+J/K/L (B)", false);
        menu.addItem(-1, "Filter: hold Shift+3 / Shift+4 (A), Shift+9 / Shift+0 (B)", false);
        menu.addItem(-1, "Filter reset: Shift+5 (A), Shift+- (B)", false);
    }
    else if (menuName == "Options")
    {
        menu.addItem(AudioSettings, "Audio settings...");
        menu.addItem(HighQualityResampling, "High-quality resampling (sinc)", true, highQuality);
    }
    else if (menuName == "Debug")
    {
        menu.addItem(DebugOverlay, "Debug overlay (F12)");
        menu.addItem(TestTone, "Test tone 440 Hz on/off");
        menu.addSeparator();
        menu.addItem(OpenLogFolder, "Open log folder");
        menu.addSeparator();
        menu.addItem(CrashUiThread, "Crash test (UI thread)");
        menu.addItem(CrashWorkerThread, "Crash test (worker thread)");
    }
    return menu;
}

void AppMenu::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/)
{
    switch (menuItemID)
    {
        case AudioSettings:
            if (onAudioSettings)
                onAudioSettings();
            break;

        case DebugOverlay:
            if (onToggleDebugOverlay)
                onToggleDebugOverlay();
            break;

        case LoadDeckA:
        case LoadDeckB:
            if (onLoadTrack)
                onLoadTrack(menuItemID == LoadDeckA ? 0 : 1);
            break;

        case UnloadDeckA:
        case UnloadDeckB:
            if (onUnloadTrack)
                onUnloadTrack(menuItemID == UnloadDeckA ? 0 : 1);
            break;

        case ResetTempoA:
        case ResetTempoB:
            if (onResetTempo)
                onResetTempo(menuItemID == ResetTempoA ? 0 : 1);
            break;

        case TempoRange6:
        case TempoRange10:
        case TempoRange16:
        case TempoRange50:
            tempoRange = menuItemID == TempoRange6    ? 6.0
                         : menuItemID == TempoRange10 ? 10.0
                         : menuItemID == TempoRange16 ? 16.0
                                                      : 50.0;
            if (onTempoRange)
                onTempoRange(tempoRange);
            break;

        case KeyLockA:
        case KeyLockB:
            if (onToggleKeyLock)
                onToggleKeyLock(menuItemID == KeyLockA ? 0 : 1);
            break;

        case CurveSmooth:
        case CurveLinear:
        case CurveCut:
            crossfaderCurve = menuItemID - CurveSmooth;
            if (onCrossfaderCurve)
                onCrossfaderCurve(crossfaderCurve);
            break;

        case Isolator:
            isolator = !isolator;
            if (onIsolator)
                onIsolator(isolator);
            break;

        case HighQualityResampling:
            highQuality = !highQuality;
            if (onHighQualityResampling)
                onHighQualityResampling(highQuality);
            break;

        case TestTone:
            if (onToggleTestTone)
                onToggleTestTone();
            break;

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
            if (menuItemID >= AssignFirst && menuItemID <= AssignLast)
            {
                const int deck = (menuItemID - AssignFirst) / 3;
                assign[deck] = (menuItemID - AssignFirst) % 3;
                if (onCrossfaderAssign)
                    onCrossfaderAssign(deck, assign[deck]);
            }
            break;
    }
}

} // namespace devildj::app
