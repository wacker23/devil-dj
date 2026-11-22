#include "ui/FxPanel.h"

#include <algorithm>
#include <cmath>

#include "dsp/fx/ReleaseFx.h"

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kFxColour(0xff9a5ce0);
const juce::Colour kText(0xffe8e8ee);

void styleButton(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kFxColour);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    b.setWantsKeyboardFocus(false);
}
} // namespace

FxPanel::FxPanel(engine::AudioEngine& e)
    : engine(e)
    , vblank(this, [this] { refresh(); })
{
    for (int t = 0; t < dsp::fx::kEffectTypeCount; ++t)
        if (auto fx = dsp::fx::makeEffect(static_cast<dsp::fx::EffectType>(t)))
            for (int i = 0; i < fx->numParams(); ++i)
                params[static_cast<std::size_t>(t)].push_back(fx->param(i));
    for (auto& row : types)
        row.fill(-1);

    for (int u = 0; u < engine::kFxUnits; ++u)
    {
        auto& unitView = units[static_cast<std::size_t>(u)];
        for (auto* b : {&unitView.target, &unitView.mode, &unitView.beatsDown, &unitView.beatsUp, &unitView.beats,
                        &unitView.tapButton})
        {
            styleButton(*b);
            addAndMakeVisible(*b);
        }
        unitView.beatsDown.setButtonText(juce::CharPointer_UTF8("\xe2\x97\x80"));
        unitView.beatsUp.setButtonText(juce::CharPointer_UTF8("\xe2\x96\xb6"));
        unitView.target.setTooltip("Where the unit is: deck A, deck B or the master (Shift+F5 / Shift+F7 cycle)");
        unitView.mode.setTooltip("Multi: three effects in a row, one knob each; single: one effect, all its knobs");
        unitView.beats.setTooltip("Beat length of the unit's effects (1/16 ... 16 beats)");
        unitView.tapButton.setTooltip("Tap the effects' tempo (instead of the master clock); right-click: the clock");
        unitView.target.onClick = [this, u] { showTargetMenu(u); };
        unitView.mode.onClick = [this, u]
        {
            single[static_cast<std::size_t>(u)] = !single[static_cast<std::size_t>(u)];
            sendFx(u, engine::FxOp::Mode, 0, 0, single[static_cast<std::size_t>(u)] ? 1.0f : 0.0f);
            resized();
        };
        unitView.beatsDown.onClick = [this, u]
        {
            const auto& steps = kBeatSteps;
            auto it = std::lower_bound(steps.begin(), steps.end(), beats[static_cast<std::size_t>(u)] - 1e-9);
            setBeats(u, it == steps.begin() ? steps.front() : *(it - 1));
        };
        unitView.beatsUp.onClick = [this, u]
        {
            const auto& steps = kBeatSteps;
            auto it = std::upper_bound(steps.begin(), steps.end(), beats[static_cast<std::size_t>(u)] + 1e-9);
            setBeats(u, it == steps.end() ? steps.back() : *it);
        };
        unitView.beats.onClick = [this, u] { setBeats(u, 1.0); };
        static constexpr const char* releaseNames[] = {"ECHO OUT", "BRAKE", "SPIN", "SWEEP"};
        for (int k = 0; k < 4; ++k)
        {
            auto& b = unitView.release[static_cast<std::size_t>(k)];
            styleButton(b);
            b.setButtonText(releaseNames[k]);
            b.setColour(juce::TextButton::buttonColourId, kPanel.interpolatedWith(juce::Colour(0xffe08a3c), 0.15f));
            b.setTooltip(juce::String("Release FX: hold for ") +
                         dsp::fx::ReleaseFx::kindName(static_cast<dsp::fx::ReleaseFx::Kind>(k)) +
                         " on the unit's deck; let go and the track ends on it over the beat length (then pauses)");
            b.onHold = [this, u, k](bool down) { releaseFx(u, k, down); };
            addAndMakeVisible(b);
        }
        unitView.tapButton.onClick = [this, u]
        {
            if (juce::ModifierKeys::getCurrentModifiers().isPopupMenu()) // right-click: the master clock again
            {
                units[static_cast<std::size_t>(u)].taps.clear();
                sendFx(u, engine::FxOp::TapBpm, 0, 0, 0.0f);
                return;
            }
            tap(u, juce::Time::getMillisecondCounterHiRes() / 1000.0);
        };
        for (int s = 0; s < engine::kFxSlots; ++s)
        {
            auto& slot = unitView.slots[static_cast<std::size_t>(s)];
            styleButton(slot.effect);
            styleButton(slot.on);
            slot.effect.setButtonText("-");
            slot.effect.setTooltip("The effect in this slot (click to choose)");
            slot.on.setTooltip("Effect on / off (F5 / F7: the whole unit)");
            slot.effect.onClick = [this, u, s] { showEffectMenu(u, s); };
            slot.on.onClick = [this, u, s]
            {
                const bool on = engine.readState().fx[static_cast<std::size_t>(u)].slots[static_cast<std::size_t>(s)].on;
                sendFx(u, engine::FxOp::On, s, 0, on ? 0.0f : 1.0f);
            };
            addAndMakeVisible(slot.effect);
            addAndMakeVisible(slot.on);
            slot.wet = std::make_unique<ValueControl>("WET", ValueControl::Style::Knob, 0.5);
            slot.wet->setAccent(kFxColour);
            slot.wet->valueText = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
            slot.wet->onChange = [this, u, s](double v) { sendFx(u, engine::FxOp::Wet, s, 0, static_cast<float>(v)); };
            addAndMakeVisible(*slot.wet);
            for (int k = 0; k < 3; ++k)
            {
                auto& knob = slot.knobs[static_cast<std::size_t>(k)];
                knob = std::make_unique<ValueControl>("", ValueControl::Style::Knob, 0.5);
                knob->setAccent(kFxColour);
                knob->valueText = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + " %"; };
                knob->onChange = [this, u, s, k](double v)
                { sendFx(u, engine::FxOp::Param, s, k, static_cast<float>(v)); };
                addChildComponent(*knob);
            }
        }
    }
    refresh();
}

FxPanel::~FxPanel() = default;

void FxPanel::sendFx(int unit, engine::FxOp op, int slot, int index, float value)
{
    engine.send(engine::FxCommand{static_cast<std::uint8_t>(unit), op, static_cast<std::uint8_t>(slot),
                                  static_cast<std::uint8_t>(index), value});
}

juce::String FxPanel::beatsText(double beats)
{
    if (beats < 1.0)
        return "1/" + juce::String(juce::roundToInt(1.0 / beats));
    return juce::String(juce::roundToInt(beats));
}

juce::String FxPanel::targetText(engine::FxTarget target)
{
    switch (target)
    {
        case engine::FxTarget::Deck0:
            return "DECK A";
        case engine::FxTarget::Deck1:
            return "DECK B";
        case engine::FxTarget::Deck2:
            return "DECK C";
        case engine::FxTarget::Deck3:
            return "DECK D";
        case engine::FxTarget::Sampler:
            return "SAMPLER";
        case engine::FxTarget::CodeDeck:
            return "CODE";
        case engine::FxTarget::Master:
            return "MASTER";
    }
    return "";
}

void FxPanel::setEffectType(int unit, int slot, int type)
{
    if (unit < 0 || unit >= engine::kFxUnits || slot < 0 || slot >= engine::kFxSlots)
        return;
    types[static_cast<std::size_t>(unit)][static_cast<std::size_t>(slot)] = type;
    auto& s = units[static_cast<std::size_t>(unit)].slots[static_cast<std::size_t>(slot)];
    s.effect.setButtonText(type >= 0 ? dsp::fx::effectTypeName(static_cast<dsp::fx::EffectType>(type)) : "-");
    const auto* info = type >= 0 ? &params[static_cast<std::size_t>(type)] : nullptr;
    for (std::size_t k = 0; k < 3; ++k)
    {
        auto& knob = *s.knobs[k];
        const bool has = info != nullptr && k < info->size();
        knob.setTooltip(has ? juce::String((*info)[k].name) : juce::String());
        knob.setValue(has ? (*info)[k].defaultValue : 0.5, false);
    }
    resized();
}

void FxPanel::releaseFx(int unit, int kind, bool down)
{
    const auto target = engine.readState().fx[static_cast<std::size_t>(unit)].target;
    if (static_cast<int>(target) > static_cast<int>(engine::FxTarget::Deck3))
        return; // on a deck's channel only
    engine.send(engine::ReleaseFxCommand{static_cast<engine::DeckId>(target), static_cast<std::uint8_t>(kind), down,
                                         static_cast<float>(beats[static_cast<std::size_t>(unit)])});
}

void FxPanel::toggleUnit(int unit)
{
    const auto& st = engine.readState().fx[static_cast<std::size_t>(unit)];
    const bool anyOn = std::any_of(st.slots.begin(), st.slots.end(), [](const auto& s) { return s.on; });
    for (int s = 0; s < engine::kFxSlots; ++s)
        if (types[static_cast<std::size_t>(unit)][static_cast<std::size_t>(s)] >= 0 || anyOn)
            sendFx(unit, engine::FxOp::On, s, 0, anyOn ? 0.0f : 1.0f);
}

void FxPanel::nextEffect(int unit)
{
    const int current = types[static_cast<std::size_t>(unit)][0];
    const int next = (current + 1) % dsp::fx::kEffectTypeCount;
    if (onEffect)
        onEffect(unit, 0, next);
}

void FxPanel::cycleTarget(int unit)
{
    const auto now = engine.readState().fx[static_cast<std::size_t>(unit)].target;
    const auto next = now == engine::FxTarget::Deck0   ? engine::FxTarget::Deck1
                      : now == engine::FxTarget::Deck1 ? engine::FxTarget::Master
                                                       : engine::FxTarget::Deck0;
    sendFx(unit, engine::FxOp::Target, 0, 0, static_cast<float>(next));
}

void FxPanel::setBeats(int unit, double b)
{
    beats[static_cast<std::size_t>(unit)] = std::clamp(b, kBeatSteps.front(), kBeatSteps.back());
    for (int s = 0; s < engine::kFxSlots; ++s)
        sendFx(unit, engine::FxOp::Beats, s, 0, static_cast<float>(beats[static_cast<std::size_t>(unit)]));
    units[static_cast<std::size_t>(unit)].beats.setButtonText(beatsText(beats[static_cast<std::size_t>(unit)]));
}

void FxPanel::tap(int unit, double timeSeconds)
{
    auto& taps = units[static_cast<std::size_t>(unit)].taps;
    if (!taps.empty() && timeSeconds - taps.back() > 2.0) // a pause: start again
        taps.clear();
    taps.push_back(timeSeconds);
    if (taps.size() > 5)
        taps.erase(taps.begin());
    if (taps.size() < 2)
        return;
    const double interval = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
    if (interval > 0.0)
        sendFx(unit, engine::FxOp::TapBpm, 0, 0, static_cast<float>(60.0 / interval));
}

void FxPanel::showEffectMenu(int unit, int slot)
{
    juce::PopupMenu m;
    m.addSectionHeader("FX" + juce::String(unit + 1) + " slot " + juce::String(slot + 1));
    const int current = types[static_cast<std::size_t>(unit)][static_cast<std::size_t>(slot)];
    for (int t = 0; t < dsp::fx::kEffectTypeCount; ++t)
        m.addItem(dsp::fx::effectTypeName(static_cast<dsp::fx::EffectType>(t)), true, current == t,
                  [this, unit, slot, t]
                  {
                      if (onEffect)
                          onEffect(unit, slot, t);
                  });
    m.addSeparator();
    m.addItem("None", true, current < 0,
              [this, unit, slot]
              {
                  if (onEffect)
                      onEffect(unit, slot, -1);
              });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(
        &units[static_cast<std::size_t>(unit)].slots[static_cast<std::size_t>(slot)].effect));
}

void FxPanel::showTargetMenu(int unit)
{
    juce::PopupMenu m;
    const auto now = engine.readState().fx[static_cast<std::size_t>(unit)].target;
    for (const auto t : {engine::FxTarget::Deck0, engine::FxTarget::Deck1, engine::FxTarget::Master})
        m.addItem(targetText(t), true, now == t,
                  [this, unit, t] { sendFx(unit, engine::FxOp::Target, 0, 0, static_cast<float>(t)); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&units[static_cast<std::size_t>(unit)].target));
}

void FxPanel::refresh()
{
    const auto& state = engine.readState();
    for (int u = 0; u < engine::kFxUnits; ++u)
    {
        auto& view = units[static_cast<std::size_t>(u)];
        const auto& st = state.fx[static_cast<std::size_t>(u)];
        const auto target = "FX" + juce::String(u + 1) + ": " + targetText(st.target);
        if (view.target.getButtonText() != target)
            view.target.setButtonText(target);
        view.mode.setButtonText(st.mode == 1 ? "SINGLE" : "MULTI");
        const auto tapText = st.manualBpm > 0.0f ? juce::String(st.manualBpm, 1) : juce::String("TAP");
        if (view.tapButton.getButtonText() != tapText)
            view.tapButton.setButtonText(tapText);
        view.tapButton.setToggleState(st.manualBpm > 0.0f, juce::dontSendNotification);
        for (int s = 0; s < engine::kFxSlots; ++s)
        {
            auto& slot = view.slots[static_cast<std::size_t>(s)];
            const auto& ss = st.slots[static_cast<std::size_t>(s)];
            slot.on.setToggleState(ss.on, juce::dontSendNotification);
            // A slot ringing out after it was switched off: the button half lit.
            slot.on.setColour(juce::TextButton::buttonColourId,
                              ss.stage >= 2 ? kPanel.interpolatedWith(kFxColour, 0.4f) : kPanel);
            slot.on.setEnabled(ss.hasEffect);
            if (!slot.wet->isBeingChanged() && std::abs(slot.wet->getValue() - ss.wet) > 1e-4)
                slot.wet->setValue(ss.wet, false);
        }
        // Release FX buttons: lit while held / ending on the unit's deck.
        const bool onDeck = static_cast<int>(st.target) <= static_cast<int>(engine::FxTarget::Deck3);
        const auto& deckState = state.decks[static_cast<std::size_t>(onDeck ? static_cast<int>(st.target) : 0)];
        for (int k = 0; k < 4; ++k)
        {
            auto& b = view.release[static_cast<std::size_t>(k)];
            b.setEnabled(onDeck);
            b.setToggleState(onDeck && deckState.releaseFx != 0 && deckState.releaseFxKind == k,
                             juce::dontSendNotification);
        }
    }
}

void FxPanel::layoutUnit(Unit& u, juce::Rectangle<int> area, bool singleMode)
{
    auto top = area.removeFromTop(22);
    u.target.setBounds(top.removeFromLeft(104));
    top.removeFromLeft(4);
    u.mode.setBounds(top.removeFromLeft(58));
    top.removeFromLeft(4);
    u.tapButton.setBounds(top.removeFromRight(52));
    top.removeFromRight(4);
    {
        // Release FX: as much room as is left, four buttons.
        auto row = top.removeFromRight(std::min(4 * 62, std::max(0, top.getWidth() - 120 - 8)));
        const int w = row.getWidth() / 4;
        for (auto& b : u.release)
        {
            b.setBounds(row.removeFromLeft(w).reduced(1, 0));
            b.setVisible(w >= 30);
        }
    }
    top.removeFromRight(4);
    u.beatsUp.setBounds(top.removeFromRight(22));
    u.beats.setBounds(top.removeFromRight(40));
    u.beatsDown.setBounds(top.removeFromRight(22));
    area.removeFromTop(4);

    const int columns = singleMode ? 1 : engine::kFxSlots;
    const int width = area.getWidth() / columns;
    for (int s = 0; s < engine::kFxSlots; ++s)
    {
        auto& slot = u.slots[static_cast<std::size_t>(s)];
        const bool shown = s < columns;
        slot.effect.setVisible(shown);
        slot.on.setVisible(shown);
        slot.wet->setVisible(shown);
        const int type = types[static_cast<std::size_t>(&u - units.data())][static_cast<std::size_t>(s)];
        const int knobCount = !shown || type < 0 ? 0
                              : singleMode       ? static_cast<int>(params[static_cast<std::size_t>(type)].size())
                                                 : std::min<int>(1, static_cast<int>(params[static_cast<std::size_t>(type)].size()));
        for (int k = 0; k < 3; ++k)
            slot.knobs[static_cast<std::size_t>(k)]->setVisible(k < knobCount);
        if (!shown)
            continue;
        auto col = area.withX(area.getX() + s * width).withWidth(width - 6);
        auto buttons = col.removeFromLeft(std::min(96, col.getWidth() / 2));
        slot.effect.setBounds(buttons.removeFromTop(buttons.getHeight() / 2).reduced(0, 1));
        slot.on.setBounds(buttons.reduced(0, 1));
        col.removeFromLeft(4);
        const int knob = std::min(col.getHeight(), 48);
        slot.wet->setBounds(col.removeFromLeft(knob));
        for (int k = 0; k < knobCount; ++k)
            slot.knobs[static_cast<std::size_t>(k)]->setBounds(col.removeFromLeft(knob));
    }
}

void FxPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    const int half = (area.getWidth() - 12) / 2;
    unitAreas[0] = area.removeFromLeft(half);
    area.removeFromLeft(12);
    unitAreas[1] = area;
    for (int u = 0; u < engine::kFxUnits; ++u)
        layoutUnit(units[static_cast<std::size_t>(u)], unitAreas[static_cast<std::size_t>(u)],
                   single[static_cast<std::size_t>(u)]);
}

void FxPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel.brighter(0.05f));
    for (const auto& a : unitAreas)
        g.drawRoundedRectangle(a.toFloat().expanded(3.0f), 4.0f, 1.0f);
}

} // namespace devildj::ui
