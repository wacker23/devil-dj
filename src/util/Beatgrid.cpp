#include "Beatgrid.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace devildj::util
{

Beatgrid Beatgrid::constant(double firstBeatSample, double bpm, double sampleRate, int barBeat)
{
    Beatgrid g;
    g.rate = sampleRate;
    g.addAnchor({0.0, firstBeatSample, bpm, std::clamp(barBeat, 1, kBeatsPerBar)});
    return g;
}

bool Beatgrid::addAnchor(const GridAnchor& a)
{
    if (count >= kMaxAnchors || a.bpm <= 0.0)
        return false;
    if (count > 0 && (a.beat <= anchors[static_cast<std::size_t>(count - 1)].beat ||
                      a.sample <= anchors[static_cast<std::size_t>(count - 1)].sample))
        return false;
    anchors[static_cast<std::size_t>(count++)] = a;
    return true;
}

int Beatgrid::segmentFor(double position) const noexcept
{
    int i = 0;
    while (i + 1 < count && anchors[static_cast<std::size_t>(i + 1)].sample <= position)
        ++i;
    return i;
}

int Beatgrid::segmentForBeat(double beat) const noexcept
{
    int i = 0;
    while (i + 1 < count && anchors[static_cast<std::size_t>(i + 1)].beat <= beat)
        ++i;
    return i;
}

double Beatgrid::beatAt(double position) const noexcept
{
    if (empty())
        return 0.0;
    const auto& a = anchors[static_cast<std::size_t>(segmentFor(position))];
    return a.beat + (position - a.sample) * a.bpm / (60.0 * rate);
}

double Beatgrid::sampleAt(double beat) const noexcept
{
    if (empty())
        return 0.0;
    const auto& a = anchors[static_cast<std::size_t>(segmentForBeat(beat))];
    return a.sample + (beat - a.beat) * 60.0 * rate / a.bpm;
}

double Beatgrid::bpmAt(double position) const noexcept
{
    return empty() ? 0.0 : anchors[static_cast<std::size_t>(segmentFor(position))].bpm;
}

double Beatgrid::secondsPerBeatAt(double position) const noexcept
{
    const double bpm = bpmAt(position);
    return bpm > 0.0 ? 60.0 / bpm : 0.0;
}

int Beatgrid::barBeatOf(long long beat) const noexcept
{
    if (empty())
        return 1;
    const auto& a = anchors[0];
    const long long offset = beat - std::llround(a.beat) + (a.barBeat - 1);
    return static_cast<int>(((offset % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar) + 1;
}

long long Beatgrid::nearestBeat(double position) const noexcept
{
    return std::llround(beatAt(position));
}

void Beatgrid::shiftSamples(double samples)
{
    for (int i = 0; i < count; ++i)
        anchors[static_cast<std::size_t>(i)].sample += samples;
}

void Beatgrid::shiftBeats(int beats)
{
    // Beat b becomes what beat b + n was: the bar phase turns by n.
    if (empty())
        return;
    auto& a = anchors[0];
    a.barBeat = ((a.barBeat - 1 - beats) % kBeatsPerBar + kBeatsPerBar) % kBeatsPerBar + 1;
}

void Beatgrid::setDownbeatAt(double position)
{
    if (empty())
        return;
    const long long b = nearestBeat(position);
    shiftSamples(position - sampleAt(static_cast<double>(b)));
    // Beat b is a downbeat: the first anchor's place in its bar follows.
    auto& a = anchors[0];
    const long long offset = std::llround(a.beat) - b;
    a.barBeat = static_cast<int>(((offset % kBeatsPerBar) + kBeatsPerBar) % kBeatsPerBar) + 1;
}

void Beatgrid::scaleTempo(double factor)
{
    if (empty() || factor <= 0.0)
        return;
    const double base = anchors[0].beat;
    for (int i = 0; i < count; ++i)
    {
        auto& a = anchors[static_cast<std::size_t>(i)];
        a.bpm *= factor;
        a.beat = base + (a.beat - base) * factor; // the same positions, more / fewer beats between them
    }
}

Beatgrid Beatgrid::rescaled(double newRate) const
{
    Beatgrid g = *this;
    if (rate > 0.0 && newRate > 0.0)
        for (int i = 0; i < count; ++i)
            g.anchors[static_cast<std::size_t>(i)].sample *= newRate / rate;
    g.rate = newRate;
    return g;
}

std::string Beatgrid::toJson() const
{
    std::string out = "[";
    for (int i = 0; i < count; ++i)
    {
        const auto& a = anchors[static_cast<std::size_t>(i)];
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s{\"beat\":%lld,\"sample\":%lld,\"bpm\":%.4f,\"bar_beat\":%d}", i == 0 ? "" : ",",
                      std::llround(a.beat), std::llround(a.sample), a.bpm, a.barBeat);
        out += buf;
    }
    return out + "]";
}

std::optional<Beatgrid> Beatgrid::fromJson(std::string_view json, double sampleRate)
{
    // The anchors_json array: objects with numeric fields only. Anything else is refused.
    Beatgrid g;
    g.rate = sampleRate;
    std::size_t pos = 0;
    const auto skip = [&]
    {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t'))
            ++pos;
    };
    const auto eat = [&](char c)
    {
        skip();
        if (pos < json.size() && json[pos] == c)
        {
            ++pos;
            return true;
        }
        return false;
    };
    if (sampleRate <= 0.0 || !eat('['))
        return std::nullopt;
    if (eat(']'))
        return g;
    do
    {
        if (!eat('{'))
            return std::nullopt;
        GridAnchor a;
        bool hasSample = false, hasBpm = false;
        do
        {
            skip();
            if (!eat('"'))
                return std::nullopt;
            const auto end = json.find('"', pos);
            if (end == std::string_view::npos)
                return std::nullopt;
            const auto key = json.substr(pos, end - pos);
            pos = end + 1;
            if (!eat(':'))
                return std::nullopt;
            skip();
            double v = 0.0;
            const auto [p, ec] = std::from_chars(json.data() + pos, json.data() + json.size(), v);
            if (ec != std::errc{})
                return std::nullopt;
            pos = static_cast<std::size_t>(p - json.data());
            if (key == "beat")
                a.beat = v;
            else if (key == "sample")
            {
                a.sample = v;
                hasSample = true;
            }
            else if (key == "bpm")
            {
                a.bpm = v;
                hasBpm = true;
            }
            else if (key == "bar_beat")
                a.barBeat = std::clamp(static_cast<int>(v), 1, kBeatsPerBar);
        } while (eat(','));
        if (!eat('}') || !hasSample || !hasBpm || !g.addAnchor(a))
            return std::nullopt;
    } while (eat(','));
    if (!eat(']'))
        return std::nullopt;
    return g;
}

void TapTempo::tap(double seconds)
{
    if (!taps.empty() && (seconds - taps.back() > 2.0 || seconds <= taps.back()))
        taps.clear();
    taps.push_back(seconds);
    if (taps.size() > 16)
        taps.erase(taps.begin());
}

std::optional<double> TapTempo::bpm() const
{
    if (taps.size() < 4)
        return std::nullopt;
    // Least squares: tap i at t0 + i * period.
    const double n = static_cast<double>(taps.size());
    double mi = 0.0, mt = 0.0;
    for (std::size_t i = 0; i < taps.size(); ++i)
    {
        mi += static_cast<double>(i);
        mt += taps[i];
    }
    mi /= n;
    mt /= n;
    double sii = 0.0, sit = 0.0;
    for (std::size_t i = 0; i < taps.size(); ++i)
    {
        sii += (static_cast<double>(i) - mi) * (static_cast<double>(i) - mi);
        sit += (static_cast<double>(i) - mi) * (taps[i] - mt);
    }
    const double period = sii > 0.0 ? sit / sii : 0.0;
    if (period <= 0.0)
        return std::nullopt;
    return 60.0 / period;
}

} // namespace devildj::util
