#include "beatmaker/ProjectFile.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace devildj::beatmaker
{

namespace
{
using juce::var;

juce::String str(const std::string& s)
{
    return juce::String::fromUTF8(s.c_str());
}

std::string utf8(const juce::String& s)
{
    return s.toStdString();
}

// ---- writing ----

// A new object holding the unknown fields kept from a load; the known ones are set on top.
juce::DynamicObject::Ptr objectWith(const std::string& unknown)
{
    if (!unknown.empty())
        if (const auto parsed = juce::JSON::parse(str(unknown)); parsed.getDynamicObject() != nullptr)
            return parsed.getDynamicObject()->clone().release();
    return new juce::DynamicObject();
}

var paramsToVar(const std::map<std::string, float>& params)
{
    auto* o = new juce::DynamicObject();
    for (const auto& [k, v] : params)
        o->setProperty(juce::Identifier(str(k)), static_cast<double>(v));
    return var(o);
}

void setMix(juce::DynamicObject& o, float volumeDb, float pan, float delaySend, float reverbSend, bool mute, bool solo)
{
    o.setProperty("volumeDb", static_cast<double>(volumeDb));
    o.setProperty("pan", static_cast<double>(pan));
    o.setProperty("delaySend", static_cast<double>(delaySend));
    o.setProperty("reverbSend", static_cast<double>(reverbSend));
    o.setProperty("mute", mute);
    o.setProperty("solo", solo);
}

const char* engineName(SynthPreset::Engine e)
{
    return e == SynthPreset::Engine::Fm ? "fm" : e == SynthPreset::Engine::SubBass ? "subBass" : "subtractive";
}

const char* modeName(ArpSettings::Mode m)
{
    switch (m)
    {
        case ArpSettings::Mode::Down: return "down";
        case ArpSettings::Mode::UpDown: return "upDown";
        case ArpSettings::Mode::Random: return "random";
        case ArpSettings::Mode::AsPlayed: return "asPlayed";
        case ArpSettings::Mode::Up: break;
    }
    return "up";
}

var drumToVar(const DrumTrack& d)
{
    auto o = objectWith(d.unknown);
    o->setProperty("name", str(d.name));
    auto s = objectWith(d.sound.unknown);
    s->setProperty("kind", d.sound.kind == SoundRef::Kind::Sample ? "sample" : "synth");
    s->setProperty("synth", str(d.sound.synth));
    if (d.sound.kind == SoundRef::Kind::Sample || !d.sound.samplePath.empty())
        s->setProperty("sample", str(d.sound.samplePath));
    s->setProperty("params", paramsToVar(d.sound.params));
    o->setProperty("sound", var(s.get()));
    // Steps: [on, velocity, probability, microTiming, ratchet] each.
    juce::Array<var> steps;
    for (const auto& st : d.steps)
        steps.add(juce::Array<var>{st.on ? 1 : 0, static_cast<int>(st.velocity), static_cast<int>(st.probability),
                                   static_cast<int>(st.microTiming), static_cast<int>(st.ratchet)});
    o->setProperty("steps", steps);
    setMix(*o, d.volumeDb, d.pan, d.delaySend, d.reverbSend, d.mute, d.solo);
    o->setProperty("chokeGroup", static_cast<int>(d.chokeGroup));
    return var(o.get());
}

var synthToVar(const SynthTrack& t)
{
    auto o = objectWith(t.unknown);
    o->setProperty("name", str(t.name));
    auto p = objectWith(t.preset.unknown);
    p->setProperty("engine", engineName(t.preset.engine));
    p->setProperty("name", str(t.preset.name));
    p->setProperty("params", paramsToVar(t.preset.params));
    o->setProperty("preset", var(p.get()));
    // Notes: [startTick, lengthTicks, pitch, velocity] each.
    juce::Array<var> notes;
    for (const auto& n : t.notes)
        notes.add(juce::Array<var>{n.startTick, n.lengthTicks, static_cast<int>(n.pitch), static_cast<int>(n.velocity)});
    o->setProperty("notes", notes);
    auto a = objectWith(t.arp.unknown);
    a->setProperty("on", t.arp.on);
    a->setProperty("rate", t.arp.rate);
    a->setProperty("mode", modeName(t.arp.mode));
    a->setProperty("octaves", t.arp.octaves);
    o->setProperty("arp", var(a.get()));
    setMix(*o, t.volumeDb, t.pan, t.delaySend, t.reverbSend, t.mute, t.solo);
    return var(o.get());
}

// ---- reading ----

struct Reader
{
    std::vector<std::string>* warnings = nullptr;

    void warn(const std::string& w) const
    {
        if (warnings != nullptr)
            warnings->push_back(w);
    }

    // The fields of `v` not in `known`, as a JSON object ("" if none).
    static std::string unknownOf(const var& v, std::initializer_list<const char*> known)
    {
        const auto* o = v.getDynamicObject();
        if (o == nullptr)
            return {};
        auto* rest = new juce::DynamicObject();
        const var keep(rest);
        for (const auto& p : o->getProperties())
        {
            const auto name = p.name.toString();
            if (std::none_of(known.begin(), known.end(), [&](const char* k) { return name == k; }))
                rest->setProperty(p.name, p.value);
        }
        return rest->getProperties().isEmpty() ? std::string() : utf8(juce::JSON::toString(keep, true));
    }

    static double number(const var& o, const char* name, double fallback)
    {
        const auto v = o.getProperty(name, var());
        return v.isInt() || v.isInt64() || v.isDouble() || v.isBool() ? static_cast<double>(v) : fallback;
    }
    static int integer(const var& o, const char* name, int fallback)
    {
        return static_cast<int>(std::llround(number(o, name, fallback)));
    }
    static bool flag(const var& o, const char* name, bool fallback)
    {
        const auto v = o.getProperty(name, var());
        return v.isBool() || v.isInt() || v.isInt64() || v.isDouble() ? static_cast<bool>(v) : fallback;
    }
    static std::string text(const var& o, const char* name, const std::string& fallback)
    {
        const auto v = o.getProperty(name, var());
        return v.isString() ? utf8(v.toString()) : fallback;
    }
    static std::map<std::string, float> params(const var& o)
    {
        std::map<std::string, float> out;
        if (const auto* p = o.getProperty("params", var()).getDynamicObject())
            for (const auto& kv : p->getProperties())
                if (kv.value.isDouble() || kv.value.isInt() || kv.value.isInt64())
                    out[utf8(kv.name.toString())] = static_cast<float>(static_cast<double>(kv.value));
        return out;
    }
    static int at(const var& array, int i, int fallback)
    {
        const auto* a = array.getArray();
        if (a == nullptr || i >= a->size())
            return fallback;
        const auto& v = a->getReference(i);
        return v.isInt() || v.isInt64() || v.isDouble() || v.isBool() ? static_cast<int>(std::llround(static_cast<double>(v)))
                                                                       : fallback;
    }

    void mix(const var& o, float& volumeDb, float& pan, float& delaySend, float& reverbSend, bool& mute, bool& solo) const
    {
        volumeDb = static_cast<float>(number(o, "volumeDb", volumeDb));
        pan = static_cast<float>(number(o, "pan", pan));
        delaySend = static_cast<float>(number(o, "delaySend", delaySend));
        reverbSend = static_cast<float>(number(o, "reverbSend", reverbSend));
        mute = flag(o, "mute", mute);
        solo = flag(o, "solo", solo);
    }

    DrumTrack drum(const var& o) const
    {
        DrumTrack d;
        d.name = text(o, "name", "");
        const auto s = o.getProperty("sound", var());
        const auto kind = text(s, "kind", "synth");
        if (kind != "synth" && kind != "sample")
            warn("drum track \"" + d.name + "\": unknown sound kind \"" + kind + "\", using a drum synth");
        d.sound.kind = kind == "sample" ? SoundRef::Kind::Sample : SoundRef::Kind::Synth;
        d.sound.synth = text(s, "synth", d.sound.synth);
        d.sound.samplePath = text(s, "sample", "");
        d.sound.params = params(s);
        d.sound.unknown = unknownOf(s, {"kind", "synth", "sample", "params"});
        if (const auto* steps = o.getProperty("steps", var()).getArray())
            for (const auto& st : *steps)
            {
                Step x;
                x.on = at(st, 0, 0) != 0;
                x.velocity = static_cast<std::uint8_t>(std::clamp(at(st, 1, 100), 0, 255));
                x.probability = static_cast<std::uint8_t>(std::clamp(at(st, 2, 100), 0, 255));
                x.microTiming = static_cast<std::int8_t>(std::clamp(at(st, 3, 0), -128, 127));
                x.ratchet = static_cast<std::uint8_t>(std::clamp(at(st, 4, 1), 0, 255));
                d.steps.push_back(x);
            }
        mix(o, d.volumeDb, d.pan, d.delaySend, d.reverbSend, d.mute, d.solo);
        d.chokeGroup = static_cast<std::uint8_t>(std::clamp(integer(o, "chokeGroup", 0), 0, 255));
        d.unknown = unknownOf(o, {"name", "sound", "steps", "volumeDb", "pan", "delaySend", "reverbSend", "mute",
                                  "solo", "chokeGroup"});
        return d;
    }

    SynthTrack synth(const var& o) const
    {
        SynthTrack t;
        t.name = text(o, "name", "");
        const auto p = o.getProperty("preset", var());
        const auto engine = text(p, "engine", "subtractive");
        t.preset.engine = engine == "fm" ? SynthPreset::Engine::Fm
                          : engine == "subBass" ? SynthPreset::Engine::SubBass
                                                : SynthPreset::Engine::Subtractive;
        if (engine != "fm" && engine != "subBass" && engine != "subtractive")
            warn("synth track \"" + t.name + "\": unknown engine \"" + engine + "\", using subtractive");
        t.preset.name = text(p, "name", t.preset.name);
        t.preset.params = params(p);
        t.preset.unknown = unknownOf(p, {"engine", "name", "params"});
        if (const auto* notes = o.getProperty("notes", var()).getArray())
            for (const auto& n : *notes)
                t.notes.push_back({at(n, 0, 0), at(n, 1, kPpq / 4), static_cast<std::uint8_t>(std::clamp(at(n, 2, 60), 0, 255)),
                                   static_cast<std::uint8_t>(std::clamp(at(n, 3, 100), 0, 255))});
        const auto a = o.getProperty("arp", var());
        t.arp.on = flag(a, "on", false);
        t.arp.rate = integer(a, "rate", 16);
        const auto mode = text(a, "mode", "up");
        t.arp.mode = mode == "down"       ? ArpSettings::Mode::Down
                     : mode == "upDown"   ? ArpSettings::Mode::UpDown
                     : mode == "random"   ? ArpSettings::Mode::Random
                     : mode == "asPlayed" ? ArpSettings::Mode::AsPlayed
                                          : ArpSettings::Mode::Up;
        t.arp.octaves = integer(a, "octaves", 1);
        t.arp.unknown = unknownOf(a, {"on", "rate", "mode", "octaves"});
        mix(o, t.volumeDb, t.pan, t.delaySend, t.reverbSend, t.mute, t.solo);
        t.unknown = unknownOf(o, {"name", "preset", "notes", "arp", "volumeDb", "pan", "delaySend", "reverbSend",
                                  "mute", "solo"});
        return t;
    }
};

std::filesystem::path toPath(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string fromPath(const std::filesystem::path& p, bool generic)
{
    const auto u = generic ? p.generic_u8string() : p.u8string();
    return std::string(u.begin(), u.end());
}

juce::File toFile(const std::filesystem::path& p)
{
    return juce::File(juce::String(p.wstring().c_str()));
}
} // namespace

std::string projectToJson(const Project& project)
{
    auto root = objectWith(project.unknown);
    root->setProperty("format", "devil-dj beat");
    root->setProperty("schemaVersion", project.schemaVersion);
    root->setProperty("followMaster", project.followMaster);
    root->setProperty("ownBpm", project.ownBpm);
    root->setProperty("swing", static_cast<double>(project.swing));
    root->setProperty("seed", static_cast<juce::int64>(project.seed));
    juce::Array<var> song;
    for (const int i : project.song)
        song.add(i);
    root->setProperty("song", song);
    juce::Array<var> patterns;
    std::set<std::string> samples;
    for (const auto& p : project.patterns)
    {
        auto o = objectWith(p.unknown);
        o->setProperty("name", str(p.name));
        o->setProperty("lengthSteps", p.lengthSteps);
        o->setProperty("stepsPerBeat", p.stepsPerBeat);
        juce::Array<var> drums, synths;
        for (const auto& d : p.drums)
        {
            drums.add(drumToVar(d));
            if (d.sound.kind == SoundRef::Kind::Sample && !d.sound.samplePath.empty())
                samples.insert(d.sound.samplePath);
        }
        for (const auto& t : p.synths)
            synths.add(synthToVar(t));
        o->setProperty("drums", drums);
        o->setProperty("synths", synths);
        patterns.add(var(o.get()));
    }
    root->setProperty("patterns", patterns);
    juce::Array<var> sampleList;
    for (const auto& s : samples)
        sampleList.add(str(s));
    root->setProperty("samples", sampleList);
    return utf8(juce::JSON::toString(var(root.get()), false));
}

util::Result<Project> projectFromJson(const std::string& json, std::vector<std::string>* warnings)
{
    var root;
    if (const auto r = juce::JSON::parse(str(json), root); r.failed())
        return util::Result<Project>::fail("Not a beat file: " + utf8(r.getErrorMessage()));
    if (root.getDynamicObject() == nullptr)
        return util::Result<Project>::fail("Not a beat file: the JSON isn't an object");
    Reader in{warnings};
    Project p;
    p.schemaVersion = Reader::integer(root, "schemaVersion", kSchemaVersion);
    if (p.schemaVersion > kSchemaVersion)
        in.warn("made by a newer Devil-DJ (format " + std::to_string(p.schemaVersion) +
                "): what this version doesn't know is kept but not used");
    p.followMaster = Reader::flag(root, "followMaster", p.followMaster);
    p.ownBpm = Reader::number(root, "ownBpm", p.ownBpm);
    p.swing = static_cast<float>(Reader::number(root, "swing", p.swing));
    p.seed = static_cast<std::uint32_t>(std::clamp<double>(Reader::number(root, "seed", 1.0), 0.0, 4294967295.0));
    if (const auto* song = root.getProperty("song", var()).getArray())
        for (int i = 0; i < song->size(); ++i)
            p.song.push_back(Reader::at(root.getProperty("song", var()), i, -1));
    if (const auto* patterns = root.getProperty("patterns", var()).getArray())
    {
        if (patterns->size() > kPatternCount)
            in.warn("more than " + std::to_string(kPatternCount) + " patterns: the rest are left out");
        for (int i = 0; i < std::min(kPatternCount, patterns->size()); ++i)
        {
            const auto& o = patterns->getReference(i);
            auto& pat = p.patterns[static_cast<std::size_t>(i)];
            pat.name = Reader::text(o, "name", pat.name);
            pat.lengthSteps = Reader::integer(o, "lengthSteps", 16);
            pat.stepsPerBeat = Reader::integer(o, "stepsPerBeat", 4);
            if (const auto* drums = o.getProperty("drums", var()).getArray())
                for (const auto& d : *drums)
                    pat.drums.push_back(in.drum(d));
            if (const auto* synths = o.getProperty("synths", var()).getArray())
                for (const auto& t : *synths)
                    pat.synths.push_back(in.synth(t));
            if (pat.drums.size() > static_cast<std::size_t>(kMaxDrumTracks) ||
                pat.synths.size() > static_cast<std::size_t>(kMaxSynthTracks))
                in.warn("pattern " + pat.name + ": more tracks than " + std::to_string(kMaxDrumTracks) + " drums / " +
                        std::to_string(kMaxSynthTracks) + " synths; the rest are left out");
            pat.unknown = Reader::unknownOf(o, {"name", "lengthSteps", "stepsPerBeat", "drums", "synths"});
        }
    }
    p.unknown = Reader::unknownOf(root, {"format", "schemaVersion", "followMaster", "ownBpm", "swing", "seed", "song",
                                         "patterns", "samples"});
    const Project before = p;
    normalize(p);
    if (!(p == before))
        in.warn("some values were out of range and have been brought into range");
    return util::Result<Project>::ok(std::move(p));
}

util::Result<bool> saveProject(const Project& project, const std::filesystem::path& file)
{
    // Samples inside the project's folder: relative to it.
    Project out = project;
    const auto folder = file.parent_path();
    for (auto& pat : out.patterns)
        for (auto& d : pat.drums)
            if (!d.sound.samplePath.empty())
            {
                const auto sample = toPath(d.sound.samplePath);
                if (sample.is_absolute())
                {
                    const auto rel = sample.lexically_normal().lexically_relative(folder.lexically_normal());
                    if (!rel.empty() && *rel.begin() != "..")
                        d.sound.samplePath = fromPath(rel, true);
                }
            }
    const auto target = toFile(file);
    if (!target.getParentDirectory().createDirectory())
        return util::Result<bool>::fail("Can't create the folder " + fromPath(folder, false));
    // replaceWithText writes a temporary file next to it and moves it over the old one.
    if (!target.replaceWithText(str(projectToJson(out)), false, false, "\n"))
        return util::Result<bool>::fail("Can't write " + fromPath(file, false));
    return util::Result<bool>::ok(true);
}

util::Result<Project> loadProject(const std::filesystem::path& file, std::vector<std::string>* warnings)
{
    const auto f = toFile(file);
    if (!f.existsAsFile())
        return util::Result<Project>::fail("No such file: " + fromPath(file, false));
    auto r = projectFromJson(utf8(f.loadFileAsString()), warnings);
    if (!r)
        return r;
    auto p = std::move(r).value();
    for (auto& pat : p.patterns)
        for (auto& d : pat.drums)
            if (!d.sound.samplePath.empty())
            {
                const auto sample = toPath(d.sound.samplePath);
                if (sample.is_relative())
                    d.sound.samplePath = fromPath((file.parent_path() / sample).lexically_normal().make_preferred(), false);
            }
    return util::Result<Project>::ok(std::move(p));
}

} // namespace devildj::beatmaker
