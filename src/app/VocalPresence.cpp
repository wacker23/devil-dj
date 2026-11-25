#include "app/VocalPresence.h"

#include "library/Connection.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>

namespace devildj::app
{

namespace
{
constexpr float kFloorDb = -40.0f;
constexpr float kBelowLoudDb = 18.0f;
} // namespace

std::optional<VocalCurve> vocalCurveFromFile(const juce::File& vocalsFlac)
{
    if (!vocalsFlac.existsAsFile())
        return std::nullopt;
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader(flac.createReaderFor(vocalsFlac.createInputStream().release(), true));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return std::nullopt;
    const int window = std::max(1, static_cast<int>(std::lround(VocalCurve::kWindowSeconds * reader->sampleRate)));
    const auto channels = static_cast<int>(std::min<unsigned>(2, reader->numChannels));
    juce::AudioBuffer<float> block(std::max(1, channels), window);
    VocalCurve curve;
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += window)
    {
        const int n = static_cast<int>(std::min<juce::int64>(window, reader->lengthInSamples - at));
        if (!reader->read(&block, 0, n, at, true, channels > 1))
            return std::nullopt;
        double sum = 0.0;
        for (int c = 0; c < std::max(1, channels); ++c)
        {
            const float* d = block.getReadPointer(c);
            for (int i = 0; i < n; ++i)
                sum += static_cast<double>(d[i]) * d[i];
        }
        const double ms = sum / (static_cast<double>(n) * std::max(1, channels));
        curve.db.push_back(static_cast<float>(10.0 * std::log10(ms + 1e-12)));
    }
    return curve;
}

bool saveVocalCurve(const VocalCurve& curve, const juce::File& json)
{
    juce::Array<juce::var> values;
    for (const float v : curve.db)
        values.add(std::round(std::max(v, -120.0f) * 10.0f) / 10.0f); // 0.1 dB is plenty
    auto* o = new juce::DynamicObject();
    o->setProperty("windowSeconds", VocalCurve::kWindowSeconds);
    o->setProperty("db", values);
    return json.replaceWithText(juce::JSON::toString(juce::var(o), true));
}

std::optional<VocalCurve> loadVocalCurve(const juce::File& json)
{
    if (!json.existsAsFile())
        return std::nullopt;
    const auto v = juce::JSON::parse(json);
    const auto* values = v["db"].getArray();
    if (values == nullptr || std::abs(static_cast<double>(v["windowSeconds"]) - VocalCurve::kWindowSeconds) > 1e-9)
        return std::nullopt;
    VocalCurve curve;
    curve.db.reserve(static_cast<std::size_t>(values->size()));
    for (const auto& x : *values)
        curve.db.push_back(static_cast<float>(static_cast<double>(x)));
    return curve;
}

std::optional<VocalCurve> ensureVocalCurve(const juce::File& stemsFolder)
{
    const auto json = stemsFolder.getChildFile("vocal.json");
    if (auto c = loadVocalCurve(json))
        return c;
    auto c = vocalCurveFromFile(stemsFolder.getChildFile("vocals.flac"));
    if (c)
        saveVocalCurve(*c, json); // a read-only cache just means it's made again next time
    return c;
}

std::vector<bool> activeWindows(const VocalCurve& curve)
{
    std::vector<bool> out(curve.db.size(), false);
    if (curve.db.empty())
        return out;
    auto sorted = curve.db;
    const auto k = static_cast<std::size_t>(0.95 * static_cast<double>(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
    const float loud = sorted[k];
    const float threshold = std::max(kFloorDb, loud - kBelowLoudDb);
    for (std::size_t i = 0; i < curve.db.size(); ++i)
        out[i] = curve.db[i] > threshold;
    return out;
}

std::vector<double> vocalPerPhrase(const VocalCurve& curve, const std::vector<library::StoredPhrase>& phrases)
{
    std::vector<double> out;
    const auto active = activeWindows(curve);
    const auto windowAt = [](double seconds) { return static_cast<long long>(std::floor(seconds / VocalCurve::kWindowSeconds)); };
    for (std::size_t i = 0; i < phrases.size(); ++i)
    {
        const auto a = std::max(0LL, windowAt(phrases[i].startSeconds));
        const auto b = i + 1 < phrases.size() ? windowAt(phrases[i + 1].startSeconds) : static_cast<long long>(active.size());
        int on = 0, total = 0;
        for (auto w = a; w < std::min<long long>(b, static_cast<long long>(active.size())); ++w)
        {
            ++total;
            on += active[static_cast<std::size_t>(w)] ? 1 : 0;
        }
        out.push_back(total > 0 ? static_cast<double>(on) / total : 0.0);
    }
    return out;
}

std::optional<VocalEnds> vocalEnds(const std::vector<double>& perPhrase)
{
    if (perPhrase.empty())
        return std::nullopt;
    return VocalEnds{perPhrase.front(), perPhrase.back()};
}

bool storeVocalEnds(library::Connection& db, std::int64_t trackId, const VocalEnds& ends)
{
    auto* s = db.prepare("INSERT INTO track_features (track_id, version, vocal_intro, vocal_outro) VALUES (?, 0, ?, ?) "
                         "ON CONFLICT(track_id) DO UPDATE SET vocal_intro = excluded.vocal_intro, "
                         "vocal_outro = excluded.vocal_outro");
    return s != nullptr && s->bind(1, trackId).bind(2, ends.intro).bind(3, ends.outro).run();
}

} // namespace devildj::app
