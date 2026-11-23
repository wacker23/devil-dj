#pragma once

// User samples for the Beat Maker's drum tracks (P4-13): WAV / AIFF / FLAC files decoded once and kept for the
// session (the audio thread may be playing any of them, so none is ever freed while the app runs).
//
// Missing files: a path that no longer exists is looked for by its file name in the samples folder
// (Documents\Devil-DJ\Samples) - "relocated"; if it isn't there either the track is "missing": it plays nothing and
// the grid says so, until a file is dropped on it or chosen.

#include "engine/Voices.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>
#include <memory>
#include <string>

namespace devildj::app
{

class SampleBank
{
public:
    explicit SampleBank(juce::File samplesFolder);

    struct Lookup
    {
        const engine::SampleData* data = nullptr; // nullptr: missing (or not a sound file)
        juce::File file;                          // where it was found
        bool relocated = false;                   // found in the samples folder, not at its path
        juce::String error;                       // why it is missing
    };
    // Message thread. Decodes the first time; the same pointer afterwards.
    Lookup get(const std::string& path);
    bool isMissing(const std::string& path) { return get(path).data == nullptr; }
    // Look again for those that were missing (a file copied in since, a project loaded).
    void retryMissing();

    const juce::File& folder() const noexcept { return samples; }
    static constexpr double kMaxSeconds = 60.0; // longer files are refused (a drum track plays one-shots)
    int loadedCount() const noexcept { return static_cast<int>(loaded.size()); }

private:
    struct Sample
    {
        juce::AudioBuffer<float> audio;
        const float* channels[2]{};
        engine::SampleData data;
    };
    std::shared_ptr<Sample> decode(const juce::File& file, juce::String& error);

    juce::File samples;
    juce::AudioFormatManager formats;
    std::map<juce::String, std::shared_ptr<Sample>> loaded; // by full path of the file read
    std::map<std::string, Lookup> lookups;                  // by the path asked for
};

} // namespace devildj::app
