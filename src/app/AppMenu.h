#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::app
{

// Temporary main-window menu bar until the real UI and ui/Actions.h exist (P1-16):
//   Decks: load / unload a track on deck A or B (until the file browser, P1-18), status overlay
//   Mixer: EQ / isolator mode, crossfader curve and assignment, headphone mix and level (until P1-15)
//   Options: Audio settings...
//   Debug (optional): debug overlay, test tone, open log folder, crash tests. Shown in Debug
//   builds, with --debug-menu, or with the showDebugMenu setting.
class AppMenu final : public juce::MenuBarModel
{
public:
    AppMenu(juce::File logDirectory, bool showDebugMenu);

    std::function<void()> onAudioSettings;
    std::function<void()> onImportFolder;   // library (P2-04)
    std::function<void()> onImportPlaylist; // M3U / M3U8 (P2-06)
    std::function<void()> onImportLibrary;  // other software's library XML (P2-09)
    std::function<void()> onCheckMissing;   // P2-10
    std::function<void()> onRelocate;       // P2-10
    std::function<void()> onBackupNow;      // P2-11
    std::function<void()> onRestoreBackup;  // P2-11
    std::function<void()> onOpenBackups;    // P2-11
    std::function<void(int notation)> onKeyNotation; // 0 musical, 1 Camelot (P2-15)
    void setKeyNotation(int notation) noexcept { keyNotation = notation; }
    std::function<void(bool on, int targetLufs)> onAutoGain; // P2-16
    void setAutoGain(bool on, int targetLufs) noexcept
    {
        autoGain = on;
        autoGainTarget = targetLufs;
    }
    static constexpr int kAutoGainTargets[] = {-8, -10, -12, -14};
    std::function<void(bool on, int eighths)> onQuantize; // P2-20
    void setQuantize(bool on, int eighths) noexcept
    {
        quantize = on;
        quantizeEighths = eighths;
    }
    static constexpr int kQuantizeEighths[] = {1, 2, 4, 8};
    std::function<void()> onToggleDebugOverlay;
    std::function<void()> onToggleTestTone;
    std::function<void(int deck)> onLoadTrack;
    std::function<void(int deck)> onUnloadTrack;
    std::function<void(int deck)> onResetTempo;
    std::function<void(double rangePercent)> onTempoRange;
    std::function<void(bool on)> onHighQualityResampling;
    std::function<void()> onReloadKeymap;
    std::function<void()> onOpenKeymapFolder;
    std::function<void(double minutes)> onSoakTest; // 0 = stop
    std::function<void(int deck)> onToggleKeyLock;
    std::function<void(bool on)> onIsolator;
    std::function<void(int curve)> onCrossfaderCurve;             // 0 smooth, 1 linear, 2 cut
    std::function<void(int deck, int assign)> onCrossfaderAssign; // 0 A, 1 THRU, 2 B
    std::function<void(double mix)> onCueMix;                     // 0 cue .. 1 master
    std::function<void(double db)> onHeadphoneLevel;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    enum ItemId
    {
        AudioSettings = 1,
        ImportFolder,
        ImportPlaylist,
        ImportLibrary,
        CheckMissing,
        Relocate,
        BackupNow,
        RestoreBackup,
        OpenBackups,
        KeyMusical,
        KeyCamelot,
        AutoGainToggle,
        AutoGainTargetFirst, // one per kAutoGainTargets
        AutoGainTargetLast = AutoGainTargetFirst + 3,
        QuantizeToggle,
        QuantizeFirst, // one per kQuantizeEighths
        QuantizeLast = QuantizeFirst + 3,
        DebugOverlay,
        TestTone,
        LoadDeckA,
        LoadDeckB,
        UnloadDeckA,
        UnloadDeckB,
        ResetTempoA,
        ResetTempoB,
        TempoRange6,
        TempoRange10,
        TempoRange16,
        TempoRange50,
        HighQualityResampling,
        ReloadKeymap,
        OpenKeymapFolder,
        Soak60,
        Soak5,
        SoakStop,
        KeyLockA,
        KeyLockB,
        CrashUiThread,
        CrashWorkerThread,
        OpenLogFolder,
        Isolator,
        CurveSmooth,
        CurveLinear,
        CurveCut,
        AssignFirst, // deck * 3 + assign (decks A and B)
        AssignLast = AssignFirst + 5,
        CueMixFirst, // 5 positions: 0, 0.25, 0.5, 0.75, 1
        CueMixLast = CueMixFirst + 4,
        LevelFirst, // 0, -6, -12, -18 dB
        LevelLast = LevelFirst + 3,
    };

    juce::File logDirectory;
    bool showDebugMenu;
    double tempoRange = 10.0;
    bool highQuality = false;
    int keyNotation = 0;
    bool autoGain = true;
    int autoGainTarget = -10;
    bool quantize = false;
    int quantizeEighths = 8;
    bool isolator = false;
    int crossfaderCurve = 0;
    int assign[2] = {0, 2};
    int cueMixIndex = 2;
    int levelIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace devildj::app
