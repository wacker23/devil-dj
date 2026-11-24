#include "app/CodeRender.h"

#include "app/CodeDeckProducer.h"
#include "app/CodeSoundBank.h"
#include "engine/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <deque>

namespace devildj::app
{

namespace
{
// A sample file decoded for this render (kept until it ends).
struct Decoded
{
    juce::AudioBuffer<float> audio;
    const float* channels[2]{};
    engine::SampleData data;
};
} // namespace

CodeRenderResult renderCode(const std::string& code, const CodeRenderOptions& o)
{
    CodeRenderResult result;
    result.bpm = std::clamp(o.bpm, 20.0, 300.0);
    const int block = std::max(16, o.blockSize);
    const double sr = o.sampleRate > 0.0 ? o.sampleRate : 48000.0;

    // The private engine: the internal clock at the tempo, the CODE channel as playback leaves it by default.
    auto engine = std::make_unique<engine::AudioEngine>();
    engine->prepare(sr, block);
    engine->send(engine::SetInternalTempo{result.bpm});
    engine->send(engine::SetClockSource{engine::MasterClock::Mode::Internal, engine::MasterClock::kInternal});

    // Its producer, ticked by hand on the render's own clock (seconds of audio).
    double seconds = 0.0;
    std::int64_t endSample = std::numeric_limits<std::int64_t>::max(); // nothing at or after the end is played
    CodeDeckProducer producer(
        [&](const engine::ScheduledEvent& e)
        {
            if (e.sampleTime >= endSample)
                return true; // past the last cycle: dropped (as if it had been played)
            return engine->codeDeckEvents().tryPush(e);
        },
        false);
    producer.now = [&] { return seconds; };
    producer.setSoundBank(std::make_shared<CodeSoundBank>(o.samplesFolder));
    producer.submit(code, true);

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::deque<Decoded> decoded;
    const auto install = [&]
    {
        for (const auto& [index, sound] : producer.takeNewSounds())
        {
            if (sound.kind == CodeSound::Kind::Drum)
                engine->send(engine::SetDrumSound{index, sound.drum, engine::SourceId::CodeDeck});
            else if (sound.kind == CodeSound::Kind::Synth)
                engine->send(engine::SetSynthSound{index, sound.synth, engine::SourceId::CodeDeck});
            else if (sound.kind == CodeSound::Kind::Sample)
            {
                const engine::SampleData* data = nullptr;
                if (std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(sound.sample)); reader != nullptr)
                {
                    auto& d = decoded.emplace_back();
                    const int channels = std::clamp(static_cast<int>(reader->numChannels), 1, 2);
                    const int frames = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, static_cast<juce::int64>(60.0 * reader->sampleRate)));
                    d.audio.setSize(channels, std::max(1, frames));
                    reader->read(&d.audio, 0, frames, 0, true, channels > 1);
                    for (int c = 0; c < channels; ++c)
                        d.channels[c] = d.audio.getReadPointer(c);
                    d.data = {d.channels, channels, frames, reader->sampleRate};
                    data = &d.data;
                }
                engine->send(engine::SetSampleSound{index, data, engine::SourceId::CodeDeck});
            }
        }
    };
    // One producer step, as the app does it: the engine's clock handed over, then the sounds named for the first time.
    const auto tick = [&]
    {
        const auto& s = engine->readState();
        producer.setClock({s.sampleClock, s.masterBeat, s.masterBpm, s.sampleRate, seconds});
        producer.tick();
        install();
    };

    std::vector<float> l(static_cast<std::size_t>(block)), r(static_cast<std::size_t>(block));
    float* outs[] = {l.data(), r.data()};
    // A first block so the engine's state is there, then the evaluation (the first tick) - as playback starts.
    engine->process(outs, 2, block);
    seconds += block / sr;
    tick();
    const auto st = producer.status();
    if (!st.last.ok)
    {
        result.error = st.last.error.empty() ? "the code didn't load" : st.last.error;
        return result;
    }
    result.beatsPerCycle = st.beatsPerCycle;
    // The cycles from the next cycle line on (where a pattern evaluated "now" starts sounding whole).
    const double startBeat = engine->readState().masterBeat;
    const double beatsPerCycle = std::max(1.0, result.beatsPerCycle);
    const double firstCycle = std::ceil(startBeat / beatsPerCycle);
    const auto beatSample = [&](double beat) { return static_cast<std::int64_t>(std::llround(beat * 60.0 * sr / result.bpm)); };
    const auto loopStart = beatSample(firstCycle * beatsPerCycle);
    endSample = beatSample((firstCycle + std::max(1, o.cycles)) * beatsPerCycle);
    result.loopSamples = static_cast<int>(endSample - loopStart);
    const auto total = endSample + static_cast<std::int64_t>(std::llround(std::max(0.0, o.tailSeconds) * sr));

    result.audio = std::make_unique<engine::TrackBuffer>();
    result.audio->sampleRate = sr;
    result.audio->audio.setSize(2, std::max(1, static_cast<int>(total - loopStart)));
    result.audio->audio.clear();
    const int ticksEvery = std::max(1, static_cast<int>(std::lround(0.025 * sr / block)));
    std::int64_t at = block;
    for (int blockIndex = 1; at < total; ++blockIndex)
    {
        if (blockIndex % ticksEvery == 0)
            tick();
        const int n = static_cast<int>(std::min<std::int64_t>(block, total - at));
        engine->process(outs, 2, n);
        // The part of this block from the first cycle line on.
        for (int i = 0; i < n; ++i)
        {
            const auto sample = at + i;
            if (sample >= loopStart)
            {
                result.audio->audio.setSample(0, static_cast<int>(sample - loopStart), l[static_cast<std::size_t>(i)]);
                result.audio->audio.setSample(1, static_cast<int>(sample - loopStart), r[static_cast<std::size_t>(i)]);
            }
        }
        seconds += n / sr;
        at += n;
    }
    result.warnings = producer.status().warnings;
    return result;
}

} // namespace devildj::app
