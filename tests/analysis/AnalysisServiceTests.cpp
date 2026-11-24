// P2-12: analysis service. Done when: 500 tracks analyze in the background while two decks play with no xruns.

#include "PhraseEval.h"
#include "TestData.h"
#include "analysis/AnalysisService.h"
#include "analysis/WaveformCache.h"
#include "engine/AudioDeviceService.h"
#include "engine/AudioEngine.h"
#include "engine/TrackDecoder.h"
#include "../engine/FakeAudioDevice.h"
#include "../engine/MediaFoundationEncoder.h"
#include "library/Connection.h"
#include "library/Database.h"
#include "library/Importer.h"
#include "library/PhraseRepository.h"
#include "library/TrackRepository.h"
#include "util/RealtimeGuard.h"
#include "util/ThreadPriority.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

using namespace devildj;
using analysis::AnalysisService;
namespace fs = std::filesystem;

namespace
{
struct Lib
{
    fs::path dir;
    std::unique_ptr<library::Database> db;
    std::unique_ptr<library::Connection> reader;
    Lib()
    {
        dir = fs::temp_directory_path() /
              ("devildj-analysis-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        db = std::move(library::Database::open(dir / "library.db")).value();
        reader = std::move(db->openReader()).value();
    }
    ~Lib()
    {
        reader.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Tracks whose "path" names their length: "<seconds>s-<n>.wav" (the fake decoder reads it from the name).
    std::vector<std::int64_t> addFake(int count, double seconds, std::optional<double> lockedBpm = std::nullopt)
    {
        std::vector<std::int64_t> ids;
        db->writeAndWait(
            [&](library::Connection& c)
            {
                for (int i = 0; i < count; ++i)
                {
                    library::Track t;
                    static int serial = 0; // every path unique, also across calls
                    t.path = "C:\\Fake\\" + std::to_string(seconds) + "s-" + std::to_string(i) + "-" +
                             std::to_string(++serial) + ".wav";
                    t.contentHash = t.path;
                    if (lockedBpm)
                    {
                        t.bpm = *lockedBpm;
                        t.bpmLocked = true;
                    }
                    ids.push_back(library::TrackRepository(c).add(t).value());
                }
            });
        return ids;
    }
    library::Track get(std::int64_t id) { return *library::TrackRepository(*reader).get(id); }
};

// A decoder that makes up audio: a 440 Hz tone, length from the file name, 44.1 kHz. "fail" in the name fails.
struct FakeDecoder
{
    std::mutex mutex;
    std::vector<std::string> order;
    std::vector<int> priorities;

    analysis::Decoder make()
    {
        return [this](const fs::path& file, const std::atomic<bool>&) -> util::Result<analysis::DecodedAudio>
        {
            const auto name = file.filename().string();
            {
                std::lock_guard lock(mutex);
                order.push_back(name);
                priorities.push_back(util::currentThreadPriority());
            }
            if (name.find("fail") != std::string::npos)
                return util::Result<analysis::DecodedAudio>::fail("can't decode");
            const double seconds = std::stod(name.substr(0, name.find('s')));
            auto samples = std::make_shared<std::vector<float>>(static_cast<std::size_t>(seconds * 44100.0));
            for (std::size_t i = 0; i < samples->size(); ++i)
                (*samples)[i] = static_cast<float>(0.25 * std::sin(6.283185307 * 440.0 * i / 44100.0));
            analysis::DecodedAudio a;
            a.left = a.right = samples->data();
            a.numSamples = static_cast<int>(samples->size());
            a.sampleRate = 44100.0;
            a.owner = samples;
            return util::Result<analysis::DecodedAudio>::ok(std::move(a));
        };
    }
};

// The app's adapter: the engine's decoder.
util::Result<analysis::DecodedAudio> realDecoder(const fs::path& file, const std::atomic<bool>& cancel)
{
    engine::DecodeOptions o;
    o.cancel = &cancel;
    auto d = engine::decodeTrack(juce::File(juce::String(file.wstring().c_str())), o);
    if (!d)
        return util::Result<analysis::DecodedAudio>::fail(d.error());
    std::shared_ptr<engine::TrackBuffer> b = std::move(d).value();
    analysis::DecodedAudio a;
    a.left = b->audio.getReadPointer(0);
    a.right = b->audio.getReadPointer(b->audio.getNumChannels() > 1 ? 1 : 0);
    a.numSamples = b->audio.getNumSamples();
    a.sampleRate = b->sampleRate;
    a.owner = b;
    return util::Result<analysis::DecodedAudio>::ok(std::move(a));
}

std::string readBytes(const fs::path& f)
{
    std::ifstream in(f, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
} // namespace

TEST_CASE("Analysis service: queue, workers, results with the analysis version", "[analysis][service]")
{
    Lib lib;
    const auto ids = lib.addFake(20, 3.0);
    lib.db->writeAndWait([&](library::Connection& c) { CHECK(c.exec("UPDATE tracks SET path = 'C:\\Fake\\fail-1.wav' WHERE id = " + std::to_string(ids[5]))); });

    int notifications = 0;
    lib.db->addChangeListener([&](const std::vector<library::Change>&) { ++notifications; });

    FakeDecoder decoder;
    std::mutex monoMutex;
    std::vector<std::size_t> monoSizes;
    AnalysisService::Options options;
    options.workers = 3;
    options.analyzers = {&AnalysisService::basicsAnalyzer,
                         [&](const analysis::AnalysisInput& in, analysis::AnalysisResult&)
                         {
                             std::lock_guard lock(monoMutex);
                             monoSizes.push_back(in.mono.size());
                         }};
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        CHECK(service.queueOutdated() == 20);
        CHECK(service.queueOutdated() == 20); // repeats are ignored (already queued)
        REQUIRE(service.waitUntilIdle(60.0));
        const auto s = service.status();
        CHECK(s.done == 19);
        CHECK(s.failed == 1);
        CHECK(s.idle());
        CHECK(service.queueOutdated() == 0); // done; the failed one isn't retried at this version
    }

    for (const auto id : ids)
    {
        const auto t = lib.get(id);
        if (id == ids[5])
        {
            CHECK(t.analysisVersion == -analysis::kAnalysisVersion);
            continue;
        }
        CHECK(t.analysisVersion == analysis::kAnalysisVersion);
        CHECK(t.durationMs == 3000);
        CHECK(t.sampleRate == 44100);
    }
    // The mono signal is at 22.05 kHz.
    REQUIRE(monoSizes.size() == 19);
    for (const auto n : monoSizes)
        CHECK(std::abs(static_cast<double>(n) - 3.0 * 22050.0) < 2.0);
    // Workers run at the lowest priority.
    for (const int p : decoder.priorities)
        CHECK(p == util::lowestThreadPriority());
    // Results are written in batches, not one transaction per track.
    CHECK(notifications < 19);
}

TEST_CASE("Analysis service: writes each track's waveform cache (P2-17)", "[analysis][service][wfm]")
{
    Lib lib;
    const auto ids = lib.addFake(3, 4.0);
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            for (std::size_t i = 0; i < ids.size(); ++i)
                CHECK(c.exec("UPDATE tracks SET content_hash = 'feed00000000000" + std::to_string(i) + "' WHERE id = " +
                             std::to_string(ids[i])));
        });
    const auto cache = lib.dir / "cache";
    FakeDecoder decoder;
    AnalysisService::Options options;
    options.workers = 2;
    options.waveformCache = cache;
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        CHECK(service.queueOutdated() == 3);
        REQUIRE(service.waitUntilIdle(60.0));
    }
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const auto w = analysis::readWaveformCache(analysis::waveformCachePath(cache, "feed00000000000" + std::to_string(i)));
        REQUIRE(w.has_value());
        CHECK(w->sampleRate == 44100.0); // the file's rate
        CHECK(std::abs(w->numPoints() - 4.0 * 150.0) <= 1.0);
        // The fake decoder's tone is 440 Hz at 0.25: mid band.
        CHECK(std::abs(w->peak[analysis::Waveform::Mid][300] - 0.25f) < 0.03f);
    }
    // Hashes that aren't plain hex-like names (the fake library's paths) don't become files.
    lib.addFake(1, 2.0);
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        service.queueOutdated();
        REQUIRE(service.waitUntilIdle(60.0));
    }
    CHECK(std::distance(fs::directory_iterator(cache), fs::directory_iterator()) == 3);
}

TEST_CASE("Analysis service: locked BPM and grids are left alone; prioritize goes first", "[analysis][service]")
{
    Lib lib;
    const auto free = lib.addFake(1, 2.0);
    const auto locked = lib.addFake(1, 2.0, 99.0);
    lib.db->writeAndWait(
        [&](library::Connection& c)
        {
            CHECK(c.exec("INSERT INTO beatgrids (track_id, anchors_json, locked) VALUES (" + std::to_string(locked[0]) +
                         ", '[\"mine\"]', 1)"));
        });
    FakeDecoder decoder;
    AnalysisService::Options options;
    options.workers = 1;
    options.analyzers = {&AnalysisService::basicsAnalyzer,
                         [](const analysis::AnalysisInput&, analysis::AnalysisResult& r)
                         {
                             r.bpm = 128.0;
                             r.beatgridJson = "[\"analyzed\"]";
                             r.musicalKey = 9;
                         }};
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        service.setPaused(true);
        const auto later = lib.addFake(3, 1.0);
        for (const auto id : later)
            service.enqueue(id);
        service.enqueue(free[0]);
        service.enqueue(locked[0]);
        service.prioritize(locked[0]); // a deck just loaded it
        CHECK(service.status().queued == 5);
        service.setPaused(false);
        REQUIRE(service.waitUntilIdle(60.0));
    }
    REQUIRE_FALSE(decoder.order.empty());
    CHECK(decoder.order.front().rfind("2.000000s", 0) == 0); // the prioritized one first
    CHECK(lib.get(free[0]).bpm == 128.0);
    CHECK(lib.get(locked[0]).bpm == 99.0); // locked
    CHECK(lib.get(locked[0]).musicalKey == 9); // other results still land
    CHECK(lib.reader->queryText("SELECT anchors_json FROM beatgrids WHERE track_id = " + std::to_string(free[0])) ==
          "[\"analyzed\"]");
    CHECK(lib.reader->queryText("SELECT anchors_json FROM beatgrids WHERE track_id = " + std::to_string(locked[0])) ==
          "[\"mine\"]");
}

TEST_CASE("Analysis service: stopping while busy keeps what finished", "[analysis][service]")
{
    Lib lib;
    lib.addFake(40, 5.0);
    FakeDecoder decoder;
    AnalysisService::Options options;
    options.workers = 2;
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        service.queueOutdated();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } // destroyed mid-way
    const auto done = lib.reader->queryInt("SELECT count(*) FROM tracks WHERE analysis_version = " + std::to_string(analysis::kAnalysisVersion)).value_or(-1);
    const auto left = lib.reader->queryInt("SELECT count(*) FROM tracks WHERE analysis_version = 0").value_or(-1);
    CHECK(done + left == 40);
    // The next start picks up the rest.
    AnalysisService service(*lib.db, decoder.make(), options);
    CHECK(service.queueOutdated() == left);
    REQUIRE(service.waitUntilIdle(60.0));
    CHECK(lib.reader->queryInt("SELECT count(*) FROM tracks WHERE analysis_version = " + std::to_string(analysis::kAnalysisVersion)) == 40);
}

namespace
{
// Imports kTracks copies of `source` (each made unique) and analyzes them while two decks play on a fake sound
// card at 48 kHz / 256 samples; checks that no audio block was late.
void analyzeWhileDecksPlay(const juce::File& source, int kTracks, bool keyLock)
{
    juce::ScopedJuceInitialiser_GUI gui;
    Lib lib;

    // The music: kTracks different MP3 files, imported like the app does (queued for analysis on the way).
    const auto bytes = readBytes(fs::path(source.getFullPathName().toWideCharPointer()));
    for (int i = 0; i < kTracks; ++i)
    {
        std::string id3(128, '\0');
        id3.replace(0, 3, "TAG");
        const auto title = "Track " + std::to_string(i);
        id3.replace(3, title.size(), title);
        const auto f = lib.dir / "Music" / (std::to_string(i) + ".mp3");
        fs::create_directories(f.parent_path());
        std::ofstream(f, std::ios::binary) << bytes << id3;
    }

    // Two decks playing with key lock on a fake sound card at 48 kHz / 256 samples.
    engine::AudioDeviceService audio;
    audio.addDeviceType(std::make_unique<test::FakeAudioDeviceType>("Fake", juce::StringArray{"Out"}));
    REQUIRE(audio.initialise({}).isEmpty());
    REQUIRE(audio.setSampleRate(48000.0).isEmpty());
    REQUIRE(audio.setBufferSize(256).isEmpty());
    engine::AudioEngine engine;
    std::vector<std::unique_ptr<engine::TrackBuffer>> decks;
    for (int d = 0; d < 2; ++d)
    {
        auto t = std::make_unique<engine::TrackBuffer>();
        t->sampleRate = 48000.0;
        t->audio.setSize(2, 48000 * 300);
        for (int i = 0; i < t->audio.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                t->audio.setSample(ch, i, static_cast<float>(0.2 * std::sin(6.283185307 * (220.0 + 110 * d) * i / 48000.0)));
        engine.send(engine::LoadTrack{static_cast<engine::DeckId>(d), t.get(), static_cast<engine::BufferId>(d + 1)});
        engine.send(engine::SetKeyLock{static_cast<engine::DeckId>(d), keyLock});
        engine.send(engine::SetTempo{static_cast<engine::DeckId>(d), d == 0 ? 3.0 : -2.0});
        engine.send(engine::Play{static_cast<engine::DeckId>(d)});
        decks.push_back(std::move(t));
    }
    audio.setAudioCallback(&engine);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(engine.readState().decks[0].playing);
    REQUIRE(engine.readState().decks[1].playing);

    const auto before = engine.readState();
    const auto deviceBefore = audio.getStatus().xruns;
    const auto guardBefore = util::realtimeViolationCount();
    const auto started = std::chrono::steady_clock::now();
    {
        AnalysisService service(*lib.db, &realDecoder);
        library::ImportOptions import;
        import.queueAnalysis = [&service](std::int64_t id) { service.enqueue(id); };
        const std::vector<fs::path> inputs{lib.dir / "Music"};
        REQUIRE(library::importTracks(*lib.db, inputs, import).added == kTracks);
        REQUIRE(service.waitUntilIdle(300.0));
        CHECK(service.status().done == kTracks);
        CHECK(service.status().failed == 0);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto after = engine.readState();
    const auto deviceAfter = audio.getStatus().xruns;
    audio.setAudioCallback(nullptr);

    WARN(kTracks << " x " << source.getFileName() << " imported and analyzed in " << seconds
                 << " s while two decks played" << (keyLock ? " with key lock" : "") << "; "
                 << (after.blockCounter - before.blockCounter) << " audio blocks, peak callback load "
                 << 100.0f * after.cpuLoadPeak << " %");
    CHECK(lib.reader->queryInt("SELECT count(*) FROM tracks WHERE analysis_version = " +
                               std::to_string(analysis::kAnalysisVersion)) == kTracks);
    CHECK(after.xruns == before.xruns);      // no block over budget
    CHECK(deviceAfter == deviceBefore);      // the device never waited for us
    CHECK(util::realtimeViolationCount() == guardBefore);
    CHECK(after.decks[0].playing);
    CHECK(after.decks[1].playing);
    CHECK(after.blockCounter - before.blockCounter > static_cast<std::uint64_t>(seconds * 48000.0 / 256 * 0.8));
}
} // namespace

TEST_CASE("Analysis service: 500 tracks while two decks play, no xruns", "[analysis][service][glitch]")
{
    const auto mp3 = test::generatedFile("formats/ref.mp3");
    if (!mp3.existsAsFile() || !test::mediaFoundationAvailable())
        SKIP("Needs the generated MP3 and Media Foundation");
#if JUCE_DEBUG
    // Debug decodes several times slower and can't run two key-locked decks in real time even without analysis
    // (the key-lock CPU tests skip Debug too): 100 tracks, plain playback. Release: the full 500, key lock on.
    analyzeWhileDecksPlay(mp3, 100, false);
#else
    analyzeWhileDecksPlay(mp3, 500, true);
#endif
}

// Hidden (run by hand: DevilDJTests.exe "[.perf]"): full-length tracks - 20 copies of the 10-minute MP3 - so each
// worker holds a 10-minute decode (~230 MB) while the decks play.
TEST_CASE("Analysis service: 10-minute tracks while two decks play", "[analysis][service][.perf]")
{
    const auto mp3 = test::generatedFile("formats/long_10min.mp3");
    if (!mp3.existsAsFile() || !test::mediaFoundationAvailable())
        SKIP("Needs the 10-minute MP3 and Media Foundation");
    analyzeWhileDecksPlay(mp3, 20, true);
}

TEST_CASE("Analysis service: phrases are written, an owner's edited phrases stay (P6-06)", "[analysis][service][phrases]")
{
    Lib lib;
    const auto ids = lib.addFake(2, 3.0);
    const std::vector<library::StoredPhrase> found{{0.5, 8, 0.4}, {16.5, 16, 0.9}};
    FakeDecoder decoder;
    AnalysisService::Options options;
    options.workers = 1;
    options.analyzers = {&AnalysisService::basicsAnalyzer,
                         [&](const analysis::AnalysisInput&, analysis::AnalysisResult& r) { r.phrases = found; }};
    // The second track's phrases were edited by the owner before.
    const std::vector<library::StoredPhrase> mine{{1.0, 32, 0.5}};
    lib.db->writeAndWait([&](library::Connection& c) { CHECK(library::PhraseRepository(c).set(ids[1], mine, true)); });
    {
        AnalysisService service(*lib.db, decoder.make(), options);
        service.queueOutdated();
        REQUIRE(service.waitUntilIdle(60.0));
    }
    library::PhraseRepository repo(*lib.reader);
    const auto a = repo.get(ids[0]);
    CHECK(a.phrases == found);
    CHECK_FALSE(a.edited);
    const auto b = repo.get(ids[1]);
    CHECK(b.phrases == mine);
    CHECK(b.edited);
    CHECK(repo.editedTracks() == std::vector<std::int64_t>{ids[1]});
}

TEST_CASE("Analysis service: the phrase analyzer works on the beat analyzer's grid", "[analysis][service][phrases]")
{
    const auto t = test::makePhraseTrack(11);
    // The "file" at 44.1 kHz: the generated 22.05 kHz track doubled (only its grid and length matter here).
    auto full = std::make_shared<std::vector<float>>(t.mono.size() * 2);
    for (std::size_t i = 0; i < full->size(); ++i)
        (*full)[i] = t.mono[i / 2];
    analysis::DecodedAudio audio;
    audio.left = audio.right = full->data();
    audio.numSamples = static_cast<int>(full->size());
    audio.sampleRate = 44100.0;
    audio.owner = full;
    analysis::AnalysisInput in{audio, t.mono};
    analysis::AnalysisResult r;
    AnalysisService::phraseAnalyzer(in, r);
    CHECK_FALSE(r.phrases); // no grid yet: nothing
    r.beatgridJson = t.grid.rescaled(44100.0).toJson();
    AnalysisService::phraseAnalyzer(in, r);
    REQUIRE(r.phrases);
    REQUIRE(r.phrases->size() >= 4);
    CHECK(std::abs(r.phrases->front().startSeconds - t.grid.sampleAt(0.0) / t.rate) < 1e-3);
}
