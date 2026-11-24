#include "app/StemDeckLoader.h"

#include "engine/StemLoader.h"

#include <chrono>

namespace devildj::app
{

// What the jobs and the message-thread deliveries share (outlives the loader if a delivery is still queued).
struct StemDeckLoader::Shared
{
    Loaded loaded;
    Failed failed;
    Post post;
    std::atomic<bool> alive{true};
    std::array<std::atomic<std::uint64_t>, kDecks> generation{};
    std::array<std::shared_ptr<std::atomic<bool>>, kDecks> cancelFlags;
};

StemDeckLoader::StemDeckLoader(Loaded loaded, Failed failed, Post post)
    : shared(std::make_shared<Shared>())
{
    shared->loaded = std::move(loaded);
    shared->failed = std::move(failed);
    shared->post = post ? std::move(post) : Post([](std::function<void()> f) { juce::MessageManager::callAsync(std::move(f)); });
}

StemDeckLoader::~StemDeckLoader()
{
    shared->alive = false;
    for (auto& c : shared->cancelFlags)
        if (c != nullptr)
            *c = true;
    pool.removeAllJobs(true, 30000);
}

void StemDeckLoader::cancel(engine::DeckId deck)
{
    if (deck >= kDecks)
        return;
    ++shared->generation[deck];
    if (auto& c = shared->cancelFlags[deck]; c != nullptr)
        *c = true;
}

bool StemDeckLoader::busy(engine::DeckId deck) const noexcept
{
    const auto& c = shared->cancelFlags[deck < kDecks ? deck : 0];
    return deck < kDecks && c != nullptr && !*c;
}

void StemDeckLoader::request(engine::DeckId deck, std::shared_ptr<const engine::TrackBuffer> track,
                             const juce::File& folder)
{
    if (deck >= kDecks || track == nullptr)
        return;
    cancel(deck);
    const auto gen = shared->generation[deck].load();
    auto flag = std::make_shared<std::atomic<bool>>(false);
    shared->cancelFlags[deck] = flag;
    pool.addJob(
        [s = shared, deck, gen, flag, track = std::move(track), folder]
        {
            const auto t0 = std::chrono::steady_clock::now();
            auto result = engine::loadStems(folder, *track, flag.get());
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::shared_ptr<engine::StemBuffer> stems;
            juce::String error;
            if (result)
                stems = std::move(result).value();
            else
                error = juce::String::fromUTF8(result.error().c_str());
            s->post(
                [s, deck, gen, flag, track, stems, error, seconds]
                {
                    if (!s->alive || s->generation[deck].load() != gen || *flag)
                        return; // superseded or cancelled: dropped here (freed on the message thread)
                    *flag = true; // done
                    if (stems != nullptr)
                    {
                        if (s->loaded)
                            s->loaded(deck, track, stems, seconds);
                    }
                    else if (s->failed)
                        s->failed(deck, error);
                });
        });
}

} // namespace devildj::app
