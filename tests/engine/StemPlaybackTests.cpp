// P6-04: stem playback - a deck mixes its track's four stems where it reads (mute / solo / level / FX send),
// the engine owns and routes them, the loader reads the stem cache, and the CPU / RAM numbers for 2 decks x 4 stems.

#include "dsp/fx/Effect.h"
#include "engine/AudioEngine.h"
#include "engine/Deck.h"
#include "engine/StemLoader.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using namespace devildj;
using namespace devildj::engine;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kDrums = 0, kBass = 1, kOther = 2, kVocals = 3;

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

// Stems: drums a constant (so a cut would be a step), the others sines; the vocals swell and fade every 0.4 s
// (an envelope to line up with key lock); left and right differ a little.
float stemValue(int stem, int ch, int i)
{
    static constexpr double kFreq[] = {0.0, 110.0, 440.0, 1300.0};
    if (stem == kDrums)
        return ch == 0 ? 0.2f : 0.18f;
    const double swell = stem == kVocals ? 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / (0.4 * kRate)) : 1.0;
    return static_cast<float>(0.1 * swell * std::sin(2.0 * std::numbers::pi * kFreq[stem] * i / kRate + 0.3 * ch));
}

// RMS over windows of 240 samples (5 ms): a loudness envelope.
std::vector<double> envelope(const std::vector<float>& a, std::size_t from)
{
    std::vector<double> e;
    for (std::size_t i = from; i + 240 <= a.size(); i += 240)
    {
        double sum = 0.0;
        for (std::size_t k = i; k < i + 240; ++k)
            sum += static_cast<double>(a[k]) * a[k];
        e.push_back(std::sqrt(sum / 240.0));
    }
    return e;
}

std::unique_ptr<StemBuffer> makeStems(const TrackBuffer* track, int n)
{
    auto s = std::make_unique<StemBuffer>();
    s->length = n;
    s->sampleRate = kRate;
    s->track = track;
    for (int stem = 0; stem < StemBuffer::kStems; ++stem)
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& v = s->samples[static_cast<std::size_t>(stem * 2 + ch)];
            v.resize(static_cast<std::size_t>(n));
            for (int i = 0; i < n; ++i)
                v[static_cast<std::size_t>(i)] = toInt16(stemValue(stem, ch, i));
        }
    return s;
}

// The track the stems make with only `mask`'s stems (bit = stem), summed as the deck sums them.
std::unique_ptr<TrackBuffer> trackOf(const StemBuffer& stems, unsigned mask)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, stems.length);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < stems.length; ++i)
        {
            float v = 0.0f;
            for (int s = 0; s < StemBuffer::kStems; ++s)
                if (mask & (1u << s))
                    v += static_cast<float>(stems.channel(s, ch)[i]) * (1.0f / 32768.0f);
            t->audio.setSample(ch, i, v);
        }
    return t;
}

struct Render
{
    std::vector<float> l, r, dl, dr;
};

// `blocks` blocks of the deck (with a dry output when asked).
Render run(Deck& d, int blocks, bool dry = false)
{
    Render out;
    std::vector<float> l(kBlock), r(kBlock), dl(kBlock), dr(kBlock);
    for (int b = 0; b < blocks; ++b)
    {
        if (dry)
            d.render(l.data(), r.data(), dl.data(), dr.data(), kBlock);
        else
            d.render(l.data(), r.data(), kBlock);
        out.l.insert(out.l.end(), l.begin(), l.end());
        out.r.insert(out.r.end(), r.begin(), r.end());
        if (dry)
        {
            out.dl.insert(out.dl.end(), dl.begin(), dl.end());
            out.dr.insert(out.dr.end(), dr.begin(), dr.end());
        }
    }
    return out;
}

double maxDiff(const std::vector<float>& a, const std::vector<float>& b, std::size_t from = 0)
{
    double m = 0.0;
    for (std::size_t i = from; i < std::min(a.size(), b.size()); ++i)
        m = std::max(m, static_cast<double>(std::abs(a[i] - b[i])));
    return m;
}

double rms(const std::vector<float>& a, std::size_t from = 0)
{
    double e = 0.0;
    for (std::size_t i = from; i < a.size(); ++i)
        e += static_cast<double>(a[i]) * a[i];
    return std::sqrt(e / static_cast<double>(std::max<std::size_t>(1, a.size() - from)));
}

struct Pair // a deck with stems and a reference deck with a track made of some of them
{
    std::unique_ptr<TrackBuffer> full;
    std::unique_ptr<StemBuffer> stems;
    Deck deck, reference;
    std::unique_ptr<TrackBuffer> refTrack;
    explicit Pair(int n = static_cast<int>(10 * kRate))
    {
        full = std::make_unique<TrackBuffer>();
        auto st = makeStems(nullptr, n);
        full = trackOf(*st, 0x0f);
        stems = makeStems(full.get(), n);
        deck.prepare(kRate, kBlock);
        reference.prepare(kRate, kBlock);
        deck.setTrack(full.get());
        deck.setStems(stems.get());
    }
    void referenceOf(unsigned mask)
    {
        refTrack = trackOf(*stems, mask);
        reference.setTrack(refTrack.get());
    }
};

constexpr std::size_t kSettled = 4 * kBlock; // past the 20 ms ramp (960 samples)

class Invert final : public dsp::fx::Effect
{
public:
    const char* name() const noexcept override { return "Invert (test)"; }
    void prepare(double, int) override {}
    void reset() noexcept override {}
    void process(float* l, float* r, int n, const dsp::fx::BeatContext&) noexcept override
    {
        for (int i = 0; i < n; ++i)
        {
            l[i] = -l[i];
            r[i] = -r[i];
        }
    }
};
} // namespace

TEST_CASE("Stem playback: with every stem at its default the deck plays the track itself, bit for bit", "[engine][stems]")
{
    Pair p;
    p.referenceOf(0x0f);
    p.reference.setTrack(p.full.get()); // the very same track, without stems
    p.deck.play();
    p.reference.play();
    const auto a = run(p.deck, 40);
    const auto b = run(p.reference, 40);
    CHECK(a.l == b.l);
    CHECK(a.r == b.r);
    CHECK_FALSE(p.deck.readingStems());
}

TEST_CASE("Stem playback: mute, solo and level", "[engine][stems]")
{
    SECTION("mute the vocals: the other three")
    {
        Pair p;
        p.referenceOf(0x07);
        p.deck.setStemMute(kVocals, true);
        p.deck.play();
        p.reference.play();
        const auto a = run(p.deck, 40);
        const auto b = run(p.reference, 40);
        CHECK(p.deck.readingStems());
        CHECK(maxDiff(a.l, b.l, kSettled) < 1e-6);
        CHECK(maxDiff(a.r, b.r, kSettled) < 1e-6);
    }
    SECTION("solo the bass (a muted drum stays off; solo wins over mute)")
    {
        Pair p;
        p.referenceOf(1u << kBass);
        p.deck.setStemMute(kBass, true);
        p.deck.setStemSolo(kBass, true);
        p.deck.play();
        p.reference.play();
        const auto a = run(p.deck, 40);
        const auto b = run(p.reference, 40);
        CHECK(maxDiff(a.l, b.l, kSettled) < 1e-6);
        // Solo off again: back to the full track (without the muted bass... mute was set: bass stays out).
        p.deck.setStemSolo(kBass, false);
        p.referenceOf(0x0f & ~(1u << kBass));
        p.reference.seek(p.deck.position());
        p.reference.play();
        const auto c = run(p.deck, 40);
        const auto d = run(p.reference, 40);
        CHECK(maxDiff(c.l, d.l, kSettled) < 1e-6);
    }
    SECTION("level: half the other stem")
    {
        Pair p;
        p.deck.setStemLevel(kOther, 0.5f);
        p.deck.play();
        const auto a = run(p.deck, 40);
        // Expected: the full track less half the other stem.
        double worst = 0.0;
        for (std::size_t i = kSettled; i < a.l.size(); ++i)
        {
            const auto n = static_cast<int>(i);
            const float expect = p.full->audio.getSample(0, n) -
                                 0.5f * static_cast<float>(p.stems->channel(kOther, 0)[i]) / 32768.0f;
            worst = std::max(worst, static_cast<double>(std::abs(a.l[i] - expect)));
        }
        CHECK(worst < 1e-6);
    }
}

TEST_CASE("Stem playback: changes ramp (no click) and back at the defaults the deck reads the track again", "[engine][stems]")
{
    Pair p;
    p.deck.play();
    run(p.deck, 10);
    p.deck.setStemMute(kDrums, true); // drums are a constant 0.2: cut at once, that'd be a 0.2 step
    const auto a = run(p.deck, 20);
    double step = 0.0;
    for (std::size_t i = 1; i < a.l.size(); ++i)
        step = std::max(step, static_cast<double>(std::abs(a.l[i] - a.l[i - 1])));
    INFO("largest sample-to-sample step " << step);
    CHECK(step < 0.05); // the sines alone move up to ~0.035 a sample
    // The drums are gone after the ramp: the mean is the sines' (about 0).
    double mean = 0.0;
    for (std::size_t i = a.l.size() - 4800; i < a.l.size(); ++i)
        mean += a.l[i];
    CHECK(std::abs(mean / 4800.0) < 0.01);

    p.deck.setStemMute(kDrums, false);
    run(p.deck, 10);
    CHECK_FALSE(p.deck.readingStems()); // all defaults, ramps done: the track
}

TEST_CASE("Stem playback: a stem with its FX send off goes to the dry output", "[engine][stems]")
{
    Pair p;
    p.referenceOf(0x07);
    p.deck.setStemFx(kVocals, false);
    p.deck.play();
    p.reference.play();
    const auto a = run(p.deck, 40, true);
    const auto b = run(p.reference, 40);
    CHECK(p.deck.dryActive());
    CHECK(maxDiff(a.l, b.l, kSettled) < 1e-6); // the rest
    double worst = 0.0;                        // the vocals alone
    for (std::size_t i = kSettled; i < a.dl.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(a.dl[i]) - p.stems->channel(kVocals, 0)[i] / 32768.0));
    CHECK(worst < 1e-6);

    SECTION("without a dry output it stays in the main one")
    {
        p.deck.seek(0.0);
        run(p.deck, 4);
        const auto c = run(p.deck, 20);
        CHECK(rms(c.l) > 0.1);
    }
}

TEST_CASE("Stem playback: key lock - the dry output is stretched in step with the rest", "[engine][stems][keylock]")
{
    Pair p;
    Pair q;
    p.deck.setStemFx(kVocals, false);
    q.referenceOf(1u << kVocals); // the vocals alone, key-locked the same way
    for (auto* d : {&p.deck, &q.reference})
    {
        d->setKeyLock(true);
        d->setTempoPercent(8.0);
        d->play();
    }
    const auto a = run(p.deck, 200, true);
    const auto b = run(q.reference, 200);
    const std::size_t from = 40 * kBlock;
    CHECK(p.deck.dryActive());
    const double dry = rms(a.dl, from), alone = rms(b.l, from);
    INFO("dry " << dry << ", vocals alone " << alone);
    CHECK(std::abs(20.0 * std::log10(dry / alone)) < 1.0);
    // In step: the dry output's loudness follows the key-locked vocals' (the same latency; two stretchers may
    // differ in phase, so samples aren't compared). Off by 2 windows (10 ms) it would line up worse.
    const auto ea = envelope(a.dl, from), eb = envelope(b.l, from);
    const auto correlation = [&](int lag)
    {
        double m1 = 0, m2 = 0;
        const std::size_t count = ea.size() - 4;
        for (std::size_t i = 2; i < count; ++i)
        {
            m1 += ea[i];
            m2 += eb[static_cast<std::size_t>(static_cast<int>(i) + lag)];
        }
        m1 /= static_cast<double>(count - 2);
        m2 /= static_cast<double>(count - 2);
        double dot = 0, v1 = 0, v2 = 0;
        for (std::size_t i = 2; i < count; ++i)
        {
            const double x = ea[i] - m1, y = eb[static_cast<std::size_t>(static_cast<int>(i) + lag)] - m2;
            dot += x * y;
            v1 += x * x;
            v2 += y * y;
        }
        return dot / std::sqrt(v1 * v2);
    };
    const double c0 = correlation(0);
    INFO("envelope correlation " << c0 << " (2 windows off: " << correlation(2) << ")");
    CHECK(c0 > 0.98);
    CHECK(c0 > correlation(2));
    CHECK(c0 > correlation(-2));
}

TEST_CASE("Stem playback: loops wrap the stems with the playhead", "[engine][stems][loop]")
{
    Pair p;
    p.referenceOf(0x07);
    p.deck.setStemMute(kVocals, true);
    for (auto* d : {&p.deck, &p.reference})
    {
        d->seek(1000.0);
        d->setLoop(1000.0, 1000.0 + 0.5 * kRate);
        d->play();
    }
    const auto a = run(p.deck, 400); // ~2 s: four wraps
    const auto b = run(p.reference, 400);
    CHECK(p.deck.loopActive());
    CHECK(maxDiff(a.l, b.l, kSettled) < 1e-6);
    CHECK(p.deck.position() == p.reference.position());
}

TEST_CASE("Stem playback: the engine takes stems for the track on the deck and hands them back with it", "[engine][stems]")
{
    Pair p;
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    const auto released = [&]
    {
        std::vector<BufferId> ids;
        BufferId id = 0;
        while (e.popReleasedBuffer(id))
            ids.push_back(id);
        return ids;
    };
    e.send(LoadTrack{0, p.full.get(), 1});
    e.process(outs, 2, kBlock);

    auto other = makeStems(nullptr, 1000); // for a track that isn't on the deck
    e.send(LoadStems{0, p.full.get(), other.get(), 2});
    e.process(outs, 2, kBlock);
    CHECK(released() == std::vector<BufferId>{2});
    CHECK_FALSE(e.readState().decks[0].stemsLoaded);

    e.send(LoadStems{0, p.full.get(), p.stems.get(), 3});
    e.send(SetStem{0, kVocals, StemParam::Mute, 1.0f});
    e.send(SetStem{0, kBass, StemParam::Level, 0.25f});
    e.send(SetStem{0, kOther, StemParam::Fx, 0.0f});
    e.process(outs, 2, kBlock);
    const auto& s = e.readState().decks[0];
    CHECK(s.stemsLoaded);
    CHECK(s.stemsId == 3);
    CHECK(s.stemMute == (1u << kVocals));
    CHECK(s.stemFx == (0x0fu & ~(1u << kOther)));
    CHECK(s.stemLevel[kBass] == 0.25f);
    CHECK(released().empty());

    auto next = std::make_unique<TrackBuffer>();
    next->sampleRate = kRate;
    next->audio.setSize(2, 1000);
    next->audio.clear();
    e.send(LoadTrack{0, next.get(), 4});
    e.process(outs, 2, kBlock);
    const auto ids = released();
    CHECK(std::find(ids.begin(), ids.end(), 1) != ids.end()); // the old track
    CHECK(std::find(ids.begin(), ids.end(), 3) != ids.end()); // and its stems
    CHECK_FALSE(e.readState().decks[0].stemsLoaded);
    CHECK(e.readState().decks[0].stemMute == 0); // a new track starts at the defaults
}

TEST_CASE("Stem playback: stems with FX off skip the deck's effects, the rest go through", "[engine][stems][fx]")
{
    Pair p;
    AudioEngine e;
    e.prepare(kRate, kBlock);
    REQUIRE(e.installEffect(0, 0, std::make_unique<Invert>()));
    e.send(FxCommand{0, FxOp::Target, 0, 0, static_cast<float>(FxTarget::Deck0)});
    e.send(FxCommand{0, FxOp::Wet, 0, 0, 1.0f});
    e.send(FxCommand{0, FxOp::On, 0, 0, 1.0f});
    e.send(LoadTrack{0, p.full.get(), 1});
    e.send(LoadStems{0, p.full.get(), p.stems.get(), 2});
    e.send(SetStem{0, kVocals, StemParam::Fx, 0.0f});
    e.send(Play{0});
    std::vector<float> l(kBlock), r(kBlock), all;
    float* outs[] = {l.data(), r.data()};
    for (int b = 0; b < 60; ++b)
    {
        e.process(outs, 2, kBlock);
        all.insert(all.end(), l.begin(), l.end());
    }
    CHECK(e.readState().decks[0].stemsDry);
    // Where output sample i is in the track: the same deck through a plain engine, matched to the track.
    std::vector<float> plain;
    {
        AudioEngine q;
        q.prepare(kRate, kBlock);
        q.send(LoadTrack{0, p.full.get(), 1});
        q.send(Play{0});
        for (int b = 0; b < 60; ++b)
        {
            q.process(outs, 2, kBlock);
            plain.insert(plain.end(), l.begin(), l.end());
        }
    }
    const auto centre = static_cast<float>(crossfaderGainA(0.5, CrossfaderCurve::Smooth));
    int offset = -1;
    for (int k = 0; k < 2048 && offset < 0; ++k)
    {
        bool match = true;
        for (std::size_t i = 30 * kBlock; i < 32 * kBlock && match; ++i)
            match = std::abs(plain[i] - centre * p.full->audio.getSample(0, static_cast<int>(i) - k)) < 1e-5;
        if (match)
            offset = k;
    }
    REQUIRE(offset >= 0);
    // Out = crossfader x (vocals - the rest): the effect inverted all but the vocals.
    double worst = 0.0;
    for (std::size_t i = 30 * kBlock; i < all.size(); ++i)
    {
        const auto t = i - static_cast<std::size_t>(offset);
        float rest = 0.0f;
        for (int s = 0; s < 3; ++s)
            rest += p.stems->channel(s, 0)[t] / 32768.0f;
        const float vocals = p.stems->channel(kVocals, 0)[t] / 32768.0f;
        worst = std::max(worst, std::abs(static_cast<double>(all[i]) - centre * (vocals - rest)));
    }
    INFO("worst " << worst);
    CHECK(worst < 1e-4);
}

TEST_CASE("Stem loader: the cache's 44.1 kHz FLACs at the track's rate and length", "[engine][stems][loader]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-stemload", "");
    REQUIRE(dir.createDirectory());
    const int n = 44100;
    static constexpr const char* names[] = {"drums", "bass", "other", "vocals"};
    for (int s = 0; s < 4; ++s)
    {
        juce::AudioBuffer<float> audio(2, n);
        for (int i = 0; i < n; ++i)
            for (int ch = 0; ch < 2; ++ch)
                audio.setSample(ch, i, static_cast<float>(0.1 * (s + 1) * std::sin(2.0 * std::numbers::pi * 200.0 * i / 44100.0)));
        juce::FlacAudioFormat flac;
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(dir.getChildFile(juce::String(names[s]) + ".flac"));
        auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16));
        REQUIRE(w != nullptr);
        REQUIRE(w->writeFromAudioSampleBuffer(audio, 0, n));
    }
    TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, 48000);
    const auto loaded = loadStems(dir, track);
    REQUIRE(loaded);
    const auto& st = *loaded.value();
    CHECK(st.length == 48000);
    CHECK(st.sampleRate == kRate);
    CHECK(st.track == &track);
    for (int s = 0; s < 4; ++s)
    {
        double e = 0.0;
        for (int i = 2000; i < 46000; ++i)
        {
            const double v = st.channel(s, 0)[i] / 32768.0;
            e += v * v;
        }
        const double level = std::sqrt(e / 44000.0);
        CHECK(std::abs(level - 0.1 * (s + 1) / std::sqrt(2.0)) < 0.002);
    }
    CHECK_FALSE(loadStems(dir.getChildFile("nope"), track)); // missing files: an error, not a crash
    dir.deleteRecursively();
}

TEST_CASE("Stem playback: 2 decks x 4 stems with key lock - CPU and RAM (logged)", "[engine][stems][perf]")
{
    if (kDebugBuild || !juce::SystemStats::getEnvironmentVariable("DEVILDJ_SKIP_PERF", {}).isEmpty())
        SKIP((kDebugBuild ? "Release only: timings of an unoptimised build mean nothing" : "DEVILDJ_SKIP_PERF set"));
    const int n = static_cast<int>(40 * kRate);
    Pair a(n), b(n);
    AudioEngine e;
    e.prepare(kRate, kBlock);
    e.send(LoadTrack{0, a.full.get(), 1});
    e.send(LoadTrack{1, b.full.get(), 2});
    e.send(LoadStems{0, a.full.get(), a.stems.get(), 3});
    e.send(LoadStems{1, b.full.get(), b.stems.get(), 4});
    for (DeckId d = 0; d < 2; ++d)
    {
        e.send(SetKeyLock{d, true});
        e.send(SetTempo{d, d == 0 ? 3.0 : -2.0});
        e.send(Play{d});
    }
    e.send(SetStem{0, kVocals, StemParam::Mute, 1.0f}); // deck A: the stems mixed
    e.send(SetStem{1, kDrums, StemParam::Fx, 0.0f});    // deck B: and a dry output (its own stretcher)
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    for (int i = 0; i < 50; ++i)
        e.process(outs, 2, kBlock);
    REQUIRE(e.readState().decks[0].stemsActive);
    REQUIRE(e.readState().decks[1].stemsDry);
    const auto timeBlocks = [&](int count)
    {
        std::vector<double> ms;
        for (int i = 0; i < count; ++i)
        {
            const auto t0 = std::chrono::steady_clock::now();
            e.process(outs, 2, kBlock);
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        return std::pair{ms[ms.size() / 2], ms[ms.size() * 99 / 100]};
    };
    const auto [median, p99] = timeBlocks(4000); // ~21 s of audio
    const double budget = 1000.0 * kBlock / kRate;
    // The same two decks on their tracks (stems back at the defaults), for comparison.
    e.send(SetStem{0, 0, StemParam::Reset, 0.0f});
    e.send(SetStem{1, 0, StemParam::Reset, 0.0f});
    for (int i = 0; i < 20; ++i)
        e.process(outs, 2, kBlock);
    REQUIRE_FALSE(e.readState().decks[0].stemsActive);
    const auto [plainMedian, plainP99] = timeBlocks(2000);
    // RAM for 6-minute tracks at 48 kHz: the track (float) and its stems (16-bit) per deck.
    const double sixMin = 6.0 * 60.0 * kRate;
    const double trackMb = sixMin * 2 * sizeof(float) / 1e6, stemsMb = sixMin * 2 * 4 * sizeof(std::int16_t) / 1e6;
    CHECK(static_cast<double>(a.stems->memoryBytes()) == n * 2.0 * 4.0 * sizeof(std::int16_t));
    WARN("2 decks x 4 stems, key lock, block 256 @ 48 kHz (" << budget << " ms): median " << median << " ms, p99 "
         << p99 << " ms = " << 100.0 * p99 / budget << " % (tracks without stems: median " << plainMedian
         << " ms, p99 " << plainP99 << " ms = " << 100.0 * plainP99 / budget << " %). RAM per deck, 6-min track: "
         << trackMb << " MB track + " << stemsMb << " MB stems; 2 decks " << 2.0 * (trackMb + stemsMb) << " MB");
    CHECK(100.0 * p99 / budget < 60.0); // ARCHITECTURE §11: 4 decks + stems on 2 within 60 %
}


TEST_CASE("Stem playback: stems, the dry output and key lock don't allocate on the audio thread", "[engine][stems]")
{
    if (!devildj::util::kRealtimeGuardEnabled)
        SKIP("RealtimeGuard is only active in Debug builds");
    Pair p(static_cast<int>(4 * kRate));
    AudioEngine e;
    e.prepare(kRate, kBlock);
    std::vector<float> l(kBlock), r(kBlock);
    float* outs[] = {l.data(), r.data()};
    e.process(outs, 2, kBlock); // warm up
    const auto before = devildj::util::realtimeViolationCount();
    e.send(LoadTrack{0, p.full.get(), 1});
    e.send(LoadStems{0, p.full.get(), p.stems.get(), 2});
    e.send(Play{0});
    for (int i = 0; i < 60; ++i)
    {
        if (i == 5)
            e.send(SetStem{0, kVocals, StemParam::Mute, 1.0f});
        if (i == 15)
            e.send(SetStem{0, kDrums, StemParam::Fx, 0.0f}); // the dry output and its strip
        if (i == 25)
            e.send(SetKeyLock{0, true}); // the second stretcher
        if (i == 35)
            e.send(Seek{0, 50000.0});
        if (i == 45)
            e.send(SetStem{0, 0, StemParam::Reset, 0.0f});
        e.process(outs, 2, kBlock);
    }
    CHECK(devildj::util::realtimeViolationCount() == before);
}
