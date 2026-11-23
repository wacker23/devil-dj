// P4-01: the Beat Maker model and the .ddjbeat file. Done when: round-trip tests.

#include "beatmaker/Model.h"
#include "beatmaker/ProjectFile.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

using namespace devildj;
using namespace devildj::beatmaker;
namespace fs = std::filesystem;

namespace
{
// Every field away from its default, random but valid.
Project busyProject(std::uint32_t seed)
{
    std::mt19937 rng(seed);
    const auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    const auto real = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    Project p = defaultProject();
    p.followMaster = false;
    p.ownBpm = 128.5;
    p.swing = 0.37f;
    p.seed = 4000000000u;
    p.song = {0, 1, 1, 7, 3};
    const int lengths[] = {16, 32, 64};
    const char* synths[] = {"kick", "snare", "clap", "closedHat", "openHat", "tomLow", "tomMid", "tomHigh", "rim",
                            "cowbell", "shaker"};
    for (int pi = 0; pi < kPatternCount; ++pi)
    {
        auto& pat = p.patterns[static_cast<std::size_t>(pi)];
        pat.drums.clear();
        pat.synths.clear();
        pat.lengthSteps = lengths[pi % 3];
        pat.stepsPerBeat = pi == 5 ? 3 : 4;
        for (int d = 0; d < (pi == 0 ? kMaxDrumTracks : pick(0, kMaxDrumTracks)); ++d)
        {
            auto t = drumTrack(synths[pick(0, 10)], pat.lengthSteps);
            t.name = "Drum \xc3\xa9 " + std::to_string(d); // UTF-8 survives
            if (d == 3)
            {
                t.sound.kind = SoundRef::Kind::Sample;
                t.sound.samplePath = "C:\\Samples\\snare one.wav";
            }
            t.sound.params = {{"tune", real(-12, 12)}, {"decay", real(0, 1)}, {"drive", real(0, 1)}};
            for (auto& s : t.steps)
            {
                s.on = pick(0, 1) == 1;
                s.velocity = static_cast<std::uint8_t>(pick(1, 127));
                s.probability = static_cast<std::uint8_t>(pick(0, 100));
                s.microTiming = static_cast<std::int8_t>(pick(-50, 50));
                s.ratchet = static_cast<std::uint8_t>(pick(1, 4));
            }
            t.volumeDb = real(-60, 12);
            t.pan = real(0, 1);
            t.delaySend = real(0, 1);
            t.reverbSend = real(0, 1);
            t.chokeGroup = static_cast<std::uint8_t>(pick(0, 4));
            t.mute = pick(0, 1) == 1;
            t.solo = pick(0, 1) == 1;
            pat.drums.push_back(t);
        }
        for (int s = 0; s < pick(0, kMaxSynthTracks); ++s)
        {
            SynthTrack t;
            t.name = "Synth " + std::to_string(s);
            t.preset.engine = static_cast<SynthPreset::Engine>(pick(0, 2));
            t.preset.name = "Preset " + std::to_string(pick(0, 9));
            t.preset.params = {{"cutoff", real(20, 20000)}, {"resonance", real(0, 1)}};
            for (int n = 0; n < pick(0, 40); ++n)
                t.notes.push_back({n * 24, pick(1, 384), static_cast<std::uint8_t>(pick(0, 127)),
                                   static_cast<std::uint8_t>(pick(1, 127))});
            t.arp.on = pick(0, 1) == 1;
            const int rates[] = {8, 16, 32};
            t.arp.rate = rates[pick(0, 2)];
            t.arp.mode = static_cast<ArpSettings::Mode>(pick(0, 4));
            t.arp.octaves = pick(1, 4);
            t.volumeDb = real(-60, 12);
            t.pan = real(0, 1);
            t.mute = pick(0, 1) == 1;
            pat.synths.push_back(t);
        }
    }
    normalize(p);
    return p;
}

fs::path tempFolder(const char* name)
{
    return fs::temp_directory_path() /
           (std::string(name) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}
} // namespace

TEST_CASE("Beat Maker model: the default project", "[beatmaker][model]")
{
    const auto p = defaultProject();
    CHECK(p.schemaVersion == kSchemaVersion);
    for (int i = 0; i < kPatternCount; ++i)
        CHECK(p.patterns[static_cast<std::size_t>(i)].name == std::string(1, static_cast<char>('A' + i)));
    REQUIRE(p.patterns[0].drums.size() == 8);
    CHECK(p.patterns[0].drums[0].name == "Kick");
    CHECK(p.patterns[0].drums[0].steps.size() == 16);
    CHECK(p.patterns[0].drums[3].chokeGroup == p.patterns[0].drums[4].chokeGroup); // the hats choke each other
    CHECK(p.patterns[0].synths.size() == 1);
    CHECK(p.patterns[0].stepTicks() == 24);
    CHECK(p.patterns[0].lengthTicks() == 16 * 24);
}

TEST_CASE("Beat Maker model: lengths and out-of-range values are brought into range", "[beatmaker][model]")
{
    Project p = defaultProject();
    auto& a = p.patterns[0];
    a.drums[0].steps[2].on = true;
    setPatternLength(a, 64);
    CHECK(a.drums[0].steps.size() == 64);
    CHECK(a.drums[0].steps[2].on);
    setPatternLength(a, 20);
    CHECK(a.lengthSteps == 32);
    a.drums[0].steps[0].velocity = 0;
    a.drums[0].steps[0].probability = 200;
    a.drums[0].steps[0].microTiming = -90;
    a.drums[0].steps[0].ratchet = 9;
    p.swing = 2.0f;
    p.song = {0, 9, -1, 2};
    a.synths[0].notes = {{96, 0, 60, 0}, {0, 10, 200, 100}};
    for (int i = 0; i < 4; ++i)
        a.drums.push_back(drumTrack("rim"));
    normalize(p);
    const auto& s = a.drums[0].steps[0];
    CHECK(s.velocity == 1);
    CHECK(s.probability == 100);
    CHECK(s.microTiming == -50);
    CHECK(s.ratchet == 4);
    CHECK(p.swing == kMaxSwing);
    CHECK(p.song == std::vector<int>{0, 2});
    CHECK(a.drums.size() == static_cast<std::size_t>(kMaxDrumTracks));
    CHECK(a.drums.back().steps.size() == 32);
    REQUIRE(a.synths[0].notes.size() == 2);
    CHECK(a.synths[0].notes[0].startTick == 0); // sorted
    CHECK(a.synths[0].notes[0].pitch == 127);
    CHECK(a.synths[0].notes[1].lengthTicks == 1);
    CHECK(a.synths[0].notes[1].velocity == 1);
}

TEST_CASE("Beat file: JSON round trip keeps every field", "[beatmaker][file]")
{
    for (const std::uint32_t seed : {1u, 2u, 3u, 42u})
    {
        const auto p = busyProject(seed);
        std::vector<std::string> warnings;
        const auto back = projectFromJson(projectToJson(p), &warnings);
        REQUIRE(back);
        CHECK(warnings.empty());
        CHECK(back.value() == p);
        // And again: the same text.
        CHECK(projectToJson(back.value()) == projectToJson(p));
    }
    const auto empty = projectFromJson(projectToJson(Project{}));
    REQUIRE(empty);
    CHECK(empty.value() == Project{});
}

TEST_CASE("Beat file: unknown fields from a newer version are kept on load and save", "[beatmaker][file]")
{
    auto json = projectToJson(busyProject(7));
    auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.c_str()));
    auto* root = parsed.getDynamicObject();
    root->setProperty("schemaVersion", 3);
    root->setProperty("futureTop", "hello");
    auto* pattern = root->getProperty("patterns")[0].getDynamicObject();
    pattern->setProperty("scale", "dorian");
    auto* drum = pattern->getProperty("drums")[0].getDynamicObject();
    drum->setProperty("lfo", juce::Array<juce::var>{1, 2, 3});
    drum->getProperty("sound").getDynamicObject()->setProperty("layer", "sub");
    auto* synth = root->getProperty("patterns")[1].getDynamicObject();
    (void)synth;
    std::vector<std::string> warnings;
    const auto loaded = projectFromJson(juce::JSON::toString(parsed).toStdString(), &warnings);
    REQUIRE(loaded);
    REQUIRE(warnings.size() == 1); // the newer format
    CHECK(loaded.value().schemaVersion == 3);
    // Saved again: every unknown field is still there.
    const auto again = juce::JSON::parse(juce::String::fromUTF8(projectToJson(loaded.value()).c_str()));
    CHECK(again.getProperty("futureTop", {}).toString() == "hello");
    CHECK(static_cast<int>(again.getProperty("schemaVersion", {})) == 3);
    const auto p0 = again.getProperty("patterns", {})[0];
    CHECK(p0.getProperty("scale", {}).toString() == "dorian");
    const auto d0 = p0.getProperty("drums", {})[0];
    CHECK(d0.getProperty("lfo", {}).size() == 3);
    CHECK(d0.getProperty("sound", {}).getProperty("layer", {}).toString() == "sub");
    // The known fields are unchanged.
    auto expected = busyProject(7);
    auto got = loaded.value();
    got.schemaVersion = expected.schemaVersion;
    got.unknown.clear();
    got.patterns[0].unknown.clear();
    got.patterns[0].drums[0].unknown.clear();
    got.patterns[0].drums[0].sound.unknown.clear();
    CHECK(got == expected);
}

TEST_CASE("Beat file: bad input is refused or repaired with a warning", "[beatmaker][file]")
{
    CHECK_FALSE(projectFromJson("not json at all {"));
    CHECK_FALSE(projectFromJson("[1, 2, 3]"));
    // Missing fields: defaults.
    const auto bare = projectFromJson("{}");
    REQUIRE(bare);
    CHECK(bare.value() == Project{});
    // Out of range: repaired, with a warning.
    std::vector<std::string> warnings;
    const auto r = projectFromJson(R"({"swing": 5, "patterns": [{"lengthSteps": 16, "drums": [{"name": "k",
        "steps": [[1, 300, 100, 0, 1]]}]}]})", &warnings);
    REQUIRE(r);
    CHECK_FALSE(warnings.empty());
    CHECK(r.value().swing == kMaxSwing);
    REQUIRE(r.value().patterns[0].drums.size() == 1);
    CHECK(r.value().patterns[0].drums[0].steps.size() == 16); // padded to the pattern's length
    CHECK(r.value().patterns[0].drums[0].steps[0].on);
    CHECK(r.value().patterns[0].drums[0].steps[0].velocity == 127);
}

TEST_CASE("Beat file: on disk, samples in the project's folder are stored relative to it", "[beatmaker][file]")
{
    const auto dir = tempFolder("devildj-beat");
    const auto file = dir / "My beat.ddjbeat";
    auto p = busyProject(11);
    const auto inside = (dir / "samples" / "kick 1.wav").make_preferred();
    p.patterns[0].drums[3].sound.samplePath = std::string(reinterpret_cast<const char*>(inside.u8string().c_str()));
    p.patterns[0].drums[2].sound.kind = SoundRef::Kind::Sample;
    p.patterns[0].drums[2].sound.samplePath = "D:\\Elsewhere\\clap.wav";
    REQUIRE(saveProject(p, file));
    // In the file: relative with forward slashes, and both listed in `samples`.
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto json = juce::JSON::parse(juce::String::fromUTF8(text.c_str()));
    const auto sound = json.getProperty("patterns", {})[0].getProperty("drums", {})[3].getProperty("sound", {});
    CHECK(sound.getProperty("sample", {}).toString() == "samples/kick 1.wav");
    const auto samples = json.getProperty("samples", {});
    REQUIRE(samples.isArray());
    bool listed = false;
    for (const auto& s : *samples.getArray())
        listed = listed || s.toString() == "samples/kick 1.wav";
    CHECK(listed);
    // Back: absolute again, everything equal.
    std::vector<std::string> warnings;
    const auto loaded = loadProject(file, &warnings);
    REQUIRE(loaded);
    CHECK(warnings.empty());
    CHECK(loaded.value() == p);
    // Saved over: still the same.
    REQUIRE(saveProject(loaded.value(), file));
    CHECK(loadProject(file).value() == p);
    CHECK_FALSE(loadProject(dir / "missing.ddjbeat"));
    std::error_code ec;
    fs::remove_all(dir, ec);
}
