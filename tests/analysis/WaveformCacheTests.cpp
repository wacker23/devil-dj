// P2-17: the waveform cache file (<content hash>.wfm) and the decks' preview while a track loads. Done when:
// loading an analyzed track shows its waveform in < 100 ms (the loader test in engine/TrackLoaderTests.cpp).

#include "TestData.h"
#include "analysis/WaveformBuilder.h"
#include "analysis/WaveformCache.h"
#include "engine/AudioEngine.h"
#include "engine/TrackBuffer.h"
#include "ui/DeckView.h"
#include "ui/WaveformView.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace devildj;
namespace fs = std::filesystem;

namespace
{
fs::path tempDir(const char* name)
{
    return fs::temp_directory_path() /
           (std::string("devildj-wfm-") + name + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// 20 s of a tone sweeping up with a slow tremolo: every band and level has something.
analysis::Waveform testWaveform(double rate)
{
    std::vector<float> x(static_cast<std::size_t>(20.0 * rate));
    double phase = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double t = static_cast<double>(i) / rate;
        phase += 2.0 * 3.141592653589793 * (50.0 * std::pow(200.0, t / 20.0)) / rate;
        x[i] = static_cast<float>((0.05 + 0.6 * (0.5 + 0.5 * std::sin(t * 3.0))) * std::sin(phase));
    }
    return analysis::buildWaveform(x.data(), x.data(), static_cast<int>(x.size()), rate);
}

// The stored level is sqrt(level) in 8 bits: compare in that domain.
void checkClose(const std::vector<float>& a, const std::vector<float>& b)
{
    REQUIRE(a.size() == b.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(std::sqrt(std::min(1.0f, a[i])) - std::sqrt(b[i]))));
    CHECK(worst <= 0.5 / 255.0 + 1e-6);
}
} // namespace

TEST_CASE("Waveform cache: write, read back, the same waveform to 8 bits", "[analysis][wfm]")
{
    const auto dir = tempDir("roundtrip");
    const auto w = testWaveform(44100.0);
    const auto file = analysis::waveformCachePath(dir, "0123456789abcdef");
    CHECK(file.filename() == "0123456789abcdef.wfm");
    REQUIRE(analysis::writeWaveformCache(file, w)); // creates the folder
    CHECK_FALSE(fs::exists(fs::path(file).concat(".tmp")));
    // ~0.9 KB per second: 6 bytes per point at 150 points per second, plus the overview.
    CHECK(fs::file_size(file) == 22 + 6 * static_cast<std::uintmax_t>(w.numPoints()) + 8 * 1024);

    const auto back = analysis::readWaveformCache(file);
    REQUIRE(back.has_value());
    CHECK(back->sampleRate == 44100.0);
    CHECK(back->samplesPerPoint == w.samplesPerPoint);
    CHECK(back->numPoints() == w.numPoints());
    for (int b = 0; b < analysis::Waveform::kBands; ++b)
    {
        checkClose(w.peak[static_cast<std::size_t>(b)], back->peak[static_cast<std::size_t>(b)]);
        checkClose(w.rms[static_cast<std::size_t>(b)], back->rms[static_cast<std::size_t>(b)]);
        checkClose(w.overviewPeak[static_cast<std::size_t>(b)], back->overviewPeak[static_cast<std::size_t>(b)]);
        checkClose(w.overviewRms[static_cast<std::size_t>(b)], back->overviewRms[static_cast<std::size_t>(b)]);
    }
    checkClose(w.overviewTotalPeak, back->overviewTotalPeak);
    checkClose(w.overviewTotalRms, back->overviewTotalRms);

    // Reading a 10-minute track's file is quick (the deck shows it as the load starts).
    {
        const auto longFile = analysis::waveformCachePath(dir, "long");
        analysis::Waveform big = w;
        for (auto* v : {&big.peak, &big.rms})
            for (auto& band : *v)
                band.resize(150 * 600, 0.25f);
        REQUIRE(analysis::writeWaveformCache(longFile, big));
        const auto started = std::chrono::steady_clock::now();
        const auto read = analysis::readWaveformCache(longFile);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        REQUIRE(read.has_value());
        CHECK(read->numPoints() == 90000);
        WARN("reading a 10-minute track's waveform: " << ms << " ms");
        CHECK(ms < 50.0);
    }

    // Not ours, another version, cut short, missing: nothing.
    const auto bytes = [&]
    {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    }();
    const auto variant = [&](const std::string& content)
    {
        const auto f = dir / "variant.wfm";
        std::ofstream(f, std::ios::binary | std::ios::trunc) << content;
        return analysis::readWaveformCache(f);
    };
    auto wrongMagic = bytes;
    wrongMagic[0] = 'X';
    auto wrongVersion = bytes;
    wrongVersion[4] = 99;
    CHECK_FALSE(variant(wrongMagic).has_value());
    CHECK_FALSE(variant(wrongVersion).has_value());
    CHECK_FALSE(variant(bytes.substr(0, bytes.size() - 1)).has_value());
    CHECK_FALSE(variant(bytes.substr(0, 10)).has_value());
    CHECK_FALSE(variant({}).has_value());
    CHECK_FALSE(analysis::readWaveformCache(dir / "missing.wfm").has_value());
    CHECK_FALSE(analysis::writeWaveformCache(dir / "empty.wfm", analysis::Waveform{}));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("Waveform cache: re-labelled for a deck at another rate", "[analysis][wfm]")
{
    // Written at the file's 44.1 kHz, shown on a 48 kHz deck: the same 150 points per second.
    auto w = testWaveform(44100.0);
    const auto points = w.numPoints();
    REQUIRE(analysis::retimeWaveform(w, 48000.0));
    CHECK(w.samplesPerPoint == 320);
    CHECK(w.sampleRate == 48000.0);
    CHECK(w.numPoints() == points);
    CHECK(std::abs(w.pointsPerSecond() - 150.0) < 1e-12);
    // The same as building it at 48 kHz, to a point.
    CHECK(std::abs(testWaveform(48000.0).numPoints() - points) <= 1);
    for (const double rate : {22050.0, 88200.0, 96000.0, 192000.0})
        CHECK(analysis::retimeWaveform(w, rate));
    // A rate without a whole number of samples per point at 150 / s: no.
    const auto before = w;
    CHECK_FALSE(analysis::retimeWaveform(w, 44000.0));
    CHECK(w.samplesPerPoint == before.samplesPerPoint);
    CHECK(w.sampleRate == before.sampleRate);
}

TEST_CASE("Decks show a loading track's cached waveform until the track is on the deck", "[ui][wfm]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::TrackBuffer track; // outlives the engine that will hold it
    engine::AudioEngine engine;
    engine.prepare(48000.0, 256);
    auto waveform = testWaveform(48000.0);
    const auto preview = std::make_shared<const analysis::Waveform>(waveform);

    ui::WaveformView view(engine, 0, [](engine::BufferId) { return nullptr; }, [] { return 0.0; });
    view.setSize(600, 120);
    const auto litPixels = [&]
    {
        juce::Image image(juce::Image::ARGB, 600, 120, true);
        {
            juce::Graphics g(image);
            view.paintEntireComponent(g, false);
        }
        int lit = 0;
        for (int x = 0; x < 600; x += 3)
            for (int y = 0; y < 120; y += 3)
                lit += image.getPixelAt(x, y).getSaturation() > 0.3f ? 1 : 0; // the band colours
        return lit;
    };
    CHECK(litPixels() == 0); // no track
    view.showPreview(preview);
    CHECK(view.hasPreview());
    CHECK(litPixels() > 50);

    ui::DeckView deck(engine, 0, [](engine::BufferId) { return nullptr; });
    deck.setSize(600, 200);
    deck.showPreview(preview, "Night Drive");
    CHECK(deck.hasPreview());

    // The track arrives on the deck: the previews end (the views draw the real track from then on).
    track.sampleRate = 48000.0;
    track.audio.setSize(2, 48000 * 20);
    track.audio.clear();
    track.waveform = waveform;
    engine.send(engine::LoadTrack{0, &track, 7});
    float l[256], r[256];
    float* outs[] = {l, r};
    engine.process(outs, 2, 256);
    litPixels(); // a frame
    deck.refresh();
    CHECK_FALSE(view.hasPreview());
    CHECK_FALSE(deck.hasPreview());
}
