#include "app/DrumSwapBank.h"

#include "app/LoopExport.h"
#include "dsp/SincResampler.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace devildj::app
{

namespace
{
const DrumSwapSlot kEmpty{};

juce::File slotFile(const juce::File& deckFolder, int index)
{
    return deckFolder.getChildFile("slot-" + juce::String(index + 1) + ".wav");
}

std::shared_ptr<engine::TrackBuffer> readWav(const juce::File& file, double sampleRate)
{
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(
        file.existsAsFile() ? wav.createReaderFor(file.createInputStream().release(), true) : nullptr);
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;
    const auto n = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> audio(2, n);
    reader->read(&audio, 0, n, 0, true, true);
    if (reader->numChannels < 2)
        audio.copyFrom(1, 0, audio, 0, 0, n);
    auto t = std::make_shared<engine::TrackBuffer>();
    t->file = file;
    t->sampleRate = sampleRate;
    if (std::abs(reader->sampleRate - sampleRate) <= 0.5)
    {
        t->audio = std::move(audio);
        return t;
    }
    std::array<std::vector<float>, 2> out;
    for (int c = 0; c < 2; ++c)
        out[static_cast<std::size_t>(c)] =
            dsp::resample(std::span<const float>(audio.getReadPointer(c), static_cast<std::size_t>(n)), reader->sampleRate,
                          sampleRate);
    const auto m = static_cast<int>(out[0].size());
    t->audio.setSize(2, m);
    for (int c = 0; c < 2; ++c)
        t->audio.copyFrom(c, 0, out[static_cast<std::size_t>(c)].data(), m);
    return t;
}
} // namespace

DrumSwapBank::DrumSwapBank(juce::File f)
    : folder(std::move(f))
{
}

juce::File DrumSwapBank::defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ").getChildFile("DrumSwap");
}

juce::File DrumSwapBank::deckFolder(int deck) const
{
    return folder.getChildFile(juce::String("Deck ") + static_cast<char>('A' + clampDeck(deck)));
}

int DrumSwapBank::load(double sampleRate)
{
    int loaded = 0;
    const auto top = juce::JSON::parse(folder.getChildFile("settings.json"));
    if (const int c = top["captureBeats"]; c > 0)
        capture = c;
    for (int d = 0; d < kDecks; ++d)
    {
        auto& deck = decks[static_cast<std::size_t>(d)];
        deck = {};
        const auto json = juce::JSON::parse(deckFolder(d).getChildFile("bank.json"));
        deck.multi = static_cast<bool>(json["multi"]);
        const auto* slots = json["slots"].getArray();
        if (slots == nullptr)
            continue;
        for (const auto& v : *slots)
        {
            const int index = v["slot"];
            const double beats = v["beats"];
            if (index < 0 || index >= kSlots || beats <= 0.0)
                continue;
            auto audio = readWav(slotFile(deckFolder(d), index), sampleRate);
            if (audio == nullptr)
                continue;
            auto& s = deck.slots[static_cast<std::size_t>(index)];
            s.name = v["name"].toString();
            s.beats = beats;
            s.audio = std::move(audio);
            ++loaded;
        }
    }
    return loaded;
}

const DrumSwapSlot& DrumSwapBank::slot(int deck, int index) const
{
    if (index < 0 || index >= kSlots)
        return kEmpty;
    return decks[static_cast<std::size_t>(clampDeck(deck))].slots[static_cast<std::size_t>(index)];
}

juce::String DrumSwapBank::saveJson(int deck) const
{
    const auto& d = decks[static_cast<std::size_t>(clampDeck(deck))];
    juce::Array<juce::var> slots;
    for (int i = 0; i < kSlots; ++i)
    {
        const auto& s = d.slots[static_cast<std::size_t>(i)];
        if (s.empty())
            continue;
        auto* o = new juce::DynamicObject();
        o->setProperty("slot", i);
        o->setProperty("name", s.name);
        o->setProperty("beats", s.beats);
        slots.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("multi", d.multi);
    root->setProperty("slots", slots);
    const auto dir = deckFolder(deck);
    if (!dir.createDirectory())
        return "can't create " + dir.getFullPathName();
    if (!dir.getChildFile("bank.json").replaceWithText(juce::JSON::toString(juce::var(root))))
        return "can't write " + dir.getChildFile("bank.json").getFullPathName();
    return {};
}

juce::String DrumSwapBank::set(int deck, int index, DrumSwapSlot s)
{
    if (index < 0 || index >= kSlots || s.empty() || s.beats <= 0.0)
        return "nothing to put in the slot";
    const auto dir = deckFolder(deck);
    if (!dir.createDirectory())
        return "can't create " + dir.getFullPathName();
    if (auto error = writeLoopWav(*s.audio, 0, s.audio->numSamples(), slotFile(dir, index)); error.isNotEmpty())
        return error;
    decks[static_cast<std::size_t>(clampDeck(deck))].slots[static_cast<std::size_t>(index)] = std::move(s);
    return saveJson(deck);
}

juce::String DrumSwapBank::clear(int deck, int index)
{
    if (index < 0 || index >= kSlots)
        return {};
    decks[static_cast<std::size_t>(clampDeck(deck))].slots[static_cast<std::size_t>(index)] = {};
    slotFile(deckFolder(deck), index).deleteFile();
    return saveJson(deck);
}

void DrumSwapBank::setMulti(int deck, bool on)
{
    decks[static_cast<std::size_t>(clampDeck(deck))].multi = on;
    saveJson(deck);
}

void DrumSwapBank::setCaptureBeats(int beats)
{
    capture = beats;
    folder.createDirectory();
    auto* o = new juce::DynamicObject();
    o->setProperty("captureBeats", capture);
    folder.getChildFile("settings.json").replaceWithText(juce::JSON::toString(juce::var(o)));
}

util::Result<std::shared_ptr<engine::TrackBuffer>> captureDrums(const engine::StemBuffer& stems,
                                                                const util::Beatgrid& grid, double position, int beats)
{
    using R = util::Result<std::shared_ptr<engine::TrackBuffer>>;
    if (grid.empty())
        return R::fail("the track has no beatgrid");
    if (beats <= 0)
        return R::fail("no length");
    auto b = static_cast<long long>(std::floor(grid.beatAt(position) + 1e-6));
    for (int k = 0; k < util::Beatgrid::kBeatsPerBar && grid.barBeatOf(b) != 1; ++k)
        --b;
    const auto start = static_cast<int>(std::llround(grid.sampleAt(static_cast<double>(b))));
    const auto end = static_cast<int>(std::llround(grid.sampleAt(static_cast<double>(b + beats))));
    if (start < 0 || end > stems.length || end <= start)
        return R::fail("not enough of the track left for " + std::to_string(beats) + " beats");
    auto t = std::make_shared<engine::TrackBuffer>();
    t->sampleRate = stems.sampleRate;
    t->audio.setSize(2, end - start);
    for (int c = 0; c < 2; ++c)
    {
        const auto* src = stems.channel(0, c) + start;
        auto* dst = t->audio.getWritePointer(c);
        for (int i = 0; i < end - start; ++i)
            dst[i] = static_cast<float>(src[i]) * (1.0f / 32768.0f);
    }
    return R::ok(std::move(t));
}

} // namespace devildj::app
