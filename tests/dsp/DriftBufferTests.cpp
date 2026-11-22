// P2-25: the buffer between the master device and a separate headphone device - two clocks that drift apart.

#include "dsp/DriftBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using devildj::dsp::DriftBuffer;

namespace
{
struct Result
{
    std::uint32_t underruns = 0, overruns = 0;
    double worstFillError = 0.0; // after settling, relative to the target
    double frequency = 0.0;      // of the tone that came out, in real time
    double worstStep = 0.0;      // largest sample-to-sample change after settling
    bool channelsApart = true;   // the right channel stayed the left one's mirror
};

// A 1 kHz tone written at `writerRate` in `writerBlock` blocks; read in `readerBlock` blocks by a device whose real
// rate is `readerRate` x (1 + ppm / 1e6) - it says readerRate, but its clock runs fast or slow.
Result run(double writerRate, int writerBlock, double readerRate, int readerBlock, double ppm, double seconds)
{
    DriftBuffer b;
    b.setWriter(writerRate, writerBlock);
    b.setReader(readerRate, readerBlock);
    const double realReaderRate = readerRate * (1.0 + ppm / 1e6);
    std::vector<float> wl(static_cast<std::size_t>(writerBlock)), wr(wl.size());
    std::vector<float> rl(static_cast<std::size_t>(readerBlock)), rr(rl.size());
    double tw = 0.0, tr = 0.0; // next block of each, in seconds
    std::int64_t writtenFrames = 0, readFrames = 0;
    const double settle = 10.0;
    Result r;
    std::uint32_t underrunsAtSettle = 0, overrunsAtSettle = 0;
    bool settled = false;
    int crossings = 0;
    std::int64_t countedFrames = 0;
    float previous = 0.0f;
    bool havePrevious = false;
    while (tw < seconds || tr < seconds)
    {
        if (tw <= tr)
        {
            for (int i = 0; i < writerBlock; ++i)
            {
                const auto v = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * 1000.0 *
                                                                 static_cast<double>(writtenFrames + i) / writerRate));
                wl[static_cast<std::size_t>(i)] = v;
                wr[static_cast<std::size_t>(i)] = -v;
            }
            b.write(wl.data(), wr.data(), writerBlock);
            writtenFrames += writerBlock;
            tw += writerBlock / writerRate;
        }
        else
        {
            b.read(rl.data(), rr.data(), readerBlock);
            readFrames += readerBlock;
            tr += readerBlock / realReaderRate;
            if (!settled && tr > settle)
            {
                settled = true;
                underrunsAtSettle = b.underruns();
                overrunsAtSettle = b.overruns();
            }
            if (settled)
            {
                r.worstFillError = std::max(r.worstFillError, std::abs(b.fillFrames() / b.targetFrames() - 1.0));
                for (int i = 0; i < readerBlock; ++i)
                {
                    const float v = rl[static_cast<std::size_t>(i)];
                    r.channelsApart = r.channelsApart && rr[static_cast<std::size_t>(i)] == -v;
                    if (havePrevious)
                    {
                        r.worstStep = std::max(r.worstStep, static_cast<double>(std::abs(v - previous)));
                        crossings += (previous < 0.0f) != (v < 0.0f) ? 1 : 0;
                    }
                    previous = v;
                    havePrevious = true;
                    ++countedFrames;
                }
            }
        }
    }
    r.underruns = b.underruns() - underrunsAtSettle;
    r.overruns = b.overruns() - overrunsAtSettle;
    r.frequency = 0.5 * crossings / (static_cast<double>(countedFrames) / realReaderRate);
    return r;
}

void check(const Result& r, double readerRate)
{
    INFO("underruns " << r.underruns << ", overruns " << r.overruns << ", fill error " << r.worstFillError
                      << ", tone " << r.frequency << " Hz, worst step " << r.worstStep);
    CHECK(r.underruns == 0);
    CHECK(r.overruns == 0);
    CHECK(r.channelsApart);
    CHECK(r.worstFillError < 0.35);
    CHECK(std::abs(r.frequency - 1000.0) < 0.5); // the tone keeps its pitch through the drift correction
    // No clicks: a 1 kHz sine at 0.5 changes at most 2 pi f / rate x 0.5 per sample.
    CHECK(r.worstStep < 1.1 * 2.0 * 3.141592653589793 * 1000.0 / readerRate * 0.5);
}
} // namespace

TEST_CASE("Drift buffer: two devices whose clocks drift apart, for minutes", "[dsp][drift]")
{
#if JUCE_DEBUG
    constexpr double kSeconds = 60.0;
#else
    constexpr double kSeconds = 300.0;
#endif
    // The same rate, the headphone device 300 ppm fast or slow (a cheap USB headset is within ~100 ppm).
    for (const double ppm : {300.0, -300.0, 0.0})
    {
        INFO(ppm << " ppm");
        check(run(48000.0, 512, 48000.0, 480, ppm, kSeconds), 48000.0);
    }
    // Different rates and blocks: 48 kHz master, 44.1 kHz headphones.
    check(run(48000.0, 256, 44100.0, 441, 120.0, kSeconds), 44100.0);
    check(run(44100.0, 441, 48000.0, 1024, -80.0, kSeconds), 48000.0);
}

TEST_CASE("Drift buffer: silent until primed, the delay is the target, a stalled reader starts again", "[dsp][drift]")
{
    DriftBuffer b;
    b.setWriter(48000.0, 256);
    b.setReader(48000.0, 256);
    CHECK(b.targetFrames() == 1.5 * 512 + 128);
    std::vector<float> l(256, 0.25f), r(256, 0.25f), outL(256), outR(256);
    // Nothing written: silence, not playing.
    b.read(outL.data(), outR.data(), 256);
    CHECK_FALSE(b.playing());
    CHECK(outL[100] == 0.0f);
    // Written, then read: plays once the target is there.
    for (int i = 0; i < 4; ++i)
        b.write(l.data(), r.data(), 256);
    b.read(outL.data(), outR.data(), 256);
    CHECK(b.playing());
    CHECK(std::abs(outL[100] - 0.25f) < 1e-6f);
    // The reader stops (device closed) and the writer goes on: it drops what doesn't fit and counts it...
    for (int i = 0; i < 400; ++i)
        b.write(l.data(), r.data(), 256);
    CHECK(b.overruns() > 0);
    // ... and a restart primes again, from fresh audio only: the target's worth written after it.
    b.restart();
    b.read(outL.data(), outR.data(), 256);
    CHECK_FALSE(b.playing());
    b.write(l.data(), r.data(), 256);
    b.read(outL.data(), outR.data(), 256);
    CHECK_FALSE(b.playing());
    for (int i = 0; i < 3; ++i)
        b.write(l.data(), r.data(), 256);
    b.read(outL.data(), outR.data(), 256);
    CHECK(b.playing());
    // Reading faster than writing runs it dry: silence, counted, priming again.
    for (int i = 0; i < 10; ++i)
        b.read(outL.data(), outR.data(), 256);
    CHECK(b.underruns() > 0);
    CHECK_FALSE(b.playing());
    CHECK(outL[200] == 0.0f);
}

TEST_CASE("Drift buffer: 60 minutes with two devices - no under- or overflow, drift corrected", "[.long][dsp][drift]")
{
    // P3-22 (built as P2-25): an hour of the master device and a headphone device 200 ppm apart (0.72 s of drift
    // uncorrected), and an hour at 48 / 44.1 kHz.
    check(run(48000.0, 512, 48000.0, 480, 200.0, 3600.0), 48000.0);
    check(run(48000.0, 256, 44100.0, 441, -150.0, 3600.0), 44100.0);
}
