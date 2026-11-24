#pragma once

// The code deck's producer (P5-09, CODEBEAT_SPEC §6 / §8.1): its own thread owns the JavaScript runtime. Every
// 25 ms it evaluates code that was submitted, takes the next window of beats up to 100 ms ahead (an exact cursor:
// windows never overlap or leave gaps), queries the pattern's onsets in it (1 cycle = setcpb() beats, 4 by
// default), turns each into a ScheduledEvent at its exact engine sample and pushes it to the engine.
//
// - Events whose time is already past when they are made (the thread was held up) are dropped and counted (late).
//   If the clock jumped, the cursor re-anchors at now (counted).
// - A query over its time budget skips that window (silence) and is counted as slow.
// - Sounds are numbered by name ("bd", "bd:3" ...) in order of first use; soundName() maps back (P5-11 resolves
//   them to voices). The highlight table comes with P5-15.
// - Jitter: the spacing of the ticks and how far ahead of now events are made are measured (status()).
//
// The thread has a 16 MB stack (QuickJS needs more than the 1 MB default). For tests, threaded = false and tick()
// by hand - on a thread with a big stack.

#include "codebeat/CodeEvaluator.h"
#include "engine/Scheduler.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace devildj::app
{

class CodeDeckProducer
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

    struct Status
    {
        bool ready = false;   // the language loaded
        bool playing = false;
        double cycle = 0.0;   // where the clock is, in cycles
        double beatsPerCycle = 4.0;
        std::uint64_t pushed = 0, dropped = 0, late = 0, reanchors = 0, slowWindows = 0, ticks = 0;
        double meanTickMs = 0.0, maxTickMs = 0.0;   // the spacing of the ticks
        double minLeadMs = 0.0, maxLeadMs = 0.0;    // how far ahead of now events were made
        std::string lastError;
    };

    using Push = std::function<bool(const engine::ScheduledEvent&)>;
    CodeDeckProducer(Push push, bool threaded = true, codebeat::JsLimits limits = {});
    ~CodeDeckProducer();
    CodeDeckProducer(const CodeDeckProducer&) = delete;
    CodeDeckProducer& operator=(const CodeDeckProducer&) = delete;

    void setClock(const Clock& clock);
    void setDj(const codebeat::DjHooks& hooks); // used before the next evaluation / query
    void submit(std::string code);              // evaluated on the next tick
    std::function<double()> now;                // seconds; juce's high-resolution counter by default
    // Called on the producer thread after each evaluation (setbpm() requests are in it: the caller decides).
    std::function<void(const codebeat::EvalResult&)> onEvaluated;

    Status status();
    std::string soundName(std::uint16_t index);
    void tick(); // one producer step (the thread calls it every 25 ms)

    static constexpr double kLookAheadSeconds = 0.1;
    static constexpr int kTickMs = 25;

private:
    struct Worker;
    std::int64_t sampleNow(double nowSeconds) const; // mutex held
    std::uint16_t soundIndex(const std::string& name);
    void emit(const std::string& reply, const engine::ClockSnapshot& map, std::int64_t sampleNowValue);

    Push push;
    codebeat::JsLimits limits;
    std::mutex mutex;
    std::optional<Clock> clock;
    std::optional<codebeat::DjHooks> dj;
    std::optional<std::string> pending;
    Status counters;
    double lastTickSeconds = -1.0, tickMsSum = 0.0;
    bool leadSeen = false;

    // Producer-thread only.
    std::unique_ptr<codebeat::CodeEvaluator> evaluator; // made on the producer thread (it is thread-confined)
    engine::ScheduleCursor cursor;
    double beatsPerCycle = 4.0;
    std::unordered_map<std::string, std::uint16_t> soundIds;
    std::vector<std::string> sounds;
    std::unique_ptr<Worker> worker;
};

} // namespace devildj::app
