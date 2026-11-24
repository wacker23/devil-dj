#include "app/CodeSoundBank.h"

#include "beatmaker/DrumKits.h"

#include <algorithm>
#include <cctype>

namespace devildj::app
{

namespace
{
struct DrumName
{
    const char* name;
    dsp::DrumKind kind;
};
constexpr DrumName kDrums[] = {
    {"kick", dsp::DrumKind::Kick},       {"bd", dsp::DrumKind::Kick},          {"snare", dsp::DrumKind::Snare},
    {"sd", dsp::DrumKind::Snare},        {"clap", dsp::DrumKind::Clap},        {"cp", dsp::DrumKind::Clap},
    {"hat", dsp::DrumKind::ClosedHat},   {"hh", dsp::DrumKind::ClosedHat},     {"ohat", dsp::DrumKind::OpenHat},
    {"oh", dsp::DrumKind::OpenHat},      {"tom", dsp::DrumKind::TomMid},       {"lt", dsp::DrumKind::TomLow},
    {"mt", dsp::DrumKind::TomMid},       {"ht", dsp::DrumKind::TomHigh},       {"rim", dsp::DrumKind::Rim},
    {"rs", dsp::DrumKind::Rim},          {"cow", dsp::DrumKind::Cowbell},      {"cb", dsp::DrumKind::Cowbell},
    {"shaker", dsp::DrumKind::Shaker},   {"sh", dsp::DrumKind::Shaker},
};
constexpr const char* kSynths[] = {"sine", "saw", "square", "tri", "supersaw", "subbass", "fm"};

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int wrap(int n, int count)
{
    return count <= 0 ? 0 : ((n % count) + count) % count;
}

dsp::SynthParams synthFor(const std::string& name)
{
    dsp::SynthParams p;
    p.engine = dsp::SynthEngine::Subtractive;
    p.oscMix = 0.0f;   // one oscillator
    p.cutoff = 12000.0f;
    p.envAmount = 0.0f; // the plain wave: the code's lpf / hpf shape it
    p.resonance = 0.1f;
    if (name == "sine")
        p.osc1Wave = 3.0f;
    else if (name == "saw")
        p.osc1Wave = 0.0f;
    else if (name == "square")
        p.osc1Wave = 1.0f;
    else if (name == "tri")
        p.osc1Wave = 2.0f;
    else if (name == "supersaw")
    {
        p.osc1Wave = 0.0f;
        p.unison = 7.0f;
        p.unisonDetune = 25.0f;
    }
    else if (name == "subbass")
        p.engine = dsp::SynthEngine::SubBass;
    else if (name == "fm")
        p.engine = dsp::SynthEngine::Fm;
    return p;
}

// Edit distance, for "did you mean".
int distance(const std::string& a, const std::string& b)
{
    std::vector<int> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j)
        row[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i)
    {
        int diag = row[0];
        row[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j)
        {
            const int up = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + (a[i - 1] == b[j - 1] ? 0 : 1)});
            diag = up;
        }
    }
    return row[b.size()];
}

bool isSoundFile(const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}
} // namespace

CodeSoundBank::CodeSoundBank(juce::File samplesFolder)
    : folder(std::move(samplesFolder))
{
    std::lock_guard<std::mutex> lock(mutex);
    scanLocked();
}

juce::File CodeSoundBank::defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ").getChildFile("Samples");
}

bool CodeSoundBank::isBuiltIn(const std::string& sound)
{
    const auto s = lower(sound);
    return std::any_of(std::begin(kDrums), std::end(kDrums), [&](const DrumName& d) { return s == d.name; }) ||
           std::any_of(std::begin(kSynths), std::end(kSynths), [&](const char* n) { return s == n; });
}

void CodeSoundBank::rescan()
{
    std::lock_guard<std::mutex> lock(mutex);
    scanLocked();
}

void CodeSoundBank::scanLocked()
{
    lastScanMs = juce::Time::getMillisecondCounterHiRes();
    folders.clear();
    if (!folder.isDirectory())
        return;
    for (const auto& dir : folder.findChildFiles(juce::File::findDirectories, false))
    {
        std::vector<juce::File> files;
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false))
            if (isSoundFile(f))
                files.push_back(f);
        if (files.empty())
            continue;
        std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
                  { return a.getFileName().compareNatural(b.getFileName()) < 0; });
        folders[lower(dir.getFileName().toStdString())] = std::move(files);
    }
}

CodeSound CodeSoundBank::resolve(const std::string& soundName, int n)
{
    const auto s = lower(soundName);
    CodeSound out;
    std::lock_guard<std::mutex> lock(mutex);
    auto found = folders.find(s);
    if (found == folders.end() && !isBuiltIn(s) && juce::Time::getMillisecondCounterHiRes() - lastScanMs > 2000.0)
    {
        scanLocked(); // perhaps copied in just now
        found = folders.find(s);
    }
    // A sample folder (it wins over a built-in name).
    if (found != folders.end())
    {
        const auto& files = found->second;
        const auto& file = files[static_cast<std::size_t>(wrap(n, static_cast<int>(files.size())))];
        out.kind = CodeSound::Kind::Sample;
        out.voice = engine::VoiceKind::Sample;
        out.sample = file;
        out.name = s + ":" + std::to_string(n) + " (" + file.getFileName().toStdString() + ")";
        return out;
    }
    for (const auto& d : kDrums)
        if (s == d.name)
        {
            const auto presets = beatmaker::drumPresets(d.kind);
            out.kind = CodeSound::Kind::Drum;
            out.voice = engine::VoiceKind::DrumSynth;
            out.drum.kind = d.kind;
            std::string presetName;
            if (!presets.empty())
            {
                const auto& preset = presets[static_cast<std::size_t>(wrap(n, static_cast<int>(presets.size())))];
                out.drum.params = preset.params;
                presetName = preset.name;
            }
            out.name = s + ":" + std::to_string(n) + " (" + dsp::drumKindName(d.kind) +
                       (presetName.empty() ? "" : ", " + presetName) + ")";
            return out;
        }
    for (const char* name : kSynths)
        if (s == name)
        {
            out.kind = CodeSound::Kind::Synth;
            out.voice = engine::VoiceKind::Synth;
            out.synth = synthFor(s);
            out.name = s + " (synth)";
            return out;
        }

    // Unknown: say so, with the nearest name when there is a close one.
    out.kind = CodeSound::Kind::Unknown;
    out.name = s;
    std::string nearest;
    int best = 3;
    auto consider = [&](const std::string& candidate)
    {
        const int d = distance(s, candidate);
        if (d < best)
        {
            best = d;
            nearest = candidate;
        }
    };
    for (const auto& d : kDrums)
        consider(d.name);
    for (const char* name : kSynths)
        consider(name);
    for (const auto& [name, files] : folders)
        consider(name);
    out.warning = s.empty() ? std::string("no sound given: add s(\"...\"), e.g. s(\"bd\")")
                            : "unknown sound \"" + s + "\"" + (nearest.empty() ? "" : " - did you mean \"" + nearest + "\"?");
    return out;
}

std::vector<std::string> CodeSoundBank::names()
{
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> out;
    for (const auto& d : kDrums)
        out.emplace_back(d.name);
    for (const char* name : kSynths)
        out.emplace_back(name);
    for (const auto& [name, files] : folders)
        if (std::find(out.begin(), out.end(), name) == out.end())
            out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace devildj::app
