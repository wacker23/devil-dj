// P6-04: a deck's stems loaded from the cache in the background; a newer request (or a cancel) drops the older.

#include "app/StemDeckLoader.h"
#include "engine/StemLoader.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_audio_formats/juce_audio_formats.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

using devildj::app::StemDeckLoader;
using namespace devildj::engine;

namespace
{
struct Messages // a stand-in message thread: posted functions wait until pumped
{
    std::mutex mutex;
    std::deque<std::function<void()>> queue;
    StemDeckLoader::Post poster()
    {
        return [this](std::function<void()> f)
        {
            std::lock_guard lock(mutex);
            queue.push_back(std::move(f));
        };
    }
    std::size_t waiting()
    {
        std::lock_guard lock(mutex);
        return queue.size();
    }
    void pump()
    {
        for (;;)
        {
            std::function<void()> f;
            {
                std::lock_guard lock(mutex);
                if (queue.empty())
                    return;
                f = std::move(queue.front());
                queue.pop_front();
            }
            f();
        }
    }
    bool waitFor(std::size_t count, double seconds)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (waiting() < count)
        {
            if (std::chrono::steady_clock::now() > until)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
};

struct Folder
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("devildj-deckstems", "");
    explicit Folder(int samples)
    {
        dir.createDirectory();
        static constexpr const char* names[] = {"drums", "bass", "other", "vocals"};
        for (int s = 0; s < 4; ++s)
        {
            juce::AudioBuffer<float> audio(2, samples);
            audio.clear();
            for (int i = 0; i < samples; ++i)
                audio.setSample(0, i, 0.1f * static_cast<float>(s + 1));
            juce::FlacAudioFormat flac;
            std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(dir.getChildFile(juce::String(names[s]) + ".flac"));
            auto w = flac.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16));
            w->writeFromAudioSampleBuffer(audio, 0, samples);
        }
    }
    ~Folder() { dir.deleteRecursively(); }
};

std::shared_ptr<TrackBuffer> track(int samples)
{
    auto t = std::make_shared<TrackBuffer>();
    t->sampleRate = 44100.0;
    t->audio.setSize(2, samples);
    t->audio.clear();
    return t;
}
} // namespace

TEST_CASE("Deck stems: loaded in the background, delivered on the message thread with peaks", "[app][stems]")
{
    Folder folder(44100);
    Messages messages;
    std::vector<std::shared_ptr<StemBuffer>> got;
    std::vector<juce::String> errors;
    StemDeckLoader loader([&](DeckId, std::shared_ptr<const TrackBuffer>, std::shared_ptr<StemBuffer> s, double)
                          { got.push_back(std::move(s)); },
                          [&](DeckId, const juce::String& e) { errors.push_back(e); }, messages.poster());
    const auto t = track(44100);
    loader.request(1, t, folder.dir);
    CHECK(loader.busy(1));
    REQUIRE(messages.waitFor(1, 30.0));
    CHECK(got.empty()); // nothing until the message thread runs it
    messages.pump();
    REQUIRE(got.size() == 1);
    CHECK_FALSE(loader.busy(1));
    const auto& s = *got[0];
    CHECK(s.track == t.get());
    CHECK(s.length == 44100);
    CHECK(s.channel(3, 0)[1000] == toInt16(0.4f));
    CHECK(s.channel(3, 1)[1000] == 0);
    REQUIRE(s.peaks[1].size() == static_cast<std::size_t>((44100 + StemBuffer::kPeakSamples - 1) / StemBuffer::kPeakSamples));
    CHECK(std::abs(s.peaks[1][10] - 0.2f) < 1e-3f);
    CHECK(errors.empty());

    loader.request(0, t, folder.dir.getChildFile("missing"));
    REQUIRE(messages.waitFor(1, 30.0));
    messages.pump();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("no stem file"));
}

TEST_CASE("Deck stems: a newer request or a cancel drops what was on its way", "[app][stems]")
{
    Folder folder(44100);
    Messages messages;
    std::vector<const TrackBuffer*> got;
    StemDeckLoader loader([&](DeckId, std::shared_ptr<const TrackBuffer> t, std::shared_ptr<StemBuffer>, double)
                          { got.push_back(t.get()); },
                          {}, messages.poster());
    const auto first = track(44100), second = track(44100);
    loader.request(0, first, folder.dir);
    REQUIRE(messages.waitFor(1, 30.0)); // the first one is done and waiting for the message thread
    loader.request(0, second, folder.dir);
    REQUIRE(messages.waitFor(2, 30.0));
    messages.pump();
    CHECK(got == std::vector<const TrackBuffer*>{second.get()});

    loader.request(0, first, folder.dir);
    REQUIRE(messages.waitFor(1, 30.0));
    loader.cancel(0);
    messages.pump();
    CHECK(got.size() == 1);
}
