#pragma once

// The code deck's producer (P5-09, CODEBEAT_SPEC §6 / §8.1): its own thread owns the JavaScript runtime. Every
// 25 ms it evaluates code that was submitted, takes the next window of beats up to 100 ms ahead (an exact cursor:
// windows never overlap or leave gaps), queries the pattern's onsets in it (1 cycle = setcpb() beats, 4 by
// default), turns each into a ScheduledEvent at its exact engine sample and pushes it to the engine.
//
// - Events whose time is already past when they are made (the thread was held up) are dropped and counted (late).
//   If the clock jumped, the cursor re-anchors at now (counted).
// - A query over its time budget skips that window (silence) and is counted as slow.
// - Sounds are numbered by name ("bd:3", "saw" ...) in order of first use (at most Voices::kSounds); soundName()
//   maps back. With a sound bank (P5-11) each new name is resolved there: the event gets its voice kind, a synth's
//   n shifts its note, and the new sounds wait in takeNewSounds() for the engine to install (P5-12). An unknown
//   name is a warning (once per name, onWarning / status) and its events are not played (counted).
//   With no bank, every name passes as a drum synth (tests). The highlight table comes with P5-15.
// - Jitter: the spacing of the ticks and how far ahead of now events are made are measured (status()).
//
// The thread has a 16 MB stack (QuickJS needs more than the 1 MB default). For tests, threaded = false and tick()
// by hand - on a thread with a big stack.

#include "app/CodeSoundBank.h"
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
        std::uint64_t unknownSounds = 0;   // events not played: their sound has no such name (P5-11)
        std::vector<std::string> warnings; // each once, the newest last (at most 50)
        double meanTickMs = 0.0, maxTickMs = 0.0;   // the spacing of the ticks
        double minLeadMs = 0.0, maxLeadMs = 0.0;    // how far ahead of now events were made
        std::string lastError;
        std::uint64_t evaluations = 0; // P5-14: how many have finished (a new one: `last` is its result)
        codebeat::EvalResult last;
    };

    using Push = std::function<bool(const engine::ScheduledEvent&)>;
    CodeDeckProducer(Push push, bool threaded = true, codebeat::JsLimits limits = {});
    ~CodeDeckProducer();
    CodeDeckProducer(const CodeDeckProducer&) = delete;
    CodeDeckProducer& operator=(const CodeDeckProducer&) = delete;

    void setClock(const Clock& clock);
    void setDj(const codebeat::DjHooks& hooks); // used before the next evaluation / query
    // Evaluated on the next tick; the pattern takes over at the next cycle line, or at once with swapNow (P5-10).
    void submit(std::string code, bool swapNow = false);
    std::function<double()> now;                // seconds; juce's high-resolution counter by default
    // Called on the producer thread after each evaluation (setbpm() requests are in it: the caller decides).
    std::function<void(const codebeat::EvalResult&)> onEvaluated;

    // P5-11: where sound names are looked up (shared with the UI for completion).
    void setSoundBank(std::shared_ptr<CodeSoundBank> bank);
    std::function<void(const std::string& warning)> onWarning; // called on the producer thread
    // Sounds used for the first time since the last call, for the engine to install (index, what it is).
    std::vector<std::pair<std::uint16_t, CodeSound>> takeNewSounds();

    Status status();
    std::string soundName(std::uint16_t index);
    void tick(); // one producer step (the thread calls it every 25 ms)

    static constexpr double kLookAheadSeconds = 0.1;
    static constexpr int kTickMs = 25;

private:
    struct Worker;
    std::int64_t sampleNow(double nowSeconds) const; // mutex held
    std::optional<std::uint16_t> soundIndex(const std::string& name, const CodeSound* resolved);
    // The event's sound from s and n (its kind and, for a synth, its note); false: not to be played.
    bool resolveSound(const std::string& sound, const std::string& n, bool hasNote, engine::ScheduledEvent& ev);
    void warn(const std::string& warning);
    void emit(const std::string& reply, const engine::ClockSnapshot& map, std::int64_t sampleNowValue);

    Push push;
    codebeat::JsLimits limits;
    std::mutex mutex;
    std::optional<Clock> clock;
    std::optional<codebeat::DjHooks> dj;
    std::optional<std::string> pending;
    bool pendingSwapNow = false;
    Status counters;
    double lastTickSeconds = -1.0, tickMsSum = 0.0;
    bool leadSeen = false;

    // Producer-thread only.
    std::unique_ptr<codebeat::CodeEvaluator> evaluator; // made on the producer thread (it is thread-confined)
    engine::ScheduleCursor cursor;
    double beatsPerCycle = 4.0;
    std::unordered_map<std::string, std::uint16_t> soundIds;
    std::vector<std::string> sounds;
    std::vector<engine::VoiceKind> soundKinds; // by index
    std::shared_ptr<CodeSoundBank> bank;
    std::vector<std::pair<std::uint16_t, CodeSound>> newSounds;
    std::unordered_map<std::string, double> unknownUntil; // unknown names: not looked up again before this time
    std::unique_ptr<Worker> worker;
};

} // namespace devildj::app
