// P1-06: deck transport — CDJ cue state machine, end of track, and no clicks on any jump.

#include "engine/Deck.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using devildj::engine::Deck;
using devildj::engine::TrackBuffer;
using Transport = Deck::Transport;

namespace
{
constexpr double kRate = 48000.0;
constexpr double kToneHz = 1000.0;
constexpr float kAmp = 0.5f;

std::unique_ptr<TrackBuffer> sineTrack(double seconds = 2.0)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    const int n = static_cast<int>(seconds * kRate);
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        const auto v = static_cast<float>(kAmp * std::sin(2.0 * std::numbers::pi * kToneHz * i / kRate));
        t->audio.setSample(0, i, v);
        t->audio.setSample(1, i, v);
    }
    return t;
}

// Sample value = its own index / length, so the output tells us the read position.
std::unique_ptr<TrackBuffer> rampTrack(int n = 48000)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, n);
    for (int i = 0; i < n; ++i)
    {
        t->audio.setSample(0, i, static_cast<float>(i) / n);
        t->audio.setSample(1, i, -static_cast<float>(i) / n);
    }
    return t;
}

struct Rendered
{
    std::vector<float> left, right;
};

void render(Deck& d, int samples, Rendered* into = nullptr)
{
    std::vector<float> l(static_cast<std::size_t>(samples)), r(static_cast<std::size_t>(samples));
    d.render(l.data(), r.data(), samples);
    if (into != nullptr)
    {
        into->left.insert(into->left.end(), l.begin(), l.end());
        into->right.insert(into->right.end(), r.begin(), r.end());
    }
}

float maxStep(const std::vector<float>& x)
{
    float m = 0.0f;
    for (std::size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::abs(x[i] - x[i - 1]));
    return m;
}

bool allZero(const std::vector<float>& x, std::size_t from = 0)
{
    for (auto i = from; i < x.size(); ++i)
        if (x[i] != 0.0f)
            return false;
    return true;
}

// Steepest a pure sine of this amplitude/frequency ever moves per sample. A click is a step well above it.
const float kSineSlope = static_cast<float>(2.0 * std::numbers::pi * kToneHz * kAmp / kRate);
const float kClickThreshold = 1.5f * kSineSlope;

Deck prepared(const TrackBuffer& t)
{
    Deck d;
    d.prepare(kRate);
    d.setTrack(&t);
    return d;
}
} // namespace

// ---- cue logic (state machine) ----

TEST_CASE("Deck starts paused at 0 with the cue at 0", "[engine][deck][cue]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 0.0);
    CHECK(d.cuePoint() == 0.0);
    CHECK(d.length() == 48000);
    CHECK(d.isSilent());
}

TEST_CASE("Paused + cue: sets the cue at the playhead and previews while held", "[engine][deck][cue]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.seek(12000.0);
    d.cuePress();
    CHECK(d.cuePoint() == 12000.0);
    CHECK(d.transport() == Transport::CuePreview);

    render(d, 2000);
    CHECK(d.position() == 14000.0); // previewing advances

    d.cueRelease();
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 12000.0); // back to the cue point
}

TEST_CASE("Playing + cue: back to the cue point and pause; the release does nothing", "[engine][deck][cue]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.seek(6000.0);
    d.cuePress(); // cue = 6000
    d.cueRelease();
    d.play();
    render(d, 3000);
    CHECK(d.position() == 9000.0);

    d.cuePress();
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 6000.0);
    CHECK(d.cuePoint() == 6000.0);

    d.cueRelease(); // no preview after a back cue
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 6000.0);
}

TEST_CASE("Preview + play: keeps playing after the cue is released", "[engine][deck][cue]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.cuePress(); // preview from 0
    render(d, 1000);
    d.play();
    CHECK(d.transport() == Transport::Playing);
    d.cueRelease();
    CHECK(d.transport() == Transport::Playing);
    render(d, 1000);
    CHECK(d.position() == 2000.0);
}

TEST_CASE("A second back cue from playing returns to the same cue point", "[engine][deck][cue]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.seek(1000.0);
    d.cuePress();
    d.cueRelease();
    for (int i = 0; i < 3; ++i)
    {
        d.play();
        render(d, 500);
        d.cuePress();
        d.cueRelease();
        CHECK(d.position() == 1000.0);
        CHECK(d.cuePoint() == 1000.0);
    }
}

TEST_CASE("Play / pause / toggle", "[engine][deck]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.togglePlay();
    CHECK(d.transport() == Transport::Playing);
    render(d, 480);
    d.togglePlay();
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 480.0);
    render(d, 480);
    CHECK(d.position() == 480.0); // paused: doesn't move
    d.pause();
    CHECK(d.transport() == Transport::Paused);
}

TEST_CASE("End of track: stops at the end, then silence", "[engine][deck]")
{
    const auto t = rampTrack(4800);
    auto d = prepared(*t);
    d.seek(4000.0);
    d.play();
    Rendered out;
    render(d, 2000, &out);
    CHECK(d.isAtEnd());
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 4800.0);
    CHECK(allZero(out.left, 1000)); // nothing after the last sample (+ no tail beyond the end)

    d.play(); // at the end: nothing to play
    CHECK(d.transport() == Transport::Paused);

    d.seek(0.0); // seeking away clears the end state
    CHECK_FALSE(d.isAtEnd());
    d.play();
    CHECK(d.transport() == Transport::Playing);
}

TEST_CASE("Cue at the end of the track sets the cue but doesn't preview", "[engine][deck][cue]")
{
    const auto t = rampTrack(4800);
    auto d = prepared(*t);
    d.seek(4800.0);
    d.cuePress();
    CHECK(d.cuePoint() == 4800.0);
    CHECK(d.transport() == Transport::Paused);
    d.cueRelease();
}

TEST_CASE("Seek is clamped and moves the playhead", "[engine][deck]")
{
    const auto t = rampTrack(4800);
    auto d = prepared(*t);
    d.seek(-100.0);
    CHECK(d.position() == 0.0);
    d.seek(1e9);
    CHECK(d.position() == 4800.0);
    d.seek(1234.5);
    CHECK(d.position() == 1234.5);
}

TEST_CASE("Playback reads the right samples", "[engine][deck]")
{
    const auto t = rampTrack();
    auto d = prepared(*t);
    d.seek(24000.0);
    d.play();
    Rendered out;
    render(d, 400, &out);
    // After the 2 ms fade-in (96 samples), the output is exactly the track at 24000 + i.
    for (int i = 200; i < 400; ++i)
    {
        CHECK(out.left[static_cast<std::size_t>(i)] == t->audio.getSample(0, 24000 + i));
        CHECK(out.right[static_cast<std::size_t>(i)] == t->audio.getSample(1, 24000 + i));
    }
}

TEST_CASE("Empty deck ignores transport and renders silence", "[engine][deck]")
{
    Deck d;
    d.prepare(kRate);
    d.play();
    d.cuePress();
    d.seek(100.0);
    CHECK(d.transport() == Transport::Paused);
    Rendered out;
    render(d, 256, &out);
    CHECK(allZero(out.left));
    CHECK(d.isSilent());
}

TEST_CASE("Loading a new track resets the deck", "[engine][deck]")
{
    const auto a = rampTrack();
    const auto b = rampTrack(1000);
    auto d = prepared(*a);
    d.seek(500.0);
    d.cuePress();
    d.play();
    d.setTrack(b.get());
    CHECK(d.transport() == Transport::Paused);
    CHECK(d.position() == 0.0);
    CHECK(d.cuePoint() == 0.0);
    CHECK(d.length() == 1000);
}

// ---- declicking ----

TEST_CASE("Seeking while playing doesn't click (jump to the opposite phase)", "[engine][deck][click]")
{
    const auto t = sineTrack();
    auto d = prepared(*t);
    d.play();
    Rendered out;
    render(d, 1000, &out);
    // Playhead 1000 = phase 0 of the 1 kHz sine (48 samples per cycle). Jump to +24 + 12: a peak with the
    // opposite slope; without a fade the output would step by ~0.5 in one sample.
    d.seek(20000.0 + 36.0);
    render(d, 1000, &out);
    INFO("max step " << maxStep(out.left) << ", sine slope " << kSineSlope);
    CHECK(maxStep(out.left) <= kClickThreshold);
    CHECK(maxStep(out.right) <= kClickThreshold);
}

TEST_CASE("Play, pause and cue on a waveform peak don't click", "[engine][deck][click]")
{
    const auto t = sineTrack();
    auto d = prepared(*t);
    Rendered out;
    d.seek(12.0); // sine peak
    d.play();
    render(d, 500, &out);
    d.pause(); // pause mid-wave
    render(d, 500, &out);
    d.cuePress(); // preview from a peak-ish position
    render(d, 300, &out);
    d.cueRelease();
    render(d, 300, &out);
    d.play();
    render(d, 300, &out);
    d.cuePress(); // back cue while playing
    render(d, 300, &out);
    CHECK(maxStep(out.left) <= kClickThreshold);
}

TEST_CASE("Rapid jumps within one fade still don't click", "[engine][deck][click]")
{
    const auto t = sineTrack();
    auto d = prepared(*t);
    d.play();
    Rendered out;
    render(d, 200, &out);
    for (int i = 0; i < 8; ++i) // a new jump every 20 samples (fade is 96)
    {
        d.seek(1000.0 + 17.0 * i + (i % 2) * 24.0);
        render(d, 20, &out);
    }
    render(d, 500, &out);
    CHECK(maxStep(out.left) <= kClickThreshold);
}

TEST_CASE("Random transport fuzz never clicks and never goes silent-stuck", "[engine][deck][click]")
{
    const auto t = sineTrack(4.0);
    auto d = prepared(*t);
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> action(0, 6);
    std::uniform_int_distribution<int> blockSize(1, 300);
    std::uniform_real_distribution<double> where(0.0, 4.0 * kRate);
    Rendered out;
    for (int step = 0; step < 2000; ++step)
    {
        switch (action(rng))
        {
            case 0:
                d.play();
                break;
            case 1:
                d.pause();
                break;
            case 2:
                d.cuePress();
                break;
            case 3:
                d.cueRelease();
                break;
            case 4:
                d.seek(where(rng));
                break;
            case 5:
                d.togglePlay();
                break;
            default:
                break;
        }
        render(d, blockSize(rng), &out);
    }
    INFO("max step " << maxStep(out.left));
    CHECK(maxStep(out.left) <= kClickThreshold);
    CHECK(maxStep(out.right) <= kClickThreshold);
}

TEST_CASE("Deck goes silent within 2 ms after pausing", "[engine][deck][click]")
{
    const auto t = sineTrack();
    auto d = prepared(*t);
    d.play();
    render(d, 1000);
    d.pause();
    CHECK_FALSE(d.isSilent()); // fading out
    Rendered out;
    render(d, 96, &out);
    CHECK(d.isSilent());
    render(d, 100, &out);
    CHECK(allZero(out.left, 96));
}
