// P6-09: vocal presence - the vocal stem's curve, active windows, per phrase, the intro / outro stored.

#include "app/VocalPresence.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/TrackRepository.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <filesystem>

using namespace devildj;
using namespace devildj::app;

namespace
{
struct TempFolder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-vocals", "");
    TempFolder() { dir.createDirectory(); }
    ~TempFolder() { dir.deleteRecursively(); }
};

// A vocal stem: bleed at -50 dBFS throughout, singing (a 0.3 sine) where `sung` says (seconds).
void writeVocals(const juce::File& file, double seconds, std::vector<std::pair<double, double>> sung, double bleedDb = -50.0)
{
    const double rate = 44100.0;
    const auto n = static_cast<int>(seconds * rate);
    juce::AudioBuffer<float> audio(2, n);
    const double bleed = std::pow(10.0, bleedDb / 20.0) * std::sqrt(2.0);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / rate;
        bool on = false;
        for (const auto& [a, b] : sung)
            on = on || (t >= a && t < b);
        const double v = (on ? 0.3 : bleed) * std::sin(2.0 * 3.141592653589793 * 330.0 * t);
        audio.setSample(0, i, static_cast<float>(v));
        audio.setSample(1, i, static_cast<float>(v));
    }
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(16));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(audio, 0, n));
}
} // namespace

TEST_CASE("Vocal presence: the curve, the active windows, per phrase", "[app][vocals]")
{
    TempFolder t;
    writeVocals(t.dir.getChildFile("vocals.flac"), 10.0, {{2.0, 5.0}, {7.0, 8.0}});
    const auto curve = vocalCurveFromFile(t.dir.getChildFile("vocals.flac"));
    REQUIRE(curve);
    CHECK(curve->db.size() == 100);
    CHECK(std::abs(curve->db[30] - 20.0f * std::log10(0.3f / std::sqrt(2.0f))) < 0.5f); // a sine's RMS
    CHECK(curve->db[5] < -45.0f);
    const auto active = activeWindows(*curve);
    int on = 0;
    for (const bool a : active)
        on += a ? 1 : 0;
    CHECK(on >= 38); // 4 s of singing (40 windows), give or take the edges
    CHECK(on <= 42);
    CHECK_FALSE(active[10]);
    CHECK(active[30]);

    // Phrases: 0-2 s nothing, 2-6 s mostly, 6-10 s a quarter.
    const auto per = vocalPerPhrase(*curve, {{0.0, 8, 0.5}, {2.0, 8, 0.5}, {6.0, 8, 0.5}});
    REQUIRE(per.size() == 3);
    CHECK(per[0] == 0.0);
    CHECK(std::abs(per[1] - 0.75) < 0.05);
    CHECK(std::abs(per[2] - 0.25) < 0.05);
    const auto ends = vocalEnds(per);
    REQUIRE(ends);
    CHECK(ends->intro == 0.0);
    CHECK(std::abs(ends->outro - 0.25) < 0.05);
    CHECK_FALSE(vocalEnds({}));
}

TEST_CASE("Vocal presence: an instrumental's bleed isn't vocals; a missing stem is nothing", "[app][vocals]")
{
    TempFolder t;
    writeVocals(t.dir.getChildFile("vocals.flac"), 5.0, {}, -48.0); // only what a separation leaves behind
    const auto curve = vocalCurveFromFile(t.dir.getChildFile("vocals.flac"));
    REQUIRE(curve);
    for (const bool a : activeWindows(*curve))
        CHECK_FALSE(a);
    CHECK_FALSE(vocalCurveFromFile(t.dir.getChildFile("nope.flac")));
    CHECK_FALSE(ensureVocalCurve(t.dir.getChildFile("empty-folder")));
}

TEST_CASE("Vocal presence: vocal.json made once beside the stems and read back", "[app][vocals]")
{
    TempFolder t;
    writeVocals(t.dir.getChildFile("vocals.flac"), 3.0, {{1.0, 2.0}});
    const auto first = ensureVocalCurve(t.dir);
    REQUIRE(first);
    REQUIRE(t.dir.getChildFile("vocal.json").existsAsFile());
    t.dir.getChildFile("vocals.flac").deleteFile(); // from now on the cache answers
    const auto again = ensureVocalCurve(t.dir);
    REQUIRE(again);
    REQUIRE(again->db.size() == first->db.size());
    for (std::size_t i = 0; i < first->db.size(); ++i)
        CHECK(std::abs(again->db[i] - first->db[i]) <= 0.051f); // kept to 0.1 dB
}

TEST_CASE("Vocal presence: vocal_intro / vocal_outro stored next to the other features", "[app][vocals][library]")
{
    const auto dir = std::filesystem::temp_directory_path() /
                     ("devildj-vocaldb-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    {
        auto db = std::move(library::Database::open(dir / "library.db")).value();
        std::int64_t a = 0, b = 0;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                library::TrackRepository tracks(c);
                library::Track t;
                t.path = "C:\\x\\a.wav";
                t.contentHash = "a";
                a = tracks.add(t).value();
                t.path = "C:\\x\\b.wav";
                t.contentHash = "b";
                b = tracks.add(t).value();
                // b already has analysis features.
                REQUIRE(c.exec("INSERT INTO track_features (track_id, version, energy) VALUES (" + std::to_string(b) + ", 3, 0.7)"));
                CHECK(storeVocalEnds(c, a, {0.0, 0.8})); // no row yet: made
                CHECK(storeVocalEnds(c, b, {0.6, 0.1}));
                // The analysis writes its features later: the vocal values stay.
                REQUIRE(c.exec("INSERT INTO track_features (track_id, version, energy_features) VALUES (" + std::to_string(a) +
                               ", 2, x'00') ON CONFLICT(track_id) DO UPDATE SET version = excluded.version, "
                               "energy_features = excluded.energy_features"));
            });
        auto reader = std::move(db->openReader()).value();
        const auto read = [&](std::int64_t id, const char* column)
        {
            auto* s = reader->prepare(std::string("SELECT ") + column + " FROM track_features WHERE track_id = ?");
            REQUIRE(s != nullptr);
            REQUIRE(s->bind(1, id).step());
            const double v = s->getDouble(0);
            s->reset();
            return v;
        };
        CHECK(read(a, "vocal_intro") == 0.0);
        CHECK(read(a, "vocal_outro") == 0.8);
        CHECK(read(b, "vocal_intro") == 0.6);
        CHECK(read(b, "energy") == 0.7); // left alone
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
