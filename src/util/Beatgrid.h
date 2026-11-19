#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace devildj::util
{

// A beatgrid (P2-14): anchors of (beat index, sample position, BPM until the next anchor, which beat of its bar
// the anchor is). One anchor = a constant tempo; more = tempo changes. Positions are in samples at `sampleRate`
// (the library stores the file's rate; a deck gets a copy scaled to its buffer's rate). A plain fixed-size value,
// so the audio thread can hold one without allocating.
struct GridAnchor
{
    double beat = 0.0;   // beat index (whole numbers for real anchors)
    double sample = 0.0; // where that beat is
    double bpm = 0.0;    // tempo from here to the next anchor
    int barBeat = 1;     // 1..4: this beat's place in its bar (4/4)
    friend bool operator==(const GridAnchor&, const GridAnchor&) = default;
};

class Beatgrid
{
public:
    static constexpr int kMaxAnchors = 16;
    static constexpr int kBeatsPerBar = 4;

    Beatgrid() = default;
    static Beatgrid constant(double firstBeatSample, double bpm, double sampleRate, int barBeat = 1);

    bool empty() const noexcept { return count == 0 || rate <= 0.0; }
    int size() const noexcept { return count; }
    const GridAnchor& anchor(int i) const noexcept { return anchors[static_cast<std::size_t>(i)]; }
    double sampleRate() const noexcept { return rate; }
    bool locked = false; // the user fixed it: analysis leaves it alone

    bool addAnchor(const GridAnchor& a); // keeps them sorted by beat; false when full or out of order

    // Mapping (positions before the first anchor use its tempo, after the last the last one's).
    double beatAt(double samplePosition) const noexcept;
    double sampleAt(double beat) const noexcept;
    double bpmAt(double samplePosition) const noexcept;
    double secondsPerBeatAt(double samplePosition) const noexcept;
    int barBeatOf(long long beatIndex) const noexcept; // 1..4
    long long nearestBeat(double samplePosition) const noexcept;
    double firstBpm() const noexcept { return empty() ? 0.0 : anchors[0].bpm; }

    // Edits.
    void shiftSamples(double samples);         // the whole grid later (+) / earlier (-)
    void shiftSeconds(double seconds) { shiftSamples(seconds * rate); }
    void shiftBeats(int beats);                 // the downbeat moves by whole beats (bar phase), beats stay
    void setDownbeatAt(double samplePosition);  // the nearest beat moves exactly here and becomes beat 1 of a bar
    void scaleTempo(double factor);             // x2 / /2: twice / half as many beats over the same time
    Beatgrid rescaled(double newSampleRate) const; // the same grid at another sample rate

    // beatgrids.anchors_json: [{"beat":0,"sample":12345,"bpm":128.0,"bar_beat":1}, ...] (sample at this rate).
    std::string toJson() const;
    static std::optional<Beatgrid> fromJson(std::string_view json, double sampleRate);

    friend bool operator==(const Beatgrid&, const Beatgrid&) = default;

private:
    int segmentFor(double samplePosition) const noexcept; // anchor whose tempo applies at this position
    int segmentForBeat(double beat) const noexcept;

    std::array<GridAnchor, kMaxAnchors> anchors{};
    int count = 0;
    double rate = 0.0;
};

// Tap tempo (P2-14): taps in seconds; the tempo is the least-squares fit of the last taps (at most 16). A gap of
// more than 2 s starts over. Message thread.
class TapTempo
{
public:
    void tap(double seconds);
    void reset() { taps.clear(); }
    int count() const noexcept { return static_cast<int>(taps.size()); }
    std::optional<double> bpm() const; // from 4 taps on
    double lastTap() const noexcept { return taps.empty() ? 0.0 : taps.back(); }

private:
    std::vector<double> taps;
};

} // namespace devildj::util
