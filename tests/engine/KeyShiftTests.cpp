// P2-22: Key Sync and Key Shift. Done when: transposed key shows in the deck header; test with generated chords.

#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/KeyDetector.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/DeckView.h"
#include "util/MusicalKey.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>
#include <vector>

using namespace devildj;

namespace
{
// A generated chord loop (keys/key_NN_*.wav) as a deck buffer at its own rate.
std::unique_ptr<engine::TrackBuffer> keyLoop(int key)
{
    const auto manifest = test::generatedManifest();
    if (!manifest.isObject())
        return nullptr;
    for (const auto& entry : *manifest["files"].getArray())
    {
        if (entry["kind"].toString() != "key" || static_cast<int>(entry["props"]["key_index"]) != key)
            continue;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(
            formats.createReaderFor(test::generatedFile(entry["file"].toString())));
        if (reader == nullptr)
            return nullptr;
        auto t = std::make_unique<engine::TrackBuffer>();
        t->sampleRate = reader->sampleRate;
        t->audio.setSize(2, static_cast<int>(reader->lengthInSamples));
        reader->read(&t->audio, 0, t->audio.getNumSamples(), 0, true, true);
        return t;
    }
    return nullptr;
}

// Plays the loop on a deck with key lock and the shift, and finds the key of what comes out.
int playedKey(const engine::TrackBuffer& track, int shift, double tempoPercent = 0.0, bool keyLock = true)
{
    engine::AudioEngine e;
    const double rate = track.sampleRate;
    e.prepare(rate, 512);
    e.send(engine::LoadTrack{0, &track, 1});
    e.send(engine::SetKeyLock{0, keyLock});
    e.send(engine::SetKeyShift{0, shift});
    e.send(engine::SetTempo{0, tempoPercent});
    e.send(engine::Play{0});
    std::vector<float> l, r;
    float bl[512], br[512];
    float* outs[] = {bl, br};
    const int blocks = static_cast<int>(0.95 * track.audio.getNumSamples() / 512 / (1.0 + tempoPercent / 100.0));
    for (int i = 0; i < blocks; ++i)
    {
        e.process(outs, 2, 512);
        l.insert(l.end(), bl, bl + 512);
        r.insert(r.end(), br, br + 512);
    }
    analysis::DecodedAudio a{l.data(), r.data(), static_cast<int>(l.size()), rate, {}};
    const auto mono = analysis::AnalysisService::monoSignal(a);
    const auto k = analysis::detectKey(mono.data(), static_cast<int>(mono.size()), analysis::AnalysisInput::kMonoRate);
    return k.valid ? k.key : -1;
}
} // namespace

TEST_CASE("Key shift: generated chord loops come out in the transposed key", "[engine][keyshift]")
{
    if (!test::generatedManifest().isObject())
        SKIP("Generated test signals missing (python tools/gen_test_signals.py)");
    const auto cMajor = keyLoop(0);
    const auto aMinor = keyLoop(21);
    REQUIRE(cMajor != nullptr);
    REQUIRE(aMinor != nullptr);
    CHECK(playedKey(*cMajor, 0) == 0);
    for (const int shift : {3, -2, 5})
    {
        INFO("C major " << shift << " semitones");
        CHECK(playedKey(*cMajor, shift) == util::transposeKey(0, shift)); // Eb, Bb, F
    }
    INFO("A minor -1");
    CHECK(playedKey(*aMinor, -1) == 20); // G# minor
    // Key lock keeps the key through a tempo change; without it, +12.25 % is 2 semitones up.
    CHECK(playedKey(*cMajor, 0, 6.0, true) == 0);
    CHECK(playedKey(*cMajor, 0, 12.25, false) == util::heardKey(0, false, 0, 1.1225)); // D major
}

TEST_CASE("Key Sync: the smallest move to the master's key (or a compatible one)", "[util][keyshift]")
{
    using util::keySyncShift;
    // C major against A minor: the same Camelot number already (8B / 8A).
    CHECK(keySyncShift(0, 21) == 0);
    CHECK(keySyncShift(0, 0) == 0);
    // C major (8B) against F#m (11A): to A major (11B), 3 down.
    CHECK(keySyncShift(0, 18) == -3);
    CHECK(util::camelotCode(util::transposeKey(0, -3)) == "11B");
    // D minor (7A) against E minor (9A): 2 up either way (1 up or down gives 2A / 12A, not near 9A).
    CHECK(keySyncShift(14, 16) == 2);
    CHECK(keySyncShift(14, 16, true) == 2);
    // G major (9B) against C major (8B): one step round the wheel - compatible as it is.
    CHECK(keySyncShift(7, 0) == 5); // exact: G -> C is 5 up (or 7 down)
    CHECK(keySyncShift(7, 0, true) == 0);
    CHECK_FALSE(keySyncShift(-1, 0).has_value());
    CHECK_FALSE(keySyncShift(0, 24).has_value());
    // Every pair of keys has a move within +-6 semitones.
    for (int a = 0; a < 24; ++a)
        for (int b = 0; b < 24; ++b)
        {
            const auto s = keySyncShift(a, b);
            REQUIRE(s.has_value());
            CHECK(std::abs(*s) <= 6);
            CHECK(util::camelotNumber(util::transposeKey(a, *s)) == util::camelotNumber(b));
        }
    // Compatible keys.
    CHECK(util::harmonicallyCompatible(0, 21));  // C / Am: same number
    CHECK(util::harmonicallyCompatible(0, 7));   // C / G: one step
    CHECK(util::harmonicallyCompatible(0, 5));   // C / F: one step back
    CHECK_FALSE(util::harmonicallyCompatible(0, 2)); // C / D: two steps
    CHECK_FALSE(util::harmonicallyCompatible(0, 16)); // 8B / 9A: other mode, other number
    // Tempo without key lock moves the key in whole semitones once it is closer to the next one.
    CHECK(util::semitonesFromRate(1.06) == 1);
    CHECK(util::semitonesFromRate(1.02) == 0);
    CHECK(util::semitonesFromRate(0.94) == -1);
    CHECK(util::heardKey(9, true, 2, 1.1) == 11);  // key lock: only the shift
    CHECK(util::heardKey(9, false, 2, 1.06) == 10); // no key lock: the tempo
}

TEST_CASE("The deck header shows the key as heard", "[ui][keyshift]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::TrackBuffer track;
    track.sampleRate = 48000.0;
    track.audio.setSize(2, 48000 * 10);
    track.audio.clear();
    engine::AudioEngine e;
    e.prepare(48000.0, 256);
    ui::DeckView deck(e, 0, [](engine::BufferId) { return nullptr; });
    float l[256], r[256];
    float* outs[] = {l, r};
    const auto step = [&]
    {
        e.process(outs, 2, 256);
        deck.refresh();
    };
    e.send(engine::LoadTrack{0, &track, 1});
    deck.setTrackKey(1, util::KeyNotation::Musical); // Db major
    step();
    CHECK(deck.keyText() == "Db");
    e.send(engine::SetKeyLock{0, true});
    e.send(engine::SetKeyShift{0, 2});
    step();
    CHECK(deck.keyText() == "Eb (+2)");
    e.send(engine::SetKeyShift{0, -1});
    step();
    CHECK(deck.keyText() == "C (-1)");
    deck.setTrackKey(1, util::KeyNotation::Camelot);
    CHECK(deck.keyText() == "8B (-1)");
    // Without key lock the tempo moves it: +6 % is a semitone up.
    e.send(engine::SetKeyLock{0, false});
    e.send(engine::SetTempo{0, 6.0});
    step();
    deck.setTrackKey(1, util::KeyNotation::Musical);
    CHECK(deck.keyText() == "D (+1)");
}
