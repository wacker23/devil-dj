// P4-05: synths. Done when: tests + listening notes.

#include "beatmaker/SynthPresets.h"
#include "dsp/Synth.h"
#include "engine/Voices.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

using namespace devildj;
using dsp::SynthEngine;
using dsp::SynthParams;

namespace
{
constexpr double kRate = 48000.0;

struct Render
{
    std::vector<float> samples;
    bool finite = true, finished = false;
    float peak = 0.0f;
};

// A note held `gate` seconds, then let go; up to `maxSeconds` in all.
Render play(const SynthParams& p, float note, double gate = 0.5, double maxSeconds = 8.0, float glideFrom = -1.0f)
{
    dsp::SynthVoice v;
    v.prepare(kRate);
    v.noteOn(p, note, 1.0f, 3, glideFrom);
    Render r;
    const auto gateSamples = static_cast<int>(gate * kRate);
    for (int i = 0; i < static_cast<int>(maxSeconds * kRate); ++i)
    {
        if (i == gateSamples)
            v.noteOff();
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

// Zero crossings upwards per second over [from, to) seconds.
double frequency(const std::vector<float>& s, double from, double to)
{
    int n = 0;
    const auto a = static_cast<std::size_t>(from * kRate), b = std::min(s.size(), static_cast<std::size_t>(to * kRate));
    for (std::size_t i = a + 1; i < b; ++i)
        n += s[i - 1] < 0.0f && s[i] >= 0.0f;
    return n / (static_cast<double>(b - a) / kRate);
}

double rms(const std::vector<float>& s, double from, double to)
{
    double sum = 0.0;
    const auto a = static_cast<std::size_t>(from * kRate), b = std::min(s.size(), static_cast<std::size_t>(to * kRate));
    for (std::size_t i = a; i < b; ++i)
        sum += static_cast<double>(s[i]) * s[i];
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, b - a)));
}

// A plain sine from the subtractive engine: both oscillators sines, the filter wide open, no envelope on it.
SynthParams sine()
{
    SynthParams p;
    p.osc1Wave = p.osc2Wave = 3;
    p.osc2Detune = 0;
    p.cutoff = 20000;
    p.envAmount = 0;
    p.keyTrack = 0;
    p.amp = {0.001f, 0.1f, 1.0f, 0.05f};
    p.gain = 1.0f;
    return p;
}
} // namespace

TEST_CASE("Synths: every factory preset and every parameter swept - finite, at most +6 dBFS, audible, ends",
          "[dsp][synth]")
{
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (const auto engine : {SynthEngine::Subtractive, SynthEngine::Fm, SynthEngine::SubBass})
    {
        const auto& presets = beatmaker::factorySynthPresets(engine);
        REQUIRE(presets.size() == 10);
        std::vector<SynthParams> cases;
        for (const auto& f : presets)
            cases.push_back(f.params);
        // Every named parameter at its low and high ends on top of the first preset, and random mixes.
        const auto names = beatmaker::synthParamNames();
        const auto basePreset = beatmaker::synthPresetOf("x", presets.front().params);
        for (const char* name : names)
            for (const float value : {0.0f, 0.5f, 1.0f, 8.0f, 20000.0f})
            {
                auto preset = basePreset;
                preset.params[name] = value;
                auto p = beatmaker::synthParamsOf(preset);
                // Lengths stay sane (a 20,000 s release would not end within the test).
                for (auto* a : {&p.amp, &p.filterEnv, &p.indexEnv})
                {
                    a->attack = std::min(a->attack, 2.0f);
                    a->decay = std::min(a->decay, 3.0f);
                    a->release = std::min(a->release, 2.0f);
                }
                p.glide = std::min(p.glide, 1.0f);
                cases.push_back(p);
            }
        for (int i = 0; i < 15; ++i)
        {
            auto p = presets[static_cast<std::size_t>(i % 10)].params;
            p.cutoff = 50.0f + 15000.0f * u(rng);
            p.resonance = u(rng);
            p.envAmount = 2.0f * u(rng) - 1.0f;
            p.unison = 1.0f + 7.0f * u(rng);
            p.ratio = 0.25f + 8.0f * u(rng);
            p.index = 10.0f * u(rng);
            p.feedback = u(rng);
            p.drive = u(rng);
            p.harmonic2 = u(rng);
            cases.push_back(p);
        }
        int quiet = 0;
        for (std::size_t c = 0; c < cases.size(); ++c)
            for (const float note : {24.0f, 60.0f, 96.0f})
            {
                const auto r = play(cases[c], note);
                INFO("engine " << static_cast<int>(engine) << " case " << c << " note " << note);
                CHECK(r.finite);
                CHECK(r.peak <= 2.0f);
                CHECK(r.finished);
                quiet += r.peak < 0.005f;
            }
        // A few extreme settings may be near silent (a 20 Hz high-pass on a sub ...); the presets never are.
        CHECK(quiet < static_cast<int>(cases.size()));
        for (const auto& f : presets)
        {
            INFO(f.name);
            CHECK(play(f.params, 48.0f).peak > 0.05f);
        }
    }
}

TEST_CASE("Synths: pitch, glide, filter, unison, FM index", "[dsp][synth]")
{
    // A4 = 440 Hz; the sub-bass A1 = 55 Hz.
    CHECK(std::abs(frequency(play(sine(), 69.0f).samples, 0.1, 0.4) - 440.0) < 5.0);
    auto sub = beatmaker::factorySynthPresets(SynthEngine::SubBass)[0].params;
    CHECK(std::abs(frequency(play(sub, 33.0f).samples, 0.1, 0.45) - 55.0) < 3.0);
    // Glide: from A2 up to A3 in 0.2 s.
    auto g = sine();
    g.glide = 0.2f;
    const auto glide = play(g, 57.0f, 1.0, 2.0, 45.0f);
    CHECK(frequency(glide.samples, 0.0, 0.05) < 130.0);
    CHECK(std::abs(frequency(glide.samples, 0.4, 0.9) - 220.0) < 4.0);
    // A low-pass at 200 Hz on a saw at 1 kHz: much quieter than at 10 kHz; 24 dB quieter still.
    SynthParams saw;
    saw.envAmount = 0;
    saw.keyTrack = 0;
    saw.osc2Detune = 0;
    saw.amp = {0.001f, 0.1f, 1.0f, 0.05f};
    saw.cutoff = 10000;
    const double open = rms(play(saw, 83.0f).samples, 0.1, 0.4);
    saw.cutoff = 200;
    const double closed = rms(play(saw, 83.0f).samples, 0.1, 0.4);
    saw.filter24 = 1;
    const double closed24 = rms(play(saw, 83.0f).samples, 0.1, 0.4);
    CHECK(closed < 0.2 * open);
    CHECK(closed24 < 0.5 * closed);
    // Unison: 8 detuned copies, about as loud (1 / sqrt(n)) and not one waveform.
    saw.cutoff = 10000;
    saw.filter24 = 0;
    saw.unison = 8;
    saw.unisonDetune = 30;
    const auto wide = play(saw, 60.0f);
    CHECK(rms(wide.samples, 0.1, 0.4) > 0.3 * open);
    CHECK(rms(wide.samples, 0.1, 0.4) < 3.0 * open);
    // FM with the index at 0 is the carrier's sine.
    SynthParams fm;
    fm.engine = SynthEngine::Fm;
    fm.index = 0.0f;
    fm.amp = {0.001f, 0.1f, 1.0f, 0.05f};
    CHECK(std::abs(frequency(play(fm, 69.0f).samples, 0.1, 0.4) - 440.0) < 5.0);
    fm.index = 5.0f;
    fm.indexEnv = {0.001f, 0.1f, 1.0f, 0.1f};
    CHECK(frequency(play(fm, 69.0f).samples, 0.1, 0.4) > 500.0); // sidebands: more crossings
    // The same seed, the same sound.
    CHECK(play(beatmaker::factorySynthPresets(SynthEngine::Subtractive)[8].params, 60.0f).samples ==
          play(beatmaker::factorySynthPresets(SynthEngine::Subtractive)[8].params, 60.0f).samples);
}

TEST_CASE("Synth presets: model <-> engine round trip, JSON files", "[dsp][synth][beatmaker]")
{
    for (const auto engine : {SynthEngine::Subtractive, SynthEngine::Fm, SynthEngine::SubBass})
        for (const auto& f : beatmaker::factorySynthPresets(engine))
        {
            const auto model = beatmaker::synthPresetOf(f.name, f.params);
            CHECK(model.params.size() == beatmaker::synthParamNames().size());
            const auto back = beatmaker::synthParamsOf(model);
            CHECK(beatmaker::synthPresetOf(f.name, back).params == model.params);
            CHECK(back.engine == engine);
        }
    const auto dir = std::filesystem::temp_directory_path() /
                     ("devildj-synth-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto preset = beatmaker::synthPresetOf("My Acid", beatmaker::factorySynthPresets(SynthEngine::Subtractive)[2].params);
    REQUIRE(beatmaker::saveSynthPreset(preset, dir / "My Acid.json"));
    const auto loaded = beatmaker::loadSynthPreset(dir / "My Acid.json");
    REQUIRE(loaded);
    CHECK(loaded.value().name == "My Acid");
    CHECK(loaded.value().params == preset.params);
    CHECK_FALSE(beatmaker::loadSynthPreset(dir / "none.json"));
    CHECK(beatmaker::userPresetFolder().filename() == "Presets");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("Synths in the voices: the gate, glide between notes, 32 voices within budget", "[engine][voices][synth]")
{
    engine::Voices v;
    v.prepare(kRate, 256);
    v.setSynthSound(0, beatmaker::factorySynthPresets(SynthEngine::Subtractive)[5].params); // Pluck
    engine::ScheduledEvent e;
    e.kind = engine::VoiceKind::Synth;
    e.note = 60.0f;
    e.durationSeconds = 0.05f;
    const auto id = v.start(e, 0);
    std::vector<float> l(48000), r(48000);
    engine::VoiceBuses b{l.data(), r.data()};
    v.process(b, 24000);
    CHECK(v.playing(id) == false); // a pluck: over within half a second
    double energy = 0.0;
    for (int i = 0; i < 24000; ++i)
        energy += std::abs(l[static_cast<std::size_t>(i)]);
    CHECK(energy > 10.0);
    // 32 voices of the plain saw for 10 s.
    v.reset();
    v.setSynthSound(1, beatmaker::factorySynthPresets(SynthEngine::Subtractive)[0].params);
    v.setSynthSound(2, beatmaker::factorySynthPresets(SynthEngine::Subtractive)[1].params); // Supersaw (x8)
    const auto timeOf = [&](std::uint16_t sound)
    {
        v.reset();
        for (int i = 0; i < engine::Voices::kSynthVoices; ++i)
        {
            auto n = e;
            n.soundIndex = sound;
            n.note = 36.0f + static_cast<float>(i);
            n.durationSeconds = 20.0f;
            n.gain = 0.03f;
            v.start(n, i);
        }
        const auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < static_cast<int>(10.0 * kRate / 256); ++k)
        {
            std::fill(l.begin(), l.begin() + 256, 0.0f);
            std::fill(r.begin(), r.begin() + 256, 0.0f);
            v.process(b, 256);
        }
        CHECK(v.activeSynthVoices() == 32);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 10.0;
    };
    const double plain = timeOf(1), wide = timeOf(2);
    WARN("32 synth voices: plain saw " << plain * 100.0 << " %, supersaw x8 " << wide * 100.0 << " % of real time");
#ifdef NDEBUG
    CHECK(plain < 0.15);
#endif
}

TEST_CASE("Synths: render every factory preset to WAV for listening (set DEVILDJ_RENDER_DIR)", "[.listen][synth]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("DEVILDJ_RENDER_DIR", {});
    if (dir.isEmpty())
        return;
    const juce::File folder(dir);
    folder.createDirectory();
    const char* engines[] = {"subtractive", "fm", "subBass"};
    for (const auto engine : {SynthEngine::Subtractive, SynthEngine::Fm, SynthEngine::SubBass})
        for (const auto& f : beatmaker::factorySynthPresets(engine))
        {
            // A short phrase: C3, Eb3, G3, C4 (bass presets an octave lower), a beat each at 120 BPM.
            const float root = engine == SynthEngine::SubBass ? 36.0f : 48.0f;
            std::vector<float> all;
            float previous = -1.0f;
            for (const float step : {0.0f, 3.0f, 7.0f, 12.0f})
            {
                const auto r = play(f.params, root + step, 0.4, 0.5, previous);
                previous = root + step;
                all.insert(all.end(), r.samples.begin(), r.samples.end());
                all.resize(all.size() + static_cast<std::size_t>(0.5 * kRate) - r.samples.size(), 0.0f);
            }
            const auto tail = play(f.params, root, 0.4, 4.0);
            all.insert(all.end(), tail.samples.begin(), tail.samples.end());
            juce::AudioBuffer<float> buffer(1, static_cast<int>(all.size()));
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample(0, i, 0.7f * all[static_cast<std::size_t>(i)]);
            const auto file = folder.getChildFile(juce::String(engines[static_cast<int>(engine)]) + " - " + f.name + ".wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor(
                out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(1).withBitsPerSample(24));
            REQUIRE(writer != nullptr);
            writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
        }
}
