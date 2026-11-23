#include "engine/TransitionCurves.h"

#include <algorithm>

namespace devildj::engine
{

namespace
{
float shaped(Shape shape, float t) noexcept
{
    switch (shape)
    {
        case Shape::EaseIn: return t * t;
        case Shape::EaseOut: return 1.0f - (1.0f - t) * (1.0f - t);
        case Shape::SCurve: return t * t * (3.0f - 2.0f * t);
        case Shape::Hold: return 0.0f;
        case Shape::Linear: break;
    }
    return t;
}

void range(Lane lane, float& lo, float& hi) noexcept
{
    switch (lane)
    {
        case Lane::Volume:
        case Lane::FxSend: lo = 0.0f; hi = 1.0f; return;
        case Lane::EqLow:
        case Lane::EqMid:
        case Lane::EqHigh: lo = -1000.0f; hi = 6.0f; return; // below -100 = kill
        case Lane::Filter: lo = -1.0f; hi = 1.0f; return;
    }
}
} // namespace

float evaluate(const LaneCurve& lane, double beat) noexcept
{
    const auto& p = lane.points;
    if (p.empty())
        return 0.0f;
    if (beat <= p.front().beat)
        return p.front().value;
    if (beat >= p.back().beat)
        return p.back().value;
    // The segment holding `beat` (lanes have a handful of points: a linear scan).
    std::size_t i = 0;
    while (i + 1 < p.size() && p[i + 1].beat <= beat)
        ++i;
    const auto& a = p[i];
    const auto& b = p[i + 1];
    const float span = b.beat - a.beat;
    if (span <= 0.0f)
        return b.value;
    const float t = static_cast<float>((beat - a.beat) / span);
    return a.value + (b.value - a.value) * shaped(a.shape, std::clamp(t, 0.0f, 1.0f));
}

float neutralValue(Lane lane) noexcept
{
    return lane == Lane::Volume ? 1.0f : 0.0f;
}

void normalize(TransitionCurves& c)
{
    c.beats = std::clamp(c.beats, 1.0f, 256.0f);
    c.loopBeats = std::clamp(c.loopBeats, 0.0f, 64.0f);
    c.fxBeats = std::clamp(c.fxBeats, 1.0f / 16.0f, 16.0f);
    for (auto& f : c.fx)
        f = static_cast<std::int8_t>(std::clamp(static_cast<int>(f), -1, 13));
    for (auto* side : {&c.from, &c.to})
        for (int l = 0; l < kLaneCount; ++l)
        {
            auto& pts = (*side)[static_cast<std::size_t>(l)].points;
            float lo = 0.0f, hi = 1.0f;
            range(static_cast<Lane>(l), lo, hi);
            for (auto& pt : pts)
            {
                pt.beat = std::clamp(pt.beat, 0.0f, c.beats);
                pt.value = std::clamp(pt.value, lo, hi);
            }
            std::stable_sort(pts.begin(), pts.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.beat < b.beat; });
        }
}

} // namespace devildj::engine
