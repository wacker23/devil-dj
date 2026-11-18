// P1-07: tempo and pitch. Done when: a 120 BPM click at +8 % measures 129.6 BPM +-0.05.

#include "engine/AudioEngine.h"
#include "engine/Deck.h"
#include "engine/TrackDecoder.h"

#include "TestData.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numeric>
#include <vector>

using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

std::unique_ptr<TrackBuffer> rampTrack(int n = 480000)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        t->audio.setSample(0, i, static_cast<float>(i) / n);
        t->audio.setSample(1, i, static_cast<float>(i) / n);
    }
    return t;
}

std::vector<float> renderLeft(Deck& d, int samples)
{
    std::vector<float> out;
    std::vector<float> l(kBlock), r(kBlock);
    for (int done = 0; done < samples; done += kBlock)
    {
        d.render(l.data(), r.data(), kBlock);
        out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

// Click onsets: first sample above the threshold after at least `gap` quiet samples. Sub-sample precision
// from linear interpolation of the threshold crossing.
std::vector<double> onsets(const std::vector<float>& x, float threshold = 0.05f, int gap = 2400)
{
    std::vector<double> result;
    int quiet = gap;
    for (std::size_t i = 1; i < x.size(); ++i)
    {
        if (std::abs(x[i]) > threshold)
        {
            if (quiet >= gap)
            {
                const float a = std::abs(x[i - 1]), b = std::abs(x[i]);
                result.push_back(static_cast<double>(i) - 1.0 + (threshold - a) / (b - a));
            }
            quiet = 0;
        }
        else
        {
            ++quiet;
        }
    }
    return result;
}

// Tempo from onset times by least squares (slope of onset index vs time).
double bpmFrom(const std::vector<double>& onsetSamples, double rate)
{
    const auto n = static_cast<double>(onsetSamples.size());
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (std::size_t k = 0; k < onsetSamples.size(); ++k)
    {
        const double x = static_cast<double>(k), y = onsetSamples[k];
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    const double samplesPerBeat = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    return 60.0 * rate / samplesPerBeat;
}
} // namespace

TEST_CASE("Tempo: a 120 BPM click at +8 % measures 129.6 BPM (+-0.05)", "[engine][tempo]")
{
    const auto file = devildj::test::generatedFile("clicks/click_120bpm.wav");
    if (!file.existsAsFile())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    DecodeOptions options;
    options.targetSampleRate = kRate; // 44.1 kHz file -> device rate, as the loader does
    auto decoded = decodeTrack(file, options);
    REQUIRE(decoded.isOk());
    const auto track = std::move(decoded).value();

    for (auto quality : {devildj::dsp::Resampler::Quality::Hermite, devildj::dsp::Resampler::Quality::Sinc16})
    {
        Deck d;
        d.prepare(kRate);
        d.setInterpolation(quality);
        d.setTrack(track.get());
        d.setTempoPercent(8.0);
        d.play();
        const auto out = renderLeft(d, static_cast<int>(25.0 * kRate)); // 27 s of track at 1.08x
        const auto clicks = onsets(out);
        REQUIRE(clicks.size() > 40);
        const double bpm = bpmFrom(clicks, kRate);
        INFO((quality == devildj::dsp::Resampler::Quality::Hermite ? "Hermite" : "Sinc16")
             << ": " << clicks.size() << " clicks, measured " << bpm << " BPM");
        CHECK(std::abs(bpm - 129.6) < 0.05);
    }
}

TEST_CASE("Tempo: other settings scale the BPM proportionally", "[engine][tempo]")
{
    const auto file = devildj::test::generatedFile("clicks/click_128bpm.wav");
    if (!file.existsAsFile())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    DecodeOptions options;
    options.targetSampleRate = kRate;
    auto decoded = decodeTrack(file, options);
    REQUIRE(decoded.isOk());
    const auto track = std::move(decoded).value();

    for (double percent : {-6.0, 0.0, 3.3, 16.0})
    {
        Deck d;
        d.prepare(kRate);
        d.setTempoRange(16.0);
        d.setTrack(track.get());
        d.setTempoPercent(percent);
        d.play();
        const double bpm = bpmFrom(onsets(renderLeft(d, static_cast<int>(20.0 * kRate))), kRate);
        INFO(percent << " % -> " << bpm << " BPM");
        CHECK(std::abs(bpm - 128.0 * (1.0 + percent / 100.0)) < 0.05);
    }
}

TEST_CASE("Tempo: the playhead advances at the rate", "[engine][tempo]")
{
    const auto t = rampTrack();
    Deck d;
    d.prepare(kRate);
    d.setTrack(t.get());
    d.setTempoPercent(-5.0);
    d.play();
    renderLeft(d, 100 * kBlock); // whole blocks: 51,200 output samples
    CHECK(std::abs(d.position() - 100.0 * kBlock * 0.95) < 1e-6);
    CHECK(d.rate() == 0.95);
}

TEST_CASE("Tempo: ranges clamp the fader; fine steps; reset", "[engine][tempo]")
{
    Deck d;
    d.prepare(kRate);
    CHECK(d.tempoRange() == 10.0); // default
    d.setTempoPercent(25.0);
    CHECK(d.tempoPercent() == 10.0);
    d.setTempoRange(50.0);
    d.setTempoPercent(25.0);
    CHECK(d.tempoPercent() == 25.0);
    d.setTempoRange(6.0); // narrowing clamps the current tempo
    CHECK(d.tempoPercent() == 6.0);
    d.setTempoRange(15.0); // snaps to the nearest real range
    CHECK(d.tempoRange() == 16.0);

    d.setTempoPercent(0.0);
    for (int i = 0; i < 7; ++i)
        d.nudgeTempo(0.1);
    CHECK(std::abs(d.tempoPercent() - 0.7) < 1e-12);
    d.nudgeTempo(-100.0);
    CHECK(d.tempoPercent() == -16.0);
    d.resetTempo();
    CHECK(d.tempoPercent() == 0.0);
}

TEST_CASE("Pitch bend is temporary and ramps instead of jumping", "[engine][tempo]")
{
    const auto t = rampTrack();
    Deck d;
    d.prepare(kRate);
    d.setTrack(t.get());
    d.play();

    d.setPitchBend(10.0); // clamped to 4
    renderLeft(d, kBlock);
    const double afterOneBlock = d.pitchBendPercent();
    CHECK(afterOneBlock > 0.0);
    CHECK(afterOneBlock < 4.0); // still ramping (40 %/s -> 0.43 % after 512 samples)
    renderLeft(d, static_cast<int>(0.2 * kRate));
    CHECK(d.pitchBendPercent() == 4.0);
    CHECK(std::abs(d.rate() - 1.04) < 1e-12);

    d.setPitchBend(0.0); // key released
    renderLeft(d, static_cast<int>(0.2 * kRate));
    CHECK(d.pitchBendPercent() == 0.0);
    CHECK(d.rate() == 1.0);
}

TEST_CASE("Tempo survives loading a new track", "[engine][tempo]")
{
    const auto a = rampTrack(1000), b = rampTrack(2000);
    Deck d;
    d.prepare(kRate);
    d.setTrack(a.get());
    d.setTempoPercent(4.0);
    d.setTrack(b.get());
    CHECK(d.tempoPercent() == 4.0);
}

TEST_CASE("Engine: tempo commands reach the deck and its published state", "[engine][tempo]")
{
    const auto t = rampTrack();
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.send(LoadTrack{0, t.get(), 1});
    e.send(SetTempoRange{0, 16.0});
    e.send(SetTempo{0, 12.0});
    e.send(NudgeTempo{0, 0.5});
    e.send(SetInterpolationQuality{InterpolationQuality::High});
    e.process(outs, 2, kBlock);
    auto s = e.readState().decks[0];
    CHECK(s.tempoRangePercent == 16.0);
    CHECK(std::abs(s.tempoPercent - 12.5) < 1e-12);
    CHECK(std::abs(s.rate - 1.125) < 1e-12);

    e.send(PitchBend{0, -4.0});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    e.send(Play{0}); // bend only slews while rendering a playing or paused deck: it's per sample
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    s = e.readState().decks[0];
    CHECK(s.pitchBendPercent == -4.0);

    e.send(ResetTempo{0});
    e.send(PitchBend{0, 0.0});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    s = e.readState().decks[0];
    CHECK(s.tempoPercent == 0.0);
    CHECK(s.rate == 1.0);
}
