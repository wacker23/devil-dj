// P3-16: mix recording. Done when: 60-min recording has no gaps (compare sample count with wall clock); file opens in
// other software. The [.long] test records 60 minutes (offline, as fast as it goes) and checks every frame arrived.

#include "RecordingSheets.h"
#include "engine/AudioEngine.h"
#include "engine/RecordTap.h"
#include "engine/Recorder.h"
#include "engine/TrackBuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace devildj;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

float signalAt(std::int64_t i, int channel)
{
    return static_cast<float>(0.3 * std::sin(2.0 * 3.141592653589793 * (channel == 0 ? 441.0 : 523.0) * i / kRate));
}

// Push `frames` of the test signal (or silence) through the tap like an audio callback would, without overrunning
// it (the writer thread drains it meanwhile). `from` = the signal's sample index to start at.
void feed(engine::RecordTap& tap, std::int64_t from, std::int64_t frames, bool silent = false)
{
    std::vector<float> l(kBlock), r(kBlock);
    for (std::int64_t done = 0; done < frames;)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(kBlock, frames - done));
        for (int i = 0; i < n; ++i)
        {
            l[static_cast<std::size_t>(i)] = silent ? 0.0f : signalAt(from + done + i, 0);
            r[static_cast<std::size_t>(i)] = silent ? 0.0f : signalAt(from + done + i, 1);
        }
        while (tap.available() > static_cast<int>(kRate)) // keep under 1 s of the 2 s ring
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        tap.write(l.data(), r.data(), n);
        done += n;
    }
}

juce::File scratchFolder(const char* name)
{
    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory)
                 .getChildFile(juce::String("devildj-") + name + "-" + juce::String(juce::Time::getHighResolutionTicks()));
    f.createDirectory();
    return f;
}

std::unique_ptr<juce::AudioFormatReader> open(const juce::File& file)
{
    static juce::AudioFormatManager formats;
    if (formats.getNumKnownFormats() == 0)
        formats.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader>(formats.createReaderFor(file));
}

std::vector<juce::File> recordings(const juce::File& folder)
{
    auto list = folder.findChildFiles(juce::File::findFiles, false, "*.wav;*.flac");
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.getFileName() < b.getFileName(); });
    return {list.begin(), list.end()};
}
} // namespace

TEST_CASE("Record tap: frames in = frames out; a full ring drops and counts, never waits", "[engine][recorder]")
{
    engine::RecordTap tap;
    tap.prepare(kRate, 0.1); // 8192 frames
    float l[256], r[256], ol[256], or_[256];
    for (int i = 0; i < 256; ++i)
    {
        l[i] = static_cast<float>(i);
        r[i] = static_cast<float>(-i);
    }
    tap.write(l, r, 256); // not enabled: ignored
    CHECK(tap.available() == 0);
    tap.setEnabled(true);
    tap.write(l, r, 256);
    REQUIRE(tap.read(ol, or_, 256) == 256);
    CHECK(ol[255] == 255.0f);
    CHECK(or_[17] == -17.0f);
    for (int b = 0; b < 40; ++b) // 10240 frames into 8192
        tap.write(l, r, 256);
    CHECK(tap.available() == 8192);
    CHECK(tap.droppedFrames() == 2048);
}

TEST_CASE("Recorder: WAV and FLAC, every frame, the samples (24-bit), readable", "[engine][recorder]")
{
    for (const auto format : {engine::Recorder::Format::Wav24, engine::Recorder::Format::Flac24})
    {
        const bool flac = format == engine::Recorder::Format::Flac24;
        INFO((flac ? "FLAC" : "WAV"));
        engine::RecordTap tap;
        tap.prepare(kRate);
        engine::Recorder recorder(tap);
        const auto folder = scratchFolder("rec");
        engine::Recorder::FileInfo closed;
        recorder.onFileClosed = [&](const engine::Recorder::FileInfo& f) { closed = f; };
        engine::Recorder::Options options;
        options.folder = folder;
        options.format = format;
        options.sampleRate = kRate;
        REQUIRE(recorder.start(options));
        CHECK(recorder.isWriting());
        const std::int64_t frames = static_cast<std::int64_t>((flac ? 30.0 : 120.0) * kRate) + 123;
        feed(tap, 0, frames);
        recorder.stop();
        CHECK(tap.droppedFrames() == 0);
        CHECK(recorder.framesTotal() == static_cast<std::uint64_t>(frames));
        CHECK(closed.frames == static_cast<std::uint64_t>(frames));
        const auto files = recordings(folder);
        REQUIRE(files.size() == 1);
        CHECK(files[0].getFileExtension() == (flac ? ".flac" : ".wav"));
        auto reader = open(files[0]);
        REQUIRE(reader != nullptr);
        CHECK(reader->lengthInSamples == frames);
        CHECK(reader->numChannels == 2);
        CHECK(reader->bitsPerSample == 24);
        juce::AudioBuffer<float> back(2, 48000);
        reader->read(&back, 0, 48000, frames - 48000, true, true); // the last second
        double worst = 0.0;
        for (int i = 0; i < 48000; ++i)
            for (int c = 0; c < 2; ++c)
                worst = std::max(worst, static_cast<double>(std::abs(back.getSample(c, i) - signalAt(frames - 48000 + i, c))));
        CHECK(worst < 1.5e-7); // 24-bit: half a step is 6e-8
        reader.reset();
        folder.deleteRecursively();
    }
}

TEST_CASE("Recorder: auto start at the first sound; auto split after 20 s of silence", "[engine][recorder]")
{
    engine::RecordTap tap;
    tap.prepare(kRate);
    engine::Recorder recorder(tap);
    const auto folder = scratchFolder("auto");
    engine::Recorder::Options options;
    options.folder = folder;
    options.sampleRate = kRate;
    options.autoStart = true;
    options.autoSplit = true;
    std::vector<juce::File> closedFiles; // in the order they were finished
    recorder.onFileClosed = [&](const engine::Recorder::FileInfo& f) { closedFiles.push_back(f.file); };
    REQUIRE(recorder.start(options));
    CHECK(recorder.isWaiting());
    feed(tap, 0, 48000, true);        // 1 s of silence: no file yet
    feed(tap, 1, 2 * 48000);          // 2 s of sound (from its sample 1: sample 0 of a sine is 0)
    feed(tap, 0, 21 * 48000, true);   // 21 s of silence: split after 20
    feed(tap, 1, 2 * 48000);          // sound again: a second file
    recorder.stop();
    REQUIRE(recordings(folder).size() == 2);
    REQUIRE(closedFiles.size() == 2);
    const auto& files = closedFiles;
    auto first = open(files[0]);
    REQUIRE(first != nullptr);
    // The sound and the 20 s that ended it (within 1 ms: the sine's last samples near zero are already quiet).
    CHECK(std::abs(static_cast<double>(first->lengthInSamples) - (2 * 48000 + 20 * 48000)) <= 48.0);
    juce::AudioBuffer<float> head(2, 4);
    first->read(&head, 0, 4, 0, true, true);
    CHECK(std::abs(head.getSample(0, 0) - signalAt(1, 0)) < 1.5e-7); // begins on the first sound sample
    auto second = open(files[1]);
    REQUIRE(second != nullptr);
    CHECK(second->lengthInSamples == 2 * 48000);
    CHECK(recorder.filesWritten() == 2);
    first.reset();
    second.reset();
    folder.deleteRecursively();
}

TEST_CASE("Recorder: 60 minutes, no gaps", "[.long][engine][recorder]")
{
    engine::RecordTap tap;
    tap.prepare(kRate);
    engine::Recorder recorder(tap);
    const auto folder = scratchFolder("hour");
    engine::Recorder::Options options;
    options.folder = folder;
    options.format = engine::Recorder::Format::Flac24;
    options.sampleRate = kRate;
    REQUIRE(recorder.start(options));
    const auto frames = static_cast<std::int64_t>(3600.0 * kRate);
    const auto t0 = std::chrono::steady_clock::now();
    feed(tap, 0, frames);
    recorder.stop();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    WARN("60 min recorded in " << seconds << " s; dropped " << tap.droppedFrames());
    CHECK(tap.droppedFrames() == 0);
    const auto files = recordings(folder);
    REQUIRE(files.size() == 1);
    auto reader = open(files[0]);
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == frames); // = 60 minutes at 48 kHz, to the sample
    reader.reset();
    folder.deleteRecursively();
}

TEST_CASE("Recording sheets: the tracks inside the recording, a tracklist and a cue sheet", "[app][recorder]")
{
    const std::vector<app::PlayedAt> plays{{1000, "A", "Early"}, {1100, "B", "Before"}, {1250, "C", "First \"one\""},
                                           {1900, "", "Second"}, {9000, "D", "After"}};
    const auto tracks = app::tracksInRecording(plays, 1200.0, 3600.0);
    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0].title == "Before"); // playing when it began: at 0
    CHECK(tracks[0].offsetSeconds == 0.0);
    CHECK(tracks[1].offsetSeconds == 50.0);
    CHECK(tracks[2].offsetSeconds == 700.0);
    CHECK(app::tracklistText(tracks) == "01. 00:00  B - Before\n02. 00:50  C - First \"one\"\n03. 11:40  Second\n");
    const auto cue = app::cueSheet("mix.flac", true, tracks);
    CHECK(cue.find("FILE \"mix.flac\" WAVE") != std::string::npos);
    CHECK(cue.find("TRACK 02 AUDIO\n    TITLE \"First 'one'\"\n    PERFORMER \"C\"\n    INDEX 01 00:50:00") != std::string::npos);
    CHECK(cue.find("INDEX 01 11:40:00") != std::string::npos);
}

TEST_CASE("Record tap in the engine: the master as it goes out (or the headphones)", "[engine][recorder]")
{
    engine::TrackBuffer track;
    track.sampleRate = kRate;
    track.audio.setSize(2, static_cast<int>(5.0 * kRate));
    for (int i = 0; i < track.audio.getNumSamples(); ++i)
    {
        track.audio.setSample(0, i, signalAt(i, 0));
        track.audio.setSample(1, i, signalAt(i, 1));
    }
    engine::AudioEngine e;
    e.prepare(kRate, 256);
    e.send(engine::LoadTrack{0, &track, 1});
    e.send(engine::Play{0});
    float l[256], r[256];
    float* outs[] = {l, r};
    e.process(outs, 2, 256);
    auto& tap = e.recordTap();
    tap.setEnabled(true);
    std::vector<float> out, recorded(256), recordedR(256);
    double worst = 0.0;
    for (int b = 0; b < 20; ++b)
    {
        e.process(outs, 2, 256);
        REQUIRE(tap.read(recorded.data(), recordedR.data(), 256) == 256);
        for (int i = 0; i < 256; ++i)
            worst = std::max(worst, static_cast<double>(std::abs(recorded[static_cast<std::size_t>(i)] - l[i])));
    }
    CHECK(worst == 0.0); // exactly what the master output is
    tap.setSource(engine::RecordTap::Source::Headphones);
    e.process(outs, 2, 256);
    CHECK(tap.read(recorded.data(), recordedR.data(), 256) == 256);
}

#include "ui/RecorderPanel.h"

TEST_CASE("Recorder panel: REC asks the app, the options change and are reported, the status", "[ui][recorder]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    engine::AudioEngine e;
    e.prepare(kRate, 256);
    engine::Recorder recorder(e.recordTap());
    ui::RecorderPanel panel(e, recorder);
    panel.setBounds(0, 0, 1000, ui::RecorderPanel::kPreferredHeight);
    int asked = 0;
    ui::RecorderPanel::Settings reported;
    panel.onRecord = [&](bool start) { asked += start ? 1 : 0; };
    panel.onSettingsChanged = [&](const ui::RecorderPanel::Settings& s) { reported = s; };
    panel.recButton().onClick();
    CHECK(asked == 1);
    panel.formatButton().onClick();
    panel.sourceButton().onClick();
    panel.autoSplitButton().onClick();
    CHECK(reported.flac);
    CHECK(reported.headphones);
    CHECK(reported.autoSplit);
    CHECK_FALSE(reported.autoStart);
    CHECK(panel.formatButton().getButtonText() == "FLAC");
    CHECK(panel.sourceButton().getButtonText() == "PHONES");
    CHECK(panel.statusText() == "not recording");
    // Recording for real: the status follows.
    const auto folder = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("devildj-panel-" + juce::String(juce::Time::getHighResolutionTicks()));
    engine::Recorder::Options options;
    options.folder = folder;
    options.autoStart = true;
    REQUIRE(recorder.start(options));
    panel.refresh();
    CHECK(panel.statusText() == "waiting for sound");
    CHECK(panel.recButton().getButtonText() == "STOP");
    recorder.stop();
    juce::Image image(juce::Image::ARGB, 1000, ui::RecorderPanel::kPreferredHeight, true);
    juce::Graphics g(image);
    panel.paintEntireComponent(g, false);
    folder.deleteRecursively();
}
