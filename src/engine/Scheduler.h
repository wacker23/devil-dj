#pragma once

// The shared scheduler (ARCHITECTURE §3.7, CODEBEAT_SPEC §8.1).
//
// - ScheduleCursor: a producer's look-ahead window in beats. Every ~25 ms it asks for the next window,
//   [cursor, now + look-ahead); the cursor is an exact Fraction so consecutive windows neither overlap nor leave
//   gaps. If the clock jumped (the cursor is more than a quarter beat from where it should be), it re-anchors.
// - EventBuffer: the audio thread's sorted events (fixed capacity, insertion by time). Each block takes those
//   due in it; those already in the past are dropped and counted as late - except kNow ones (live hits), which
//   start at the block's first sample.
// - EventQueue: one per producer, producer thread -> audio thread.

#include "engine/ScheduledEvent.h"
#include "util/Fraction.h"
#include "util/SpscQueue.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace devildj::engine
{

constexpr std::size_t kEventQueueCapacity = 2048;
using EventQueue = util::SpscQueue<ScheduledEvent, kEventQueueCapacity>;

// Where the engine clock was at one moment: enough to turn beats into samples for the next ~150 ms.
struct ClockSnapshot
{
    std::int64_t sampleTime = 0; // the engine sample clock at `beat`
    double beat = 0.0;
    double bpm = 120.0;
    double sampleRate = 48000.0;

    double sampleAt(double b) const noexcept { return static_cast<double>(sampleTime) + (b - beat) * 60.0 * sampleRate / bpm; }
    std::int64_t sampleAt(util::Fraction b) const noexcept
    {
        // Exact beats -> the nearest sample: the whole beats and the fraction apart keep the double small.
        const auto whole = b.floor() - static_cast<std::int64_t>(std::floor(beat));
        const double rest = b.fractionalPart().toDouble() - (beat - std::floor(beat));
        return sampleTime + std::llround((static_cast<double>(whole) + rest) * 60.0 * sampleRate / bpm);
    }
    double beatAt(double sample) const noexcept { return beat + (sample - static_cast<double>(sampleTime)) * bpm / (60.0 * sampleRate); }
};

class ScheduleCursor
{
public:
    struct Window
    {
        util::Fraction begin, end;
        bool reanchored = false;
    };

    // nowBeat: the clock's beat now; aheadBeat: the beat at now + the look-ahead. Window of exact beats with the
    // end rounded to `grain` (1/96000 beat by default).
    Window next(double nowBeat, double aheadBeat, std::int64_t grain = 96000) noexcept
    {
        Window w;
        const auto end = util::Fraction(static_cast<std::int64_t>(std::floor(aheadBeat * static_cast<double>(grain))), grain);
        if (!cursor || std::abs(cursor->toDouble() - nowBeat) > kReanchorBeats + (aheadBeat - nowBeat))
        {
            cursor = util::Fraction(static_cast<std::int64_t>(std::floor(nowBeat * static_cast<double>(grain))), grain);
            w.reanchored = true;
        }
        w.begin = *cursor;
        w.end = end > *cursor ? end : *cursor;
        cursor = w.end;
        return w;
    }
    void reset() noexcept { cursor.reset(); }
    std::optional<util::Fraction> position() const noexcept { return cursor; }

    static constexpr double kReanchorBeats = 0.25;

private:
    std::optional<util::Fraction> cursor;
};

// Fixed-capacity sorted events. Audio thread only (no allocation, no locks).
template <std::size_t Capacity = 1024>
class EventBuffer
{
public:
    // Insertion by time (events with equal times keep their order). False (and counted) if full.
    bool insert(const ScheduledEvent& e) noexcept
    {
        if (count == Capacity)
        {
            ++overflows;
            return false;
        }
        std::size_t i = count;
        while (i > 0 && events[i - 1].sampleTime > e.sampleTime)
        {
            events[i] = events[i - 1];
            --i;
        }
        events[i] = e;
        ++count;
        return true;
    }

    // Everything from a producer's queue.
    void drain(EventQueue& queue) noexcept
    {
        ScheduledEvent e;
        while (queue.tryPop(e))
            insert(e);
    }

    // Calls f(event, offset in the block) for each event due in [blockStart, blockStart + numSamples), in order,
    // and removes them; events before blockStart are dropped (lateEvents).
    template <typename F>
    void takeDue(std::int64_t blockStart, int numSamples, F&& f) noexcept
    {
        std::size_t used = 0;
        const std::int64_t blockEnd = blockStart + numSamples;
        while (used < count && events[used].sampleTime < blockEnd)
        {
            const auto& e = events[used];
            if (e.sampleTime == kNow) // a live hit: as soon as possible
                f(e, 0);
            else if (e.sampleTime < blockStart)
                ++late;
            else
                f(e, static_cast<int>(e.sampleTime - blockStart));
            ++used;
        }
        for (std::size_t i = used; i < count; ++i)
            events[i - used] = events[i];
        count -= used;
    }

    void clear() noexcept { count = 0; }
    std::size_t size() const noexcept { return count; }
    std::uint64_t lateEvents() const noexcept { return late; }
    std::uint64_t overflowEvents() const noexcept { return overflows; }

private:
    std::array<ScheduledEvent, Capacity> events{};
    std::size_t count = 0;
    std::uint64_t late = 0, overflows = 0;
};

} // namespace devildj::engine
