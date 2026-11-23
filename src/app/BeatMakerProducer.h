#pragma once

// The Beat Maker's producer (P4-10, CODEBEAT_SPEC §8.1 / BEATMAKER_SPEC §2): every 25 ms on its own thread it
// takes the next window of beats up to 100 ms ahead, has the sequencer emit the pattern's (or song's) events in it,
// turns them into engine samples and pushes them into the engine's Beat Maker queue.
//
// Its timeline is the master clock (the project follows the master) or its own tempo (ownBpm), anchored where the
// master was when it switched, so the beat never jumps. The app hands it a clock snapshot now and then (the
// engine state: sample clock, beat, tempo, when) and it extrapolates "now" with the wall clock.
//
// Everything here is behind one mutex (the message thread edits, the producer thread reads); never the audio
// thread's. For tests, threaded = false and tick() by hand.

#include "beatmaker/Model.h"
#include "beatmaker/Sequencer.h"
#include "engine/Scheduler.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace devildj::app
{

class BeatMakerProducer
{
public:
    struct Clock
    {
        std::int64_t sampleClock = 0; // the engine's sample clock ...
        double beat = 0.0;            // ... the master beat there ...
        double bpm = 120.0;           // ... its tempo ...
        double sampleRate = 48000.0;
        double hostSeconds = 0.0;     // ... and when (seconds, the same clock as now())
    };

    using Push = std::function<bool(const engine::ScheduledEvent&)>;
    BeatMakerProducer(Push push, bool threaded = true);
    ~BeatMakerProducer();
    BeatMakerProducer(const BeatMakerProducer&) = delete;
    BeatMakerProducer& operator=(const BeatMakerProducer&) = delete;

    void setProject(std::shared_ptr<const beatmaker::Project> project);
    void setClock(const Clock& clock);
    // A clock for "now" (seconds); the default is juce's high-resolution millisecond counter.
    std::function<double()> now;

    // Start from the next bar of its timeline: the song or `pattern`.
    void play(int pattern, bool song);
    void stop();
    // While playing: from the next bar (or the playing pattern's end) on.
    void queuePattern(int pattern, bool atEnd);
    void queueSong(bool on, int pattern, bool atEnd);

    struct Status
    {
        bool playing = false;
        beatmaker::Sequencer::Position position;
        int queued = -1; // a pattern, -2 the song, -1 none
        double beat = 0.0;
        std::uint64_t pushed = 0, dropped = 0;
    };
    Status status();

    // One producer step (the thread calls it every 25 ms).
    void tick();

    static constexpr double kLookAheadSeconds = 0.1;
    static constexpr int kTickMs = 25;

private:
    engine::ClockSnapshot timeline(double nowSeconds); // the beat <-> sample map in force (mutex held)
    std::int64_t sampleNow(double nowSeconds) const;   // mutex held
    void loop();

    Push push;
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    std::shared_ptr<const beatmaker::Project> project;
    std::optional<Clock> clock;
    beatmaker::Sequencer sequencer;
    engine::ScheduleCursor cursor;
    bool playing = false;
    // Own tempo: the anchor (a sample and its beat) where it took over from the master.
    bool ownTempo = false;
    std::int64_t ownSample = 0;
    double ownBeat = 0.0, ownBpm = 120.0;
    std::uint64_t pushedCount = 0, droppedCount = 0;
    std::vector<beatmaker::BeatEvent> events;
    std::thread worker;
};

} // namespace devildj::app
