#include "DeckView.h"

#include "ui/WaveformColours.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kAccent(0xffe0483c); // Devil-DJ red
const juce::Colour kCueColour(0xfff0a030);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
constexpr int kMargin = 8;
constexpr int kTempoWidth = 46;
} // namespace

// ---- helpers ----

double DeckView::sampleForX(float x, juce::Rectangle<float> area, int lengthSamples) noexcept
{
    if (lengthSamples <= 0 || area.getWidth() <= 0.0f)
        return 0.0;
    const double t = std::clamp((x - area.getX()) / area.getWidth(), 0.0f, 1.0f);
    return t * lengthSamples;
}

juce::String DeckView::formatTime(double seconds, bool remaining)
{
    seconds = std::max(0.0, seconds);
    const int tenths = static_cast<int>(std::floor(seconds * 10.0));
    const int minutes = tenths / 600;
    const int secs = (tenths / 10) % 60;
    return juce::String(remaining ? "-" : "") + juce::String(minutes).paddedLeft('0', 2) + ":" +
           juce::String(secs).paddedLeft('0', 2) + "." + juce::String(tenths % 10);
}

juce::String DeckView::formatBpm(double tagBpm, double tempoPercent)
{
    if (tagBpm <= 0.0)
        return "--.--";
    const double bpm = tagBpm * (1.0 + tempoPercent / 100.0);
    return juce::String(std::round(bpm * 100.0) / 100.0, 2);
}

juce::String DeckView::displayTitle(const engine::TrackBuffer& t)
{
    return t.title.isNotEmpty() ? t.title : t.file.getFileNameWithoutExtension();
}

// ---- hold button ----

void DeckView::HoldButton::buttonStateChanged()
{
    const bool down = isDown();
    if (down != wasDown)
    {
        wasDown = down;
        if (onHold)
            onHold(down);
    }
}

// ---- deck view ----

DeckView::DeckView(engine::AudioEngine& audioEngine, engine::DeckId deckId, TrackLookup trackLookup)
    : engine(audioEngine)
    , deck(deckId)
    , lookup(std::move(trackLookup))
    , cue(std::make_unique<HoldButton>("CUE"))
    , overviewView(std::make_unique<Overview>(*this))
    , vblank(this, [this] { refresh(); })
{
    cue->onHold = [this](bool down)
    {
        if (down)
            engine.send(engine::CuePress{deck});
        else
            engine.send(engine::CueRelease{deck});
    };
    play.onClick = [this] { engine.send(engine::TogglePlay{deck}); };
    grid.onClick = [this]
    {
        if (onGridMenu)
            onGridMenu();
    };
    tap.onClick = [this]
    {
        if (onTap)
            onTap();
    };
    grid.setTooltip("Beatgrid tools: set downbeat, shift, BPM x2 / /2, metronome, lock (Ctrl+Shift keys)");
    tap.setTooltip("Tap tempo: tap 4 times or more in time with the track");
    keyLock.onClick = [this] { engine.send(engine::SetKeyLock{deck, !state.keyLock}); };
    sync.onClick = [this] { engine.send(engine::SetSync{deck, !state.synced}); };
    sync.setTooltip("Sync: match the master's tempo and beat (C / . keys); the master is the deck marked MASTER");
    keyLock.setTooltip("Key lock: tempo changes keep the pitch");
    cue->setTooltip("Cue: hold to preview from the cue point; while paused, sets the cue point");

    // Tempo slider: like a CDJ, down = faster. Slider value = -tempo %, so the top is the minus end.
    tempo.setRange(-10.0, 10.0, 0.01);
    tempo.setDoubleClickReturnValue(true, 0.0);
    tempo.setTooltip("Tempo (drag; double-click resets to 0 %)");
    // refresh() updates the slider without notification, so this only fires for the user (drag, double-click).
    tempo.onValueChange = [this] { engine.send(engine::SetTempo{deck, 0.0 - tempo.getValue()}); };

    for (auto* b : {static_cast<juce::Button*>(cue.get()), static_cast<juce::Button*>(&play),
                    static_cast<juce::Button*>(&sync), static_cast<juce::Button*>(&keyLock),
                    static_cast<juce::Button*>(&grid), static_cast<juce::Button*>(&tap)})
    {
        b->setColour(juce::TextButton::buttonColourId, kPanel);
        b->setColour(juce::TextButton::buttonOnColourId, kAccent);
        b->setColour(juce::TextButton::textColourOffId, kText);
        b->setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        b->setWantsKeyboardFocus(false); // keys stay with the main window's deck keys
        addAndMakeVisible(b);
    }
    cue->setColour(juce::TextButton::buttonOnColourId, kCueColour);
    tempo.setColour(juce::Slider::thumbColourId, kAccent);
    tempo.setColour(juce::Slider::trackColourId, kPanel.brighter(0.2f));
    tempo.setWantsKeyboardFocus(false);
    addAndMakeVisible(tempo);
    addAndMakeVisible(*overviewView);
    refresh();
}

DeckView::~DeckView() = default;

void DeckView::refresh()
{
    const auto& s = engine.readState();
    if (s.sampleRate > 0.0)
        sampleRate = s.sampleRate;
    state = s.decks[deck];

    if (state.bufferId != shownBuffer)
    {
        shownBuffer = state.bufferId;
        track = state.loaded && lookup ? lookup(shownBuffer) : nullptr;
    }
    if (preview != nullptr && state.bufferId != previewOver)
        clearPreview(); // the loaded track is on the deck now

    play.setButtonText(state.playing && !state.cuePreview ? "PAUSE" : "PLAY");
    play.setToggleState(state.playing && !state.cuePreview, juce::dontSendNotification);
    cue->setToggleState(state.cuePreview, juce::dontSendNotification);
    keyLock.setToggleState(state.keyLock, juce::dontSendNotification);
    for (auto* b : {static_cast<juce::Button*>(cue.get()), static_cast<juce::Button*>(&play),
                    static_cast<juce::Button*>(&keyLock), static_cast<juce::Button*>(&grid),
                    static_cast<juce::Button*>(&tap)})
        b->setEnabled(state.loaded);
    grid.setToggleState(state.metronome || state.gridLocked, juce::dontSendNotification);
    sync.setToggleState(state.synced, juce::dontSendNotification);
    sync.setEnabled(state.loaded && state.hasGrid); // sync needs a beatgrid

    const double range = state.tempoRangePercent > 0.0 ? state.tempoRangePercent : 10.0;
    if (tempo.getMaximum() != range)
        tempo.setRange(-range, range, 0.01);
    if (!tempo.isMouseButtonDown())
        tempo.setValue(-state.tempoPercent, juce::dontSendNotification);

    repaint();
}

void DeckView::resized()
{
    auto area = getLocalBounds().reduced(kMargin);
    tempo.setBounds(area.removeFromRight(kTempoWidth));
    area.removeFromRight(kMargin);
    headerArea = area.removeFromTop(46);
    infoArea = area.removeFromTop(26);
    {
        auto tools = infoArea;
        tap.setBounds(tools.removeFromRight(48).reduced(0, 2));
        tools.removeFromRight(4);
        grid.setBounds(tools.removeFromRight(56).reduced(0, 2));
    }
    area.removeFromTop(4);
    auto buttons = area.removeFromBottom(52);
    area.removeFromBottom(kMargin);
    overviewView->setBounds(area);

    const int w = std::min(96, (buttons.getWidth() - 3 * kMargin) / 4);
    cue->setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(kMargin);
    play.setBounds(buttons.removeFromLeft(w));
    buttons.removeFromLeft(kMargin);
    sync.setBounds(buttons.removeFromLeft(w));
    keyLock.setBounds(buttons.removeFromRight(w + 16));
}

void DeckView::paint(juce::Graphics& g)
{
    g.setColour(kBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);

    auto header = headerArea;
    // Deck letter badge.
    auto badge = header.removeFromLeft(40).reduced(0, 4).toFloat();
    g.setColour(state.playing ? kAccent : kPanel.brighter(0.15f));
    g.fillRoundedRectangle(badge, 6.0f);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(24.0f, juce::Font::bold));
    g.drawText(juce::String::charToString(static_cast<juce::juce_wchar>(engine::deckLetter(deck))),
               badge.toNearestInt(), juce::Justification::centred);
    header.removeFromLeft(kMargin);

    // BPM block on the right.
    auto bpmArea = header.removeFromRight(120);
    g.setColour(kText);
    g.setFont(juce::FontOptions(26.0f, juce::Font::bold));
    // The grid's BPM once the track is analyzed (P2-13 / P2-14), else the tag's.
    g.drawText(formatBpm(state.hasGrid ? state.gridBpm : track ? track->tagBpm : 0.0, state.tempoPercent),
               bpmArea.removeFromTop(28), juce::Justification::centredRight);
    g.setColour(state.tempoPercent == 0.0 ? kDim : kText);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(juce::String(state.tempoPercent >= 0.0 ? "+" : "") + juce::String(state.tempoPercent, 2) + " %  (" +
                   juce::String(juce::roundToInt(state.tempoRangePercent)) + ")",
               bpmArea, juce::Justification::centredRight);

    // Key, left of the BPM.
    if (key.isNotEmpty())
    {
        auto keyArea = header.removeFromRight(64);
        g.setColour(kText);
        g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
        g.drawText(key, keyArea.removeFromTop(28), juce::Justification::centredRight);
        g.setColour(kDim);
        g.setFont(juce::FontOptions(11.0f));
        g.drawText("KEY", keyArea, juce::Justification::centredRight);
        header.removeFromRight(kMargin);
    }

    // Title / artist.
    g.setColour(kText);
    g.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    g.drawText(preview ? previewTitle + "  (loading)"
               : track ? displayTitle(*track)
                       : juce::String("No track - Decks menu to load"),
               header.removeFromTop(26), juce::Justification::centredLeft, true);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(14.0f));
    g.drawText(track ? track->artist : juce::String(), header, juce::Justification::centredLeft, true);

    // Times and key lock indicator.
    auto info = infoArea;
    const double length = state.lengthSamples / sampleRate;
    const double position = state.positionSamples / sampleRate;
    // Monospaced digits so the running time doesn't jitter.
    g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 19.0f, juce::Font::bold));
    g.setColour(kText);
    g.drawText(track ? formatTime(position, false) : juce::String("--:--.-"), info.removeFromLeft(110),
               juce::Justification::centredLeft);
    const bool nearEnd = track && length - position < 30.0 && state.playing;
    g.setColour(nearEnd ? kAccent : kDim);
    g.drawText(track ? formatTime(length - position, true) : juce::String(), info.removeFromLeft(110),
               juce::Justification::centredLeft);
    info.removeFromRight(112); // GRID / TAP
    juce::StringArray badges;
    if (state.master)
        badges.add("MASTER");
    if (state.hasGrid && state.playing)
        badges.add("BEAT " + juce::String(state.barBeat));
    if (state.metronome)
        badges.add("MET");
    if (state.gridLocked)
        badges.add("GRID LOCK");
    if (state.keyLock)
        badges.add("KEY LOCK");
    if (!badges.isEmpty())
    {
        g.setColour(kAccent);
        g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
        g.drawText(badges.joinIntoString("  "), info, juce::Justification::centredRight, true);
    }
}

void DeckView::showPreview(std::shared_ptr<const analysis::Waveform> waveform, const juce::String& title)
{
    preview = std::move(waveform);
    previewTitle = title;
    previewOver = engine.readState().decks[deck].bufferId;
    repaint();
    overviewView->repaint();
}

void DeckView::clearPreview()
{
    preview.reset();
    previewTitle.clear();
    repaint();
    overviewView->repaint();
}

void DeckView::setKeyText(const juce::String& text)
{
    if (text == key)
        return;
    key = text;
    repaint();
}

void DeckView::paintOverChildren(juce::Graphics& g)
{
    if (isDropHovering())
        drawDropHighlight(g, getLocalBounds().toFloat(), juce::String("Drop to load deck ") + engine::deckLetter(deck));
}

// ---- overview ----

void DeckView::Overview::paint(juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour(kPanel);
    g.fillRoundedRectangle(area, 4.0f);
    const auto& t = deckView.track;
    const auto& s = deckView.state;
    const bool loading = deckView.preview != nullptr;
    if (!loading && (t == nullptr || t->waveform.overviewPeak[0].empty() || s.lengthSamples <= 0))
        return;
    const auto& w = loading ? *deckView.preview : t->waveform;
    if (w.overviewPeak[0].empty())
        return;
    const int points = static_cast<int>(w.overviewPeak[0].size());
    const float mid = area.getCentreY();
    const float half = area.getHeight() * 0.5f - 2.0f;
    const float playedX =
        loading ? area.getX() : area.getX() + area.getWidth() * static_cast<float>(s.positionSamples / s.lengthSamples);
    const int columns = std::max(1, static_cast<int>(area.getWidth()));
    for (int c = 0; c < columns; ++c)
    {
        // Each pixel column shows the loudest overview point under it, in the 3-band colours; played = dimmed.
        const int from = c * points / columns, to = std::max(from + 1, (c + 1) * points / columns);
        float peaks[3] = {};
        for (int p = from; p < to && p < points; ++p)
            for (int b = 0; b < 3; ++b)
                peaks[b] = std::max(peaks[b], w.overviewPeak[static_cast<std::size_t>(b)][static_cast<std::size_t>(p)]);
        const float x = area.getX() + static_cast<float>(c);
        drawBandColumn(g, x, 1.0f, mid, half, peaks, x < playedX ? 0.35f : 0.95f);
    }

    if (loading)
        return; // no cue or playhead until the track is on the deck
    // Cue marker and playhead.
    const float cueX = area.getX() + area.getWidth() * static_cast<float>(s.cuePointSamples / s.lengthSamples);
    g.setColour(kCueColour);
    juce::Path marker;
    marker.addTriangle(cueX - 5.0f, area.getY(), cueX + 5.0f, area.getY(), cueX, area.getY() + 7.0f);
    g.fillPath(marker);
    g.fillRect(cueX - 0.5f, area.getY(), 1.0f, area.getHeight());
    g.setColour(juce::Colours::white);
    g.fillRect(playedX - 1.0f, area.getY(), 2.0f, area.getHeight());
}

void DeckView::Overview::seekToX(float x)
{
    const auto& s = deckView.state;
    if (!s.loaded)
        return;
    deckView.engine.send(engine::Seek{deckView.deck, sampleForX(x, getLocalBounds().toFloat(), s.lengthSamples)});
}

void DeckView::Overview::mouseDown(const juce::MouseEvent& e)
{
    seekToX(e.position.x);
}

void DeckView::Overview::mouseDrag(const juce::MouseEvent& e)
{
    seekToX(e.position.x);
}

} // namespace devildj::ui
