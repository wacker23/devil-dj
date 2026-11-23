#include "app/AutoTransition.h"

#include "util/MusicalKey.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace devildj::app
{

namespace
{
constexpr float kSilentDb = -60.0f;   // a bar quieter than this has no sound
constexpr float kBassStepDb = 6.0f;   // the bass "comes in" / "leaves" when its level moves this much
constexpr float kNoBassDb = -40.0f;   // and below this a bar has no bass to speak of

float toDb(double rms)
{
    return rms > 1e-6 ? static_cast<float>(20.0 * std::log10(rms)) : -120.0f;
}

float meanLow(const std::vector<BarLevels>& bars, int from, int to)
{
    from = std::max(0, from);
    to = std::min(static_cast<int>(bars.size()), to);
    if (to <= from)
        return -120.0f;
    double sum = 0.0;
    for (int b = from; b < to; ++b)
        sum += bars[static_cast<std::size_t>(b)].low;
    return static_cast<float>(sum / (to - from));
}

float maxLow(const std::vector<BarLevels>& bars, int from, int to)
{
    float m = -120.0f;
    for (int b = std::max(0, from); b < std::min(static_cast<int>(bars.size()), to); ++b)
        m = std::max(m, bars[static_cast<std::size_t>(b)].low);
    return m;
}

std::string text(const char* format, double a = 0.0, double b = 0.0, double c = 0.0)
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), format, a, b, c);
    return buffer;
}

int snap4(int bar)
{
    return std::max(0, bar / 4 * 4);
}
} // namespace

float BarLevels::total() const noexcept
{
    // Power sum of the three bands.
    const double p = std::pow(10.0, low / 10.0) + std::pow(10.0, mid / 10.0) + std::pow(10.0, high / 10.0);
    return p > 0.0 ? static_cast<float>(10.0 * std::log10(p)) : -120.0f;
}

double firstDownbeatBeat(const util::Beatgrid& grid)
{
    if (grid.empty())
        return 0.0;
    auto k = static_cast<long long>(std::ceil(grid.beatAt(0.0) - 1e-9));
    for (int guard = 0; guard < 8 && grid.barBeatOf(k) != 1; ++guard)
        ++k;
    return static_cast<double>(k);
}

std::vector<BarLevels> barProfile(const analysis::Waveform& w, const util::Beatgrid& grid, int numSamples)
{
    std::vector<BarLevels> out;
    if (w.empty() || grid.empty() || w.samplesPerPoint <= 0)
        return out;
    const double first = firstDownbeatBeat(grid);
    for (int bar = 0;; ++bar)
    {
        const double a = grid.sampleAt(first + 4.0 * bar), b = grid.sampleAt(first + 4.0 * (bar + 1));
        if (b > numSamples || a >= numSamples)
            break;
        const int p0 = std::clamp(static_cast<int>(a / w.samplesPerPoint), 0, w.numPoints());
        const int p1 = std::clamp(static_cast<int>(b / w.samplesPerPoint), p0, w.numPoints());
        BarLevels lv;
        if (p1 > p0)
        {
            float* fields[] = {&lv.low, &lv.mid, &lv.high};
            for (int band = 0; band < analysis::Waveform::kBands; ++band)
            {
                double sum = 0.0;
                for (int p = p0; p < p1; ++p)
                {
                    const double r = w.rms[static_cast<std::size_t>(band)][static_cast<std::size_t>(p)];
                    sum += r * r;
                }
                *fields[band] = toDb(std::sqrt(sum / (p1 - p0)));
            }
        }
        out.push_back(lv);
    }
    return out;
}

TrackShape trackShape(const std::vector<BarLevels>& bars, double firstDownbeat)
{
    TrackShape s;
    s.firstDownbeatBeat = firstDownbeat;
    s.bars = static_cast<int>(bars.size());
    s.contentEnd = s.bars;
    while (s.contentEnd > 0 && bars[static_cast<std::size_t>(s.contentEnd - 1)].total() < kSilentDb)
        --s.contentEnd;
    // The intro's end: the first phrase line in the first 45 % where the bass steps up and is really there.
    s.introEnd = 0;
    if (meanLow(bars, 0, 4) < kNoBassDb || meanLow(bars, 0, 4) + kBassStepDb < meanLow(bars, 4, 12))
        for (int b = 4; b <= s.contentEnd * 45 / 100; b += 4)
            if (meanLow(bars, b, b + 4) >= meanLow(bars, b - 4, b) + kBassStepDb && meanLow(bars, b, b + 4) > kNoBassDb)
            {
                s.introEnd = b;
                break;
            }
    // The outro: the last phrase line in the last 45 % where the bass steps down and stays down to the end.
    s.outroStart = s.contentEnd;
    for (int b = snap4(s.contentEnd - 4); b >= s.contentEnd * 55 / 100; b -= 4)
        if (maxLow(bars, b, s.contentEnd) + kBassStepDb <= meanLow(bars, b - 4, b) && // out, and out to the end
            meanLow(bars, b - 4, b) > kNoBassDb)
            s.outroStart = b;
        else if (s.outroStart != s.contentEnd)
            break; // the bass was there just before: the outro starts at the later line
    return s;
}

AutoPlan planAutoTransition(const AutoInput& out, const AutoInput& in)
{
    AutoPlan plan;
    if (out.waveform == nullptr || in.waveform == nullptr || out.grid.empty() || in.grid.empty())
    {
        plan.reasons.push_back("needs both tracks analyzed (waveform and beatgrid)");
        return plan;
    }
    const auto outBars = barProfile(*out.waveform, out.grid, out.numSamples);
    const auto inBars = barProfile(*in.waveform, in.grid, in.numSamples);
    if (outBars.size() < 8 || inBars.size() < 8)
    {
        plan.reasons.push_back("a track is shorter than 8 bars");
        return plan;
    }
    plan.outgoing = trackShape(outBars, firstDownbeatBeat(out.grid));
    plan.incoming = trackShape(inBars, firstDownbeatBeat(in.grid));
    const auto& o = plan.outgoing;
    const auto& i = plan.incoming;
    const bool hasOutro = o.outroStart < o.contentEnd;
    const int intro = std::min(i.introEnd, 32);

    // Keys, tempo, energy.
    const bool keysKnown = util::isValidKey(out.key) && util::isValidKey(in.key);
    const bool keysFit = !keysKnown || util::harmonicallyCompatible(out.key, in.key);
    const double gap = out.bpm > 0.0 && in.bpm > 0.0 ? std::abs(in.bpm / out.bpm - 1.0) : 0.0;
    const float outLevel = meanLow(outBars, o.contentEnd * 2 / 5, o.outroStart), inLevel = meanLow(inBars, i.introEnd, i.introEnd + 16);
    const bool moreEnergy = inLevel > outLevel + 2.0f;

    // Timing: the incoming bass entry on the outgoing bass exit.
    int start = 0, bars = 8;
    float swap = 4.0f;
    if (!hasOutro || gap > 0.08)
    {
        // No outro to mix over (or tempi too far apart to ride together): out on an echo over its last 4 bars.
        bars = 4;
        start = snap4(o.contentEnd - 4);
        swap = 3.0f;
        plan.preset = TransitionPreset::EchoOut;
        plan.reasons.push_back(hasOutro ? text("tempi %.0f%% apart: a short echo out rather than riding them together", gap * 100.0)
                                        : std::string("the outgoing track ends at full level (no outro): it ends on an echo"));
    }
    else
    {
        start = snap4(o.outroStart - intro);
        start = std::max(start, snap4(o.contentEnd / 2)); // not from the middle of the track
        bars = std::clamp(o.contentEnd - start, 4, 32);
        bars = std::max(4, bars / 4 * 4);
        swap = static_cast<float>(std::clamp(o.outroStart - start, 1, bars - 1));
        if (intro > 0)
            plan.reasons.push_back(text("the new track's bass comes in after its %.0f-bar intro, where the old track's bass leaves for its outro (bar %.0f)",
                                        intro, o.outroStart + 1));
        else
            plan.reasons.push_back(text("the new track starts with its bass: it comes in at the old track's outro (bar %.0f)", o.outroStart + 1));
        if (!keysFit)
        {
            bars = std::min(bars, 8);
            start = std::max(start, snap4(o.contentEnd - bars));
            swap = std::clamp(swap, 1.0f, static_cast<float>(bars - 1));
            plan.preset = TransitionPreset::HighPassOut;
            plan.reasons.push_back(std::string("the keys clash (") + util::camelotCode(out.key) + " / " + util::camelotCode(in.key) +
                                   "): short, the old track filtered away");
        }
        else if (intro >= 16 && bars >= 16 && !moreEnergy)
        {
            plan.preset = TransitionPreset::LongBlend;
            plan.reasons.push_back(text("a %.0f-bar intro and keys that fit: a long blend", intro));
        }
        else if (moreEnergy)
        {
            plan.preset = TransitionPreset::FilterRise;
            plan.reasons.push_back("the new track brings more energy: it rises out of a filter");
        }
        else if (intro > 0)
        {
            plan.preset = TransitionPreset::BassSwap;
            plan.reasons.push_back("the intro has no bass: the new track comes in early, the basses swap on the phrase");
        }
        else
        {
            plan.preset = TransitionPreset::SmoothBlend;
            plan.reasons.push_back("both carry bass: a smooth blend, the basses swapped on the phrase");
        }
        if (keysKnown && keysFit)
            plan.reasons.push_back(std::string("keys fit: ") + util::camelotCode(out.key) + " to " + util::camelotCode(in.key));
    }
    plan.bars = bars;
    plan.swapBar = swap;
    plan.outBeat = o.firstDownbeatBeat + 4.0 * start;
    plan.inBeat = i.firstDownbeatBeat; // from its first downbeat
    plan.reasons.push_back(text("%.0f bars from the old track's bar %.0f, on a phrase line", bars, start + 1));

    // The preset, its bass swap moved onto the swap bar.
    const float beats = 4.0f * static_cast<float>(bars);
    plan.curves = makePreset(plan.preset, beats);
    for (const bool incoming : {false, true})
    {
        auto& low = plan.curves.lane(incoming, engine::Lane::EqLow).points;
        const bool swaps = low.size() == 2 && low[0].shape == engine::Shape::Hold &&
                           ((low[0].value == 0.0f) != (low[1].value == 0.0f));
        if (swaps)
            low[1].beat = 4.0f * swap;
    }
    engine::normalize(plan.curves);
    plan.ok = true;
    return plan;
}

} // namespace devildj::app
