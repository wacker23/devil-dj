#include "ui/PianoRoll.h"

#include "beatmaker/NoteEdit.h"
#include "beatmaker/SynthPresets.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kRowIn(0xff23232b);
const juce::Colour kRowOut(0xff1a1a20);
const juce::Colour kLine(0xff30303c);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
const juce::Colour kNote(0xff4fb3e8);

using GA = GlobalAction;
using beatmaker::ArpSettings;

struct SoundChoice
{
    juce::String label;
    dsp::SynthEngine engine;
    int preset;
};

const std::vector<SoundChoice>& soundChoices()
{
    static const auto list = []
    {
        std::vector<SoundChoice> out;
        const char* names[] = {"Synth", "FM", "Sub"};
        for (const auto e : {dsp::SynthEngine::Subtractive, dsp::SynthEngine::Fm, dsp::SynthEngine::SubBass})
        {
            const auto& presets = beatmaker::factorySynthPresets(e);
            for (int i = 0; i < static_cast<int>(presets.size()); ++i)
                out.push_back({juce::String(names[static_cast<int>(e)]) + ": " + presets[static_cast<std::size_t>(i)].name, e, i});
        }
        return out;
    }();
    return list;
}

const char* modeName(ArpSettings::Mode m)
{
    switch (m)
    {
        case ArpSettings::Mode::Down: return "DOWN";
        case ArpSettings::Mode::UpDown: return "UP-DOWN";
        case ArpSettings::Mode::Random: return "RANDOM";
        case ArpSettings::Mode::AsPlayed: return "AS PLAYED";
        case ArpSettings::Mode::Up: break;
    }
    return "UP";
}

void styleButton(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kAccent);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    b.setWantsKeyboardFocus(false);
}

bool isBlackKey(int pitch)
{
    const int pc = pitch % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

juce::String pitchName(int pitch)
{
    const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return juce::String(names[pitch % 12]) + juce::String(pitch / 12 - 1);
}
} // namespace

PianoRoll::PianoRoll()
{
    setWantsKeyboardFocus(true);
    for (int i = 0; i < beatmaker::kMaxSynthTracks; ++i)
    {
        auto& b = trackButtons[static_cast<std::size_t>(i)];
        styleButton(b);
        b.setButtonText("SYNTH " + juce::String(i + 1));
        b.setTooltip("Synth track " + juce::String(i + 1) + " (" + juce::String(i + 1) + ")");
        b.onClick = [this, i] { setTrack(i); };
        addAndMakeVisible(b);
    }
    for (int c = 0; c < static_cast<int>(soundChoices().size()); ++c)
        sound.addItem(soundChoices()[static_cast<std::size_t>(c)].label, c + 1);
    sound.setTooltip("The track's sound (Shift+Left / Shift+Right)");
    sound.onChange = [this] { setSoundChoice(sound.getSelectedId() - 1); };
    for (int s = 0; s < static_cast<int>(beatmaker::scales().size()); ++s)
        scaleChoice.addItem(beatmaker::scales()[static_cast<std::size_t>(s)].name, s + 1);
    scaleChoice.setTooltip("Scale lock: its rows highlighted (K cycles)");
    scaleChoice.onChange = [this]
    {
        scaleIndex = scaleChoice.getSelectedId() - 1;
        repaint();
    };
    for (int r = 0; r < 12; ++r)
        rootChoice.addItem(pitchName(r + 12).dropLastCharacters(1), r + 1);
    rootChoice.setTooltip("The scale's root");
    rootChoice.onChange = [this]
    {
        rootNote = rootChoice.getSelectedId() - 1;
        repaint();
    };
    for (int c = 0; c < static_cast<int>(beatmaker::chords().size()); ++c)
        chordChoice.addItem(beatmaker::chords()[static_cast<std::size_t>(c)].name, c + 1);
    chordChoice.setTooltip("Chord stamp: a click draws the whole chord (C cycles)");
    chordChoice.onChange = [this] { chordIndex = chordChoice.getSelectedId() - 1; };
    for (auto* c : {&sound, &scaleChoice, &rootChoice, &chordChoice})
    {
        c->setWantsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
    for (auto* b : {&snapButton, &scaleSnapButton, &quantizeNow, &arpOn, &arpRate, &arpMode, &arpOctaves})
    {
        styleButton(*b);
        addAndMakeVisible(*b);
    }
    snapButton.setTooltip("Snap: 1/4 .. 1/32, then triplets (G)");
    snapButton.onClick = [this] { perform(GA::BeatMakerSnap); };
    scaleSnapButton.setTooltip("Notes drawn and moved stay in the scale (Shift+K)");
    scaleSnapButton.onClick = [this] { perform(GA::BeatMakerScaleSnap); };
    quantizeNow.setTooltip("Every note's start to the snap grid (Q)");
    quantizeNow.onClick = [this] { perform(GA::BeatMakerQuantize); };
    arpOn.setTooltip("Arpeggiator on / off (A)");
    arpOn.onClick = [this] { perform(GA::BeatMakerArp); };
    arpRate.setTooltip("Arpeggiator rate (Shift+A)");
    arpRate.onClick = [this] { perform(GA::BeatMakerArpRate); };
    arpMode.setTooltip("Arpeggiator mode (Alt+A)");
    arpMode.onClick = [this] { perform(GA::BeatMakerArpMode); };
    arpOctaves.setTooltip("Arpeggiator octaves (Ctrl+A)");
    arpOctaves.onClick = [this] { perform(GA::BeatMakerArpOctaves); };
    refresh();
}

void PianoRoll::setProject(beatmaker::Project* p)
{
    project = p;
    refresh();
}

void PianoRoll::setPattern(int index)
{
    patternIndex = std::clamp(index, 0, beatmaker::kPatternCount - 1);
    modelChanged();
}

void PianoRoll::setTrack(int synthTrack)
{
    trackIndex = std::clamp(synthTrack, 0, beatmaker::kMaxSynthTracks - 1);
    refresh();
}

void PianoRoll::modelChanged()
{
    if (auto* p = pattern())
        cursorAt = std::clamp(cursorAt, 0, std::max(0, p->lengthTicks() - snap()));
    refresh();
}

beatmaker::Pattern* PianoRoll::pattern()
{
    return project == nullptr ? nullptr : &project->patterns[static_cast<std::size_t>(patternIndex)];
}

beatmaker::SynthTrack* PianoRoll::current()
{
    auto* p = pattern();
    if (p == nullptr || trackIndex >= static_cast<int>(p->synths.size()))
        return nullptr;
    return &p->synths[static_cast<std::size_t>(trackIndex)];
}

beatmaker::SynthTrack& PianoRoll::ensureTrack()
{
    auto& p = *pattern();
    while (static_cast<int>(p.synths.size()) <= trackIndex)
    {
        beatmaker::SynthTrack t;
        t.name = "Synth " + std::to_string(p.synths.size() + 1);
        const auto& f = beatmaker::factorySynthPresets(dsp::SynthEngine::Subtractive)[0];
        t.preset = beatmaker::synthPresetOf(f.name, f.params);
        p.synths.push_back(t);
    }
    return p.synths[static_cast<std::size_t>(trackIndex)];
}

int PianoRoll::snap() const noexcept
{
    return beatmaker::snapTicks(division, triplet);
}

int PianoRoll::visiblePitches() const noexcept
{
    return std::max(1, gridArea.getHeight() / rowHeight());
}

void PianoRoll::edited()
{
    refresh();
    if (onChanged)
        onChanged();
}

void PianoRoll::refresh()
{
    for (int i = 0; i < beatmaker::kMaxSynthTracks; ++i)
        trackButtons[static_cast<std::size_t>(i)].setToggleState(i == trackIndex, juce::dontSendNotification);
    const auto* t = current();
    int id = 0;
    if (t != nullptr)
        for (std::size_t c = 0; c < soundChoices().size() && id == 0; ++c)
        {
            const auto& choice = soundChoices()[c];
            const auto& f = beatmaker::factorySynthPresets(choice.engine)[static_cast<std::size_t>(choice.preset)];
            if (t->preset.name == f.name && beatmaker::synthParamsOf(t->preset).engine == choice.engine)
                id = static_cast<int>(c) + 1;
        }
    sound.setSelectedId(id, juce::dontSendNotification);
    if (id == 0)
        sound.setText(t != nullptr ? juce::String::fromUTF8(t->preset.name.c_str()) : juce::String("(empty track)"),
                      juce::dontSendNotification);
    scaleChoice.setSelectedId(scaleIndex + 1, juce::dontSendNotification);
    rootChoice.setSelectedId(rootNote + 1, juce::dontSendNotification);
    chordChoice.setSelectedId(chordIndex + 1, juce::dontSendNotification);
    snapButton.setButtonText("1/" + juce::String(division) + (triplet ? "T" : ""));
    scaleSnapButton.setToggleState(scaleSnap, juce::dontSendNotification);
    const ArpSettings arp = t != nullptr ? t->arp : ArpSettings{};
    arpOn.setToggleState(arp.on, juce::dontSendNotification);
    arpRate.setButtonText("1/" + juce::String(arp.rate));
    arpMode.setButtonText(modeName(arp.mode));
    arpOctaves.setButtonText(juce::String(arp.octaves) + " OCT");
    repaint();
}

void PianoRoll::setSoundChoice(int choice)
{
    if (project == nullptr || choice < 0 || choice >= static_cast<int>(soundChoices().size()))
        return;
    const auto& c = soundChoices()[static_cast<std::size_t>(choice)];
    const auto& f = beatmaker::factorySynthPresets(c.engine)[static_cast<std::size_t>(c.preset)];
    ensureTrack().preset = beatmaker::synthPresetOf(f.name, f.params);
    edited();
}

int PianoRoll::tickAt(int x) const
{
    if (project == nullptr || gridArea.getWidth() <= 0)
        return 0;
    const int length = project->patterns[static_cast<std::size_t>(patternIndex)].lengthTicks();
    return std::clamp(static_cast<int>(static_cast<long long>(x - gridArea.getX()) * length / gridArea.getWidth()), 0,
                      std::max(0, length - 1));
}

int PianoRoll::pitchAt(int y) const
{
    const int row = (gridArea.getBottom() - 1 - y) / rowHeight();
    return std::clamp(lowPitch + row, 0, 127);
}

juce::Point<int> PianoRoll::pointFor(int tick, int pitch) const
{
    const int length = project == nullptr ? 384 : project->patterns[static_cast<std::size_t>(patternIndex)].lengthTicks();
    const int x = gridArea.getX() + static_cast<int>(static_cast<long long>(tick) * gridArea.getWidth() / std::max(1, length));
    const int y = gridArea.getBottom() - (pitch - lowPitch + 1) * rowHeight();
    return {x, y};
}

int PianoRoll::placePitch(int pitch) const
{
    return scaleSnap ? beatmaker::snapToScale(pitch, rootNote, scaleIndex) : pitch;
}

void PianoRoll::stamp(int tick, int pitch)
{
    auto& t = ensureTrack();
    std::vector<beatmaker::Note> notes;
    for (const int p : beatmaker::chordPitches(placePitch(pitch), chordIndex))
        notes.push_back({tick, snap(), static_cast<std::uint8_t>(placePitch(p)), 100});
    const auto first = beatmaker::addNotes(t, notes);
    // The drawn notes are what a following drag lengthens.
    dragNotes.clear();
    for (std::size_t i = 0; i < t.notes.size(); ++i)
        if (t.notes[i].startTick == tick && t.notes[i].lengthTicks == snap() &&
            std::any_of(notes.begin(), notes.end(), [&](const beatmaker::Note& n) { return n.pitch == t.notes[i].pitch; }))
            dragNotes.push_back(i);
    (void)first;
}

std::optional<std::size_t> PianoRoll::cursorNoteIndex()
{
    const auto* t = current();
    return t == nullptr ? std::nullopt : beatmaker::noteAt(*t, cursorAt, cursorNote);
}

void PianoRoll::keepCursorVisible()
{
    if (cursorNote < lowPitch)
        lowPitch = cursorNote;
    if (cursorNote >= lowPitch + visiblePitches())
        lowPitch = cursorNote - visiblePitches() + 1;
    lowPitch = std::clamp(lowPitch, 0, std::max(0, 128 - visiblePitches()));
}

void PianoRoll::scrollRows(int rows)
{
    lowPitch = std::clamp(lowPitch + rows, 0, std::max(0, 128 - visiblePitches()));
    repaint();
}

bool PianoRoll::perform(GlobalAction a)
{
    if (project == nullptr)
        return false;
    const int length = pattern()->lengthTicks();
    const auto noteMove = [&](int dTick, int dPitch)
    {
        auto* t = current();
        const auto i = cursorNoteIndex();
        if (t == nullptr || !i)
            return;
        auto& n = t->notes[*i];
        n.startTick = std::clamp(n.startTick + dTick, 0, std::max(0, length - snap()));
        if (dPitch != 0)
            n.pitch = static_cast<std::uint8_t>(scaleSnap ? beatmaker::stepInScale(n.pitch, rootNote, scaleIndex, dPitch)
                                                          : std::clamp(n.pitch + dPitch, 0, 127));
        cursorAt = n.startTick;
        cursorNote = n.pitch;
        keepCursorVisible();
        beatmaker::sortNotes(*t);
        edited();
    };
    const int ai = static_cast<int>(a);
    if (ai >= static_cast<int>(GA::BeatMakerTrack1) && ai < static_cast<int>(GA::BeatMakerTrack1) + beatmaker::kMaxSynthTracks)
    {
        setTrack(ai - static_cast<int>(GA::BeatMakerTrack1));
        return true;
    }
    switch (a)
    {
        case GA::BeatMakerCursorLeft:
            cursorAt = std::max(0, beatmaker::snapDown(cursorAt - 1, snap()));
            refresh();
            return true;
        case GA::BeatMakerCursorRight:
            cursorAt = std::min(std::max(0, length - snap()), beatmaker::snapDown(cursorAt, snap()) + snap());
            refresh();
            return true;
        case GA::BeatMakerCursorUp:
        case GA::BeatMakerCursorDown:
        {
            const int dir = a == GA::BeatMakerCursorUp ? 1 : -1;
            cursorNote = scaleSnap ? beatmaker::stepInScale(cursorNote, rootNote, scaleIndex, dir) : std::clamp(cursorNote + dir, 0, 127);
            keepCursorVisible();
            refresh();
            return true;
        }
        case GA::BeatMakerToggleStep:
            if (const auto i = cursorNoteIndex())
            {
                auto& t = *current();
                t.notes.erase(t.notes.begin() + static_cast<std::ptrdiff_t>(*i));
            }
            else
                stamp(beatmaker::snapDown(cursorAt, snap()), cursorNote);
            edited();
            return true;
        case GA::BeatMakerNoteLeft: noteMove(-snap(), 0); return true;
        case GA::BeatMakerNoteRight: noteMove(snap(), 0); return true;
        case GA::BeatMakerNoteUp: noteMove(0, 1); return true;
        case GA::BeatMakerNoteDown: noteMove(0, -1); return true;
        case GA::BeatMakerMicroEarlier: // shorter
        case GA::BeatMakerMicroLater:   // longer
            if (const auto i = cursorNoteIndex())
            {
                auto& n = current()->notes[*i];
                n.lengthTicks = std::clamp(n.lengthTicks + (a == GA::BeatMakerMicroLater ? snap() : -snap()), snap(),
                                           std::max(snap(), length - n.startTick));
                edited();
            }
            return true;
        case GA::BeatMakerVelocityUp:
        case GA::BeatMakerVelocityDown:
            if (const auto i = cursorNoteIndex())
            {
                auto& n = current()->notes[*i];
                n.velocity = static_cast<std::uint8_t>(std::clamp(n.velocity + (a == GA::BeatMakerVelocityUp ? 8 : -8), 1, 127));
                edited();
            }
            return true;
        case GA::BeatMakerSoundPrevious:
        case GA::BeatMakerSoundNext:
        {
            const int n = static_cast<int>(soundChoices().size());
            const int at = std::max(0, sound.getSelectedId() - 1);
            setSoundChoice((at + (a == GA::BeatMakerSoundNext ? 1 : -1) + n) % n);
            return true;
        }
        case GA::BeatMakerSnap:
            // 1/4 -> 1/8 -> 1/16 -> 1/32 -> 1/4T ... -> 1/32T -> 1/4
            if (division == 32)
            {
                division = 4;
                triplet = !triplet;
            }
            else
                division *= 2;
            refresh();
            return true;
        case GA::BeatMakerScale:
            scaleIndex = (scaleIndex + 1) % static_cast<int>(beatmaker::scales().size());
            refresh();
            return true;
        case GA::BeatMakerScaleSnap:
            scaleSnap = !scaleSnap;
            refresh();
            return true;
        case GA::BeatMakerChord:
            chordIndex = (chordIndex + 1) % static_cast<int>(beatmaker::chords().size());
            refresh();
            return true;
        case GA::BeatMakerQuantize:
            if (auto* t = current())
            {
                beatmaker::quantize(*t, snap());
                edited();
            }
            return true;
        case GA::BeatMakerArp:
            ensureTrack().arp.on = !ensureTrack().arp.on;
            edited();
            return true;
        case GA::BeatMakerArpRate:
        {
            auto& arp = ensureTrack().arp;
            arp.rate = arp.rate == 8 ? 16 : arp.rate == 16 ? 32 : 8;
            edited();
            return true;
        }
        case GA::BeatMakerArpMode:
        {
            auto& arp = ensureTrack().arp;
            arp.mode = static_cast<ArpSettings::Mode>((static_cast<int>(arp.mode) + 1) % 5);
            edited();
            return true;
        }
        case GA::BeatMakerArpOctaves:
        {
            auto& arp = ensureTrack().arp;
            arp.octaves = arp.octaves % 4 + 1;
            edited();
            return true;
        }
        case GA::BeatMakerCopy:
        case GA::BeatMakerPaste:
            return false; // the panel's clipboard (notes) - see BeatMakerPanel
        case GA::BeatMakerClear:
            if (auto* t = current())
            {
                t->notes.clear();
                edited();
            }
            return true;
        default:
            return false;
    }
}

void PianoRoll::pressAt(juce::Point<int> p, juce::ModifierKeys mods)
{
    drag = Drag::None;
    if (project == nullptr)
        return;
    if (laneArea.contains(p))
    {
        drag = Drag::Lane;
        dragTo(p);
        return;
    }
    if (!gridArea.contains(p))
        return;
    const int tick = tickAt(p.x), pitch = pitchAt(p.y);
    cursorAt = beatmaker::snapDown(tick, snap());
    cursorNote = pitch;
    auto* t = current();
    const auto hit = t != nullptr ? beatmaker::noteAt(*t, tick, pitch) : std::nullopt;
    if (hit && mods.isPopupMenu())
    {
        t->notes.erase(t->notes.begin() + static_cast<std::ptrdiff_t>(*hit));
        edited();
        return;
    }
    if (hit)
    {
        const auto& n = t->notes[*hit];
        const int rightEdge = pointFor(n.startTick + n.lengthTicks, n.pitch).x;
        drag = p.x >= rightEdge - 6 ? Drag::Resize : Drag::Move;
        dragNotes = {*hit};
        dragStart = {n};
        dragTick = tick;
        dragPitch = pitch;
        cursorAt = n.startTick;
        refresh();
        return;
    }
    if (mods.isPopupMenu())
        return;
    stamp(cursorAt, pitch);
    drag = Drag::Draw;
    dragTick = cursorAt;
    edited();
}

void PianoRoll::dragTo(juce::Point<int> p)
{
    auto* t = current();
    if (t == nullptr || drag == Drag::None)
        return;
    const int length = pattern()->lengthTicks();
    if (drag == Drag::Lane)
    {
        // The notes under x get the velocity of y (top = 127).
        const int tick = tickAt(p.x);
        const float v = 1.0f - std::clamp(static_cast<float>(p.y - laneArea.getY()) / static_cast<float>(std::max(1, laneArea.getHeight())), 0.0f, 1.0f);
        bool any = false;
        for (auto& n : t->notes)
            if (tick >= n.startTick && tick < n.startTick + std::max(n.lengthTicks, snap()))
            {
                n.velocity = static_cast<std::uint8_t>(std::clamp(juce::roundToInt(1.0f + 126.0f * v), 1, 127));
                any = true;
            }
        if (any)
            edited();
        return;
    }
    const int tick = tickAt(p.x);
    if (drag == Drag::Draw || drag == Drag::Resize)
    {
        // Longer / shorter to the snap line after the pointer (at least one snap).
        for (const auto i : dragNotes)
        {
            if (i >= t->notes.size())
                continue;
            auto& n = t->notes[i];
            const int end = beatmaker::snapDown(tick, snap()) + snap();
            n.lengthTicks = std::clamp(end - n.startTick, snap(), std::max(snap(), length - n.startTick));
        }
        edited();
        return;
    }
    // Move: by whole snaps and semitones (the scale's if snapping).
    const int dTick = beatmaker::snapNearest(tick - dragTick, snap());
    const int dPitch = pitchAt(p.y) - dragPitch;
    for (std::size_t k = 0; k < dragNotes.size() && k < dragStart.size(); ++k)
    {
        const auto i = dragNotes[k];
        if (i >= t->notes.size())
            continue;
        auto& n = t->notes[i];
        const auto& from = dragStart[k];
        n.startTick = std::clamp(from.startTick + dTick, 0, std::max(0, length - snap()));
        n.pitch = static_cast<std::uint8_t>(placePitch(std::clamp(from.pitch + dPitch, 0, 127)));
        cursorAt = n.startTick;
        cursorNote = n.pitch;
    }
    edited();
}

void PianoRoll::release()
{
    if (drag == Drag::Move || drag == Drag::Draw || drag == Drag::Resize)
        if (auto* t = current())
            beatmaker::sortNotes(*t);
    drag = Drag::None;
    dragNotes.clear();
    dragStart.clear();
}

void PianoRoll::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    pressAt(e.getPosition(), e.mods);
}

void PianoRoll::mouseDrag(const juce::MouseEvent& e)
{
    dragTo(e.getPosition());
}

void PianoRoll::mouseUp(const juce::MouseEvent&)
{
    release();
}

void PianoRoll::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    scrollRows(wheel.deltaY > 0 ? 2 : -2);
}

void PianoRoll::resized()
{
    auto area = getLocalBounds().reduced(4);
    auto bar = area.removeFromTop(28);
    for (auto& b : trackButtons)
        b.setBounds(bar.removeFromLeft(70).reduced(2));
    sound.setBounds(bar.removeFromLeft(170).reduced(2));
    snapButton.setBounds(bar.removeFromLeft(56).reduced(2));
    rootChoice.setBounds(bar.removeFromLeft(56).reduced(2));
    scaleChoice.setBounds(bar.removeFromLeft(130).reduced(2));
    scaleSnapButton.setBounds(bar.removeFromLeft(110).reduced(2));
    chordChoice.setBounds(bar.removeFromLeft(80).reduced(2));
    quantizeNow.setBounds(bar.removeFromLeft(80).reduced(2));
    arpOn.setBounds(bar.removeFromLeft(46).reduced(2));
    arpRate.setBounds(bar.removeFromLeft(46).reduced(2));
    arpMode.setBounds(bar.removeFromLeft(80).reduced(2));
    arpOctaves.setBounds(bar.removeFromLeft(56).reduced(2));
    area.removeFromTop(4);
    laneArea = area.removeFromBottom(std::min(50, area.getHeight() / 5));
    area.removeFromBottom(4);
    keysArea = area.removeFromLeft(40);
    gridArea = area;
    laneArea = laneArea.withX(gridArea.getX()).withWidth(gridArea.getWidth());
    keepCursorVisible();
}

void PianoRoll::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    if (project == nullptr)
        return;
    const auto& pat = project->patterns[static_cast<std::size_t>(patternIndex)];
    const int length = pat.lengthTicks();
    // Rows: in-scale lighter; the keys at the left.
    for (int r = 0; r < visiblePitches(); ++r)
    {
        const int pitch = lowPitch + r;
        if (pitch > 127)
            break;
        const int y = gridArea.getBottom() - (r + 1) * rowHeight();
        const bool in = scaleIndex == 0 ? !isBlackKey(pitch) : beatmaker::inScale(pitch, rootNote, scaleIndex);
        g.setColour(in ? kRowIn : kRowOut);
        g.fillRect(gridArea.getX(), y, gridArea.getWidth(), rowHeight() - 1);
        g.setColour(isBlackKey(pitch) ? juce::Colour(0xff101014) : kDim.withAlpha(0.8f));
        g.fillRect(keysArea.getX(), y, keysArea.getWidth() - 2, rowHeight() - 1);
        if (pitch % 12 == 0)
        {
            g.setColour(kBackground);
            g.setFont(juce::FontOptions(9.0f));
            g.drawText(pitchName(pitch), keysArea.getX() + 2, y, keysArea.getWidth() - 4, rowHeight(), juce::Justification::centredLeft);
        }
    }
    // Snap lines, beats and bars.
    for (int tick = 0; tick < length; tick += snap())
    {
        const int x = pointFor(tick, 0).x;
        g.setColour(tick % (4 * beatmaker::kPpq) == 0 ? kDim.withAlpha(0.6f) : tick % beatmaker::kPpq == 0 ? kLine.brighter(0.3f) : kLine);
        g.drawVerticalLine(x, static_cast<float>(gridArea.getY()), static_cast<float>(gridArea.getBottom()));
    }
    // Notes, and the velocity lane.
    g.setColour(kPanel);
    g.fillRect(laneArea);
    if (const auto* t = trackIndex < static_cast<int>(pat.synths.size()) ? &pat.synths[static_cast<std::size_t>(trackIndex)] : nullptr)
        for (const auto& n : t->notes)
        {
            const auto a = pointFor(n.startTick, n.pitch), b = pointFor(n.startTick + n.lengthTicks, n.pitch);
            const juce::Rectangle<int> box(a.x, a.y, std::max(3, b.x - a.x), rowHeight() - 1);
            g.setColour(kNote.withAlpha(0.45f + 0.55f * static_cast<float>(n.velocity) / 127.0f));
            g.fillRect(box);
            g.setColour(kNote.brighter());
            g.drawRect(box);
            const int h = laneArea.getHeight() * n.velocity / 127;
            g.setColour(kNote);
            g.fillRect(a.x, laneArea.getBottom() - h, 3, h);
        }
    // The keyboard cursor.
    const auto c = pointFor(cursorAt, cursorNote), c2 = pointFor(cursorAt + snap(), cursorNote);
    g.setColour(kAccent);
    g.drawRect(juce::Rectangle<int>(c.x, c.y, std::max(3, c2.x - c.x), rowHeight()), 2);
    g.setColour(kDim);
    g.setFont(juce::FontOptions(10.0f));
    g.drawText("velocity", laneArea.reduced(4, 2), juce::Justification::topRight);
}

} // namespace devildj::ui
