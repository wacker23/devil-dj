#include "app/BeatRender.h"

#include "beatmaker/DrumKits.h"
#include "beatmaker/Sequencer.h"
#include "beatmaker/SynthPresets.h"
#include "engine/AudioEngine.h"

#include <algorithm>
#include <cmath>

namespace devildj::app
{

using util::Fraction;

namespace
{
Fraction patternBeats(const beatmaker::Pattern& p)
{
    return Fraction(p.lengthSteps, std::max(1, p.stepsPerBeat));
}
} // namespace

double renderBeats(const beatmaker::Project& project, const BeatRenderOptions& o)
{
    Fraction total = 0;
    if (o.song)
        for (const int i : project.song)
            if (i >= 0 && i < beatmaker::kPatternCount)
                total += patternBeats(project.patterns[static_cast<std::size_t>(i)]);
    if (total == Fraction(0))
        total = patternBeats(project.patterns[static_cast<std::size_t>(std::clamp(o.pattern, 0, beatmaker::kPatternCount - 1))]) *
                Fraction(std::max(1, o.passes));
    return total.toDouble();
}

BeatRenderResult renderBeat(const beatmaker::Project& project, const BeatRenderOptions& o)
{
    BeatRenderResult result;
    result.bpm = project.followMaster ? o.masterBpm : project.ownBpm;
    result.bpm = std::clamp(result.bpm, 20.0, 300.0);
    result.beats = renderBeats(project, o);
    const int block = std::max(16, o.blockSize);
    result.loopSamples = static_cast<int>(std::llround(result.beats * 60.0 * o.sampleRate / result.bpm));
    const int total = result.loopSamples + static_cast<int>(std::llround(std::max(0.0, o.tailSeconds) * o.sampleRate));

    // The private engine: the internal clock at the tempo, the BEATS channel as playback leaves it by default.
    auto engine = std::make_unique<engine::AudioEngine>();
    engine->prepare(o.sampleRate, block);
    engine->send(engine::SetInternalTempo{result.bpm});
    engine->send(engine::SetClockSource{engine::MasterClock::Mode::Internal, engine::MasterClock::kInternal});
    for (int p = 0; p < beatmaker::kPatternCount; ++p)
    {
        const auto& pat = project.patterns[static_cast<std::size_t>(p)];
        for (std::size_t t = 0; t < pat.drums.size() && t < 16; ++t)
        {
            const auto index = static_cast<std::uint16_t>(p * 16 + static_cast<int>(t));
            const auto& sound = pat.drums[t].sound;
            if (sound.kind == beatmaker::SoundRef::Kind::Sample)
                engine->send(engine::SetSampleSound{index, o.sample ? o.sample(sound.samplePath) : nullptr});
            else
                engine->send(engine::SetDrumSound{index, beatmaker::engineDrumSound(sound)});
        }
        for (std::size_t t = 0; t < pat.synths.size() && t < 16; ++t)
            engine->send(engine::SetSynthSound{static_cast<std::uint16_t>(p * 16 + static_cast<int>(t)),
                                               beatmaker::synthParamsOf(pat.synths[t].preset)});
    }

    // Every event, from beat 0 at sample 0 (pushed a little ahead of each block: the queues hold 2,048).
    beatmaker::Sequencer seq;
    seq.playPattern(o.pattern, 0);
    if (o.song && !project.song.empty())
        seq.playSong(0);
    std::vector<beatmaker::BeatEvent> events;
    seq.emit(project, 0, Fraction::fromDouble(result.beats, 96000), events);
    const engine::ClockSnapshot clock{0, 0.0, result.bpm, o.sampleRate};
    std::vector<engine::ScheduledEvent> scheduled;
    scheduled.reserve(events.size());
    for (const auto& e : events)
        scheduled.push_back(beatmaker::toScheduled(e, clock));

    result.audio = std::make_unique<engine::TrackBuffer>();
    result.audio->sampleRate = o.sampleRate;
    result.audio->audio.setSize(2, std::max(1, total));
    result.audio->audio.clear();
    std::vector<float> l(static_cast<std::size_t>(block)), r(static_cast<std::size_t>(block));
    float* outs[] = {l.data(), r.data()};
    std::size_t next = 0;
    for (int at = 0; at < total; at += block)
    {
        const int n = std::min(block, total - at);
        while (next < scheduled.size() && scheduled[next].sampleTime < at + 2 * block &&
               engine->beatMakerEvents().tryPush(scheduled[next]))
            ++next;
        engine->process(outs, 2, n);
        result.audio->audio.copyFrom(0, at, l.data(), n);
        result.audio->audio.copyFrom(1, at, r.data(), n);
    }
    return result;
}

} // namespace devildj::app
