#pragma once

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/TrackDropTarget.h"
#include "util/MusicalKey.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace devildj::ui
{

// One deck (P1-13): title / artist, elapsed / remaining time, BPM with tempo %, key lock, CUE (hold), PLAY,
// SYNC (until beat grids: P2-19), tempo slider, and the whole-track overview with playhead, cue marker and
// click / drag to seek. Reads the engine state at display rate (VBlankAttachment); sends engine commands.
// Every control is a plain JUCE component, so mouse and touch both work.
class DeckView final : public juce::Component, public TrackDropTarget
{
public:
    using TrackLookup = std::function<std::shared_ptr<const engine::TrackBuffer>(engine::BufferId)>;

    DeckView(engine::AudioEngine& engine, engine::DeckId deck, TrackLookup lookup);
    ~DeckView() override;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

    // Pulls the latest engine state into the controls (called every frame; public for tests).
    void refresh();

    // Overview: track position (samples) for an x position inside `area` (clamped to the track).
    static double sampleForX(float x, juce::Rectangle<float> area, int lengthSamples) noexcept;
    // Text helpers (public for tests).
    static juce::String formatTime(double seconds, bool remaining);
    static juce::String formatBpm(double tagBpm, double tempoPercent);
    static juce::String displayTitle(const engine::TrackBuffer& track);

    // Children, exposed for tests.
    class HoldButton;
    class Overview;
    HoldButton& cueButton() noexcept { return *cue; }
    juce::TextButton& playButton() noexcept { return play; }
    juce::TextButton& syncButton() noexcept { return sync; }
    juce::TextButton& keyLockButton() noexcept { return keyLock; }
    juce::TextButton& gridButton() noexcept { return grid; }
    juce::TextButton& tapButton() noexcept { return tap; }
    std::function<void()> onGridMenu; // GRID: the beatgrid tools (P2-14)
    // A track being loaded (P2-17): its title and cached waveform in the overview until the deck has it.
    void showPreview(std::shared_ptr<const analysis::Waveform> waveform, const juce::String& title);
    void clearPreview();
    bool hasPreview() const noexcept { return preview != nullptr; }

    // The loaded track's key (the library's, 0..23; -1 = none) in the user's notation (P2-15). The header shows the
    // key it is heard in: moved by Key Shift with key lock, or by the tempo without (P2-22), e.g. "Ebm (+1)".
    void setTrackKey(int musicalKey, util::KeyNotation notation);
    juce::String keyText() const;
    std::function<void()> onTap;      // TAP: tap tempo
    // Hot cues (P3-01): the pad strip shows the current page (A-H or I-P); a pad press / release, a right-click.
    class PadButton;
    std::function<void(int pad, bool down)> onHotCuePad;
    std::function<void(int pad)> onHotCueMenu;
    // Memory cues (P3-02): MEM opens the list; their positions (buffer samples) and colours for the overview.
    std::function<void()> onMemoryPanel;
    // Loops (P3-03): the loop row's buttons; the app sends the engine commands (auto loop size, export, ...).
    enum class LoopButton
    {
        In,
        Out,
        Auto,
        Halve,
        Double,
        Reloop,
        Menu,
    };
    std::function<void(LoopButton)> onLoopButton;
    juce::TextButton& loopButton(LoopButton b) noexcept { return loopButtons[static_cast<std::size_t>(b)]; }
    void setMemoryCues(std::vector<std::pair<double, std::uint32_t>> cues);
    juce::TextButton& memButton() noexcept { return mem; }
    void setHotCuePage(int page);
    // The colours and labels of the 16 hot cues (positions and set / empty come from the engine).
    void setHotCueLooks(const std::array<std::uint32_t, 16>& colours, const std::array<juce::String, 16>& labels);
    PadButton& padButton(int pad) noexcept { return *pads[static_cast<std::size_t>(pad)]; }
    int hotCuePage() const noexcept { return padPage; }
    // Beat jump (P2-21): the arrows jump back / forward by the size; the size button opens the size menu.
    std::function<void(int direction)> onBeatJump;
    std::function<void()> onJumpSizeMenu;
    void setJumpSizeText(const juce::String& text) { jumpSize.setButtonText(text); }
    juce::TextButton& jumpBackButton() noexcept { return jumpBack; }
    juce::TextButton& jumpForwardButton() noexcept { return jumpForward; }
    juce::TextButton& jumpSizeButton() noexcept { return jumpSize; }
    juce::Slider& tempoSlider() noexcept { return tempo; }
    Overview& overview() noexcept { return *overviewView; }

private:
    engine::AudioEngine& engine;
    engine::DeckId deck;
    TrackLookup lookup;
    engine::BufferId shownBuffer = 0;
    std::shared_ptr<const engine::TrackBuffer> track;
    std::shared_ptr<const analysis::Waveform> preview;
    juce::String previewTitle;
    engine::BufferId previewOver = 0;
    engine::DeckState state;
    double sampleRate = 48000.0;
    int trackKey = -1;
    util::KeyNotation keyNotation = util::KeyNotation::Musical;
    juce::String shownKey;

    std::unique_ptr<HoldButton> cue;
    juce::TextButton play{"PLAY"}, sync{"SYNC"}, keyLock{"KEY LOCK"}, grid{"GRID"}, tap{"TAP"}, mem{"MEM"};
    juce::TextButton jumpBack{juce::CharPointer_UTF8("\xe2\x97\x80")}, jumpForward{juce::CharPointer_UTF8("\xe2\x96\xb6")},
        jumpSize{"4"};
    juce::Slider tempo{juce::Slider::LinearVertical, juce::Slider::NoTextBox};
    std::unique_ptr<Overview> overviewView;
    juce::VBlankAttachment vblank;

    juce::Rectangle<int> headerArea, infoArea;
    std::array<std::unique_ptr<PadButton>, 8> pads;
    int padPage = 0;
    std::array<std::uint32_t, 16> hotCueColours{};
    std::array<juce::String, 16> hotCueLabels;
    std::vector<std::pair<double, std::uint32_t>> memoryCues;
    std::array<juce::TextButton, 7> loopButtons;
    static juce::String beatsText(double beats); // "1/4", "4", "32" 
    void refreshPads();
    void dropHoverChanged() override { repaint(); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeckView)
};

// Sends press / release as the button goes down / up (mouse or finger), for CDJ-style cue.
class DeckView::HoldButton final : public juce::TextButton
{
public:
    explicit HoldButton(const juce::String& text)
        : juce::TextButton(text)
    {
    }
    std::function<void(bool down)> onHold;

private:
    void buttonStateChanged() override;
    bool wasDown = false;
};

// A hot cue pad: press / release like the CUE button (mouse or finger), right-click for its menu.
class DeckView::PadButton final : public juce::TextButton
{
public:
    std::function<void(bool down)> onHold;
    std::function<void()> onMenu;
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onMenu)
                onMenu();
            return;
        }
        juce::TextButton::mouseDown(e);
    }

private:
    void buttonStateChanged() override
    {
        const bool down = isDown();
        if (down != wasDown && onHold)
            onHold(down);
        wasDown = down;
    }
    bool wasDown = false;
};

class DeckView::Overview final : public juce::Component
{
public:
    explicit Overview(DeckView& owner)
        : deckView(owner)
    {
    }
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void seekToX(float x); // what a click or drag at x does

private:
    DeckView& deckView;
};

} // namespace devildj::ui
