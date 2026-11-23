#pragma once

// Saved transitions (P4-19): one per track pair (the outgoing track, the incoming track) in the library's
// `transitions` table - the curves (as a 0..1 shape and its length), where it starts on the outgoing track and where
// the incoming one comes in, the preset it came from. The editor saves what the owner edits; Automix and the T key
// play the saved one for a pair, else the Auto plan.

#include "engine/TransitionCurves.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <map>
#include <optional>
#include <utility>

namespace devildj::library
{
class Connection;
}

namespace devildj::app
{

struct SavedTransition
{
    std::int64_t fromTrack = 0, toTrack = 0;
    juce::String preset; // what it was made from ("Custom" once edited)
    engine::TransitionCurves curves; // at its real length
    double outBeat = 0.0, inBeat = 0.0;
};

// On a writer connection (Database::write).
bool saveTransition(library::Connection& db, const SavedTransition& t);
bool deleteTransition(library::Connection& db, std::int64_t fromTrack, std::int64_t toTrack);
// On a reader connection.
std::optional<SavedTransition> loadTransition(library::Connection& db, std::int64_t fromTrack, std::int64_t toTrack);

// The playlists' "Mix" column: which pairs have a saved transition, by preset name (message thread; read once,
// kept up to date by the app as it saves).
class TransitionLabels
{
public:
    void load(library::Connection& db);
    void set(std::int64_t fromTrack, std::int64_t toTrack, const juce::String& preset);
    void erase(std::int64_t fromTrack, std::int64_t toTrack);
    juce::String get(std::int64_t fromTrack, std::int64_t toTrack) const; // empty: none
    int size() const noexcept { return static_cast<int>(labels.size()); }

private:
    std::map<std::pair<std::int64_t, std::int64_t>, juce::String> labels;
};

} // namespace devildj::app
