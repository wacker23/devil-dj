#include "app/CodeDeckProducer.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <string_view>

namespace devildj::app
{

using util::Fraction;

struct CodeDeckProducer::Worker final : juce::Thread
{
    CodeDeckProducer& owner;
    explicit Worker(CodeDeckProducer& o)
        : juce::Thread("Code deck", 16 * 1024 * 1024)
        , owner(o)
    {
    }
    void run() override
    {
        while (!threadShouldExit())
        {
            owner.tick();
            wait(kTickMs);
        }
    }
};

namespace
{
std::optional<std::int64_t> integer(std::string_view s)
{
    std::int64_t x = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
    if (ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return x;
}

std::optional<double> real(std::string_view s)
{
    double x = 0.0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), x);
    if (ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return x;
}

// "n/d" -> a Fraction.
std::optional<Fraction> fraction(std::string_view s)
{
    const auto slash = s.find('/');
    if (slash == std::string_view::npos)
        return std::nullopt;
    const auto n = integer(s.substr(0, slash));
    const auto d = integer(s.substr(slash + 1));
    if (!n || !d || *d <= 0)
        return std::nullopt;
    return Fraction(*n, *d);
}
} // namespace

CodeDeckProducer::CodeDeckProducer(Push p, bool threaded, codebeat::JsLimits l)
    : now([] { return juce::Time::getMillisecondCounterHiRes() * 0.001; })
    , push(std::move(p))
    , limits(l)
{
    if (threaded)
    {
        worker = std::make_unique<Worker>(*this);
        worker->startThread();
    }
}

CodeDeckProducer::~CodeDeckProducer()
{
    if (worker != nullptr)
    {
        worker->signalThreadShouldExit();
        worker->notify();
        worker->stopThread(5000); // an evaluation may be running (at most its 200 ms budget)
    }
}

void CodeDeckProducer::setClock(const Clock& c)
{
    std::lock_guard<std::mutex> lock(mutex);
    clock = c;
}

void CodeDeckProducer::setDj(const codebeat::DjHooks& hooks)
{
    std::lock_guard<std::mutex> lock(mutex);
    dj = hooks;
}

void CodeDeckProducer::submit(std::string code, bool swapNow)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        pending = std::move(code);
        pendingSwapNow = swapNow;
    }
    if (worker != nullptr)
        worker->notify(); // evaluate now rather than at the next tick
}

std::int64_t CodeDeckProducer::sampleNow(double nowSeconds) const
{
    if (!clock)
        return 0;
    const double ahead = std::max(0.0, nowSeconds - clock->hostSeconds);
    return clock->sampleClock + static_cast<std::int64_t>(ahead * clock->sampleRate);
}

CodeDeckProducer::Status CodeDeckProducer::status()
{
    std::lock_guard<std::mutex> lock(mutex);
    Status s = counters;
    if (clock)
    {
        const engine::ClockSnapshot map{clock->sampleClock, clock->beat, std::max(1.0, clock->bpm), clock->sampleRate};
        s.cycle = map.beatAt(static_cast<double>(sampleNow(now()))) / s.beatsPerCycle;
    }
    return s;
}

std::string CodeDeckProducer::soundName(std::uint16_t index)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (index >= sounds.size())
        return {};
    const auto& name = sounds[index];
    return name.ends_with("~synth") ? name.substr(0, name.size() - 6) : name; // a synth's one sound for every n
}

void CodeDeckProducer::setSoundBank(std::shared_ptr<CodeSoundBank> b)
{
    std::lock_guard<std::mutex> lock(mutex);
    bank = std::move(b);
}

std::vector<std::pair<std::uint16_t, CodeSound>> CodeDeckProducer::takeNewSounds()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<std::uint16_t, CodeSound>> out;
    out.swap(newSounds);
    return out;
}

void CodeDeckProducer::warn(const std::string& warning)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto& w = counters.warnings;
        if (std::find(w.begin(), w.end(), warning) != w.end())
            return; // each once
        w.push_back(warning);
        if (w.size() > 50)
            w.erase(w.begin());
    }
    if (onWarning)
        onWarning(warning);
}

std::optional<std::uint16_t> CodeDeckProducer::soundIndex(const std::string& name, const CodeSound* resolved)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = soundIds.find(name); it != soundIds.end())
            return it->second;
        if (sounds.size() < static_cast<std::size_t>(engine::Voices::kSounds))
        {
            const auto index = static_cast<std::uint16_t>(sounds.size());
            sounds.push_back(name);
            soundKinds.push_back(resolved != nullptr ? resolved->voice : engine::VoiceKind::DrumSynth);
            soundIds.emplace(name, index);
            if (resolved != nullptr)
                newSounds.emplace_back(index, *resolved);
            return index;
        }
    }
    warn("more than " + std::to_string(engine::Voices::kSounds) + " different sounds: \"" + name + "\" is silent");
    return std::nullopt;
}

bool CodeDeckProducer::resolveSound(const std::string& sound, const std::string& n, bool hasNote, engine::ScheduledEvent& ev)
{
    std::shared_ptr<CodeSoundBank> b;
    {
        std::lock_guard<std::mutex> lock(mutex);
        b = bank;
    }
    if (b == nullptr) // no bank (tests): every name is a drum synth of its own
    {
        const auto index = soundIndex(n.empty() ? sound : sound + ":" + n, nullptr);
        if (index)
            ev.soundIndex = *index;
        return index.has_value();
    }
    // A note with no sound plays the triangle synth.
    const std::string name = sound.empty() ? std::string("tri") : sound;
    const int number = n.empty() ? 0 : static_cast<int>(std::lround(real(n).value_or(0.0)));
    const std::string key = name + ":" + std::to_string(number);
    std::optional<std::uint16_t> index;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = soundIds.find(key); it != soundIds.end())
            index = it->second;
        else if (const auto syn = soundIds.find(name + "~synth"); syn != soundIds.end())
            index = syn->second; // a synth: one sound whatever its n
    }
    if (!index)
    {
        const double t = now();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (const auto it = unknownUntil.find(name); it != unknownUntil.end() && t < it->second)
            {
                ++counters.unknownSounds;
                return false;
            }
        }
        const auto resolved = b->resolve(name, number);
        if (resolved.kind == CodeSound::Kind::Unknown)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++counters.unknownSounds;
                unknownUntil[name] = t + 2.0; // looked up again in 2 s (a folder may be copied in)
            }
            warn(resolved.warning);
            return false;
        }
        index = soundIndex(resolved.kind == CodeSound::Kind::Synth ? name + "~synth" : key, &resolved);
        if (!index)
            return false;
    }
    ev.soundIndex = *index;
    {
        std::lock_guard<std::mutex> lock(mutex);
        ev.kind = soundKinds[*index];
    }
    // A synth's n is a pitch: semitones above middle C (when no note is given).
    if (ev.kind == engine::VoiceKind::Synth && !hasNote && !n.empty())
        ev.note = 60.0f + static_cast<float>(real(n).value_or(0.0));
    return true;
}

void CodeDeckProducer::tick()
{
    const double t = now();
    std::optional<std::string> code;
    std::optional<codebeat::DjHooks> hooks;
    std::optional<Clock> c;
    bool swapNow = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        code.swap(pending);
        swapNow = pendingSwapNow;
        hooks = dj;
        c = clock;
        if (lastTickSeconds >= 0.0)
        {
            const double ms = (t - lastTickSeconds) * 1000.0;
            tickMsSum += ms;
            counters.maxTickMs = std::max(counters.maxTickMs, ms);
            counters.meanTickMs = tickMsSum / static_cast<double>(counters.ticks);
        }
        lastTickSeconds = t;
        ++counters.ticks;
    }

    if (evaluator == nullptr)
    {
        evaluator = std::make_unique<codebeat::CodeEvaluator>(limits); // on this thread: it is thread-confined
        std::lock_guard<std::mutex> lock(mutex);
        counters.ready = evaluator->ready();
        counters.lastError = evaluator->startupError();
    }
    if (!evaluator->ready())
        return;
    if (hooks)
        evaluator->setDj(*hooks);
    if (code)
    {
        const auto r = evaluator->evaluate(*code, swapNow);
        if (r.ok && r.cpb)
            beatsPerCycle = *r.cpb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (r.ok)
                counters.playing = r.playing;
            counters.lastError = r.ok ? std::string() : r.error;
            counters.beatsPerCycle = beatsPerCycle;
            ++counters.evaluations;
            counters.last = r;
        }
        if (onEvaluated)
            onEvaluated(r);
    }
    if (!c)
        return;

    // The window: from the cursor to 100 ms past now, in exact beats.
    const engine::ClockSnapshot map{c->sampleClock, c->beat, std::max(1.0, c->bpm), c->sampleRate};
    const double ahead = std::max(0.0, t - c->hostSeconds);
    const std::int64_t sNow = c->sampleClock + static_cast<std::int64_t>(ahead * c->sampleRate);
    const bool anchoredBefore = cursor.position().has_value();
    const auto window = cursor.next(map.beatAt(static_cast<double>(sNow)),
                                    map.beatAt(static_cast<double>(sNow) + kLookAheadSeconds * map.sampleRate));
    if (window.reanchored && anchoredBefore)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++counters.reanchors;
    }
    bool playing = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        playing = counters.playing;
    }
    if (!playing || !(window.begin < window.end))
        return;

    // Beats -> cycles, exactly.
    const Fraction cpb = Fraction::fromDouble(beatsPerCycle, 1000);
    const Fraction b = window.begin / cpb, e = window.end / cpb;
    const double range[] = {static_cast<double>(b.numerator()), static_cast<double>(b.denominator()),
                            static_cast<double>(e.numerator()), static_cast<double>(e.denominator())};
    const auto r = evaluator->runtime().callGlobal("__query", range);
    if (!r.ok)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (r.timedOut)
            ++counters.slowWindows; // this window stays silent
        else
            counters.lastError = r.error;
        return;
    }
    emit(r.value, map, sNow);
}

void CodeDeckProducer::emit(const std::string& reply, const engine::ClockSnapshot& map, std::int64_t sNow)
{
    const Fraction cpb = Fraction::fromDouble(beatsPerCycle, 1000);
    std::string_view rest = reply;
    while (!rest.empty())
    {
        const auto nl = rest.find('\n');
        const std::string_view line = rest.substr(0, nl);
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);

        // "<begin> <end>|<fields>|<locations>"
        const auto bar1 = line.find('|');
        const auto bar2 = bar1 == std::string_view::npos ? bar1 : line.find('|', bar1 + 1);
        if (bar2 == std::string_view::npos)
            continue;
        const auto times = line.substr(0, bar1);
        const auto space = times.find(' ');
        if (space == std::string_view::npos)
            continue;
        const auto begin = fraction(times.substr(0, space));
        const auto end = fraction(times.substr(space + 1));
        if (!begin || !end)
            continue;

        engine::ScheduledEvent ev;
        ev.source = engine::SourceId::CodeDeck;
        ev.sampleTime = map.sampleAt(*begin * cpb);
        if (ev.sampleTime < sNow)
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++counters.late; // its moment has passed already: dropped
            continue;
        }

        std::string sound;
        std::string sampleNumber;
        bool hasNote = false;
        double legato = 1.0;
        std::string_view fields = line.substr(bar1 + 1, bar2 - bar1 - 1);
        while (!fields.empty())
        {
            const auto semi = fields.find(';');
            const auto field = fields.substr(0, semi);
            fields = semi == std::string_view::npos ? std::string_view{} : fields.substr(semi + 1);
            const auto eq = field.find('=');
            if (eq == std::string_view::npos)
                continue;
            const auto key = field.substr(0, eq), value = field.substr(eq + 1);
            if (key == "s")
            {
                sound = std::string(value);
                continue;
            }
            if (key == "bank")
                continue; // sound banks: P5-11
            const auto x = real(value);
            if (!x)
                continue;
            const auto f = static_cast<float>(*x);
            if (key == "n")
                sampleNumber = std::string(value);
            else if (key == "note")
            {
                ev.note = f;
                hasNote = true;
            }
            else if (key == "gain")
                ev.gain = f;
            else if (key == "pan")
                ev.pan = f;
            else if (key == "speed")
                ev.speed = f;
            else if (key == "lpf")
                ev.cutoffHz = f;
            else if (key == "hpf")
                ev.hpfHz = f;
            else if (key == "resonance")
                ev.resonance = f;
            else if (key == "attack")
                ev.attack = f;
            else if (key == "decay")
                ev.decay = f;
            else if (key == "sustain")
                ev.sustain = f;
            else if (key == "release")
                ev.release = f;
            else if (key == "legato")
                legato = *x;
            else if (key == "room")
                ev.roomSend = f;
            else if (key == "delay")
                ev.delaySend = f;
            else if (key == "crush")
                ev.crush = f;
            else if (key == "cut")
                ev.chokeGroup = static_cast<std::uint8_t>(std::clamp(*x, 0.0, 255.0));
        }
        if (!resolveSound(sound, sampleNumber, hasNote, ev))
            continue; // an unknown sound (warned) or one too many
        const double beats = ((*end - *begin) * cpb).toDouble();
        ev.durationSeconds = static_cast<float>(beats * 60.0 / map.bpm * legato);

        const double leadMs = static_cast<double>(ev.sampleTime - sNow) * 1000.0 / map.sampleRate;
        const bool ok = push(ev);
        std::lock_guard<std::mutex> lock(mutex);
        if (!leadSeen)
        {
            counters.minLeadMs = counters.maxLeadMs = leadMs;
            leadSeen = true;
        }
        counters.minLeadMs = std::min(counters.minLeadMs, leadMs);
        counters.maxLeadMs = std::max(counters.maxLeadMs, leadMs);
        if (ok)
            ++counters.pushed;
        else
            ++counters.dropped;
    }
}

} // namespace devildj::app
