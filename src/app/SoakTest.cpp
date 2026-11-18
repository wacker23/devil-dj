#include "SoakTest.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace devildj::app
{

namespace
{
using engine::ChannelParam;
using engine::channelParam;
using engine::SetParam;

constexpr double kJumpBackBeforeEndSeconds = 8.0;
constexpr double kJumpBackToSeconds = 30.0;

double wave(double t, double periodSeconds, double phase = 0.0)
{
    return std::sin(2.0 * std::numbers::pi * (t / periodSeconds + phase));
}
} // namespace

SoakTest::SoakTest(Hooks h)
    : hooks(std::move(h))
{
}

SoakTest::~SoakTest()
{
    stopTimer();
}

int SoakTest::suggestedBuffer(int currentBuffer) noexcept
{
    int next = 64;
    while (next <= currentBuffer)
        next *= 2;
    return next;
}

juce::String SoakTest::start(Options o, bool useTimer)
{
    if (running)
        return "A soak test is already running";
    const auto s = hooks.engineState();
    if (!s.decks[0].loaded || !s.decks[1].loaded)
        return "Load a track on deck A and deck B first";

    options = o;
    running = true;
    perMinute.clear();
    startTime = minuteStart = hooks.now();
    endTime = startTime + options.minutes * 60.0;
    engineXrunsAtMinute = s.xruns;
    deviceAtStart = hooks.deviceStatus();
    deviceXrunsAtMinute = deviceAtStart.xruns;
    cpuSum = 0.0;
    cpuSamples = 0;
    cpuPeak = 0.0f;
    startedAt = juce::Time::getCurrentTime().toString(true, true, true, true);

    const double tempo[] = {3.0, -2.0};
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        hooks.send(engine::SetKeyLock{d, true});
        hooks.send(engine::SetTempo{d, tempo[d]});
        hooks.send(engine::Seek{d, kJumpBackToSeconds * s.sampleRate});
        hooks.send(engine::Play{d});
    }
    if (options.silent)
        hooks.send(SetParam{engine::ParamId::MasterGain, -1000.0f});
    if (useTimer)
        startTimerHz(10);
    return {};
}

void SoakTest::automate(double t, const engine::EngineState& s)
{
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        const double phase = d * 0.37;
        const auto set = [&](ChannelParam p, double v)
        { hooks.send(SetParam{channelParam(d, p), static_cast<float>(v)}); };
        // EQ in its usual range (-12 ... +3 dB), each band at its own speed; the filter sweeps both ways.
        set(ChannelParam::EqLow, -4.5 + 7.5 * wave(t, 7.0, phase));
        set(ChannelParam::EqMid, -4.5 + 7.5 * wave(t, 11.0, phase));
        set(ChannelParam::EqHigh, -4.5 + 7.5 * wave(t, 13.0, phase));
        set(ChannelParam::Filter, 0.5 + 0.3 * wave(t, 23.0, phase));

        // Keep both decks playing: jump back before the end (no loops until Phase 2).
        const auto& deck = s.decks[d];
        const double rate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
        if (deck.loaded && deck.lengthSamples > 0 &&
            deck.positionSamples > deck.lengthSamples - kJumpBackBeforeEndSeconds * rate)
            hooks.send(engine::Seek{d, std::min(kJumpBackToSeconds * rate, 0.1 * deck.lengthSamples)});
        if (deck.loaded && !deck.playing)
            hooks.send(engine::Play{d});
    }
    hooks.send(SetParam{engine::ParamId::Crossfader, static_cast<float>(0.5 + 0.3 * wave(t, 31.0))});
}

void SoakTest::closeMinute(double now, const engine::EngineState& s, const engine::AudioDeviceStatus& d)
{
    Minute m;
    m.index = static_cast<int>(perMinute.size()) + 1;
    m.engineXruns = s.xruns - engineXrunsAtMinute;
    m.deviceXruns = static_cast<std::uint32_t>(std::max(0, d.xruns - deviceXrunsAtMinute));
    m.cpuMean = cpuSamples > 0 ? static_cast<float>(cpuSum / cpuSamples) : 0.0f;
    m.cpuPeak = cpuPeak;
    m.memoryMb = hooks.memoryMb ? hooks.memoryMb() : 0.0;
    perMinute.push_back(m);
    engineXrunsAtMinute = s.xruns;
    deviceXrunsAtMinute = d.xruns;
    cpuSum = 0.0;
    cpuSamples = 0;
    cpuPeak = 0.0f;
    minuteStart = now;
}

void SoakTest::tick()
{
    if (!running)
        return;
    const double now = hooks.now();
    const auto s = hooks.engineState();
    cpuSum += s.cpuLoad;
    ++cpuSamples;
    cpuPeak = std::max({cpuPeak, s.cpuLoad, s.cpuLoadPeak});
    automate(now - startTime, s);
    if (now - minuteStart >= 60.0)
        closeMinute(now, s, hooks.deviceStatus());
    if (now >= endTime)
        finish();
}

void SoakTest::stop()
{
    if (running)
        finish();
}

void SoakTest::finish()
{
    stopTimer();
    const double now = hooks.now();
    if (cpuSamples > 0 && now - minuteStart > 1.0) // the last, partial minute
        closeMinute(now, hooks.engineState(), hooks.deviceStatus());
    running = false;
    endTime = now;

    // Put the mixer back the way a DJ expects it.
    for (engine::DeckId d = 0; d < 2; ++d)
    {
        hooks.send(engine::Pause{d});
        hooks.send(engine::ResetTempo{d});
        for (auto p : {ChannelParam::EqLow, ChannelParam::EqMid, ChannelParam::EqHigh})
            hooks.send(SetParam{channelParam(d, p), 0.0f});
        hooks.send(SetParam{channelParam(d, ChannelParam::Filter), 0.5f});
    }
    hooks.send(SetParam{engine::ParamId::Crossfader, 0.5f});
    if (options.silent)
        hooks.send(SetParam{engine::ParamId::MasterGain, 0.0f});
    if (onFinished)
        onFinished(report());
}

double SoakTest::elapsedMinutes() const
{
    return ((running ? hooks.now() : endTime) - startTime) / 60.0;
}

std::uint32_t SoakTest::totalXruns() const noexcept
{
    std::uint32_t n = 0;
    for (const auto& m : perMinute)
        n += m.engineXruns + m.deviceXruns;
    return n;
}

juce::String SoakTest::statusLine() const
{
    if (!running)
        return {};
    std::uint32_t xruns = totalXruns();
    const auto s = hooks.engineState();
    xruns += s.xruns - engineXrunsAtMinute; // the minute in progress
    return "SOAK TEST " + juce::String(elapsedMinutes(), 1) + " / " + juce::String(options.minutes, 0) +
           " min, glitches so far " + juce::String(static_cast<int>(xruns)) + "  (Options > Stop soak test)";
}

juce::String SoakTest::report() const
{
    const auto& d = deviceAtStart;
    juce::String r;
    const bool ok = passed();
    r << "Devil-DJ soak test (P1-20)\n"
      << "Started:  " << startedAt << "\n"
      << "Duration: " << juce::String(elapsedMinutes(), 1) << " of " << juce::String(options.minutes, 0) << " minutes"
      << (options.silent ? " (silent: master muted, everything still processed)" : "") << "\n"
      << "Device:   " << d.typeName << " / " << d.deviceName << ", " << juce::roundToInt(d.sampleRate) << " Hz, buffer "
      << d.bufferSize << " samples (" << juce::String(d.bufferMs, 1) << " ms), output latency "
      << juce::String(d.totalOutputLatencyMs, 1) << " ms\n"
      << "Load:     decks A and B playing with key lock (+3 % / -2 %), EQ, filter and crossfader moving\n\n"
      << "Result:   "
      << (ok ? "PASS - no glitches" : "FAIL - " + juce::String(static_cast<int>(totalXruns())) + " glitches") << "\n";
    if (!ok)
        r << "          Try a bigger buffer (" << suggestedBuffer(d.bufferSize)
          << " samples) in Options > Audio settings and run it again.\n";
    float worstPeak = 0.0f, meanSum = 0.0f;
    double memFirst = perMinute.empty() ? 0.0 : perMinute.front().memoryMb;
    double memLast = perMinute.empty() ? 0.0 : perMinute.back().memoryMb;
    for (const auto& m : perMinute)
    {
        worstPeak = std::max(worstPeak, m.cpuPeak);
        meanSum += m.cpuMean;
    }
    if (!perMinute.empty())
        r << "Audio CPU: mean " << juce::String(100.0f * meanSum / static_cast<float>(perMinute.size()), 1)
          << " %, worst peak " << juce::roundToInt(100.0f * worstPeak) << " % of the block time\n"
          << "Memory:   " << juce::roundToInt(memFirst) << " MB after the first minute, " << juce::roundToInt(memLast)
          << " MB at the end\n";
    r << "\nminute  engine xruns  driver xruns  cpu mean %  cpu peak %  memory MB\n";
    for (const auto& m : perMinute)
        r << juce::String(m.index).paddedLeft(' ', 6)
          << juce::String(static_cast<int>(m.engineXruns)).paddedLeft(' ', 14)
          << juce::String(static_cast<int>(m.deviceXruns)).paddedLeft(' ', 14)
          << juce::String(100.0f * m.cpuMean, 1).paddedLeft(' ', 12)
          << juce::String(juce::roundToInt(100.0f * m.cpuPeak)).paddedLeft(' ', 12)
          << juce::String(juce::roundToInt(m.memoryMb)).paddedLeft(' ', 11) << "\n";
    return r;
}

} // namespace devildj::app
