#pragma once

// The Beat Maker's data model (BEATMAKER_SPEC §1). Plain values, no JUCE: the sequencer (P4-02) reads it, the UI
// edits it on the message thread, ProjectFile saves and loads it as a .ddjbeat (JSON).

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace devildj::beatmaker
{

constexpr int kPpq = 96;            // ticks per quarter note (beat)
constexpr int kSchemaVersion = 1;   // the .ddjbeat format this build writes
constexpr int kPatternCount = 8;    // A .. H
constexpr int kMaxDrumTracks = 8;
constexpr int kMaxSynthTracks = 4;
constexpr float kMaxSwing = 0.75f;

struct Step
{
    bool on = false;
    std::uint8_t velocity = 100;   // 1..127
    std::uint8_t probability = 100; // 0..100 %
    std::int8_t microTiming = 0;   // -50..+50 % of a step
    std::uint8_t ratchet = 1;      // 1..4 hits within the step

    bool operator==(const Step&) const = default;
};

// What a drum track plays: one of the built-in drum synths (BEATMAKER_SPEC §4) with its parameters, or a user
// sample (a WAV path).
struct SoundRef
{
    enum class Kind
    {
        Synth,
        Sample
    };
    Kind kind = Kind::Synth;
    std::string synth = "kick";         // kick, snare, clap, closedHat, openHat, tomLow, tomMid, tomHigh, rim,
                                        // cowbell, shaker
    std::string samplePath;             // Kind::Sample
    std::map<std::string, float> params; // tune, decay, tone, snap, drive (those the synth has; missing = default)
    std::string unknown;                // fields this build doesn't know, as a JSON object (kept on save)

    bool operator==(const SoundRef&) const = default;
};

struct DrumTrack
{
    std::string name;
    SoundRef sound;
    std::vector<Step> steps; // size = the pattern's length
    float volumeDb = 0.0f, pan = 0.5f, delaySend = 0.0f, reverbSend = 0.0f;
    std::uint8_t chokeGroup = 0; // 0 = none
    bool mute = false, solo = false;
    std::string unknown;

    bool operator==(const DrumTrack&) const = default;
};

struct Note
{
    int startTick = 0;
    int lengthTicks = kPpq / 4;
    std::uint8_t pitch = 60;
    std::uint8_t velocity = 100;

    bool operator==(const Note&) const = default;
};

// A synth engine (BEATMAKER_SPEC §5) and its parameters.
struct SynthPreset
{
    enum class Engine
    {
        Subtractive,
        Fm,
        SubBass
    };
    Engine engine = Engine::Subtractive;
    std::string name = "Init";
    std::map<std::string, float> params;
    std::string unknown;

    bool operator==(const SynthPreset&) const = default;
};

struct ArpSettings
{
    enum class Mode
    {
        Up,
        Down,
        UpDown,
        Random,
        AsPlayed
    };
    bool on = false;
    int rate = 16;   // 8, 16 or 32 (1/8, 1/16, 1/32 notes)
    Mode mode = Mode::Up;
    int octaves = 1; // 1..4
    std::string unknown;

    bool operator==(const ArpSettings&) const = default;
};

struct SynthTrack
{
    std::string name;
    SynthPreset preset;
    std::vector<Note> notes;
    ArpSettings arp;
    float volumeDb = 0.0f, pan = 0.5f, delaySend = 0.0f, reverbSend = 0.0f;
    bool mute = false, solo = false;
    std::string unknown;

    bool operator==(const SynthTrack&) const = default;
};

struct Pattern
{
    std::string name;      // "A" .. "H"
    int lengthSteps = 16;  // 16, 32 or 64
    int stepsPerBeat = 4;  // 16th notes
    std::vector<DrumTrack> drums;   // up to 8
    std::vector<SynthTrack> synths; // up to 4
    std::string unknown;

    int stepTicks() const noexcept { return kPpq / stepsPerBeat; }
    int lengthTicks() const noexcept { return lengthSteps * stepTicks(); }
    bool operator==(const Pattern&) const = default;
};

struct Project
{
    int schemaVersion = kSchemaVersion;
    bool followMaster = true;
    double ownBpm = 120.0;
    float swing = 0.0f; // 0..0.75, applied to every 2nd step
    std::array<Pattern, kPatternCount> patterns;
    std::vector<int> song;  // pattern indices in order; empty = loop the current one
    std::uint32_t seed = 1; // the probability RNG's
    std::string unknown;

    Project();
    bool operator==(const Project&) const = default;
};

// A new project: patterns A .. H, pattern A with the default kit (8 drum tracks, empty steps) and one bass synth.
Project defaultProject();
// The default kit's drum track for synth `name` (empty steps of `lengthSteps`).
DrumTrack drumTrack(const std::string& synthName, int lengthSteps = 16);
// P4-13: a user sample on a drum track (its name the file's, no choke group change).
void setSample(DrumTrack& track, const std::string& path, const std::string& name);
// The pattern's length: 16, 32 or 64 steps; every drum track's steps resized to it (new steps off).
void setPatternLength(Pattern& pattern, int lengthSteps);
// Values brought into range (after a load or an edit): step fields, track counts, lengths, swing, song indices.
void normalize(Project& project);

} // namespace devildj::beatmaker
