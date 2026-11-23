#include "app/SampleBank.h"

namespace devildj::app
{

SampleBank::SampleBank(juce::File folder)
    : samples(std::move(folder))
{
    formats.registerBasicFormats();
}

std::shared_ptr<SampleBank::Sample> SampleBank::decode(const juce::File& file, juce::String& error)
{
    if (const auto it = loaded.find(file.getFullPathName()); it != loaded.end())
        return it->second;
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "not a sound file this app reads: " + file.getFileName();
        return nullptr;
    }
    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        error = "empty: " + file.getFileName();
        return nullptr;
    }
    if (static_cast<double>(reader->lengthInSamples) / reader->sampleRate > kMaxSeconds)
    {
        error = "longer than " + juce::String(kMaxSeconds, 0) + " s: " + file.getFileName();
        return nullptr;
    }
    auto s = std::make_shared<Sample>();
    const int frames = static_cast<int>(reader->lengthInSamples);
    const int channels = std::clamp(static_cast<int>(reader->numChannels), 1, 2);
    s->audio.setSize(channels, frames);
    reader->read(&s->audio, 0, frames, 0, true, channels > 1);
    for (int c = 0; c < channels; ++c)
        s->channels[c] = s->audio.getReadPointer(c);
    s->data = {s->channels, channels, frames, reader->sampleRate};
    loaded[file.getFullPathName()] = s;
    return s;
}

void SampleBank::retryMissing()
{
    std::erase_if(lookups, [](const auto& kv) { return kv.second.data == nullptr; });
}

SampleBank::Lookup SampleBank::get(const std::string& path)
{
    if (const auto it = lookups.find(path); it != lookups.end())
        return it->second;
    Lookup out;
    const juce::File asked(juce::String::fromUTF8(path.c_str()));
    juce::File file = asked;
    if (!file.existsAsFile())
    {
        // Moved or on another computer: the same name in the samples folder (or a folder inside it).
        const auto name = asked.getFileName();
        file = juce::File();
        if (name.isNotEmpty() && samples.isDirectory())
        {
            if (const auto direct = samples.getChildFile(name); direct.existsAsFile())
                file = direct;
            else if (const auto found = samples.findChildFiles(juce::File::findFiles, true, name); !found.isEmpty())
                file = found.getFirst();
        }
        out.relocated = file != juce::File();
        if (!out.relocated)
            out.error = "missing: " + (name.isNotEmpty() ? name : juce::String("(no file)"));
    }
    if (file != juce::File())
    {
        juce::String error;
        if (const auto s = decode(file, error))
        {
            out.data = &s->data;
            out.file = file;
        }
        else
        {
            out.error = error;
            out.relocated = false;
        }
    }
    lookups[path] = out;
    return out;
}

} // namespace devildj::app
