#include "app/TransitionStore.h"

#include "app/TransitionPresets.h"
#include "library/Connection.h"

namespace devildj::app
{

bool saveTransition(library::Connection& db, const SavedTransition& t)
{
    if (t.fromTrack == 0 || t.toTrack == 0)
        return false;
    auto* st = db.prepare("INSERT INTO transitions (from_track, to_track, preset, beats, out_beat, in_beat, curves_json, updated_at) "
                          "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                          "ON CONFLICT (from_track, to_track) DO UPDATE SET preset = excluded.preset, beats = excluded.beats, "
                          "out_beat = excluded.out_beat, in_beat = excluded.in_beat, curves_json = excluded.curves_json, "
                          "updated_at = excluded.updated_at");
    if (st == nullptr)
        return false;
    st->bind(1, t.fromTrack)
        .bind(2, t.toTrack)
        .bind(3, t.preset.toStdString())
        .bind(4, static_cast<double>(t.curves.beats))
        .bind(5, t.outBeat)
        .bind(6, t.inBeat)
        .bind(7, curvesToJson(t.curves, t.preset).toStdString())
        .bind(8, static_cast<std::int64_t>(juce::Time::currentTimeMillis() / 1000));
    const bool ok = st->run();
    st->reset();
    return ok;
}

bool deleteTransition(library::Connection& db, std::int64_t fromTrack, std::int64_t toTrack)
{
    auto* st = db.prepare("DELETE FROM transitions WHERE from_track = ? AND to_track = ?");
    if (st == nullptr)
        return false;
    st->bind(1, fromTrack).bind(2, toTrack);
    const bool ok = st->run();
    st->reset();
    return ok;
}

std::optional<SavedTransition> loadTransition(library::Connection& db, std::int64_t fromTrack, std::int64_t toTrack)
{
    auto* st = db.prepare("SELECT preset, beats, out_beat, in_beat, curves_json FROM transitions WHERE from_track = ? AND to_track = ?");
    if (st == nullptr)
        return std::nullopt;
    st->bind(1, fromTrack).bind(2, toTrack);
    std::optional<SavedTransition> out;
    if (st->step())
    {
        SavedTransition t;
        t.fromTrack = fromTrack;
        t.toTrack = toTrack;
        t.preset = juce::String::fromUTF8(st->getText(0).c_str());
        const auto beats = static_cast<float>(st->getDouble(1));
        t.outBeat = st->getDouble(2);
        t.inBeat = st->getDouble(3);
        engine::TransitionCurves shape;
        juce::String name;
        if (curvesFromJson(juce::String::fromUTF8(st->getText(4).c_str()), shape, name))
        {
            t.curves = scaled(shape, beats);
            out = std::move(t);
        }
    }
    st->reset();
    return out;
}

void TransitionLabels::load(library::Connection& db)
{
    labels.clear();
    auto* st = db.prepare("SELECT from_track, to_track, preset FROM transitions");
    if (st == nullptr)
        return;
    while (st->step())
        labels[{st->getInt(0), st->getInt(1)}] = juce::String::fromUTF8(st->getText(2).c_str());
    st->reset();
}

void TransitionLabels::set(std::int64_t fromTrack, std::int64_t toTrack, const juce::String& preset)
{
    labels[{fromTrack, toTrack}] = preset.isEmpty() ? juce::String("Custom") : preset;
}

void TransitionLabels::erase(std::int64_t fromTrack, std::int64_t toTrack)
{
    labels.erase({fromTrack, toTrack});
}

juce::String TransitionLabels::get(std::int64_t fromTrack, std::int64_t toTrack) const
{
    const auto it = labels.find({fromTrack, toTrack});
    return it != labels.end() ? it->second : juce::String();
}

} // namespace devildj::app
