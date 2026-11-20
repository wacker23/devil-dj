// P2-24: Track Matcher features v1 - energy, sound vector, groove - their storage (schema v2) and the library
// normalization. Done when: unit tests on generated signals (denser clicks -> higher energy; four-on-the-floor vs
// broken-beat clicks give clearly different groove vectors); library-percentile normalization recomputed after
// imports.

#include "analysis/FeatureNormalizer.h"
#include "analysis/TrackFeatures.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/TrackRepository.h"
#include "util/Beatgrid.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

using namespace devildj;
namespace fs = std::filesystem;

namespace
{
constexpr double kRate = 22050.0;

// Drums on a 16-step pattern per bar (x = hit): kick, snare, hats. `level` scales them all.
struct Pattern
{
    const char* kick;
    const char* snare;
    const char* hats;
};
constexpr Pattern kFourOnTheFloor{"x...x...x...x...", "....x.......x...", "..x...x...x...x."};
constexpr Pattern kBrokenBeat{"x.....x...x.....", ".....x.......x..", "x.x.x.x.x.x.x.x."};

std::vector<float> drums(const Pattern& p, double bpm, double seconds, double first, double level = 1.0, unsigned seed = 3)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const double step = 60.0 / bpm / 4.0;
    int i = 0;
    for (double t = first; t < seconds - 0.3; t += step, ++i)
    {
        const int s = i % 16;
        const int start = static_cast<int>(t * kRate);
        const auto add = [&](int length, auto voice)
        {
            for (int k = 0; k < length && start + k < static_cast<int>(out.size()); ++k)
                out[static_cast<std::size_t>(start + k)] += static_cast<float>(level * voice(k / kRate));
        };
        if (p.kick[s] == 'x')
            add(static_cast<int>(0.15 * kRate),
                [](double u) { return 0.8 * std::exp(-u / 0.05) * std::sin(2.0 * 3.14159265 * (50.0 * u + 1.5 * (1.0 - std::exp(-u / 0.03)))); });
        if (p.snare[s] == 'x')
            add(static_cast<int>(0.12 * kRate),
                [&](double u) { return std::exp(-u / 0.04) * (0.4 * noise(rng) + 0.3 * std::sin(2.0 * 3.14159265 * 200.0 * u)); });
        if (p.hats[s] == 'x')
        {
            float prev = 0.0f;
            add(static_cast<int>(0.03 * kRate),
                [&](double u)
                {
                    const float n = noise(rng);
                    const double v = 0.3 * std::exp(-u / 0.008) * (n - prev);
                    prev = n;
                    return v;
                });
        }
    }
    return out;
}

// 1 kHz clicks, `perBeat` of them per beat.
std::vector<float> clicks(double bpm, int perBeat, double seconds, double level)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    for (double t = 0.2; t < seconds - 0.1; t += 60.0 / bpm / perBeat)
    {
        const int start = static_cast<int>(t * kRate);
        for (int k = 0; k < 220 && start + k < static_cast<int>(out.size()); ++k)
            out[static_cast<std::size_t>(start + k)] +=
                static_cast<float>(level * std::exp(-k / 44.0) * std::sin(2.0 * 3.14159265 * 1000.0 * k / kRate));
    }
    return out;
}

util::Beatgrid gridFor(double bpm, double first)
{
    return util::Beatgrid::constant(first * kRate, bpm, kRate);
}

analysis::TrackFeatures features(const std::vector<float>& x, const util::Beatgrid& grid)
{
    return analysis::computeTrackFeatures(x.data(), static_cast<int>(x.size()), kRate, grid);
}

double meanRaw(const analysis::TrackFeatures& f, const analysis::EnergyStats& s)
{
    double sum = 0.0;
    for (const auto& w : f.windows)
        sum += analysis::rawEnergy(w, s);
    return f.windows.empty() ? 0.0 : sum / static_cast<double>(f.windows.size());
}
} // namespace

TEST_CASE("Energy: denser and louder clicks score higher; 16-bar windows", "[analysis][features]")
{
    const auto sparse = features(clicks(128.0, 1, 70.0, 0.3), gridFor(128.0, 0.2));
    const auto dense = features(clicks(128.0, 4, 70.0, 0.3), gridFor(128.0, 0.2));
    const auto loud = features(clicks(128.0, 4, 70.0, 0.9), gridFor(128.0, 0.2));
    // 70 s at 128 BPM from 0.2 s: two 16-bar windows (30 s each) and the 9.8 s left (over a quarter of a window: its
    // own); 63 s would end with 2.8 s, joined to the window before.
    REQUIRE(sparse.windows.size() == 3);
    CHECK(std::abs(sparse.windows[0].seconds - 30.0f) < 0.01f);
    CHECK(std::abs(sparse.windows[2].seconds - 9.8f) < 0.01f);
    const auto shorter = features(clicks(128.0, 1, 63.0, 0.3), gridFor(128.0, 0.2));
    REQUIRE(shorter.windows.size() == 2);
    CHECK(std::abs(shorter.windows[1].seconds - 32.8f) < 0.01f);
    CHECK(std::abs(sparse.windows[0].onsetRate - 128.0f / 60.0f) < 0.15f);
    CHECK(std::abs(dense.windows[0].onsetRate - 4 * 128.0f / 60.0f) < 0.5f);
    CHECK(loud.windows[0].loudnessDb > dense.windows[0].loudnessDb + 8.0f);
    CHECK(dense.windows[0].loudnessDb > sparse.windows[0].loudnessDb + 4.0f); // 4x the clicks: +6 dB of power
    std::vector<analysis::EnergyWindow> all;
    for (const auto* f : {&sparse, &dense, &loud})
        all.insert(all.end(), f->windows.begin(), f->windows.end());
    const auto stats = analysis::energyStats(all);
    CHECK(meanRaw(dense, stats) > meanRaw(sparse, stats));
    CHECK(meanRaw(loud, stats) > meanRaw(dense, stats));
    // Without a grid: 30 s windows.
    const auto noGrid = features(clicks(128.0, 1, 95.0, 0.3), util::Beatgrid());
    CHECK(noGrid.windows.size() == 3);
    CHECK_FALSE(noGrid.groove.has_value());
    // Silence: nothing to say.
    const std::vector<float> silence(static_cast<std::size_t>(10 * kRate), 0.0f);
    const auto quiet = features(silence, util::Beatgrid());
    CHECK_FALSE(quiet.sound.has_value());
    REQUIRE(quiet.windows.size() == 1);
    CHECK(quiet.windows[0].loudnessDb == -100.0f);
}

TEST_CASE("Groove: four-on-the-floor and broken beat are clearly different; tempo doesn't matter", "[analysis][features]")
{
    const auto four124 = features(drums(kFourOnTheFloor, 124.0, 40.0, 0.5), gridFor(124.0, 0.5));
    const auto four128 = features(drums(kFourOnTheFloor, 128.0, 40.0, 0.3, 1.0, 9), gridFor(128.0, 0.3));
    const auto broken = features(drums(kBrokenBeat, 124.0, 40.0, 0.5), gridFor(124.0, 0.5));
    REQUIRE(four124.groove.has_value());
    REQUIRE(four128.groove.has_value());
    REQUIRE(broken.groove.has_value());
    const double same = analysis::cosine(*four124.groove, *four128.groove);
    const double different = analysis::cosine(*four124.groove, *broken.groove);
    WARN("groove cosine: four-on-the-floor 124 vs 128 " << same << ", vs broken beat " << different);
    CHECK(same > 0.9);
    CHECK(different < 0.4);
    // Unit length; the kick band has its accents on the beats (positions 0, 4, 8, 12).
    double norm = 0.0;
    for (const float v : *four124.groove)
        norm += v * v;
    CHECK(std::abs(norm - 1.0) < 1e-4);
    const auto& g = *four124.groove;
    for (const int beat : {0, 4, 8, 12})
        CHECK(g[static_cast<std::size_t>(beat)] > g[static_cast<std::size_t>(beat + 2)]);
}

TEST_CASE("Sound vector: 32 numbers that tell a tone from noise and drums", "[analysis][features]")
{
    std::vector<float> tone(static_cast<std::size_t>(20 * kRate)), hiss(tone.size());
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    for (std::size_t i = 0; i < tone.size(); ++i)
    {
        tone[i] = static_cast<float>(0.3 * std::sin(2.0 * 3.14159265 * 440.0 * static_cast<double>(i) / kRate));
        hiss[i] = noise(rng);
    }
    const auto t = features(tone, util::Beatgrid());
    const auto n = features(hiss, util::Beatgrid());
    const auto d = features(drums(kFourOnTheFloor, 124.0, 20.0, 0.5), gridFor(124.0, 0.5));
    REQUIRE(t.sound.has_value());
    REQUIRE(n.sound.has_value());
    REQUIRE(d.sound.has_value());
    for (const float v : *t.sound)
        CHECK(std::isfinite(v));
    CHECK((*n.sound)[28] > (*t.sound)[28] + 20.0f);  // flatness (dB): noise is flat, a tone isn't
    CHECK((*n.sound)[26] > (*t.sound)[26] + 2.0f);   // centroid (log2 Hz): noise far brighter than 440 Hz
    CHECK((*d.sound)[29] > (*t.sound)[29] + 3.0f);   // onset rate: drums have onsets, a held tone none
    CHECK((*d.sound)[30] > (*n.sound)[30]);          // low ratio: the kick
    // The same signal gives the same vector.
    const auto again = features(tone, util::Beatgrid());
    CHECK(*again.sound == *t.sound);
}

TEST_CASE("Feature storage helpers: floats, windows, float16", "[analysis][features]")
{
    const std::vector<float> v{0.0f, 1.5f, -2.25f, 1e-3f, 12345.678f};
    CHECK(analysis::unpackFloats(analysis::packFloats(v)) == v);
    const std::vector<analysis::EnergyWindow> w{{0.0f, 30.0f, -12.5f, 2.1f, 0.4f, 1500.0f}, {30.0f, 28.0f, -9.0f, 8.0f, 0.5f, 2200.0f}};
    const auto back = analysis::unpackWindows(analysis::packWindows(w));
    REQUIRE(back.size() == 2);
    CHECK(back[1].centroidHz == 2200.0f);
    CHECK(back[0].loudnessDb == -12.5f);
    for (const float x : {0.0f, 1.0f, -1.0f, 0.5f, 0.333f, 0.999f, 65504.0f, 6.1e-5f, 1e-6f})
    {
        INFO(x);
        CHECK(std::abs(analysis::fromHalf(analysis::toHalf(x)) - x) <= std::abs(x) * 1e-3f + 1e-7f);
    }
    const std::vector<float> curve{0.1f, 0.25f, 0.9f};
    const auto h = analysis::unpackHalves(analysis::packHalves(curve));
    REQUIRE(h.size() == 3);
    CHECK(std::abs(h[2] - 0.9f) < 1e-3f);
}

TEST_CASE("Library normalization: energy percentiles over the library, recomputed when tracks are added",
          "[analysis][features][library]")
{
    const auto dir = fs::temp_directory_path() /
                     ("devildj-features-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto db = std::move(library::Database::open(dir / "library.db")).value();
    const auto add = [&](const std::vector<float>& signal, const char* name)
    {
        const auto f = features(signal, gridFor(128.0, 0.2));
        std::int64_t id = 0;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::Track t;
                t.path = std::string("C:\\Music\\") + name + ".wav";
                t.contentHash = name;
                id = library::TrackRepository(c).add(t).value();
                const auto windows = analysis::packWindows(f.windows);
                const auto sound = analysis::packFloats(*f.sound);
                auto* s = c.prepare("INSERT INTO track_features (track_id, version, energy_features, sound_v1) "
                                    "VALUES (?, ?, ?, ?)");
                REQUIRE(s != nullptr);
                CHECK(s->bind(1, id).bind(2, analysis::kFeatureVersion).bind(3, std::span<const std::uint8_t>(windows))
                          .bind(4, std::span<const std::uint8_t>(sound))
                          .run());
            });
        return id;
    };
    const auto energyOf = [&](std::int64_t id)
    {
        auto reader = std::move(db->openReader()).value();
        const auto text = reader->queryText("SELECT energy FROM track_features WHERE track_id = " + std::to_string(id));
        return text ? std::stod(*text) : -1.0;
    };
    const auto normalize = [&]
    {
        int n = 0;
        db->writeAndWait([&](library::Connection& c) { n = analysis::normalizeLibraryFeatures(c); });
        return n;
    };

    const auto sparse = add(clicks(128.0, 1, 40.0, 0.3), "sparse");
    const auto dense = add(clicks(128.0, 4, 40.0, 0.3), "dense");
    CHECK(normalize() == 2);
    const double sparseBefore = energyOf(sparse), denseBefore = energyOf(dense);
    CHECK(denseBefore > sparseBefore);
    CHECK(denseBefore <= 1.0);
    CHECK(sparseBefore >= 0.0);

    // More tracks come in, louder and denser: the percentiles move - the old tracks sit lower in the library now.
    add(clicks(128.0, 4, 40.0, 0.9), "loud");
    add(clicks(128.0, 8, 40.0, 0.9), "louder");
    CHECK(normalize() == 4);
    CHECK(energyOf(dense) < denseBefore);
    CHECK(energyOf(sparse) <= sparseBefore);
    // The curve, intro, outro and the library statistics are there.
    auto reader = std::move(db->openReader()).value();
    CHECK(reader->queryInt("SELECT count(*) FROM track_features WHERE energy_curve IS NOT NULL AND energy_intro IS "
                           "NOT NULL AND energy_outro IS NOT NULL") == 4);
    CHECK(reader->queryInt("SELECT count(*) FROM feature_stats WHERE name IN ('energy', 'sound_mean', 'sound_sd')") == 3);
    // Schema v2: the other Track Matcher tables exist too.
    for (const char* table : {"track_embeddings", "choice_log", "matcher_models", "track_neighbors"})
    {
        INFO(table);
        CHECK(reader->queryInt(std::string("SELECT count(*) FROM sqlite_master WHERE name = '") + table + "'") == 1);
    }
    reader.reset();
    db.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}
