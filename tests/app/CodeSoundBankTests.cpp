// P5-11: the code deck's sound names - built-in drums and synths, sample folders, warnings for unknown names.

#include "app/CodeSoundBank.h"
#include "beatmaker/DrumKits.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

using devildj::app::CodeSound;
using devildj::app::CodeSoundBank;

namespace
{
void writeWav(const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    juce::AudioBuffer<float> audio(1, 480);
    audio.clear();
    audio.setSample(0, 0, 0.5f);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    const auto options = juce::AudioFormatWriterOptions{}.withSampleRate(48000.0).withNumChannels(1).withBitsPerSample(16);
    auto writer = wav.createWriterFor(out, options);
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
}

struct TempSamples
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-samples", "");
    TempSamples() { dir.createDirectory(); }
    ~TempSamples() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("Sound bank: the built-in drums and their presets", "[app][codebeat][sounds]")
{
    TempSamples t;
    CodeSoundBank bank(t.dir);
    for (const auto& [name, kind] : std::vector<std::pair<std::string, devildj::dsp::DrumKind>>{
             {"bd", devildj::dsp::DrumKind::Kick}, {"kick", devildj::dsp::DrumKind::Kick},
             {"sd", devildj::dsp::DrumKind::Snare}, {"cp", devildj::dsp::DrumKind::Clap},
             {"hh", devildj::dsp::DrumKind::ClosedHat}, {"oh", devildj::dsp::DrumKind::OpenHat},
             {"lt", devildj::dsp::DrumKind::TomLow}, {"ht", devildj::dsp::DrumKind::TomHigh},
             {"rs", devildj::dsp::DrumKind::Rim}, {"cb", devildj::dsp::DrumKind::Cowbell},
             {"sh", devildj::dsp::DrumKind::Shaker}, {"BD", devildj::dsp::DrumKind::Kick}})
    {
        const auto s = bank.resolve(name);
        INFO(name);
        CHECK(s.kind == CodeSound::Kind::Drum);
        CHECK(s.voice == devildj::engine::VoiceKind::DrumSynth);
        CHECK(s.drum.kind == kind);
        CHECK(s.warning.empty());
    }
    // :n picks the n-th preset of that drum, wrapping.
    const auto presets = devildj::beatmaker::drumPresets(devildj::dsp::DrumKind::Kick);
    REQUIRE(presets.size() >= 2);
    const auto second = bank.resolve("bd", 1);
    CHECK(second.drum.params.decay == presets[1].params.decay);
    CHECK(second.name.find(presets[1].name) != std::string::npos);
    const auto wrapped = bank.resolve("bd", static_cast<int>(presets.size()) + 1);
    CHECK(wrapped.name.find(presets[1].name) != std::string::npos);
    CHECK(wrapped.drum.params.decay == presets[1].params.decay);
}

TEST_CASE("Sound bank: the built-in synths", "[app][codebeat][sounds]")
{
    TempSamples t;
    CodeSoundBank bank(t.dir);
    for (const char* name : {"sine", "saw", "square", "tri", "supersaw", "subbass", "fm"})
    {
        const auto s = bank.resolve(name);
        INFO(name);
        CHECK(s.kind == CodeSound::Kind::Synth);
        CHECK(s.voice == devildj::engine::VoiceKind::Synth);
    }
    CHECK(bank.resolve("sine").synth.osc1Wave == 3.0f);
    CHECK(bank.resolve("square").synth.osc1Wave == 1.0f);
    CHECK(bank.resolve("supersaw").synth.unison > 1.0f);
    CHECK(bank.resolve("subbass").synth.engine == devildj::dsp::SynthEngine::SubBass);
    CHECK(bank.resolve("fm").synth.engine == devildj::dsp::SynthEngine::Fm);
}

TEST_CASE("Sound bank: sample folders - sorted, :n wraps, a folder wins over a built-in, new folders found", "[app][codebeat][sounds]")
{
    TempSamples t;
    writeWav(t.dir.getChildFile("breaks").getChildFile("b10.wav"));
    writeWav(t.dir.getChildFile("breaks").getChildFile("b2.wav"));
    writeWav(t.dir.getChildFile("breaks").getChildFile("a1.wav"));
    t.dir.getChildFile("breaks").getChildFile("notes.txt").replaceWithText("not a sound");
    writeWav(t.dir.getChildFile("Kick").getChildFile("mine.wav"));
    t.dir.getChildFile("empty").createDirectory();
    CodeSoundBank bank(t.dir);

    auto s = bank.resolve("breaks", 0);
    CHECK(s.kind == CodeSound::Kind::Sample);
    CHECK(s.voice == devildj::engine::VoiceKind::Sample);
    CHECK(s.sample.getFileName() == "a1.wav");
    CHECK(bank.resolve("breaks", 1).sample.getFileName() == "b2.wav");  // natural order: b2 before b10
    CHECK(bank.resolve("breaks", 2).sample.getFileName() == "b10.wav");
    CHECK(bank.resolve("breaks", 3).sample.getFileName() == "a1.wav");  // wraps
    CHECK(bank.resolve("breaks", -1).sample.getFileName() == "b10.wav");
    CHECK(bank.resolve("BREAKS").kind == CodeSound::Kind::Sample);
    // The owner's "Kick" folder plays instead of the kick synth.
    CHECK(bank.resolve("kick").kind == CodeSound::Kind::Sample);
    CHECK(bank.resolve("bd").kind == CodeSound::Kind::Drum);
    // A folder with no sound files isn't a sound.
    CHECK(bank.resolve("empty").kind == CodeSound::Kind::Unknown);
    const auto names = bank.names();
    CHECK(std::find(names.begin(), names.end(), "breaks") != names.end());
    CHECK(std::find(names.begin(), names.end(), "bd") != names.end());
    CHECK(std::find(names.begin(), names.end(), "empty") == names.end());

    // A folder copied in while playing is found (after a rescan).
    writeWav(t.dir.getChildFile("vox").getChildFile("hey.wav"));
    bank.rescan();
    CHECK(bank.resolve("vox").kind == CodeSound::Kind::Sample);
}

TEST_CASE("Sound bank: an unknown name is a warning with the nearest name", "[app][codebeat][sounds]")
{
    TempSamples t;
    writeWav(t.dir.getChildFile("breaks").getChildFile("a.wav"));
    CodeSoundBank bank(t.dir);
    auto s = bank.resolve("bdd");
    CHECK(s.kind == CodeSound::Kind::Unknown);
    CHECK(s.warning == "unknown sound \"bdd\" - did you mean \"bd\"?");
    CHECK(bank.resolve("braeks").warning == "unknown sound \"braeks\" - did you mean \"breaks\"?");
    CHECK(bank.resolve("superpiano").warning == "unknown sound \"superpiano\"");
    CHECK_FALSE(bank.resolve("").warning.empty());
}
