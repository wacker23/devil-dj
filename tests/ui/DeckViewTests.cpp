// P1-13: deck view. Done when: all controls usable with mouse and touch; screenshot in DEVLOG.
// Touch arrives in JUCE as mouse events on the same components, so driving the components' mouse / button
// paths covers both. The [.screenshot] test renders docs/images/p1-13-deck-view.png.

#include "analysis/WaveformBuilder.h"
#include "engine/AudioEngine.h"
#include "ui/DeckView.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <numbers>

using namespace devildj;
using engine::AudioEngine;
using ui::DeckView;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// A music-like test track: 128 BPM kick pulses over a quieter pad, with a breakdown in the middle.
std::shared_ptr<engine::TrackBuffer> demoTrack(double seconds = 240.0)
{
    auto t = std::make_shared<engine::TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    const double beat = 60.0 / 128.0;
    for (int i = 0; i < n; ++i)
    {
        const double time = i / kRate;
        const double phase = std::fmod(time, beat) / beat;
        const bool breakdown = time > seconds * 0.45 && time < seconds * 0.55;
        const double intro = std::min(1.0, time / 20.0);
        const double kick = breakdown ? 0.0 : std::exp(-phase * 12.0) * std::sin(2.0 * std::numbers::pi * 55.0 * time);
        const double pad = 0.25 * std::sin(2.0 * std::numbers::pi * 220.0 * time) * (0.6 + 0.4 * std::sin(time));
        const auto v = static_cast<float>(intro * (0.8 * kick + pad));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    t->file = juce::File::getCurrentWorkingDirectory().getChildFile("night_drive_extended.wav");
    t->title = "Night Drive (Extended Mix)";
    t->artist = "Generated test signal";
    t->tagBpm = 128.0;
    t->waveform = analysis::buildWaveform(t->audio.getReadPointer(0), t->audio.getReadPointer(1), n, kRate);
    return t;
}

struct Rig
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::shared_ptr<engine::TrackBuffer> track = demoTrack();
    AudioEngine engine;
    std::vector<float> l = std::vector<float>(kBlock), r = std::vector<float>(kBlock);
    std::unique_ptr<DeckView> view;

    Rig()
    {
        engine.prepare(kRate, kBlock);
        engine.send(engine::LoadTrack{0, track.get(), 1});
        run(1);
        view = std::make_unique<DeckView>(
            engine, 0, [this](engine::BufferId id)
            { return id == 1 ? std::shared_ptr<const engine::TrackBuffer>(track) : nullptr; });
        view->setSize(620, 250);
        view->refresh();
    }
    void run(int blocks)
    {
        float* outs[] = {l.data(), r.data()};
        for (int i = 0; i < blocks; ++i)
            engine.process(outs, 2, kBlock);
    }
    const engine::DeckState& deck() { return engine.readState().decks[0]; }
};
} // namespace

TEST_CASE("Deck view: text helpers", "[ui][deckview]")
{
    CHECK(DeckView::formatTime(0.0, false) == "00:00.0");
    CHECK(DeckView::formatTime(83.46, false) == "01:23.4");
    CHECK(DeckView::formatTime(600.0, true) == "-10:00.0");
    CHECK(DeckView::formatBpm(128.0, 0.0) == "128.00");
    CHECK(DeckView::formatBpm(128.0, 2.5) == "131.20");
    CHECK(DeckView::formatBpm(0.0, 2.5) == "--.--");
    engine::TrackBuffer t;
    t.file = juce::File::getCurrentWorkingDirectory().getChildFile("my_track.flac");
    CHECK(DeckView::displayTitle(t) == "my_track"); // no tags: file name
    t.title = "Real Title";
    CHECK(DeckView::displayTitle(t) == "Real Title");
}

TEST_CASE("Deck view: overview x maps to track position", "[ui][deckview]")
{
    const juce::Rectangle<float> area(10.0f, 0.0f, 200.0f, 50.0f);
    CHECK(DeckView::sampleForX(10.0f, area, 48000) == 0.0);
    CHECK(DeckView::sampleForX(110.0f, area, 48000) == 24000.0);
    CHECK(DeckView::sampleForX(210.0f, area, 48000) == 48000.0);
    CHECK(DeckView::sampleForX(-50.0f, area, 48000) == 0.0); // dragged off the left
    CHECK(DeckView::sampleForX(500.0f, area, 48000) == 48000.0);
}

TEST_CASE("Deck view: play, cue (hold), key lock, tempo and seek drive the deck", "[ui][deckview]")
{
    Rig rig;
    CHECK(rig.view->playButton().isEnabled());
    CHECK_FALSE(rig.view->syncButton().isEnabled()); // until P2-19

    // Click on the overview at 25 %: seek there.
    auto& overview = rig.view->overview();
    overview.seekToX(overview.getWidth() * 0.25f);
    rig.run(1);
    const double onePixel = static_cast<double>(rig.track->numSamples()) / overview.getWidth();
    CHECK(std::abs(rig.deck().positionSamples - 0.25 * rig.track->numSamples()) <= onePixel);

    // PLAY (a click: mouse up over the button, or a tap).
    rig.view->playButton().onClick();
    rig.run(4);
    CHECK(rig.deck().playing);
    rig.view->refresh();
    CHECK(rig.view->playButton().getButtonText() == "PAUSE");
    rig.view->playButton().onClick();
    rig.run(4);
    CHECK_FALSE(rig.deck().playing);

    // CUE held while paused: preview from the cue point; released: back to it, paused.
    auto& cue = rig.view->cueButton();
    cue.setState(juce::Button::buttonDown);
    rig.run(4);
    CHECK(rig.deck().cuePreview);
    cue.setState(juce::Button::buttonNormal);
    rig.run(4);
    CHECK_FALSE(rig.deck().playing);
    CHECK(rig.deck().positionSamples == rig.deck().cuePointSamples);

    // Key lock toggle.
    rig.view->keyLockButton().onClick();
    rig.run(2);
    CHECK(rig.deck().keyLock);
    rig.view->refresh();
    CHECK(rig.view->keyLockButton().getToggleState());

    // Tempo slider: down = faster (the slider's value is -tempo %).
    rig.view->tempoSlider().setValue(-3.0, juce::sendNotificationSync);
    rig.run(2);
    CHECK(std::abs(rig.deck().tempoPercent - 3.0) < 1e-9);
    rig.view->tempoSlider().setValue(0.0, juce::sendNotificationSync); // what a double-click does
    rig.run(2);
    CHECK(rig.deck().tempoPercent == 0.0);

    // State changed elsewhere (keyboard) shows up on the slider.
    rig.engine.send(engine::SetTempo{0, -1.5});
    rig.run(2);
    rig.view->refresh();
    CHECK(std::abs(rig.view->tempoSlider().getValue() - 1.5) < 1e-9);
}

TEST_CASE("Deck view: empty deck disables transport and paints", "[ui][deckview]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    AudioEngine engine;
    engine.prepare(kRate, kBlock);
    DeckView view(engine, 1, [](engine::BufferId) { return nullptr; });
    view.setSize(620, 250);
    view.refresh();
    CHECK_FALSE(view.playButton().isEnabled());
    CHECK_FALSE(view.cueButton().isEnabled());
    const auto image = view.createComponentSnapshot(view.getLocalBounds());
    CHECK(image.getWidth() == 620);
    view.overview().seekToX(10.0f); // nothing loaded: ignored
}

TEST_CASE("Deck view screenshot", "[.screenshot]")
{
    Rig rig;
    // Deck A: playing at +2 % with key lock, a third of the way in, cue point set earlier.
    rig.engine.send(engine::Seek{0, 0.12 * rig.track->numSamples()});
    rig.engine.send(engine::CuePress{0}); // paused: sets the cue point here
    rig.engine.send(engine::CueRelease{0});
    rig.run(4);
    rig.engine.send(engine::Seek{0, 0.34 * rig.track->numSamples()});
    rig.engine.send(engine::SetTempo{0, 2.0});
    rig.engine.send(engine::SetKeyLock{0, true});
    rig.engine.send(engine::Play{0});
    rig.run(20);

    AudioEngine emptyEngine;
    emptyEngine.prepare(kRate, kBlock);
    DeckView empty(emptyEngine, 1, [](engine::BufferId) { return nullptr; });
    empty.setSize(620, 250);

    rig.view->refresh();
    empty.refresh();
    juce::Image image(juce::Image::ARGB, 1264, 266, true);
    {
        juce::Graphics g(image);
        g.fillAll(juce::Colour(0xff101014));
        g.drawImageAt(rig.view->createComponentSnapshot(rig.view->getLocalBounds()), 8, 8);
        g.drawImageAt(empty.createComponentSnapshot(empty.getLocalBounds()), 636, 8);
    }
    const auto out =
        juce::File(juce::String::fromUTF8(DEVILDJ_SOURCE_DIR)).getChildFile("docs/images/p1-13-deck-view.png");
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    juce::FileOutputStream stream(out);
    REQUIRE(stream.openedOk());
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, stream));
}
