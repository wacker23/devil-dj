#pragma once

// The code deck's sound names (P5-11, CODEBEAT_SPEC §10): what s("bd:3") plays.
//
//   kick bd / snare sd / clap cp / hat hh / ohat oh / tom lt mt ht / rim rs / cow cb / shaker sh
//       the Beat Maker's drum synths; ":n" picks the n-th preset of that drum (wrapping)
//   sine saw square tri supersaw subbass fm
//       synth voices, played with note(...) (n shifts the pitch instead of picking a variant)
//   any folder in Documents\Devil-DJ\Samples
//       its sound files (wav / aif / aiff / flac), sorted by name; ":n" picks one (wrapping). A folder wins over a
//       built-in of the same name, so a "kick" folder plays the owner's own kicks.
//
// An unknown name gives a warning ("unknown sound "bdd" - did you mean "bd"?") instead of silence without a word.
// Folders are scanned when the bank is made and again (at most every 2 s) when a name isn't found, so a folder
// copied in while playing is picked up. Thread-safe; it only lists files - decoding is the sample bank's (P5-12).

#include "dsp/Synth.h"
#include "engine/ScheduledEvent.h"
#include "engine/Voices.h"

#include <juce_core/juce_core.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace devildj::app
{

struct CodeSound
{
    enum class Kind
    {
        Drum,
        Synth,
        Sample,
        Unknown,
    };
    Kind kind = Kind::Unknown;
    engine::VoiceKind voice = engine::VoiceKind::DrumSynth;
    engine::DrumSound drum;
    dsp::SynthParams synth;
    juce::File sample;
    std::string name;    // what it is: "bd:3 (kick, preset Deep)", "breaks:2 (amen.wav)"
    std::string warning; // why it can't play (Unknown), or a note
};

class CodeSoundBank
{
public:
    explicit CodeSoundBank(juce::File samplesFolder = defaultFolder());

    CodeSound resolve(const std::string& sound, int n = 0);
    std::vector<std::string> names(); // built-in names and sample folders, sorted (for completion: P5-16)
    void rescan();

    static juce::File defaultFolder(); // Documents\Devil-DJ\Samples
    static bool isBuiltIn(const std::string& sound);

private:
    void scanLocked();

    juce::File folder;
    std::mutex mutex;
    std::map<std::string, std::vector<juce::File>> folders; // lower-case name -> its files, sorted
    double lastScanMs = -1.0e9;
};

} // namespace devildj::app
