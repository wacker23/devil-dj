#include "beatmaker/Model.h"

#include <algorithm>

namespace devildj::beatmaker
{

Project::Project()
{
    for (int i = 0; i < kPatternCount; ++i)
        patterns[static_cast<std::size_t>(i)].name = std::string(1, static_cast<char>('A' + i));
}

DrumTrack drumTrack(const std::string& synthName, int lengthSteps)
{
    DrumTrack t;
    t.sound.synth = synthName;
    static const std::map<std::string, std::string> names{
        {"kick", "Kick"},       {"snare", "Snare"},       {"clap", "Clap"},       {"closedHat", "Closed hat"},
        {"openHat", "Open hat"}, {"tomLow", "Low tom"},    {"tomMid", "Mid tom"},  {"tomHigh", "High tom"},
        {"rim", "Rim"},         {"cowbell", "Cowbell"},   {"shaker", "Shaker"}};
    const auto it = names.find(synthName);
    t.name = it != names.end() ? it->second : synthName;
    if (synthName == "closedHat" || synthName == "openHat")
        t.chokeGroup = 1; // the open hat stops when the closed one hits
    t.steps.resize(static_cast<std::size_t>(lengthSteps));
    return t;
}

Project defaultProject()
{
    Project p;
    auto& a = p.patterns[0];
    for (const char* s : {"kick", "snare", "clap", "closedHat", "openHat", "tomLow", "rim", "shaker"})
        a.drums.push_back(drumTrack(s, a.lengthSteps));
    SynthTrack bass;
    bass.name = "Bass";
    bass.preset.engine = SynthPreset::Engine::SubBass;
    bass.preset.name = "Sub";
    a.synths.push_back(bass);
    return p;
}

void setPatternLength(Pattern& pattern, int lengthSteps)
{
    pattern.lengthSteps = lengthSteps <= 16 ? 16 : lengthSteps <= 32 ? 32 : 64;
    for (auto& d : pattern.drums)
        d.steps.resize(static_cast<std::size_t>(pattern.lengthSteps));
}

namespace
{
template <typename T>
T clampTo(int v, int lo, int hi)
{
    return static_cast<T>(std::clamp(v, lo, hi));
}

void normalizeMix(float& volumeDb, float& pan, float& delaySend, float& reverbSend)
{
    volumeDb = std::clamp(volumeDb, -60.0f, 12.0f);
    pan = std::clamp(pan, 0.0f, 1.0f);
    delaySend = std::clamp(delaySend, 0.0f, 1.0f);
    reverbSend = std::clamp(reverbSend, 0.0f, 1.0f);
}
} // namespace

void normalize(Project& project)
{
    project.swing = std::clamp(project.swing, 0.0f, kMaxSwing);
    project.ownBpm = std::clamp(project.ownBpm, 20.0, 300.0);
    for (auto& p : project.patterns)
    {
        p.stepsPerBeat = p.stepsPerBeat == 2 || p.stepsPerBeat == 3 || p.stepsPerBeat == 6 || p.stepsPerBeat == 8
                             ? p.stepsPerBeat
                             : 4;
        if (p.drums.size() > static_cast<std::size_t>(kMaxDrumTracks))
            p.drums.resize(static_cast<std::size_t>(kMaxDrumTracks));
        if (p.synths.size() > static_cast<std::size_t>(kMaxSynthTracks))
            p.synths.resize(static_cast<std::size_t>(kMaxSynthTracks));
        setPatternLength(p, p.lengthSteps);
        for (auto& d : p.drums)
        {
            for (auto& s : d.steps)
            {
                s.velocity = clampTo<std::uint8_t>(s.velocity, 1, 127);
                s.probability = clampTo<std::uint8_t>(s.probability, 0, 100);
                s.microTiming = clampTo<std::int8_t>(s.microTiming, -50, 50);
                s.ratchet = clampTo<std::uint8_t>(s.ratchet, 1, 4);
            }
            normalizeMix(d.volumeDb, d.pan, d.delaySend, d.reverbSend);
        }
        for (auto& t : p.synths)
        {
            for (auto& n : t.notes)
            {
                n.startTick = std::max(0, n.startTick);
                n.lengthTicks = std::max(1, n.lengthTicks);
                n.pitch = clampTo<std::uint8_t>(n.pitch, 0, 127);
                n.velocity = clampTo<std::uint8_t>(n.velocity, 1, 127);
            }
            std::stable_sort(t.notes.begin(), t.notes.end(),
                             [](const Note& a, const Note& b) { return a.startTick < b.startTick; });
            t.arp.rate = t.arp.rate <= 8 ? 8 : t.arp.rate <= 16 ? 16 : 32;
            t.arp.octaves = std::clamp(t.arp.octaves, 1, 4);
            normalizeMix(t.volumeDb, t.pan, t.delaySend, t.reverbSend);
        }
    }
    std::erase_if(project.song, [](int i) { return i < 0 || i >= kPatternCount; });
}

} // namespace devildj::beatmaker
