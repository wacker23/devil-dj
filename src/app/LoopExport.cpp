#include "LoopExport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace devildj::app
{

juce::String writeLoopWav(const engine::TrackBuffer& track, int start, int end, const juce::File& file)
{
    const int length = track.audio.getNumSamples();
    start = std::clamp(start, 0, length);
    end = std::clamp(end, 0, length);
    if (end <= start)
        return "the loop is empty";
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    if (!static_cast<juce::FileOutputStream*>(out.get())->openedOk())
        return "can't write " + file.getFullPathName();
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(track.sampleRate)
                             .withNumChannels(2)
                             .withBitsPerSample(24);
    auto writer = wav.createWriterFor(out, options);
    if (writer == nullptr)
        return "can't make a WAV writer";
    const int channels = track.audio.getNumChannels();
    const float* src[2] = {track.audio.getReadPointer(0) + start,
                           track.audio.getReadPointer(channels > 1 ? 1 : 0) + start};
    if (!writer->writeFromFloatArrays(src, 2, end - start))
        return "writing failed";
    return {};
}

} // namespace devildj::app
