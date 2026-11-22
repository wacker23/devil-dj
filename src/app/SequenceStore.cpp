#include "SequenceStore.h"

#include <algorithm>

namespace devildj::app
{

juce::String toJson(const SequenceStore& store)
{
    juce::Array<juce::var> banks;
    for (const auto& bank : store)
    {
        juce::Array<juce::var> sequences;
        for (const auto& s : bank)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("bars", s.bars);
            juce::Array<juce::var> events;
            for (const auto& e : s.events)
            {
                juce::Array<juce::var> triple; // [beat, slot, kind]
                triple.add(static_cast<double>(e.beat));
                triple.add(static_cast<int>(e.slot));
                triple.add(static_cast<int>(e.kind));
                events.add(triple);
            }
            o->setProperty("events", events);
            sequences.add(juce::var(o));
        }
        banks.add(sequences);
    }
    return juce::JSON::toString(juce::var(banks), true);
}

SequenceStore sequencesFromJson(const juce::String& json)
{
    SequenceStore store;
    const auto parsed = juce::JSON::parse(json);
    const auto* banks = parsed.getArray();
    if (banks == nullptr)
        return store;
    for (int b = 0; b < std::min(kSequenceBanks, banks->size()); ++b)
    {
        const auto* sequences = (*banks)[b].getArray();
        if (sequences == nullptr)
            continue;
        for (int i = 0; i < std::min(kSequencesPerBank, sequences->size()); ++i)
        {
            const auto& o = (*sequences)[i];
            if (!o.isObject())
                continue;
            StoredSequence s;
            const int bars = static_cast<int>(o.getProperty("bars", 1));
            s.bars = bars >= 4 ? 4 : bars >= 2 ? 2 : 1;
            if (const auto* events = o.getProperty("events", {}).getArray())
                for (const auto& e : *events)
                {
                    const auto* t = e.getArray();
                    if (t == nullptr || t->size() < 3)
                        continue;
                    const auto beat = static_cast<float>(static_cast<double>((*t)[0]));
                    const int slot = static_cast<int>((*t)[1]);
                    const int kind = static_cast<int>((*t)[2]);
                    if (!(beat >= 0.0f) || beat >= 4.0f * s.bars || slot < 0 || slot >= 32 || (kind != 0 && kind != 1))
                        continue;
                    if (static_cast<int>(s.events.size()) < engine::kMaxSequenceEvents)
                        s.events.push_back({beat, static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(kind)});
                }
            store[static_cast<std::size_t>(b)][static_cast<std::size_t>(i)] = std::move(s);
        }
    }
    return store;
}

std::vector<engine::EngineCommand> loadCommands(const StoredSequence& sequence)
{
    std::vector<engine::EngineCommand> out;
    out.push_back(engine::SequencerCommand{engine::SequencerOp::Clear, 0, static_cast<float>(sequence.bars)});
    for (std::size_t i = 0; i < sequence.events.size(); i += engine::SequencerEvents::kChunk)
    {
        engine::SequencerEvents chunk;
        const auto n = std::min<std::size_t>(engine::SequencerEvents::kChunk, sequence.events.size() - i);
        chunk.count = static_cast<std::uint8_t>(n);
        for (std::size_t k = 0; k < n; ++k)
            chunk.events[k] = sequence.events[i + k];
        out.push_back(chunk);
    }
    return out;
}

StoredSequence fromEngine(const engine::Sequence& sequence)
{
    StoredSequence s;
    s.bars = sequence.bars;
    for (int i = 0; i < sequence.count; ++i)
        s.events.push_back(sequence.events[static_cast<std::size_t>(i)]);
    std::sort(s.events.begin(), s.events.end(), [](const auto& a, const auto& b) { return a.beat < b.beat; });
    return s;
}

} // namespace devildj::app
