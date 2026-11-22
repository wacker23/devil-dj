#pragma once

#include "engine/EngineCommand.h"
#include "engine/Sequencer.h"

#include <juce_core/juce_core.h>

#include <array>
#include <vector>

namespace devildj::app
{

// The sampler's sequences (P3-15): up to 8 per sampler bank (4 banks), each a length in bars and its events. Kept
// in the settings as JSON; one is in the engine at a time (sent with loadCommands), and what the engine recorded
// is taken back from its state (fromEngine) when recording stops.
struct StoredSequence
{
    int bars = 1;
    std::vector<engine::SequenceEvent> events;
    bool empty() const noexcept { return events.empty(); }
};

constexpr int kSequenceBanks = 4;
constexpr int kSequencesPerBank = 8;
using SequenceBank = std::array<StoredSequence, kSequencesPerBank>;
using SequenceStore = std::array<SequenceBank, kSequenceBanks>;

juce::String toJson(const SequenceStore& store);
SequenceStore sequencesFromJson(const juce::String& json); // bad / missing parts: empty sequences

// The engine commands that put a sequence in the engine (Clear with its bars, then the events in chunks).
std::vector<engine::EngineCommand> loadCommands(const StoredSequence& sequence);
// What the engine has now (from its state).
StoredSequence fromEngine(const engine::Sequence& sequence);

} // namespace devildj::app
