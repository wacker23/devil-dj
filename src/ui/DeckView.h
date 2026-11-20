#pragma once

#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/TrackDropTarget.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

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

    // The loaded track's key as the app shows it (library key, in the user's notation; P2-15). Empty = none.
    void setKeyText(const juce::String& text);
    const juce::String& keyText() const noexcept { return key; }
    std::function<void()> onTap;      // TAP: tap tempo
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
    juce::String key;

    std::unique_ptr<HoldButton> cue;
    juce::TextButton play{"PLAY"}, sync{"SYNC"}, keyLock{"KEY LOCK"}, grid{"GRID"}, tap{"TAP"};
    juce::Slider tempo{juce::Slider::LinearVertical, juce::Slider::NoTextBox};
    std::unique_ptr<Overview> overviewView;
    juce::VBlankAttachment vblank;

    juce::Rectangle<int> headerArea, infoArea;
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
