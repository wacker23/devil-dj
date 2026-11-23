// P4-04: drum synths. Done when: each sound has a parameter-sweep test (no NaN, bounded output) and a listening note.

#include "beatmaker/DrumKits.h"
#include "dsp/DrumSynth.h"
#include "engine/Voices.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>
#include <vector>

using namespace devildj;
using dsp::DrumKind;
using dsp::DrumParams;

namespace
{
constexpr double kRate = 48000.0;

struct Render
{
    std::vector<float> samples;
    bool finite = true;
    float peak = 0.0f;
    bool finished = false;
};

Render render(DrumKind kind, const DrumParams& p, float velocity = 1.0f, std::uint32_t seed = 1, double maxSeconds = 4.0)
{
    dsp::DrumVoice v;
    v.prepare(kRate);
    v.trigger(kind, p, velocity, seed);
    Render r;
    for (int i = 0; i < static_cast<int>(maxSeconds * kRate); ++i)
    {
        const float x = v.next();
        r.finite = r.finite && std::isfinite(x);
        r.peak = std::max(r.peak, std::abs(x));
        r.samples.push_back(x);
        if (v.finished())
        {
            r.finished = true;
            break;
        }
    }
    return r;
}

const char* kindName(DrumKind k)
{
    return dsp::drumKindName(k);
}
} // namespace

TEST_CASE("Drum synths: every sound, every parameter swept 0 .. 1 - finite, at most +6 dBFS, audible, ends",
          "[dsp][drums]")
{
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int k = 0; k < dsp::kDrumKinds; ++k)
    {
        const auto kind = static_cast<DrumKind>(k);
        std::vector<DrumParams> cases;
        // Each parameter on its own at 0, 0.25, 0.5, 0.75, 1 ...
        for (int f = 0; f < 8; ++f)
            for (const float value : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
            {
                DrumParams p;
                float* const fieldsOf[] = {&p.tune, &p.decay, &p.tone, &p.snap, &p.drive, &p.pitchAmount, &p.click, &p.spread};
                *fieldsOf[f] = value;
                cases.push_back(p);
            }
        // ... all at the extremes, out of range, and 20 at random.
        cases.push_back({0, 0, 0, 0, 0, 0, 0, 0});
        cases.push_back({1, 1, 1, 1, 1, 1, 1, 1});
        cases.push_back({-5, 7, -1, 2, 9, -3, 4, -2});
        for (int i = 0; i < 20; ++i)
            cases.push_back({u(rng), u(rng), u(rng), u(rng), u(rng), u(rng), u(rng), u(rng)});
        float loudest = 0.0f, quietest = 10.0f;
        for (const auto& p : cases)
        {
            const auto r = render(kind, p);
            INFO(kindName(kind) << " tune " << p.tune << " decay " << p.decay << " tone " << p.tone << " drive " << p.drive);
            CHECK(r.finite);
            CHECK(r.peak <= 2.0f); // +6 dBFS
            CHECK(r.finished);     // ends by itself (within 4 s)
            loudest = std::max(loudest, r.peak);
            quietest = std::min(quietest, r.peak);
        }
        INFO(kindName(kind));
        CHECK(quietest > 0.05f); // every setting makes a sound
        WARN(kindName(kind) << ": peaks " << quietest << " .. " << loudest);
    }
}

TEST_CASE("Drum synths: deterministic per seed; velocity scales; the presets and kits", "[dsp][drums]")
{
    const DrumParams p;
    const auto a = render(DrumKind::Snare, p, 1.0f, 7), b = render(DrumKind::Snare, p, 1.0f, 7),
               c = render(DrumKind::Snare, p, 1.0f, 8);
    CHECK(a.samples == b.samples);
    CHECK(a.samples != c.samples);
    const auto soft = render(DrumKind::Kick, p, 0.5f), loud = render(DrumKind::Kick, p, 1.0f);
    CHECK(std::abs(soft.peak - 0.5f * loud.peak) < 0.01f);
    // The kick's pitch falls: more zero crossings early than late.
    const auto crossings = [&](const std::vector<float>& s, int from, int to)
    {
        int n = 0;
        for (int i = from + 1; i < to; ++i)
            n += (s[static_cast<std::size_t>(i - 1)] < 0.0f) != (s[static_cast<std::size_t>(i)] < 0.0f);
        return n;
    };
    CHECK(crossings(loud.samples, 0, 2400) > 2 * crossings(loud.samples, 9600, 12000));
    // Names, presets (3-5 each), kits (5 of 8 sounds, every preset found).
    for (int k = 0; k < dsp::kDrumKinds; ++k)
    {
        const auto kind = static_cast<DrumKind>(k);
        CHECK(dsp::drumKindFromName(dsp::drumKindName(kind)) == kind);
        CHECK(beatmaker::drumPresets(kind).size() >= 3);
        CHECK(beatmaker::drumPresets(kind).size() <= 5);
        for (const auto& preset : beatmaker::drumPresets(kind))
            CHECK(render(kind, preset.params).peak <= 2.0f);
    }
    REQUIRE(beatmaker::drumKits().size() == 5);
    for (const auto& kit : beatmaker::drumKits())
    {
        for (const auto& s : kit.sounds)
        {
            INFO(kit.name << " " << s.synth << " " << s.preset);
            CHECK(beatmaker::findDrumPreset(dsp::drumKindFromName(s.synth), s.preset) != nullptr);
        }
        const auto tracks = beatmaker::kitTracks(kit);
        CHECK(tracks.size() == 8);
    }
    // Model -> engine: the named parameters.
    const auto tracks = beatmaker::kitTracks(beatmaker::drumKits()[3]); // Trap
    const auto sound = beatmaker::engineDrumSound(tracks[0].sound);
    CHECK(sound.kind == DrumKind::Kick);
    CHECK(sound.params.decay == 1.0f);
    CHECK(sound.params.drive == 0.15f);
}

TEST_CASE("Drum synths in the voices: 16 per source, one-shots, the hats choke", "[engine][voices][drums]")
{
    engine::Voices v;
    v.prepare(kRate, 256);
    v.setDrumSound(0, {DrumKind::OpenHat, {}});
    v.setDrumSound(1, {DrumKind::ClosedHat, {}});
    v.setDrumSound(2, {DrumKind::Kick, {}});
    engine::ScheduledEvent e;
    e.kind = engine::VoiceKind::DrumSynth;
    e.soundIndex = 2;
    e.durationSeconds = 0.01f; // a step: ignored, a drum plays out
    const auto kick = v.start(e, 0);
    std::vector<float> l(4800), r(4800);
    engine::VoiceBuses b{l.data(), r.data()};
    v.process(b, 4800);
    CHECK(v.playing(kick)); // still going after 100 ms
    for (int i = 0; i < 20; ++i)
        v.start(e, 0);
    CHECK(v.activeDrumVoices() == 16); // the Beat Maker's 16
    auto code = e;
    code.source = engine::SourceId::CodeDeck;
    v.start(code, 0);
    CHECK(v.activeDrumVoices() == 17); // the code deck has its own
    v.reset();
    e.soundIndex = 0;
    e.chokeGroup = 1;
    const auto open = v.start(e, 0);
    e.soundIndex = 1;
    v.start(e, 2400);
    v.process(b, 4800);
    CHECK_FALSE(v.playing(open));
}

TEST_CASE("Drum synths: render every preset to WAV for listening (set DEVILDJ_RENDER_DIR)", "[.listen][drums]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("DEVILDJ_RENDER_DIR", {});
    if (dir.isEmpty())
        return;
    const juce::File folder(dir);
    folder.createDirectory();
    for (int k = 0; k < dsp::kDrumKinds; ++k)
    {
        const auto kind = static_cast<DrumKind>(k);
        for (const auto& preset : beatmaker::drumPresets(kind))
        {
            const auto r = render(kind, preset.params);
            juce::AudioBuffer<float> buffer(1, static_cast<int>(r.samples.size()));
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(0, i, 0.5f * r.samples[static_cast<std::size_t>(i)]);
            const auto file = folder.getChildFile(juce::String(dsp::drumKindName(kind)) + " - " + preset.name + ".wav");
            file.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
            auto writer = wav.createWriterFor(
                out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
            REQUIRE(writer != nullptr);
            writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
        }
    }
}
