#include "beatmaker/Sequencer.h"

#include <algorithm>
#include <cmath>

namespace devildj::beatmaker
{

using util::Fraction;

namespace
{
std::uint64_t splitmix(std::uint64_t x) noexcept
{
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

std::uint64_t hashOf(std::uint32_t seed, int pattern, int track, int step, std::int64_t loop) noexcept
{
    std::uint64_t h = splitmix(seed);
    for (const auto v : {static_cast<std::uint64_t>(pattern), static_cast<std::uint64_t>(track),
                         static_cast<std::uint64_t>(step), static_cast<std::uint64_t>(loop)})
        h = splitmix(h ^ (v + 0x632be59bd9b4e019ull));
    return h;
}

Fraction patternBeats(const Pattern& p)
{
    return Fraction(p.lengthSteps, std::max(1, p.stepsPerBeat));
}

float gainOf(float volumeDb)
{
    return std::pow(10.0f, volumeDb / 20.0f);
}

bool inWindow(Fraction t, Fraction begin, Fraction end)
{
    return t >= begin && t < end;
}
} // namespace

int Sequencer::chance(std::uint32_t seed, int pattern, int track, int step, std::int64_t loop) noexcept
{
    return static_cast<int>(hashOf(seed, pattern, track, step, loop) % 100u);
}

void Sequencer::playPattern(int index, Fraction startBeat) noexcept
{
    plan.assign(1, Placement{startBeat, std::clamp(index, 0, kPatternCount - 1), startBeat, false});
}

void Sequencer::playSong(Fraction startBeat) noexcept
{
    plan.assign(1, Placement{startBeat, plan.back().pattern, startBeat, true});
}

void Sequencer::queuePattern(int index, Fraction atBeat)
{
    const int fallback = std::clamp(index, 0, kPatternCount - 1);
    std::erase_if(plan, [&](const Placement& p) { return !(p.from < atBeat); });
    plan.push_back(Placement{atBeat, fallback, atBeat, false});
}

void Sequencer::queueSong(Fraction atBeat)
{
    std::erase_if(plan, [&](const Placement& p) { return !(p.from < atBeat); });
    const int fallback = plan.empty() ? 0 : plan.back().pattern;
    plan.push_back(Placement{atBeat, fallback, atBeat, true});
}

Fraction Sequencer::nextBar(Fraction beat)
{
    return Fraction((beat / Fraction(4)).ceil() * 4);
}

std::vector<int> Sequencer::songList(const Project& project)
{
    std::vector<int> list;
    for (const int i : project.song)
        if (i >= 0 && i < kPatternCount)
            list.push_back(i);
    return list;
}

const Sequencer::Placement& Sequencer::at(Fraction beat) const
{
    const Placement* found = &plan.front();
    for (const auto& p : plan)
        if (!(beat < p.from))
            found = &p;
    return *found;
}

void Sequencer::prune(Fraction beat)
{
    while (plan.size() > 1 && !(beat < plan[1].from))
        plan.erase(plan.begin());
}

std::optional<Sequencer::Queued> Sequencer::queuedAfter(Fraction beat) const
{
    for (const auto& p : plan)
        if (beat < p.from && &p != &plan.front())
            return Queued{p.song ? -1 : p.pattern, p.song, p.from};
    return std::nullopt;
}

Fraction Sequencer::patternEnd(const Project& project, Fraction beat) const
{
    const auto& p = at(beat);
    const auto list = p.song ? songList(project) : std::vector<int>{};
    if (list.empty())
    {
        const auto len = patternBeats(project.patterns[static_cast<std::size_t>(p.pattern)]);
        if (beat < p.start)
            return p.start;
        return p.start + len * Fraction(((beat - p.start) / len).ceil());
    }
    // In the song: the end of the song pattern playing at `beat`.
    Fraction total = 0;
    for (const int i : list)
        total += patternBeats(project.patterns[static_cast<std::size_t>(i)]);
    if (beat < p.start)
        return p.start;
    Fraction t = p.start + total * Fraction(((beat - p.start) / total).floor());
    for (const int i : list)
    {
        t += patternBeats(project.patterns[static_cast<std::size_t>(i)]);
        if (!(t < beat))
            return t;
    }
    return t;
}

Sequencer::Position Sequencer::position(const Project& project, Fraction beat) const
{
    const auto& p = at(beat);
    if (beat < p.start)
        return {};
    const auto list = p.song ? songList(project) : std::vector<int>{};
    int index = p.pattern;
    Fraction offset = beat - p.start;
    if (list.empty())
    {
        const auto len = patternBeats(project.patterns[static_cast<std::size_t>(index)]);
        offset = offset - len * Fraction((offset / len).floor());
    }
    else
    {
        Fraction total = 0;
        for (const int i : list)
            total += patternBeats(project.patterns[static_cast<std::size_t>(i)]);
        offset = offset - total * Fraction((offset / total).floor());
        for (const int i : list)
        {
            const auto len = patternBeats(project.patterns[static_cast<std::size_t>(i)]);
            index = i;
            if (offset < len)
                break;
            offset -= len;
        }
    }
    const auto& pat = project.patterns[static_cast<std::size_t>(index)];
    return {index, static_cast<int>((offset * Fraction(std::max(1, pat.stepsPerBeat))).floor())};
}

void Sequencer::emit(const Project& project, Fraction begin, Fraction end, std::vector<BeatEvent>& out) const
{
    if (!(begin < end))
        return;
    const auto first = out.size();
    // Each placement over its part of the window: from its beat to the next one's.
    for (std::size_t i = 0; i < plan.size(); ++i)
    {
        const Fraction from = i == 0 ? begin : std::max(begin, plan[i].from);
        const Fraction to = i + 1 < plan.size() ? std::min(end, plan[i + 1].from) : end;
        if (from < to)
            emitPlacement(project, plan[i], from, to, out);
    }
    std::stable_sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
                     [](const BeatEvent& a, const BeatEvent& b) { return a.beat < b.beat; });
}

void Sequencer::emitPlacement(const Project& project, const Placement& placement, Fraction begin, Fraction end,
                              std::vector<BeatEvent>& out) const
{
    // An event can sit up to 0.5 step before its step (micro-timing) or up to 0.875 step after it (swing +
    // micro-timing), so the passes that touch the window are those starting within a pass of it.
    const int pattern = placement.pattern;
    const Fraction start = placement.start;
    const auto list = placement.song ? songList(project) : std::vector<int>{};
    if (list.empty())
    {
        const auto& p = project.patterns[static_cast<std::size_t>(pattern)];
        const auto len = patternBeats(p);
        const auto n0 = std::max<std::int64_t>(0, ((begin - start) / len).floor() - 1);
        const auto n1 = ((end - start) / len).floor() + 1;
        for (auto n = n0; n <= n1; ++n)
            emitPass(project, pattern, start + len * Fraction(n), n, begin, end, out);
    }
    else
    {
        Fraction total = 0;
        for (const int i : list)
            total += patternBeats(project.patterns[static_cast<std::size_t>(i)]);
        const auto c0 = std::max<std::int64_t>(0, ((begin - start) / total).floor() - 1);
        const auto c1 = ((end - start) / total).floor() + 1;
        for (auto c = c0; c <= c1; ++c)
        {
            Fraction at = start + total * Fraction(c);
            for (std::size_t k = 0; k < list.size(); ++k)
            {
                const auto len = patternBeats(project.patterns[static_cast<std::size_t>(list[k])]);
                if (at < end + len && at + len + len > begin)
                    emitPass(project, list[k], at, c * static_cast<std::int64_t>(list.size()) + static_cast<std::int64_t>(k),
                             begin, end, out);
                at += len;
            }
        }
    }
}

void Sequencer::emitPass(const Project& project, int index, Fraction passStart, std::int64_t loop, Fraction begin,
                         Fraction end, std::vector<BeatEvent>& out) const
{
    const auto& p = project.patterns[static_cast<std::size_t>(index)];
    const Fraction step(1, std::max(1, p.stepsPerBeat));
    const auto len = patternBeats(p);
    if (passStart + len + step < begin || !(passStart - step < end))
        return;
    const bool anySolo = std::any_of(p.drums.begin(), p.drums.end(), [](const DrumTrack& d) { return d.solo; }) ||
                         std::any_of(p.synths.begin(), p.synths.end(), [](const SynthTrack& s) { return s.solo; });
    const auto plays = [&](bool mute, bool solo) { return anySolo ? solo : !mute; };
    const auto swing = Fraction::fromDouble(std::clamp(static_cast<double>(project.swing), 0.0, 0.75), 1000);

    // Drum tracks.
    for (std::size_t t = 0; t < p.drums.size(); ++t)
    {
        const auto& d = p.drums[t];
        if (!plays(d.mute, d.solo))
            continue;
        const int steps = std::min(p.lengthSteps, static_cast<int>(d.steps.size()));
        for (int k = 0; k < steps; ++k)
        {
            const auto& s = d.steps[static_cast<std::size_t>(k)];
            if (!s.on)
                continue;
            const Fraction offset = (k % 2 == 1 ? swing * step / Fraction(2) : Fraction(0)) +
                                    Fraction(s.microTiming, 100) * step;
            const Fraction t0 = passStart + step * Fraction(k) + offset;
            if (t0 + step < begin || !(t0 < end))
                continue;
            if (s.probability < 100 &&
                chance(project.seed, index, static_cast<int>(t), k, loop) >= static_cast<int>(s.probability))
                continue;
            const int hits = std::clamp(static_cast<int>(s.ratchet), 1, 4);
            const Fraction hitLength = step / Fraction(hits);
            float velocity = static_cast<float>(s.velocity) / 127.0f;
            for (int h = 0; h < hits; ++h, velocity *= 0.9f)
            {
                const auto at = t0 + hitLength * Fraction(h);
                if (!inWindow(at, begin, end))
                    continue;
                BeatEvent e;
                e.beat = at;
                e.duration = hitLength;
                e.kind = d.sound.kind == SoundRef::Kind::Sample ? engine::VoiceKind::Sample : engine::VoiceKind::DrumSynth;
                e.pattern = index;
                e.track = static_cast<int>(t);
                e.step = k;
                e.velocity = velocity;
                e.gain = gainOf(d.volumeDb);
                e.pan = d.pan;
                e.delaySend = d.delaySend;
                e.reverbSend = d.reverbSend;
                e.chokeGroup = d.chokeGroup;
                out.push_back(e);
            }
        }
    }

    // Synth tracks.
    const int lengthTicks = p.lengthSteps * (kPpq / std::max(1, p.stepsPerBeat));
    for (std::size_t t = 0; t < p.synths.size(); ++t)
    {
        const auto& s = p.synths[t];
        if (!plays(s.mute, s.solo))
            continue;
        const auto push = [&](Fraction at, Fraction duration, int stepIndex, std::uint8_t pitch, std::uint8_t velocity)
        {
            if (!inWindow(at, begin, end))
                return;
            BeatEvent e;
            e.beat = at;
            e.duration = duration;
            e.kind = engine::VoiceKind::Synth;
            e.pattern = index;
            e.track = static_cast<int>(t);
            e.step = stepIndex;
            e.pitch = pitch;
            e.velocity = static_cast<float>(velocity) / 127.0f;
            e.gain = gainOf(s.volumeDb);
            e.pan = s.pan;
            e.delaySend = s.delaySend;
            e.reverbSend = s.reverbSend;
            out.push_back(e);
        };
        if (!s.arp.on)
        {
            for (std::size_t j = 0; j < s.notes.size(); ++j)
            {
                const auto& n = s.notes[j];
                if (n.startTick < lengthTicks)
                    push(passStart + Fraction(n.startTick, kPpq), Fraction(n.lengthTicks, kPpq), static_cast<int>(j), n.pitch,
                         n.velocity);
            }
            continue;
        }
        // The arpeggiator: at each of its steps, the notes held then, in its order, one after another; a new
        // chord starts the order again.
        const int arpTicks = std::max(1, 4 * kPpq / std::clamp(s.arp.rate, 8, 32));
        const Fraction arpStep(arpTicks, kPpq);
        std::vector<std::pair<int, const Note*>> held, before; // (order key, note)
        int counter = 0;
        for (int j = 0; j * arpTicks < lengthTicks; ++j)
        {
            const int tick = j * arpTicks;
            held.clear();
            for (const auto& n : s.notes)
                if (n.startTick <= tick && tick < n.startTick + n.lengthTicks && n.startTick < lengthTicks)
                    held.push_back({s.arp.mode == ArpSettings::Mode::AsPlayed ? n.startTick : n.pitch, &n});
            if (held.empty())
            {
                before.clear();
                continue;
            }
            std::stable_sort(held.begin(), held.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            if (held != before)
                counter = 0;
            before = held;
            std::vector<std::pair<int, std::uint8_t>> order; // (pitch, velocity)
            for (int o = 0; o < std::clamp(s.arp.octaves, 1, 4); ++o)
                for (const auto& h : held)
                    if (h.second->pitch + 12 * o <= 127)
                        order.push_back({h.second->pitch + 12 * o, h.second->velocity});
            if (s.arp.mode == ArpSettings::Mode::Down)
                std::reverse(order.begin(), order.end());
            else if (s.arp.mode == ArpSettings::Mode::UpDown && order.size() > 2)
                for (auto i = order.size() - 2; i >= 1; --i)
                    order.push_back(order[i]);
            std::size_t pick = static_cast<std::size_t>(counter) % order.size();
            if (s.arp.mode == ArpSettings::Mode::Random)
                pick = static_cast<std::size_t>(hashOf(project.seed, index, 100 + static_cast<int>(t), j, loop) % order.size());
            ++counter;
            push(passStart + Fraction(tick, kPpq), arpStep * Fraction(9, 10), j, static_cast<std::uint8_t>(order[pick].first),
                 order[pick].second);
        }
    }
}

engine::ScheduledEvent toScheduled(const BeatEvent& e, const engine::ClockSnapshot& clock)
{
    engine::ScheduledEvent s;
    s.sampleTime = clock.sampleAt(e.beat);
    s.source = engine::SourceId::BeatMaker;
    s.kind = e.kind;
    s.soundIndex = static_cast<std::uint16_t>(e.pattern * 16 + e.track); // the voices' sound tables (P4-10)
    s.note = static_cast<float>(e.pitch);
    s.velocity = e.velocity;
    s.gain = e.gain;
    s.pan = e.pan;
    s.delaySend = e.delaySend;
    s.roomSend = e.reverbSend;
    s.chokeGroup = e.chokeGroup;
    s.durationSeconds = static_cast<float>(e.duration.toDouble() * 60.0 / clock.bpm);
    s.highlightToken = (static_cast<std::uint32_t>(e.track) << 16) | static_cast<std::uint32_t>(e.step & 0xffff);
    return s;
}

} // namespace devildj::beatmaker
