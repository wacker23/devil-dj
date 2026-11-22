#include "LoopExport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace devildj::app
{

juce::String writeLoopWav(const engine::TrackBuffer& track, int start, int end, const juce::File& file,
                          int bitsPerSample)
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
                             .withBitsPerSample(bitsPerSample == 32 ? 32 : bitsPerSample == 16 ? 16 : 24);
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

std::unique_ptr<engine::TrackBuffer> copyRange(const engine::TrackBuffer& track, int start, int end)
{
    const int length = track.audio.getNumSamples();
    start = std::clamp(start, 0, length);
    end = std::clamp(end, 0, length);
    if (end <= start)
        return nullptr;
    auto copy = std::make_unique<engine::TrackBuffer>();
    copy->sampleRate = track.sampleRate;
    copy->sourceSampleRate = track.sampleRate;
    copy->sourceChannels = 2;
    copy->sourceLengthSamples = end - start;
    copy->title = track.title;
    copy->artist = track.artist;
    copy->audio.setSize(2, end - start);
    const int channels = track.audio.getNumChannels();
    for (int c = 0; c < 2; ++c)
        copy->audio.copyFrom(c, 0, track.audio, channels > 1 ? c : 0, start, end - start);
    return copy;
}

} // namespace devildj::app
