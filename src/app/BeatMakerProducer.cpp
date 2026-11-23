#include "app/BeatMakerProducer.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>

namespace devildj::app
{

using util::Fraction;

BeatMakerProducer::BeatMakerProducer(Push p, bool threaded)
    : now([] { return juce::Time::getMillisecondCounterHiRes() * 0.001; })
    , push(std::move(p))
{
    events.reserve(512);
    if (threaded)
        worker = std::thread([this] { loop(); });
}

BeatMakerProducer::~BeatMakerProducer()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        quit = true;
    }
    wake.notify_all();
    if (worker.joinable())
        worker.join();
}

void BeatMakerProducer::loop()
{
    juce::Thread::setCurrentThreadName("Beat Maker producer");
    std::unique_lock<std::mutex> lock(mutex);
    while (!quit)
    {
        lock.unlock();
        tick();
        lock.lock();
        wake.wait_for(lock, std::chrono::milliseconds(kTickMs), [this] { return quit; });
    }
}

void BeatMakerProducer::setProject(std::shared_ptr<const beatmaker::Project> p)
{
    std::lock_guard<std::mutex> lock(mutex);
    project = std::move(p);
}

void BeatMakerProducer::setClock(const Clock& c)
{
    std::lock_guard<std::mutex> lock(mutex);
    clock = c;
}

std::int64_t BeatMakerProducer::sampleNow(double nowSeconds) const
{
    if (!clock)
        return 0;
    const double ahead = std::max(0.0, nowSeconds - clock->hostSeconds);
    return clock->sampleClock + static_cast<std::int64_t>(ahead * clock->sampleRate);
}

engine::ClockSnapshot BeatMakerProducer::timeline(double nowSeconds)
{
    const Clock c = clock.value_or(Clock{});
    const engine::ClockSnapshot master{c.sampleClock, c.beat, std::max(1.0, c.bpm), c.sampleRate};
    const bool follow = project == nullptr || project->followMaster;
    if (follow)
    {
        ownTempo = false;
        return master;
    }
    const double bpm = std::clamp(project->ownBpm, 20.0, 300.0);
    if (!ownTempo)
    {
        // Taking over: anchored where the master is now, so the beat goes on from there.
        ownTempo = true;
        ownSample = sampleNow(nowSeconds);
        ownBeat = master.beatAt(static_cast<double>(ownSample));
        ownBpm = bpm;
    }
    else if (bpm != ownBpm)
    {
        // A new tempo: re-anchored at now (the beat stays continuous).
        const engine::ClockSnapshot old{ownSample, ownBeat, ownBpm, c.sampleRate};
        const auto s = sampleNow(nowSeconds);
        ownBeat = old.beatAt(static_cast<double>(s));
        ownSample = s;
        ownBpm = bpm;
    }
    return {ownSample, ownBeat, ownBpm, c.sampleRate};
}

void BeatMakerProducer::play(int pattern, bool song)
{
    std::lock_guard<std::mutex> lock(mutex);
    const double t = now();
    const auto map = timeline(t);
    const auto start = beatmaker::Sequencer::nextBar(Fraction::fromDouble(map.beatAt(static_cast<double>(sampleNow(t))), 96000));
    sequencer.playPattern(pattern, start);
    if (song)
        sequencer.playSong(start);
    cursor.reset();
    playing = true;
}

void BeatMakerProducer::stop()
{
    std::lock_guard<std::mutex> lock(mutex);
    playing = false;
}

void BeatMakerProducer::queuePattern(int pattern, bool atEnd)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!playing || project == nullptr)
        return;
    // From the first beat not yet sent (the cursor), so nothing already queued in the engine is contradicted.
    const double t = now();
    const auto from = cursor.position().value_or(
        Fraction::fromDouble(timeline(t).beatAt(static_cast<double>(sampleNow(t))), 96000)); // nothing sent yet: now
    sequencer.queuePattern(pattern, atEnd ? sequencer.patternEnd(*project, from) : beatmaker::Sequencer::nextBar(from));
}

void BeatMakerProducer::queueSong(bool on, int pattern, bool atEnd)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!playing || project == nullptr)
        return;
    const double t = now();
    const auto from = cursor.position().value_or(
        Fraction::fromDouble(timeline(t).beatAt(static_cast<double>(sampleNow(t))), 96000)); // nothing sent yet: now
    const auto at = atEnd ? sequencer.patternEnd(*project, from) : beatmaker::Sequencer::nextBar(from);
    if (on)
        sequencer.queueSong(at);
    else
        sequencer.queuePattern(pattern, at);
}

std::optional<beatmaker::Sequencer::Position> BeatMakerProducer::hit(int shownPattern, int track, int velocity)
{
    std::lock_guard<std::mutex> lock(mutex); // the queue has one producer at a time: tick() holds this lock too
    if (project == nullptr)
        return std::nullopt;
    std::optional<beatmaker::Sequencer::Position> nearest;
    int pattern = std::clamp(shownPattern, 0, beatmaker::kPatternCount - 1);
    if (playing && clock)
    {
        // The nearest step: the step playing half a step from now.
        const double t = now();
        const auto map = timeline(t);
        const double beat = map.beatAt(static_cast<double>(sampleNow(t)));
        const auto here = sequencer.position(*project, Fraction::fromDouble(beat, 96000));
        if (here.pattern >= 0)
        {
            const auto& pat = project->patterns[static_cast<std::size_t>(here.pattern)];
            const double half = 0.5 / std::max(1, pat.stepsPerBeat);
            nearest = sequencer.position(*project, Fraction::fromDouble(beat + half, 96000));
            pattern = here.pattern;
        }
    }
    const auto& pat = project->patterns[static_cast<std::size_t>(pattern)];
    if (track < 0 || track >= static_cast<int>(pat.drums.size()))
        return nearest;
    const auto& d = pat.drums[static_cast<std::size_t>(track)];
    engine::ScheduledEvent e;
    e.sampleTime = engine::kNow;
    e.kind = d.sound.kind == beatmaker::SoundRef::Kind::Sample ? engine::VoiceKind::Sample : engine::VoiceKind::DrumSynth;
    e.soundIndex = static_cast<std::uint16_t>(pattern * 16 + track);
    e.velocity = static_cast<float>(std::clamp(velocity, 1, 127)) / 127.0f;
    e.gain = std::pow(10.0f, d.volumeDb / 20.0f);
    e.pan = d.pan;
    e.delaySend = d.delaySend;
    e.roomSend = d.reverbSend;
    e.chokeGroup = d.chokeGroup;
    e.highlightToken = static_cast<std::uint32_t>(track) << 16;
    if (push(e))
        ++pushedCount;
    else
        ++droppedCount;
    return nearest;
}

BeatMakerProducer::Status BeatMakerProducer::status()
{
    std::lock_guard<std::mutex> lock(mutex);
    Status s;
    s.playing = playing;
    s.pushed = pushedCount;
    s.dropped = droppedCount;
    if (project == nullptr || !clock)
        return s;
    const double t = now();
    const auto map = timeline(t);
    s.beat = map.beatAt(static_cast<double>(sampleNow(t)));
    if (playing)
    {
        const auto b = Fraction::fromDouble(s.beat, 96000);
        s.position = sequencer.position(*project, b);
        if (const auto q = sequencer.queuedAfter(b))
            s.queued = q->song ? -2 : q->pattern;
    }
    return s;
}

void BeatMakerProducer::tick()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!playing || project == nullptr || !clock)
        return;
    const double t = now();
    const auto map = timeline(t);
    const auto s = sampleNow(t);
    const auto window = cursor.next(map.beatAt(static_cast<double>(s)),
                                    map.beatAt(static_cast<double>(s) + kLookAheadSeconds * map.sampleRate));
    events.clear();
    sequencer.emit(*project, window.begin, window.end, events);
    for (const auto& e : events)
    {
        if (push(beatmaker::toScheduled(e, map)))
            ++pushedCount;
        else
            ++droppedCount;
    }
    sequencer.prune(window.begin);
}

} // namespace devildj::app
