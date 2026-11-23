#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <cmath>
#include <algorithm>

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
    // The mixer's saved state, for the ticks in the Mixer menu.
    void setMixerState(double cueMix, double headphoneDb, int curve, int assignA, int assignB) noexcept
    {
        cueMixIndex = std::clamp(static_cast<int>(std::lround(cueMix * 4.0)), 0, 4);
        levelIndex = std::clamp(static_cast<int>(std::lround(-headphoneDb / 6.0)), 0, 3);
        crossfaderCurve = std::clamp(curve, 0, 2);
        assign[0] = std::clamp(assignA, 0, 2);
        assign[1] = std::clamp(assignB, 0, 2);
    }
    // P3-05: how PAUSE stops (0 instantly, 1 brake, 2 spin-back) and the stop times.
    std::function<void(int style, double brakeSeconds, double spinBackSeconds)> onStopStyle;
    void setStopStyle(int style, double brake, double spin) noexcept
    {
        pauseStyle = style;
        brakeSeconds = brake;
        spinBackSeconds = spin;
    }
    static constexpr double kStopTimes[] = {0.25, 0.5, 1.0, 2.0, 4.0};
    // P3-17: the auto-transition's length (8 / 16 / 32 beats) and style (0 blend, 1 filter sweep, 2 echo out).
    std::function<void(int beats, int style)> onTransition;
    void setTransition(int beats, int style) noexcept
    {
        transitionBeats = beats;
        transitionStyle = style;
    }
    std::function<void(bool on)> onKeySyncCompatible; // P2-22
    void setKeySyncCompatible(bool on) noexcept { keySyncCompatible = on; }
    std::function<void()> onToggleDebugOverlay;
    std::function<void()> onToggleTestTone;
    std::function<void(int deck)> onLoadTrack;
    std::function<void(int deck)> onUnloadTrack;
    std::function<void(int deck)> onResetTempo;
    std::function<void(double rangePercent)> onTempoRange;
    std::function<void(bool on)> onHighQualityResampling;
    // P3-21: touch mode - everything drawn larger (bigger targets for fingers).
    std::function<void(bool on)> onTouchMode;
    // P3-23: the layout - waveforms 0 horizontal / 1 vertical, the browser enlarged (Ctrl+B).
    std::function<void(int waveforms, bool bigBrowser)> onLayout;
    std::function<void(bool show)> onNextTrackPanel; // P3-24
    // P4-20: Spotify - 0 the Client ID, 1 sign in, 2 sign out, 3 import playlists (P4-21).
    std::function<void(int op)> onSpotify;
    void setSpotifyState(bool hasClientId, bool signedIn) noexcept
    {
        spotifyHasClientId = hasClientId;
        spotifySignedIn = signedIn;
    }
    void setNextTrackPanel(bool show) noexcept { layoutNextTrack = show; }
    void setLayout(int waveforms, bool bigBrowser) noexcept
    {
        layoutWaves = waveforms;
        layoutBigBrowser = bigBrowser;
    }
    void setTouchMode(bool on) noexcept { touchMode = on; }
    std::function<void()> onReloadKeymap;
    std::function<void()> onEditKeymap; // P3-20
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
        KeySyncCompatible,
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
        TouchMode,
        LayoutHorizontal,
        LayoutVertical,
        LayoutBigBrowser,
        LayoutNextTrack,
        SpotifyClientId,
        SpotifySignIn,
        SpotifySignOut,
        SpotifyImport,
        ReloadKeymap,
        EditKeymap,
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
        PauseStyleFirst, // instantly, brake, spin-back
        PauseStyleLast = PauseStyleFirst + 2,
        BrakeTimeFirst, // one per kStopTimes
        BrakeTimeLast = BrakeTimeFirst + 4,
        SpinTimeFirst,
        SpinTimeLast = SpinTimeFirst + 4,
        TransitionBeatsFirst, // 8, 16, 32
        TransitionBeatsLast = TransitionBeatsFirst + 2,
        TransitionStyleFirst, // blend, filter sweep, echo out
        TransitionStyleLast = TransitionStyleFirst + 2,
    };

    juce::File logDirectory;
    bool showDebugMenu;
    double tempoRange = 10.0;
    bool highQuality = false;
    bool touchMode = false;
    int layoutWaves = 0;
    bool layoutBigBrowser = false;
    bool layoutNextTrack = true;
    bool spotifyHasClientId = false, spotifySignedIn = false;
    int keyNotation = 0;
    bool autoGain = true;
    int autoGainTarget = -10;
    bool quantize = false;
    int quantizeEighths = 8;
    bool keySyncCompatible = false;
    int pauseStyle = 0;
    double brakeSeconds = 1.0, spinBackSeconds = 1.0;
    int transitionBeats = 16, transitionStyle = 0;
    bool isolator = false;
    int crossfaderCurve = 0;
    int assign[2] = {0, 2};
    int cueMixIndex = 2;
    int levelIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AppMenu)
};

} // namespace devildj::app
