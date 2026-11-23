#include "ui/StepGrid.h"

#include "beatmaker/DrumKits.h"

#include <algorithm>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kBeat(0xff272730);
const juce::Colour kBar(0xff30303c);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
const juce::Colour kOn(0xffe8a23c);

using GA = GlobalAction;

// Probability / micro-timing / ratchet for one step: three sliders in a call-out.
class StepEditor final : public juce::Component
{
public:
    StepEditor(beatmaker::Step& s, std::function<void()> changed)
        : step(s)
        , onChanged(std::move(changed))
    {
        const auto setUp = [this](juce::Slider& sl, juce::Label& label, const char* text, double lo, double hi, double v)
        {
            sl.setSliderStyle(juce::Slider::LinearHorizontal);
            sl.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
            sl.setRange(lo, hi, 1.0);
            sl.setValue(v, juce::dontSendNotification);
            label.setText(text, juce::dontSendNotification);
            label.setColour(juce::Label::textColourId, kText);
            addAndMakeVisible(sl);
            addAndMakeVisible(label);
        };
        setUp(probability, probabilityLabel, "Probability %", 0, 100, step.probability);
        setUp(micro, microLabel, "Micro-timing %", -50, 50, step.microTiming);
        setUp(ratchet, ratchetLabel, "Ratchet", 1, 4, step.ratchet);
        probability.onValueChange = [this]
        {
            step.probability = static_cast<std::uint8_t>(probability.getValue());
            onChanged();
        };
        micro.onValueChange = [this]
        {
            step.microTiming = static_cast<std::int8_t>(micro.getValue());
            onChanged();
        };
        ratchet.onValueChange = [this]
        {
            step.ratchet = static_cast<std::uint8_t>(ratchet.getValue());
            onChanged();
        };
        setSize(300, 96);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(4);
        for (auto [label, slider] : {std::pair{&probabilityLabel, &probability}, std::pair{&microLabel, &micro},
                                     std::pair{&ratchetLabel, &ratchet}})
        {
            auto row = area.removeFromTop(28);
            label->setBounds(row.removeFromLeft(100));
            slider->setBounds(row);
        }
    }

private:
    beatmaker::Step& step;
    std::function<void()> onChanged;
    juce::Slider probability, micro, ratchet;
    juce::Label probabilityLabel, microLabel, ratchetLabel;
};

void styleButton(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kAccent);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    b.setWantsKeyboardFocus(false);
}
} // namespace

const std::vector<StepGrid::SoundChoice>& StepGrid::soundChoices()
{
    static const auto list = []
    {
        std::vector<SoundChoice> out;
        const char* names[] = {"Kick", "Snare", "Clap", "Closed hat", "Open hat", "Low tom",
                               "Mid tom", "High tom", "Rim", "Cowbell", "Shaker"};
        for (int k = 0; k < dsp::kDrumKinds; ++k)
            for (const auto& p : beatmaker::drumPresets(static_cast<dsp::DrumKind>(k)))
                out.push_back({juce::String(names[k]) + ": " + p.name, dsp::drumKindName(static_cast<dsp::DrumKind>(k)), p.name});
        return out;
    }();
    return list;
}

StepGrid::StepGrid()
{
    setWantsKeyboardFocus(true);
    for (auto* b : {&play, &length, &pageButton})
    {
        styleButton(*b);
        addAndMakeVisible(*b);
    }
    play.setTooltip("Start / stop the Beat Maker, on the bar (Space)");
    play.onClick = [this]
    {
        if (onPlayStop)
            onPlayStop();
    };
    length.setTooltip("The pattern's length: 16, 32 or 64 steps (L)");
    length.onClick = [this] { perform(GA::BeatMakerLength); };
    pageButton.setTooltip("The 16 steps shown (Page Up / Page Down)");
    pageButton.onClick = [this] { perform(GA::BeatMakerPageNext); };
    for (int i = 0; i < beatmaker::kPatternCount; ++i)
    {
        auto& b = patternButtons[static_cast<std::size_t>(i)];
        styleButton(b);
        b.setButtonText(juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)));
        b.setTooltip("Pattern " + b.getButtonText() + " (Ctrl+" + juce::String(i + 1) + ")");
        b.onClick = [this, i] { selectPattern(i); };
        addAndMakeVisible(b);
    }
    swing.setTooltip("Swing: every 2nd step later, up to 75 % (- / =)");
    swing.valueText = [](double v) { return juce::String(juce::roundToInt(v * 75.0)) + " %"; };
    swing.onChange = [this](double v)
    {
        if (project != nullptr)
        {
            project->swing = static_cast<float>(v * beatmaker::kMaxSwing);
            edited();
        }
    };
    addAndMakeVisible(swing);
    for (int i = 0; i < beatmaker::kMaxDrumTracks; ++i)
    {
        auto& r = rows[static_cast<std::size_t>(i)];
        r.name.setColour(juce::Label::textColourId, kText);
        r.name.setFont(juce::FontOptions(12.0f));
        r.name.setInterceptsMouseClicks(false, false);
        for (int c = 0; c < static_cast<int>(soundChoices().size()); ++c)
            r.sound.addItem(soundChoices()[static_cast<std::size_t>(c)].label, c + 1);
        r.sound.setTooltip("Track " + juce::String(i + 1) + "'s sound (Shift+Left / Shift+Right on the cursor track)");
        r.sound.setWantsKeyboardFocus(false);
        r.sound.onChange = [this, i] { setSound(i, rows[static_cast<std::size_t>(i)].sound.getSelectedId() - 1); };
        for (auto* b : {&r.mute, &r.solo})
            styleButton(*b);
        r.mute.setClickingTogglesState(false);
        r.mute.setTooltip("Mute (M on the cursor track)");
        r.solo.setTooltip("Solo (S on the cursor track)");
        r.mute.onClick = [this, i]
        {
            if (auto* t = trackAt(i))
            {
                t->mute = !t->mute;
                edited();
            }
        };
        r.solo.onClick = [this, i]
        {
            if (auto* t = trackAt(i))
            {
                t->solo = !t->solo;
                edited();
            }
        };
        for (juce::Component* c : {static_cast<juce::Component*>(&r.name), static_cast<juce::Component*>(&r.sound),
                                   static_cast<juce::Component*>(&r.mute), static_cast<juce::Component*>(&r.solo)})
            addAndMakeVisible(c);
    }
    refresh();
}

StepGrid::~StepGrid() = default;

void StepGrid::setProject(beatmaker::Project* p)
{
    project = p;
    patternIndex = 0;
    track = step = pageIndex = 0;
    ensureTracks();
    refresh();
}

beatmaker::Pattern& StepGrid::shown()
{
    return project->patterns[static_cast<std::size_t>(patternIndex)];
}

void StepGrid::ensureTracks()
{
    // A pattern shown for the first time gets pattern A's sounds (empty steps), so every pattern has 8 rows.
    if (project == nullptr)
        return;
    auto& p = shown();
    if (p.drums.empty())
    {
        const auto& a = project->patterns[0];
        if (!a.drums.empty() && patternIndex != 0)
            for (const auto& d : a.drums)
            {
                auto copy = d;
                copy.steps.assign(static_cast<std::size_t>(p.lengthSteps), {});
                p.drums.push_back(copy);
            }
        else
            p.drums = beatmaker::kitTracks(beatmaker::drumKits()[0], p.lengthSteps);
    }
}

int StepGrid::pages() const noexcept
{
    if (project == nullptr)
        return 1;
    return std::max(1, project->patterns[static_cast<std::size_t>(patternIndex)].lengthSteps / kVisibleSteps);
}

beatmaker::DrumTrack* StepGrid::trackAt(int index)
{
    if (project == nullptr || index < 0 || index >= static_cast<int>(shown().drums.size()))
        return nullptr;
    return &shown().drums[static_cast<std::size_t>(index)];
}

beatmaker::Step* StepGrid::stepAt(int t, int s)
{
    auto* d = trackAt(t);
    if (d == nullptr || s < 0 || s >= static_cast<int>(d->steps.size()))
        return nullptr;
    return &d->steps[static_cast<std::size_t>(s)];
}

void StepGrid::edited(bool layoutToo)
{
    if (layoutToo)
        resized();
    refresh();
    if (onChanged)
        onChanged();
}

void StepGrid::refresh()
{
    const bool have = project != nullptr;
    play.setToggleState(playing, juce::dontSendNotification);
    for (int i = 0; i < beatmaker::kPatternCount; ++i)
        patternButtons[static_cast<std::size_t>(i)].setToggleState(i == patternIndex, juce::dontSendNotification);
    if (have)
    {
        length.setButtonText(juce::String(shown().lengthSteps) + " STEPS");
        pageButton.setButtonText("PAGE " + juce::String(pageIndex + 1) + "/" + juce::String(pages()));
        swing.setValue(project->swing / beatmaker::kMaxSwing, false);
    }
    for (int i = 0; i < beatmaker::kMaxDrumTracks; ++i)
    {
        auto& r = rows[static_cast<std::size_t>(i)];
        const auto* t = trackAt(i);
        for (juce::Component* c : {static_cast<juce::Component*>(&r.name), static_cast<juce::Component*>(&r.sound),
                                   static_cast<juce::Component*>(&r.mute), static_cast<juce::Component*>(&r.solo)})
            c->setVisible(t != nullptr);
        if (t == nullptr)
            continue;
        r.name.setText(juce::String(i + 1) + " " + juce::String::fromUTF8(t->name.c_str()), juce::dontSendNotification);
        r.name.setColour(juce::Label::textColourId, i == track ? kOn : kText);
        int id = 0;
        for (std::size_t c = 0; c < soundChoices().size() && id == 0; ++c)
        {
            const auto& choice = soundChoices()[c];
            if (choice.synth == t->sound.synth)
                if (const auto* preset = beatmaker::findDrumPreset(dsp::drumKindFromName(choice.synth.c_str()), choice.preset))
                    if (beatmaker::drumSoundFromPreset(dsp::drumKindFromName(choice.synth.c_str()), *preset).params == t->sound.params)
                        id = static_cast<int>(c) + 1;
        }
        r.sound.setSelectedId(id, juce::dontSendNotification);
        if (id == 0)
            r.sound.setText(t->sound.kind == beatmaker::SoundRef::Kind::Sample ? "Sample" : juce::String(t->sound.synth),
                            juce::dontSendNotification);
        r.mute.setToggleState(t->mute, juce::dontSendNotification);
        r.solo.setToggleState(t->solo, juce::dontSendNotification);
    }
    repaint();
}

void StepGrid::modelChanged()
{
    if (project != nullptr)
    {
        ensureTracks();
        step = std::clamp(step, 0, shown().lengthSteps - 1);
        track = std::clamp(track, 0, std::max(0, static_cast<int>(shown().drums.size()) - 1));
        pageIndex = std::clamp(pageIndex, 0, pages() - 1);
    }
    refresh();
}

void StepGrid::setPlayback(bool isPlaying, int stepIndex)
{
    if (isPlaying == playing && stepIndex == playingStep)
        return;
    playing = isPlaying;
    playingStep = stepIndex;
    play.setToggleState(playing, juce::dontSendNotification);
    repaint(gridArea);
}

void StepGrid::selectPattern(int index)
{
    patternIndex = std::clamp(index, 0, beatmaker::kPatternCount - 1);
    pageIndex = 0;
    ensureTracks();
    step = std::min(step, shown().lengthSteps - 1);
    track = std::min(track, std::max(0, static_cast<int>(shown().drums.size()) - 1));
    refresh();
    if (onPattern)
        onPattern(patternIndex);
}

void StepGrid::moveCursor(int dTrack, int dStep)
{
    if (project == nullptr)
        return;
    const int tracks = std::max(1, static_cast<int>(shown().drums.size()));
    track = (track + dTrack + tracks) % tracks;
    const int len = shown().lengthSteps;
    step = (step + dStep + len) % len;
    pageIndex = step / kVisibleSteps; // the page follows the cursor
    refresh();
}

void StepGrid::setSound(int trackIndex, int choice)
{
    auto* t = trackAt(trackIndex);
    if (t == nullptr || choice < 0 || choice >= static_cast<int>(soundChoices().size()))
        return;
    const auto& c = soundChoices()[static_cast<std::size_t>(choice)];
    const auto kind = dsp::drumKindFromName(c.synth.c_str());
    if (const auto* preset = beatmaker::findDrumPreset(kind, c.preset))
    {
        t->sound = beatmaker::drumSoundFromPreset(kind, *preset);
        t->name = beatmaker::drumTrack(c.synth).name;
        t->chokeGroup = kind == dsp::DrumKind::ClosedHat || kind == dsp::DrumKind::OpenHat ? 1 : 0;
        edited();
    }
}

bool StepGrid::perform(GlobalAction a)
{
    if (project == nullptr)
        return false;
    auto* s = stepAt(track, step);
    auto* t = trackAt(track);
    const auto adjust = [&](auto& field, int delta, int lo, int hi)
    {
        field = static_cast<std::remove_reference_t<decltype(field)>>(std::clamp(static_cast<int>(field) + delta, lo, hi));
        edited();
    };
    const int ai = static_cast<int>(a);
    if (ai >= static_cast<int>(GA::BeatMakerTrack1) && ai <= static_cast<int>(GA::BeatMakerTrack8))
    {
        const int target = ai - static_cast<int>(GA::BeatMakerTrack1);
        if (target < static_cast<int>(shown().drums.size()))
            track = target;
        refresh();
        return true;
    }
    if (ai >= static_cast<int>(GA::BeatMakerPattern1) && ai <= static_cast<int>(GA::BeatMakerPattern8))
    {
        selectPattern(ai - static_cast<int>(GA::BeatMakerPattern1));
        return true;
    }
    switch (a)
    {
        case GA::BeatMakerStartStop:
            if (onPlayStop)
                onPlayStop();
            return true;
        case GA::BeatMakerCursorUp: moveCursor(-1, 0); return true;
        case GA::BeatMakerCursorDown: moveCursor(1, 0); return true;
        case GA::BeatMakerCursorLeft: moveCursor(0, -1); return true;
        case GA::BeatMakerCursorRight: moveCursor(0, 1); return true;
        case GA::BeatMakerToggleStep:
            if (s != nullptr)
            {
                s->on = !s->on;
                edited();
            }
            return true;
        case GA::BeatMakerVelocityDown:
            if (s != nullptr)
                adjust(s->velocity, -8, 1, 127);
            return true;
        case GA::BeatMakerVelocityUp:
            if (s != nullptr)
                adjust(s->velocity, 8, 1, 127);
            return true;
        case GA::BeatMakerProbabilityDown:
            if (s != nullptr)
                adjust(s->probability, -10, 0, 100);
            return true;
        case GA::BeatMakerProbabilityUp:
            if (s != nullptr)
                adjust(s->probability, 10, 0, 100);
            return true;
        case GA::BeatMakerMicroEarlier:
            if (s != nullptr)
                adjust(s->microTiming, -10, -50, 50);
            return true;
        case GA::BeatMakerMicroLater:
            if (s != nullptr)
                adjust(s->microTiming, 10, -50, 50);
            return true;
        case GA::BeatMakerRatchet:
            if (s != nullptr)
            {
                s->ratchet = static_cast<std::uint8_t>(s->ratchet % 4 + 1);
                edited();
            }
            return true;
        case GA::BeatMakerMute:
            if (t != nullptr)
            {
                t->mute = !t->mute;
                edited();
            }
            return true;
        case GA::BeatMakerSolo:
            if (t != nullptr)
            {
                t->solo = !t->solo;
                edited();
            }
            return true;
        case GA::BeatMakerSoundPrevious:
        case GA::BeatMakerSoundNext:
        {
            const int n = static_cast<int>(soundChoices().size());
            int current = rows[static_cast<std::size_t>(track)].sound.getSelectedId() - 1;
            if (current < 0)
                current = 0;
            setSound(track, (current + (a == GA::BeatMakerSoundNext ? 1 : -1) + n) % n);
            return true;
        }
        case GA::BeatMakerLength:
        {
            const int len = shown().lengthSteps;
            beatmaker::setPatternLength(shown(), len == 16 ? 32 : len == 32 ? 64 : 16);
            step = std::min(step, shown().lengthSteps - 1);
            pageIndex = std::min(pageIndex, pages() - 1);
            edited();
            return true;
        }
        case GA::BeatMakerPageNext:
        case GA::BeatMakerPagePrevious:
            pageIndex = (pageIndex + (a == GA::BeatMakerPageNext ? 1 : pages() - 1)) % pages();
            step = pageIndex * kVisibleSteps + step % kVisibleSteps;
            refresh();
            return true;
        case GA::BeatMakerSwingDown:
        case GA::BeatMakerSwingUp:
            project->swing = std::clamp(project->swing + (a == GA::BeatMakerSwingUp ? 0.05f : -0.05f), 0.0f,
                                        beatmaker::kMaxSwing);
            edited();
            return true;
        case GA::BeatMakerStepEditor:
            openStepEditor(track, step);
            return true;
        case GA::BeatMakerCopy:
            if (t != nullptr)
                clipboard = t->steps;
            return true;
        case GA::BeatMakerPaste:
            if (t != nullptr && !clipboard.empty())
            {
                for (std::size_t i = 0; i < t->steps.size(); ++i)
                    t->steps[i] = clipboard[i % clipboard.size()];
                edited();
            }
            return true;
        case GA::BeatMakerClear:
            if (t != nullptr)
            {
                std::fill(t->steps.begin(), t->steps.end(), beatmaker::Step{});
                edited();
            }
            return true;
        default:
            return false;
    }
}

void StepGrid::openStepEditor(int t, int s)
{
    auto* st = stepAt(t, s);
    if (st == nullptr)
        return;
    auto editor = std::make_unique<StepEditor>(*st, [this] { edited(); });
    const auto target = getScreenBounds().getTopLeft() + cellBounds(t, s).getCentre();
    if (isShowing())
        juce::CallOutBox::launchAsynchronously(std::move(editor), {target.x, target.y, 1, 1}, nullptr);
}

std::optional<StepGrid::Cell> StepGrid::cellAt(juce::Point<int> p) const
{
    if (project == nullptr || !gridArea.contains(p))
        return std::nullopt;
    const auto& pat = project->patterns[static_cast<std::size_t>(patternIndex)];
    const int rowsShown = static_cast<int>(pat.drums.size());
    const int rowH = gridArea.getHeight() / beatmaker::kMaxDrumTracks;
    const int colW = gridArea.getWidth() / kVisibleSteps;
    if (rowH <= 0 || colW <= 0)
        return std::nullopt;
    const int r = (p.y - gridArea.getY()) / rowH, c = (p.x - gridArea.getX()) / colW;
    if (r >= rowsShown || c >= kVisibleSteps)
        return std::nullopt;
    return Cell{r, pageIndex * kVisibleSteps + c};
}

juce::Rectangle<int> StepGrid::cellBounds(int t, int s) const
{
    const int rowH = gridArea.getHeight() / beatmaker::kMaxDrumTracks;
    const int colW = gridArea.getWidth() / kVisibleSteps;
    return {gridArea.getX() + (s % kVisibleSteps) * colW, gridArea.getY() + t * rowH, colW, rowH};
}

void StepGrid::pressAt(juce::Point<int> p, juce::ModifierKeys mods)
{
    ++gestures;
    drag = Drag::None;
    if (laneArea.contains(p))
    {
        drag = Drag::Lane;
        dragTo(p, mods);
        return;
    }
    const auto cell = cellAt(p);
    if (!cell)
        return;
    track = cell->track;
    step = cell->step;
    auto* s = stepAt(cell->track, cell->step);
    if (s == nullptr)
        return;
    if (mods.isPopupMenu())
    {
        refresh();
        openStepEditor(cell->track, cell->step);
        return;
    }
    if (mods.isAltDown())
    {
        drag = Drag::Velocity;
        dragTrack = cell->track;
        dragTo(p, mods);
        return;
    }
    // Click toggles; dragging on paints the same state over the steps it crosses.
    s->on = !s->on;
    paintOn = s->on;
    drag = Drag::Paint;
    dragTrack = cell->track;
    edited();
}

void StepGrid::dragTo(juce::Point<int> p, juce::ModifierKeys)
{
    if (drag == Drag::Lane)
    {
        // The velocity lane: x picks the step, y the velocity (top = 127).
        const int colW = laneArea.getWidth() / kVisibleSteps;
        if (colW <= 0)
            return;
        const int c = std::clamp((p.x - laneArea.getX()) / colW, 0, kVisibleSteps - 1);
        const float v = 1.0f - std::clamp(static_cast<float>(p.y - laneArea.getY()) / static_cast<float>(laneArea.getHeight()), 0.0f, 1.0f);
        if (auto* s = stepAt(track, pageIndex * kVisibleSteps + c))
        {
            s->velocity = static_cast<std::uint8_t>(std::clamp(juce::roundToInt(1.0f + v * 126.0f), 1, 127));
            edited();
        }
        return;
    }
    if (drag == Drag::Velocity)
    {
        // Alt-drag on a step: its height in the row sets the velocity.
        const auto cell = cellBounds(dragTrack, step);
        const float v = 1.0f - std::clamp(static_cast<float>(p.y - cell.getY()) / static_cast<float>(cell.getHeight()), 0.0f, 1.0f);
        if (auto* s = stepAt(dragTrack, step))
        {
            s->velocity = static_cast<std::uint8_t>(std::clamp(juce::roundToInt(1.0f + v * 126.0f), 1, 127));
            s->on = true;
            edited();
        }
        return;
    }
    if (drag == Drag::Paint)
    {
        const auto cell = cellAt({p.x, cellBounds(dragTrack, 0).getCentreY()}); // along the row it started on
        if (cell)
            if (auto* s = stepAt(dragTrack, cell->step); s != nullptr && s->on != paintOn)
            {
                s->on = paintOn;
                step = cell->step;
                edited();
            }
    }
}

void StepGrid::release()
{
    drag = Drag::None;
}

void StepGrid::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    pressAt(e.getPosition(), e.mods);
}

void StepGrid::mouseDrag(const juce::MouseEvent& e)
{
    dragTo(e.getPosition(), e.mods);
}

void StepGrid::mouseUp(const juce::MouseEvent&)
{
    release();
}

void StepGrid::resized()
{
    auto area = getLocalBounds().reduced(4);
    auto bar = area.removeFromTop(30);
    play.setBounds(bar.removeFromLeft(70).reduced(2));
    bar.removeFromLeft(8);
    for (auto& b : patternButtons)
        b.setBounds(bar.removeFromLeft(30).reduced(2));
    bar.removeFromLeft(8);
    length.setBounds(bar.removeFromLeft(90).reduced(2));
    pageButton.setBounds(bar.removeFromLeft(90).reduced(2));
    swing.setBounds(bar.removeFromLeft(60));
    area.removeFromTop(4);
    laneArea = area.removeFromBottom(std::min(56, area.getHeight() / 5));
    area.removeFromBottom(4);
    headerArea = area.removeFromLeft(std::min(250, area.getWidth() / 3));
    gridArea = area;
    const int rowH = gridArea.getHeight() / beatmaker::kMaxDrumTracks;
    laneArea = laneArea.withX(gridArea.getX()).withWidth(gridArea.getWidth());
    for (int i = 0; i < beatmaker::kMaxDrumTracks; ++i)
    {
        auto row = juce::Rectangle<int>(headerArea.getX(), gridArea.getY() + i * rowH, headerArea.getWidth(), rowH).reduced(1);
        auto& r = rows[static_cast<std::size_t>(i)];
        r.solo.setBounds(row.removeFromRight(26).reduced(1));
        r.mute.setBounds(row.removeFromRight(26).reduced(1));
        r.name.setBounds(row.removeFromLeft(std::min(90, row.getWidth() / 2)));
        r.sound.setBounds(row.reduced(1));
    }
}

void StepGrid::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    if (project == nullptr)
        return;
    const auto& pat = shown();
    const int beatSteps = std::max(1, pat.stepsPerBeat);
    for (int r = 0; r < static_cast<int>(pat.drums.size()); ++r)
    {
        const auto& d = pat.drums[static_cast<std::size_t>(r)];
        for (int c = 0; c < kVisibleSteps; ++c)
        {
            const int s = pageIndex * kVisibleSteps + c;
            if (s >= static_cast<int>(d.steps.size()))
                break;
            auto box = cellBounds(r, s).reduced(1);
            // Bar / beat shading.
            g.setColour(s % (4 * beatSteps) == 0 ? kBar : (s / beatSteps) % 2 == 0 ? kBeat : kPanel);
            g.fillRect(box);
            const auto& st = d.steps[static_cast<std::size_t>(s)];
            if (st.on)
            {
                g.setColour(kOn.withAlpha(0.35f + 0.65f * static_cast<float>(st.velocity) / 127.0f));
                g.fillRect(box.reduced(3));
                // Small marks: probability < 100 (a corner), ratchet (ticks), micro-timing (a line left / right).
                g.setColour(kBackground);
                if (st.probability < 100)
                    g.fillRect(box.getRight() - 9, box.getY() + 3, 6, 6);
                for (int h = 1; h < st.ratchet; ++h)
                    g.fillRect(box.getX() + 4 + h * 5, box.getBottom() - 7, 3, 3);
                if (st.microTiming != 0)
                    g.fillRect(box.getCentreX() + st.microTiming * box.getWidth() / 200, box.getY() + 3, 2, box.getHeight() - 6);
            }
            if (s == playingStep)
            {
                g.setColour(juce::Colours::white.withAlpha(0.25f));
                g.fillRect(box);
            }
            if (r == track && s == step)
            {
                g.setColour(kAccent);
                g.drawRect(box, 2);
            }
        }
    }
    // The cursor track's velocity lane.
    g.setColour(kPanel);
    g.fillRect(laneArea);
    if (const auto* d = pat.drums.empty() ? nullptr : &pat.drums[static_cast<std::size_t>(std::min<int>(track, static_cast<int>(pat.drums.size()) - 1))])
    {
        const int colW = laneArea.getWidth() / kVisibleSteps;
        for (int c = 0; c < kVisibleSteps; ++c)
        {
            const int s = pageIndex * kVisibleSteps + c;
            if (s >= static_cast<int>(d->steps.size()))
                break;
            const auto& st = d->steps[static_cast<std::size_t>(s)];
            const int h = laneArea.getHeight() * st.velocity / 127;
            g.setColour(st.on ? kOn : kDim.withAlpha(0.4f));
            g.fillRect(laneArea.getX() + c * colW + 3, laneArea.getBottom() - h, colW - 6, h);
        }
        g.setColour(kDim);
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("velocity", laneArea.reduced(4, 2), juce::Justification::topLeft);
    }
}

} // namespace devildj::ui
