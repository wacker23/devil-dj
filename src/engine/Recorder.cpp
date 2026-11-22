#include "engine/Recorder.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace devildj::engine
{

Recorder::Recorder(RecordTap& t)
    : juce::Thread("Devil-DJ recorder")
    , tap(t)
{
}

Recorder::~Recorder()
{
    stop();
}

bool Recorder::start(const Options& o)
{
    if (isThreadRunning())
        return false;
    if (!o.folder.createDirectory())
        return false;
    options = o;
    files = 0;
    totalFrames = 0;
    fileFrames = 0;
    silentRun = 0;
    // Whatever the tap had from before is old: drop it.
    std::vector<float> discard(4096);
    while (tap.read(discard.data(), discard.data(), static_cast<int>(discard.size())) > 0)
    {
    }
    if (!options.autoStart && !openFile(0))
        return false;
    tap.setEnabled(true);
    startThread(juce::Thread::Priority::high);
    return true;
}

void Recorder::stop()
{
    if (!isThreadRunning())
    {
        closeFile();
        return;
    }
    tap.setEnabled(false);  // nothing new comes in...
    signalThreadShouldExit(); // ...and the thread writes what is left, then ends
    stopThread(10000);
    closeFile();
}

juce::File Recorder::currentFile() const
{
    std::lock_guard<std::mutex> lock(fileMutex);
    return writing ? info.file : juce::File();
}

bool Recorder::openFile(int samplesBehind)
{
    closeFile();
    const auto now = juce::Time::getCurrentTime();
    const auto name = "Devil-DJ " + now.formatted("%Y-%m-%d %H%M%S");
    auto file = options.folder.getChildFile(name + extension(options.format));
    for (int n = 2; file.exists(); ++n) // the same second twice (a split): a suffix
        file = options.folder.getChildFile(name + " (" + juce::String(n) + ")" + extension(options.format));
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    if (!static_cast<juce::FileOutputStream*>(out.get())->openedOk())
        return false;
    const auto writerOptions = juce::AudioFormatWriterOptions{}
                                   .withSampleRate(options.sampleRate)
                                   .withNumChannels(2)
                                   .withBitsPerSample(24);
    std::unique_ptr<juce::AudioFormatWriter> w;
    if (options.format == Format::Flac24)
    {
        juce::FlacAudioFormat flac;
        w = flac.createWriterFor(out, writerOptions);
    }
    else
    {
        juce::WavAudioFormat wav;
        w = wav.createWriterFor(out, writerOptions);
    }
    if (w == nullptr)
        return false;
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        writer = std::move(w);
        info = {};
        info.file = file;
        info.started = now - juce::RelativeTime::seconds(samplesBehind / options.sampleRate);
    }
    fileFrames = 0;
    silentRun = 0;
    writing = true;
    return true;
}

void Recorder::closeFile()
{
    FileInfo closed;
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        if (writer == nullptr)
            return;
        writer.reset(); // flushes and finishes the header
        info.frames = fileFrames.load();
        closed = info;
        writing = false;
    }
    ++files;
    if (onFileClosed)
        onFileClosed(closed);
}

void Recorder::run()
{
    std::vector<float> l(4096), r(4096);
    for (;;)
    {
        const int n = tap.read(l.data(), r.data(), static_cast<int>(l.size()));
        if (n > 0)
        {
            process(l.data(), r.data(), n);
            continue;
        }
        if (threadShouldExit())
            break; // stopped and drained
        wait(5);
    }
}

void Recorder::process(const float* left, const float* right, int n)
{
    totalFrames += static_cast<std::uint64_t>(n);
    const float threshold = options.threshold;
    const auto silenceLimit = static_cast<std::uint64_t>(options.silenceSeconds * options.sampleRate);
    for (int from = 0; from < n;)
    {
        if (!writing) // waiting for sound: the file begins at its first sample
        {
            while (from < n && std::abs(left[from]) <= threshold && std::abs(right[from]) <= threshold)
                ++from;
            if (from == n || !openFile(tap.available() + (n - from)))
                return;
        }
        // Write up to the end of the block - or, with auto split, up to the sample that completes the silence.
        int to = n;
        if (options.autoSplit)
            for (int i = from; i < n; ++i)
            {
                if (std::abs(left[i]) > threshold || std::abs(right[i]) > threshold)
                    silentRun = 0;
                else if (++silentRun >= silenceLimit)
                {
                    to = i + 1;
                    break;
                }
            }
        const float* channels[] = {left + from, right + from};
        if (writer != nullptr)
            writer->writeFromFloatArrays(channels, 2, to - from);
        fileFrames += static_cast<std::uint64_t>(to - from);
        if (options.autoSplit && silentRun >= silenceLimit)
            closeFile(); // the rest of the block: waiting for the next sound
        from = to;
    }
}

} // namespace devildj::engine
