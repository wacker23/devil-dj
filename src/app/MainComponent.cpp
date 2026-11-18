#include "MainComponent.h"

#include "engine/TrackDecoder.h"

#include <taglib/tversionnumber.h>

#include <algorithm>
#include <cmath>

namespace devildj::app
{

namespace
{
constexpr int kOverlayMargin = 12;
}

MainComponent::MainComponent(engine::AudioEngine& audioEngine, engine::AudioDeviceService& audioService,
                             engine::TrackLoader& trackLoader, engine::BufferRegistry& bufferRegistry)
    : engine(audioEngine)
    , audio(audioService)
    , loader(trackLoader)
    , registry(bufferRegistry)
    , overlay([&audioEngine] { return audioEngine.readState(); }, [&audioService] { return audioService.getStatus(); })
{
    addAndMakeVisible(deckA);
    addAndMakeVisible(deckB);
    addAndMakeVisible(mixer);
    addAndMakeVisible(waveA);
    addAndMakeVisible(waveB);
    overlay.setExtraLines([this] { return statusLines(); });
    addChildComponent(overlay); // added last: on top of the jog strips

    loader.callbacks.onProgress = [this](engine::DeckId deck, const juce::File& file, double fraction)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] =
                "loading " + file.getFileName() + " " + juce::String(juce::roundToInt(100 * fraction)) + " %";
    };
    loader.callbacks.onLoaded = [this](engine::DeckId deck, const engine::TrackBuffer& t)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] =
                t.file.getFileName() + " (" + juce::String(t.durationSeconds(), 1) + " s, " + t.formatName + ")";
    };
    loader.callbacks.onError = [this](engine::DeckId deck, const juce::File& file, const juce::String& error)
    {
        if (deck < deckStatus.size())
            deckStatus[deck] = "failed: " + file.getFileName() + " - " + error;
    };
    setWantsKeyboardFocus(true);
    setSize(1280, 800);
}

// Temporary key handling until the KeyboardController binds keys by scan code (P1-16).
bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::F12Key)
    {
        toggleDebugOverlay();
        return true;
    }
    if (handleMixerKey(key))
        return true;

    // Ctrl+W / Ctrl+I: key lock deck A / B (KEYMAP).
    if (key.getModifiers().isCtrlDown() && !key.getModifiers().isShiftDown())
    {
        if (key.getKeyCode() == 'W')
        {
            toggleKeyLock(0);
            return true;
        }
        if (key.getKeyCode() == 'I')
        {
            toggleKeyLock(1);
            return true;
        }
    }

    // Shift + bend key = fine tempo step; repeats while held (KEYMAP: tempo -/+ 0.1 %).
    if (key.getModifiers().isShiftDown())
        for (const auto& k : deckKeys)
            if ((k.role == KeyRole::BendDown || k.role == KeyRole::BendUp) && key.getKeyCode() == k.keyCode)
            {
                engine.send(engine::NudgeTempo{k.deck, k.role == KeyRole::BendUp ? 0.1 : -0.1});
                return true;
            }

    // Other deck keys are handled on press/release edges in keyStateChanged (no key repeat); swallow the repeats.
    for (const auto& k : deckKeys)
        if (static_cast<int>(juce::CharacterFunctions::toUpperCase(static_cast<juce::juce_wchar>(key.getKeyCode()))) ==
            k.keyCode)
            return true;
    return false;
}

bool MainComponent::keyStateChanged(bool /*isKeyDown*/)
{
    handleDeckKeys();
    return false;
}

void MainComponent::handleDeckKeys()
{
    const bool shift = juce::ModifierKeys::currentModifiers.isShiftDown();
    for (auto& k : deckKeys)
    {
        const bool down = juce::KeyPress::isKeyCurrentlyDown(k.keyCode);
        if (down == k.down)
            continue;
        k.down = down;

        if (k.role == KeyRole::BendDown || k.role == KeyRole::BendUp)
        {
            // Hold to bend; Shift turns these keys into tempo steps (keyPressed). Releasing always ends the bend.
            const double amount =
                k.role == KeyRole::BendUp ? engine::Deck::kMaxPitchBendPercent : -engine::Deck::kMaxPitchBendPercent;
            engine.send(engine::PitchBend{k.deck, down && !shift ? amount : 0.0});
            continue;
        }

        if (k.role == KeyRole::Cue)
        {
            if (down && shift)
                engine.send(engine::Seek{k.deck, 0.0}); // Shift+Z / Shift+M: jump to track start
            else if (down)
                engine.send(engine::CuePress{k.deck});
            else
                engine.send(engine::CueRelease{k.deck});
        }
        else if (down)
        {
            engine.send(engine::TogglePlay{k.deck});
        }
    }
}

bool MainComponent::handleMixerKey(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (mods.isCtrlDown() && !mods.isShiftDown()) // EQ kills, headphone CUE (toggles)
    {
        if (code == 'E' || code == 'O')
        {
            const int ch = code == 'E' ? 0 : 1;
            const bool on = !engine.readState().channels[static_cast<std::size_t>(ch)].cue;
            engine.send(engine::SetParam{engine::channelParam(ch, engine::ChannelParam::Cue), on ? 1.0f : 0.0f});
            return true;
        }
        static constexpr int killKeys[2][3] = {{'A', 'S', 'D'}, {'J', 'K', 'L'}};
        for (int ch = 0; ch < 2; ++ch)
            for (int band = 0; band < 3; ++band)
                if (code == killKeys[ch][band])
                {
                    // Toggle from what the engine has (the mixer knobs may have moved it).
                    const bool kill = !std::isinf(
                        engine.readState().channels[static_cast<std::size_t>(ch)].eqDb[static_cast<std::size_t>(band)]);
                    const auto param =
                        static_cast<engine::ChannelParam>(static_cast<int>(engine::ChannelParam::EqLow) + band);
                    engine.send(engine::SetParam{engine::channelParam(ch, param), kill ? -1000.0f : 0.0f});
                    return true;
                }
    }
    if (mods.isShiftDown() && !mods.isCtrlDown())
    {
        if (code == '5' || code == '-') // filter reset
        {
            const int ch = code == '5' ? 0 : 1;
            filterKnob[static_cast<std::size_t>(ch)] = 0.5;
            sendFilter(ch);
            return true;
        }
        if (code == '3' || code == '4' || code == '9' || code == '0')
            return true;                // swept while held in sweepHeldKeys()
        if (code == 'G' || code == 'H') // crossfader snap
        {
            crossfader = code == 'G' ? 0.0 : 1.0;
            sendCrossfader();
            return true;
        }
    }
    if (!mods.isAnyModifierKeyDown())
    {
        if (code == 'B') // crossfader centre
        {
            crossfader = 0.5;
            sendCrossfader();
            return true;
        }
        if (code == 'G' || code == 'H')
            return true; // glides while held in sweepHeldKeys()
    }
    return false;
}

void MainComponent::sweepHeldKeys()
{
    const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const double dt = std::clamp(now - lastSweepSeconds, 0.0, 0.1);
    lastSweepSeconds = now;
    if (!hasKeyboardFocus(true))
        return;
    const auto mods = juce::ModifierKeys::currentModifiers;
    if (!mods.isAnyModifierKeyDown())
    {
        const int direction =
            (juce::KeyPress::isKeyCurrentlyDown('H') ? 1 : 0) - (juce::KeyPress::isKeyCurrentlyDown('G') ? 1 : 0);
        if (direction != 0)
        {
            if (!crossfaderGliding) // start from where the crossfader is (the mixer may have moved it)
                crossfader = engine.readState().crossfader;
            crossfaderGliding = true;
            crossfader = std::clamp(crossfader + direction * dt, 0.0, 1.0);
            sendCrossfader();
        }
        else
        {
            crossfaderGliding = false;
        }
    }
    if (!mods.isShiftDown())
    {
        filterSweeping = {};
        return;
    }
    static constexpr int sweepKeys[2][2] = {{'3', '4'}, {'9', '0'}}; // towards low-pass, towards high-pass
    for (int ch = 0; ch < 2; ++ch)
    {
        const int direction = (juce::KeyPress::isKeyCurrentlyDown(sweepKeys[ch][1]) ? 1 : 0) -
                              (juce::KeyPress::isKeyCurrentlyDown(sweepKeys[ch][0]) ? 1 : 0);
        auto& sweeping = filterSweeping[static_cast<std::size_t>(ch)];
        if (direction == 0)
        {
            sweeping = false;
            continue;
        }
        auto& knob = filterKnob[static_cast<std::size_t>(ch)];
        if (!sweeping)
            knob = engine.readState().channels[static_cast<std::size_t>(ch)].filter;
        sweeping = true;
        knob = std::clamp(knob + direction * 0.5 * dt, 0.0, 1.0);
        sendFilter(ch);
    }
}

void MainComponent::sendFilter(int channel)
{
    engine.send(engine::SetParam{engine::channelParam(channel, engine::ChannelParam::Filter),
                                 static_cast<float>(filterKnob[static_cast<std::size_t>(channel)])});
}

void MainComponent::sendCrossfader()
{
    engine.send(engine::SetParam{engine::ParamId::Crossfader, static_cast<float>(crossfader)});
}

void MainComponent::setCrossfaderCurve(int curve)
{
    engine.send(engine::SetParam{engine::ParamId::CrossfaderCurve, static_cast<float>(curve)});
}

void MainComponent::setCrossfaderAssign(int deck, int assign)
{
    engine.send(engine::SetParam{engine::channelParam(deck, engine::ChannelParam::CrossfaderAssign),
                                 static_cast<float>(assign)});
}

void MainComponent::setCueMix(double mix)
{
    engine.send(engine::SetParam{engine::ParamId::CueMix, static_cast<float>(mix)});
}

void MainComponent::setHeadphoneLevel(double db)
{
    engine.send(engine::SetParam{engine::ParamId::HeadphoneLevel, static_cast<float>(db)});
}

void MainComponent::setIsolator(bool on)
{
    for (int ch = 0; ch < 2; ++ch)
        engine.send(engine::SetParam{engine::channelParam(ch, engine::ChannelParam::EqMode), on ? 1.0f : 0.0f});
}

void MainComponent::toggleDebugOverlay()
{
    overlay.setVisible(!overlay.isVisible());
    if (overlay.isVisible())
        overlay.toFront(false);
}

void MainComponent::toggleTestTone()
{
    testToneOn = !testToneOn;
    engine.send(engine::SetTestTone{testToneOn, 440.0f, -18.0f});
}

void MainComponent::resetTempo(engine::DeckId deck)
{
    engine.send(engine::ResetTempo{deck});
}

void MainComponent::setTempoRange(double rangePercent)
{
    for (engine::DeckId d = 0; d < 2; ++d)
        engine.send(engine::SetTempoRange{d, rangePercent});
}

void MainComponent::setHighQualityResampling(bool on)
{
    engine.send(engine::SetInterpolationQuality{on ? engine::InterpolationQuality::High
                                                   : engine::InterpolationQuality::Standard});
}

void MainComponent::toggleKeyLock(engine::DeckId deck)
{
    const bool on = engine.readState().decks[deck].keyLock;
    engine.send(engine::SetKeyLock{deck, !on});
}

MainComponent::~MainComponent()
{
    loader.callbacks = {}; // the loader outlives this component; don't let late results call into it
}

void MainComponent::chooseTrackForDeck(engine::DeckId deck)
{
    juce::StringArray patterns;
    for (const auto& ext : engine::supportedTrackExtensions())
        patterns.add("*" + ext);

    chooser = std::make_unique<juce::FileChooser>(juce::String("Load track to deck ") + engine::deckLetter(deck),
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  patterns.joinIntoString(";"));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, deck](const juce::FileChooser& fc)
                         {
                             const auto file = fc.getResult();
                             if (file == juce::File())
                                 return;
                             deckStatus[deck] = "loading " + file.getFileName();
                             loader.load(deck, file, audio.getStatus().sampleRate);
                         });
}

void MainComponent::unloadDeck(engine::DeckId deck)
{
    loader.unload(deck);
    deckStatus[deck] = "empty";
}

namespace
{
juce::String clock(double samples, double rate)
{
    if (rate <= 0.0)
        return "--:--.-";
    const double seconds = samples / rate;
    const int minutes = static_cast<int>(seconds / 60.0);
    const double rest = seconds - minutes * 60.0;
    return juce::String(minutes).paddedLeft('0', 2) + ":" + juce::String(rest, 1).paddedLeft('0', 4);
}
} // namespace

namespace
{
juce::String levelDb(float linear)
{
    return linear < 1e-5f ? juce::String("-inf") : juce::String(20.0 * std::log10(linear), 1);
}
} // namespace

juce::StringArray MainComponent::statusLines()
{
    juce::StringArray lines;
    const auto& state = engine.readState();
    for (engine::DeckId d = 0; d < deckStatus.size(); ++d)
    {
        lines.add(juce::String("Deck ") + engine::deckLetter(d) + ": " + deckStatus[d]);
        const auto& s = state.decks[d];
        if (s.loaded)
        {
            const juce::String what = s.cuePreview ? "CUE" : s.playing ? "PLAY" : s.atEnd ? "END" : "pause";
            lines.add("   " + what + "  " + clock(s.positionSamples, state.sampleRate) + " / " +
                      clock(s.lengthSamples, state.sampleRate) + "  cue " + clock(s.cuePointSamples, state.sampleRate));
            lines.add(juce::String("   ") + (s.scratching ? "SCRATCH  " : "") + (s.keyLock ? "KEY LOCK  " : "") +
                      "tempo " + juce::String(s.tempoPercent >= 0 ? "+" : "") + juce::String(s.tempoPercent, 1) +
                      " % (range +-" + juce::String(juce::roundToInt(s.tempoRangePercent)) + ")  bend " +
                      juce::String(s.pitchBendPercent, 1) + " %  rate " + juce::String(s.rate, 4));
        }
        const auto& c = state.channels[d];
        const auto band = [](float db) { return std::isinf(db) ? juce::String("KILL") : juce::String(db, 1); };
        const double knob = c.filter;
        const juce::String filter =
            std::abs(knob - 0.5) <= engine::ChannelStrip::kFilterDeadZone ? juce::String("off")
            : knob < 0.5
                ? "LP " + juce::String(juce::roundToInt(engine::ChannelStrip::filterTarget(knob).lowPassHz)) + " Hz"
                : "HP " + juce::String(juce::roundToInt(engine::ChannelStrip::filterTarget(knob).highPassHz)) + " Hz";
        lines.add(juce::String("   ") + (c.eqMode == 1 ? "ISO" : "EQ") + "  low " + band(c.eqDb[0]) + "  mid " +
                  band(c.eqDb[1]) + "  high " + band(c.eqDb[2]) + "  filter " + filter);
        static const char* const sides[] = {"xf A", "xf THRU", "xf B"};
        lines.add(juce::String("   ") + (c.cue ? "CUE  " : "") + sides[std::min<int>(c.crossfaderAssign, 2)] +
                  "  meter " + levelDb(std::max(c.peakL, c.peakR)) + " dB peak, " + levelDb(c.rms) + " dB rms");
    }
    lines.add("Keys A: Z cue, X play, 1/2 bend, Shift+1/2 tempo, Shift+Z start, Ctrl+W key lock");
    lines.add("Keys B: M cue, , play, 7/8 bend, Shift+7/8 tempo, Shift+M start, Ctrl+I key lock");
    static const char* const curves[] = {"smooth", "linear", "cut"};
    lines.add("Master: crossfader " + juce::String(state.crossfader, 2) + " (" +
              curves[std::min<int>(state.crossfaderCurve, 2)] + ")  peak " + levelDb(state.masterPeakL) + " / " +
              levelDb(state.masterPeakR) + " dB  limiter -" + juce::String(state.limiterGainReductionDb, 1) + " dB");
    lines.add("Headphones: mix " + juce::String(state.cueMix, 2) + " (0 cue .. 1 master)  level " +
              juce::String(state.headphoneLevelDb, 0) + " dB  peak " +
              levelDb(std::max(state.cuePeakL, state.cuePeakR)) + " dB" +
              (state.cueRouted ? "" : "  [not routed: Options > Audio settings > output layout]"));
    lines.add("Crossfader: hold G / H glide, Shift+G / Shift+H snap, B centre;  CUE: Ctrl+E (A), Ctrl+O (B)");
    lines.add("Mixer: Ctrl+A/S/D, Ctrl+J/K/L kill; hold Shift+3/4, Shift+9/0 filter; Shift+5, Shift+- reset");
    lines.add("Waveforms: drag = scratch (playing) / scrub (paused), wheel = nudge, Ctrl+wheel = zoom");
    for (const auto* w : {&waveA, &waveB})
    {
        const auto f = w->frameStats();
        lines.add(juce::String("   frame: paint ") + juce::String(f.paintMsAverage, 2) + " ms avg, " +
                  juce::String(f.paintMsMax, 2) + " ms max; " +
                  (f.frameIntervalMs > 0.0 ? juce::String(1000.0 / f.frameIntervalMs, 1) : juce::String("--")) +
                  " fps");
    }
    lines.add("Track buffers: " + juce::String(engine::TrackBuffer::liveCount()) + " alive, " +
              juce::String(static_cast<int>(registry.size())) + " registered, " +
              juce::String(juce::roundToInt(static_cast<double>(registry.memoryBytes()) / (1024.0 * 1024.0))) + " MB");
    return lines;
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    // Temporary build-info line; moves to the About box later.
    const auto version = TagLib::runtimeVersion();
    const auto info = juce::String("JUCE ") + juce::String(JUCE_MAJOR_VERSION) + "." +
                      juce::String(JUCE_MINOR_VERSION) + "." + juce::String(JUCE_BUILDNUMBER) + "  |  TagLib " +
                      juce::String(version.majorVersion()) + "." + juce::String(version.minorVersion()) + "." +
                      juce::String(version.patchVersion());
    g.setColour(juce::Colours::white.withAlpha(0.35f));
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(info, getLocalBounds().reduced(12), juce::Justification::bottomLeft);
}

double MainComponent::outputLatencySeconds() const
{
    return audio.getStatus().totalOutputLatencyMs / 1000.0;
}

void MainComponent::resized()
{
    // Top: deck A | mixer | deck B. Below: the two zoomed waveforms.
    auto top = getLocalBounds().reduced(12).removeFromTop(ui::MixerView::kPreferredHeight);
    const int deckWidth = (top.getWidth() - ui::MixerView::kPreferredWidth - 12) / 2;
    deckA.setBounds(top.removeFromLeft(deckWidth).withHeight(250));
    top.removeFromLeft(6);
    deckB.setBounds(top.removeFromRight(deckWidth).withHeight(250));
    top.removeFromRight(6);
    mixer.setBounds(top);

    auto waves = getLocalBounds().reduced(12).withTrimmedTop(ui::MixerView::kPreferredHeight + 8);
    waves = waves.removeFromTop(std::min(2 * 130 + 6, waves.getHeight() - 20));
    const int waveHeight = (waves.getHeight() - 6) / 2;
    waveA.setBounds(waves.removeFromTop(waveHeight));
    waves.removeFromTop(6);
    waveB.setBounds(waves.removeFromTop(waveHeight));

    overlay.setBounds(getWidth() - ui::DebugOverlay::kWidth - kOverlayMargin, kOverlayMargin, ui::DebugOverlay::kWidth,
                      ui::DebugOverlay::kHeight);
}

} // namespace devildj::app
