#include "app/StemService.h"

#include "app/VocalPresence.h"
#include "dsp/SincResampler.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <array>
#include <chrono>

namespace devildj::app
{

struct StemService::Worker final : juce::Thread
{
    StemService& owner;
    explicit Worker(StemService& o)
        : juce::Thread("Stem separation")
        , owner(o)
    {
    }
    void run() override
    {
        while (!threadShouldExit())
        {
            std::optional<StemRequest> next;
            {
                std::lock_guard<std::mutex> lock(owner.mutex);
                if (!owner.queue.empty())
                {
                    next = std::move(owner.queue.front());
                    owner.queue.pop_front();
                    owner.state.queued = static_cast<int>(owner.queue.size());
                    owner.state.running = *next;
                    owner.state.progress = 0.0;
                }
            }
            if (!next)
            {
                wait(250);
                continue;
            }
            owner.cancelRunning = false;
            owner.process(*next);
            std::lock_guard<std::mutex> lock(owner.mutex);
            owner.state.running.reset();
        }
    }
};

StemService::StemService(juce::File cacheRoot, juce::File modelFile, Decoder decoder)
    : cache(std::move(cacheRoot))
    , model(std::move(modelFile))
    , decode(std::move(decoder))
{
    state.modelMissing = !model.existsAsFile();
    worker = std::make_unique<Worker>(*this);
    worker->startThread(juce::Thread::Priority::background);
}

StemService::~StemService()
{
    cancelAll();
    worker->signalThreadShouldExit();
    worker->notify();
    worker->stopThread(60000); // a segment in progress finishes first (seconds)
}

juce::File StemService::defaultModel()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Devil-DJ")
        .getChildFile("Models")
        .getChildFile("htdemucs_core.onnx");
}

juce::File StemService::folderFor(const std::string& contentHash) const
{
    return cache.getChildFile("stems").getChildFile(juce::String(contentHash));
}

bool StemService::ready(const std::string& contentHash) const
{
    const auto folder = folderFor(contentHash);
    for (int s = 0; s < analysis::demucs::kSources; ++s)
        if (!folder.getChildFile(juce::String(stemName(s)) + ".flac").existsAsFile())
            return false;
    return folder.getChildFile("stems.json").existsAsFile();
}

void StemService::enqueue(StemRequest request)
{
    if (request.contentHash.empty() || ready(request.contentHash))
        return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        state.modelMissing = !model.existsAsFile();
        if (state.running && state.running->trackId == request.trackId)
            return;
        for (const auto& q : queue)
            if (q.trackId == request.trackId)
                return;
        queue.push_back(std::move(request));
        state.queued = static_cast<int>(queue.size());
    }
    worker->notify();
}

void StemService::cancelAll()
{
    std::lock_guard<std::mutex> lock(mutex);
    queue.clear();
    state.queued = 0;
    cancelRunning = true;
}

StemService::Status StemService::status() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return state;
}

bool StemService::waitUntilIdle(double timeoutSeconds) const
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (std::chrono::steady_clock::now() < until)
    {
        if (status().idle())
            return true;
        juce::Thread::sleep(50);
    }
    return status().idle();
}

void StemService::process(const StemRequest& r)
{
    const auto t0 = std::chrono::steady_clock::now();
    const auto final = folderFor(r.contentHash);
    const auto part = final.getSiblingFile(final.getFileName() + ".part");
    part.deleteRecursively();
    const auto result = separateInto(r, part);
    std::lock_guard<std::mutex> lock(mutex);
    if (result)
    {
        final.deleteRecursively();
        if (part.moveFileTo(final))
        {
            ensureVocalCurve(final); // P6-09: vocal.json beside the stems
            ++state.done;
            state.lastDone = r.title;
            state.lastDoneTrack = r.trackId;
            state.lastDoneHash = r.contentHash;
            state.lastSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return;
        }
        state.lastError = "can't move the stems into " + final.getFullPathName();
    }
    else if (cancelRunning.load() || worker->threadShouldExit())
    {
        part.deleteRecursively(); // cancelled: not a failure
        return;
    }
    else
        state.lastError = r.title + ": " + juce::String::fromUTF8(result.error().c_str());
    part.deleteRecursively();
    ++state.failed;
}

util::Result<bool> StemService::separateInto(const StemRequest& r, const juce::File& partFolder)
{
    using R = util::Result<bool>;
    if (separator == nullptr)
    {
        if (!model.existsAsFile())
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.modelMissing = true;
            return R::fail("no stem model at " + model.getFullPathName().toStdString());
        }
        auto s = analysis::StemSeparator::open(std::filesystem::path(model.getFullPathName().toWideCharPointer()));
        if (!s)
            return R::fail(s.error());
        separator = std::move(s.value());
    }
    auto audio = decode(r.file);
    if (!audio)
        return R::fail("can't decode the track: " + audio.error());
    auto& a = audio.value();
    if (std::abs(a.sampleRate - analysis::demucs::kSampleRate) > 0.5)
    {
        // The model's rate: the analysis resampler (windowed sinc), each channel.
        a.left = dsp::resample(a.left, a.sampleRate, analysis::demucs::kSampleRate);
        a.right = dsp::resample(a.right, a.sampleRate, analysis::demucs::kSampleRate);
        a.sampleRate = analysis::demucs::kSampleRate;
    }
    if (!partFolder.createDirectory())
        return R::fail("can't create " + partFolder.getFullPathName().toStdString());

    // Four FLAC writers, 16-bit stereo at 44.1 kHz, fed as the stems come.
    juce::FlacAudioFormat flac;
    std::array<std::unique_ptr<juce::AudioFormatWriter>, analysis::demucs::kSources> writers;
    for (int s = 0; s < analysis::demucs::kSources; ++s)
    {
        std::unique_ptr<juce::OutputStream> out =
            std::make_unique<juce::FileOutputStream>(partFolder.getChildFile(juce::String(stemName(s)) + ".flac"));
        if (!static_cast<juce::FileOutputStream*>(out.get())->openedOk())
            return R::fail("can't write the stems");
        const auto options = juce::AudioFormatWriterOptions{}
                                 .withSampleRate(analysis::demucs::kSampleRate)
                                 .withNumChannels(2)
                                 .withBitsPerSample(16);
        writers[static_cast<std::size_t>(s)] = flac.createWriterFor(out, options);
        if (writers[static_cast<std::size_t>(s)] == nullptr)
            return R::fail("can't make a FLAC writer");
    }
    bool writeFailed = false;
    const auto result = separator->separate(
        a.left.data(), a.right.data(), static_cast<std::int64_t>(a.left.size()), r.quality,
        [&](const float* const* channels, int count)
        {
            for (int s = 0; s < analysis::demucs::kSources; ++s)
            {
                const float* pair[2] = {channels[s * 2], channels[s * 2 + 1]};
                auto& w = *writers[static_cast<std::size_t>(s)];
                writeFailed = writeFailed || !w.writeFromFloatArrays(pair, 2, count);
            }
        },
        [this](double p)
        {
            std::lock_guard<std::mutex> lock(mutex);
            state.progress = p;
            return !cancelRunning.load() && !worker->threadShouldExit();
        });
    for (auto& w : writers)
        w.reset(); // flushed and closed
    if (!result)
        return result;
    if (writeFailed)
        return R::fail("writing the stems failed (disk full?)");
    auto* info = new juce::DynamicObject();
    info->setProperty("model", "htdemucs");
    info->setProperty("quality", r.quality == analysis::StemSeparator::Quality::Quality ? "quality" : "fast");
    info->setProperty("sampleRate", analysis::demucs::kSampleRate);
    info->setProperty("samples", static_cast<juce::int64>(a.left.size()));
    info->setProperty("stems", juce::StringArray{"drums", "bass", "other", "vocals"});
    if (!partFolder.getChildFile("stems.json").replaceWithText(juce::JSON::toString(juce::var(info))))
        return R::fail("can't write stems.json");
    return R::ok(true);
}

} // namespace devildj::app
