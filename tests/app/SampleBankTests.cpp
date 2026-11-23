// P4-13: user samples. Done when: missing-sample handling tested.

#include "app/BeatRender.h"
#include "app/SampleBank.h"
#include "beatmaker/DrumKits.h"
#include "beatmaker/ProjectFile.h"
#include "engine/AudioEngine.h"
#include "ui/StepGrid.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>

using namespace devildj;

namespace
{
juce::File tempFolder(const char* name)
{
    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile(juce::String(name) + "-" + juce::String(std::chrono::steady_clock::now().time_since_epoch().count()));
    f.createDirectory();
    return f;
}

// A short decaying 200 Hz blip as a 24-bit WAV.
void writeBlip(const juce::File& file, double rate = 44100.0, int channels = 2)
{
    juce::AudioBuffer<float> b(channels, static_cast<int>(0.2 * rate));
    for (int i = 0; i < b.getNumSamples(); ++i)
        for (int c = 0; c < channels; ++c)
            b.setSample(c, i, static_cast<float>(0.8 * std::exp(-i / (0.03 * rate)) * std::sin(2.0 * 3.14159265 * 200.0 * i / rate)));
    file.getParentDirectory().createDirectory();
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(channels).withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
}
} // namespace

TEST_CASE("Samples: decoded once and kept; missing files found in the samples folder, else reported",
          "[app][samples]")
{
    const auto dir = tempFolder("devildj-samples");
    const auto samplesFolder = dir.getChildFile("Samples");
    samplesFolder.createDirectory();
    app::SampleBank bank(samplesFolder);
    const auto blip = dir.getChildFile("kit").getChildFile("blip.wav");
    writeBlip(blip);
    auto a = bank.get(blip.getFullPathName().toStdString());
    REQUIRE(a.data != nullptr);
    CHECK(a.data->numChannels == 2);
    CHECK(a.data->numFrames == static_cast<int>(0.2 * 44100.0));
    CHECK(a.data->sampleRate == 44100.0);
    CHECK_FALSE(a.relocated);
    CHECK(bank.get(blip.getFullPathName().toStdString()).data == a.data); // the same, not decoded again
    CHECK(bank.loadedCount() == 1);
    // Moved away: missing...
    const auto elsewhere = dir.getChildFile("gone").getChildFile("snare one.wav");
    auto missing = bank.get(elsewhere.getFullPathName().toStdString());
    CHECK(missing.data == nullptr);
    CHECK(missing.error.contains("snare one.wav"));
    CHECK(bank.isMissing(elsewhere.getFullPathName().toStdString()));
    // ... until a file of that name is in the samples folder (a sub-folder will do) and it is looked for again.
    writeBlip(samplesFolder.getChildFile("found").getChildFile("snare one.wav"), 48000.0, 1);
    CHECK(bank.isMissing(elsewhere.getFullPathName().toStdString())); // remembered as missing ...
    bank.retryMissing();                                               // ... until asked to look again
    auto relocated = bank.get(elsewhere.getFullPathName().toStdString());
    REQUIRE(relocated.data != nullptr);
    CHECK(relocated.relocated);
    CHECK(relocated.data->numChannels == 1);
    // Not a sound file / too long: refused with a reason.
    const auto text = dir.getChildFile("notes.wav");
    text.replaceWithText("this is not audio");
    auto bad = bank.get(text.getFullPathName().toStdString());
    CHECK(bad.data == nullptr);
    CHECK(bad.error.isNotEmpty());
    CHECK(bank.get("").data == nullptr);
    dir.deleteRecursively();
}

TEST_CASE("Samples: a track's sample plays; a missing one is silent and nothing breaks", "[app][samples]")
{
    const auto dir = tempFolder("devildj-samples-play");
    app::SampleBank bank(dir.getChildFile("Samples"));
    const auto blip = dir.getChildFile("blip.wav");
    writeBlip(blip, 48000.0);
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    auto& a = project.patterns[0].drums;
    beatmaker::setSample(a[0], blip.getFullPathName().toStdString(), "blip");
    beatmaker::setSample(a[1], dir.getChildFile("nowhere.wav").getFullPathName().toStdString(), "nowhere");
    for (int s = 0; s < 16; s += 4)
    {
        a[0].steps[static_cast<std::size_t>(s)].on = true; // the sample on the beats
        a[1].steps[static_cast<std::size_t>(s + 2)].on = true; // the missing one off the beats
    }
    app::BeatRenderOptions o;
    o.tailSeconds = 0.0;
    o.sample = [&](const std::string& path) { return bank.get(path).data; };
    const auto r = app::renderBeat(project, o);
    // The blip at each beat (every 24,000 samples at 120 BPM); silence where the missing one would be.
    const auto level = [&](int from, int to)
    {
        float peak = 0.0f;
        for (int i = from; i < to; ++i)
            peak = std::max(peak, std::abs(r.audio->audio.getSample(0, i)));
        return peak;
    };
    for (int beat = 0; beat < 4; ++beat)
    {
        CHECK(level(beat * 24000, beat * 24000 + 2000) > 0.1f);
        CHECK(level(beat * 24000 + 12000, beat * 24000 + 12500) < 1e-3f); // where the missing sample's steps are
    }
    // In the engine: the missing sample's events are counted as unhandled, no voice, no crash.
    engine::AudioEngine e;
    e.prepare(48000.0, 256);
    e.send(engine::SetSampleSound{1, nullptr});
    engine::ScheduledEvent ev;
    ev.kind = engine::VoiceKind::Sample;
    ev.soundIndex = 1;
    ev.sampleTime = engine::kNow;
    REQUIRE(e.beatMakerEvents().tryPush(ev));
    std::vector<float> l(256), rr(256);
    float* outs[] = {l.data(), rr.data()};
    e.process(outs, 2, 256);
    CHECK(e.readState().beatVoices == 0);
    // A project saved with a sample and opened elsewhere: the path is kept, the track says missing.
    const auto file = dir.getChildFile("song.ddjbeat");
    REQUIRE(beatmaker::saveProject(project, file.getFullPathName().toWideCharPointer()));
    blip.moveFileTo(dir.getChildFile("moved.wav"));
    app::SampleBank fresh(dir.getChildFile("Samples"));
    const auto loaded = beatmaker::loadProject(file.getFullPathName().toWideCharPointer());
    REQUIRE(loaded);
    const auto& path = loaded.value().patterns[0].drums[0].sound.samplePath;
    CHECK(path == blip.getFullPathName().toStdString());
    CHECK(fresh.isMissing(path));
    dir.deleteRecursively();
}

TEST_CASE("Samples on the grid: dropped on a row, chosen from the picker or Ctrl+O, MISSING shown", "[app][samples][ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto project = beatmaker::defaultProject();
    project.patterns[0].drums = beatmaker::kitTracks(beatmaker::drumKits()[0]);
    beatmaker::setSample(project.patterns[0].drums[2], "C:\\nowhere\\clap 9.wav", "clap 9");
    ui::StepGrid grid;
    grid.setBounds(0, 0, 1400, 420);
    grid.isSampleMissing = [](const std::string& p) { return p.find("nowhere") != std::string::npos; };
    grid.setProject(&project);
    CHECK(grid.soundBox(2).getText() == "MISSING: clap 9.wav");
    std::vector<std::pair<int, juce::String>> dropped;
    grid.onSampleFile = [&](int t, const juce::File& f) { dropped.push_back({t, f.getFileName()}); };
    CHECK(grid.isInterestedInFileDrag({"C:\\x\\a.wav"}));
    CHECK_FALSE(grid.isInterestedInFileDrag({"C:\\x\\a.mp4"}));
    const int y = grid.cellBounds(5, 0).getCentreY();
    grid.filesDropped({"C:\\x\\a.wav", "C:\\x\\notes.txt", "C:\\x\\b.flac"}, 300, y);
    CHECK(dropped == std::vector<std::pair<int, juce::String>>{{5, "a.wav"}, {6, "b.flac"}});
    int chosen = -1;
    grid.onChooseSample = [&](int t) { chosen = t; };
    grid.perform(ui::GlobalAction::BeatMakerTrack4);
    grid.perform(ui::GlobalAction::BeatMakerLoadSample);
    CHECK(chosen == 3);
    chosen = -1;
    grid.soundBox(1).setSelectedId(1000, juce::sendNotificationSync); // "Sample file..."
    CHECK(chosen == 1);
    CHECK(project.patterns[0].drums[1].sound.kind == beatmaker::SoundRef::Kind::Synth); // unchanged until a file
}
