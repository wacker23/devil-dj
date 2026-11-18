#include "MixerView.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kAccent(0xffe0483c);
const juce::Colour kCueColour(0xfff0a030);
const juce::Colour kText(0xffd8d8e0);
constexpr double kKill = -1000.0;

juce::String dbText(double db)
{
    if (db <= -100.0)
        return db <= kKill ? "kill" : "-inf";
    return (db > 0.0 ? "+" : "") + juce::String(db, 1) + " dB";
}
} // namespace

// ---- mappings ----

double MixerView::trimDb(double knob) noexcept
{
    return -12.0 + 24.0 * std::clamp(knob, 0.0, 1.0);
}

double MixerView::trimKnob(double db) noexcept
{
    return std::clamp((db + 12.0) / 24.0, 0.0, 1.0);
}

double MixerView::eqDb(double knob, bool isolator) noexcept
{
    knob = std::clamp(knob, 0.0, 1.0);
    if (knob >= 0.5)
        return 6.0 * (2.0 * knob - 1.0);
    if (isolator) // left half: gain falls linearly to a full kill
        return knob <= 0.0 ? kKill : 20.0 * std::log10(2.0 * knob);
    return -26.0 * (1.0 - 2.0 * knob);
}

double MixerView::eqKnob(double db, bool isolator) noexcept
{
    if (db >= 0.0)
        return std::clamp(0.5 + db / 12.0, 0.5, 1.0);
    if (isolator)
        return std::isinf(db) || db <= -100.0 ? 0.0 : std::clamp(0.5 * std::pow(10.0, db / 20.0), 0.0, 0.5);
    return std::isinf(db) ? 0.0 : std::clamp(0.5 * (1.0 + db / 26.0), 0.0, 0.5);
}

double MixerView::levelDb(double knob) noexcept
{
    knob = std::clamp(knob, 0.0, 1.0);
    if (knob >= 0.5)
        return 6.0 * (2.0 * knob - 1.0);
    return knob <= 0.0 ? -100.0 : 20.0 * std::log10(2.0 * knob);
}

double MixerView::levelKnob(double db) noexcept
{
    if (db >= 0.0)
        return std::clamp(0.5 + db / 12.0, 0.5, 1.0);
    return db <= -100.0 ? 0.0 : std::clamp(0.5 * std::pow(10.0, db / 20.0), 0.0, 0.5);
}

// ---- view ----

MixerView::MixerView(engine::AudioEngine& audioEngine)
    : engine(audioEngine)
    , vblank(this, [this] { refresh(); })
{
    using S = ValueControl::Style;
    using P = engine::ChannelParam;
    for (int ch = 0; ch < kChannels; ++ch)
    {
        auto& c = channels[static_cast<std::size_t>(ch)];
        const auto param = [ch](P p) { return engine::channelParam(ch, p); };
        const auto isolator = [this, ch]
        { return engine.readState().channels[static_cast<std::size_t>(ch)].eqMode == 1; };

        c.trim = std::make_unique<ValueControl>("TRIM", S::Knob, 0.5);
        c.trim->onChange = [this, param](double v) { send(param(P::Trim), trimDb(v)); };
        c.trim->valueText = [](double v) { return dbText(trimDb(v)); };

        const std::pair<std::unique_ptr<ValueControl>*, P> bands[] = {
            {&c.hi, P::EqHigh}, {&c.mid, P::EqMid}, {&c.low, P::EqLow}};
        const char* const names[] = {"HI", "MID", "LOW"};
        for (int b = 0; b < 3; ++b)
        {
            auto& knob = *bands[b].first;
            const auto which = bands[b].second;
            knob = std::make_unique<ValueControl>(names[b], S::Knob, 0.5);
            knob->onChange = [this, param, which, isolator](double v) { send(param(which), eqDb(v, isolator())); };
            knob->valueText = [isolator](double v) { return dbText(eqDb(v, isolator())); };
        }

        c.filter = std::make_unique<ValueControl>("FILTER", S::Knob, 0.5);
        c.filter->setAccent(juce::Colour(0xff4aa8e0));
        c.filter->onChange = [this, param](double v) { send(param(P::Filter), v); };
        c.filter->valueText = [](double v)
        {
            const auto t = engine::ChannelStrip::filterTarget(v);
            if (t.dry >= 1.0)
                return juce::String("off");
            return t.lowPass > 0.0 ? "LP " + juce::String(juce::roundToInt(t.lowPassHz)) + " Hz"
                                   : "HP " + juce::String(juce::roundToInt(t.highPassHz)) + " Hz";
        };

        c.fader = std::make_unique<ValueControl>(juce::String::charToString(static_cast<juce::juce_wchar>(
                                                     engine::deckLetter(static_cast<engine::DeckId>(ch)))),
                                                 S::VerticalFader, 1.0);
        c.fader->onChange = [this, param](double v) { send(param(P::Fader), v); };
        c.fader->valueText = [](double v) { return juce::String(juce::roundToInt(100.0 * v)) + " %"; };

        c.cue = std::make_unique<juce::TextButton>("CUE");
        c.cue->setClickingTogglesState(false);
        c.cue->setColour(juce::TextButton::buttonColourId, kPanel);
        c.cue->setColour(juce::TextButton::buttonOnColourId, kCueColour);
        c.cue->setColour(juce::TextButton::textColourOnId, juce::Colours::black);
        c.cue->setWantsKeyboardFocus(false);
        c.cue->setTooltip("Headphone cue (Ctrl+E / Ctrl+O)");
        c.cue->onClick = [this, param, ch]
        {
            const bool on = engine.readState().channels[static_cast<std::size_t>(ch)].cue;
            send(param(P::Cue), on ? 0.0 : 1.0);
        };

        c.meter = std::make_unique<LevelMeterView>(
            [this, ch]
            {
                const auto& s = engine.readState().channels[static_cast<std::size_t>(ch)];
                return std::pair<float, float>{s.peakL, s.peakR};
            });

        for (auto* comp :
             std::initializer_list<juce::Component*>{c.trim.get(), c.hi.get(), c.mid.get(), c.low.get(), c.filter.get(),
                                                     c.fader.get(), c.cue.get(), c.meter.get()})
            addAndMakeVisible(comp);
    }

    xfader = std::make_unique<ValueControl>("", S::HorizontalFader, 0.5);
    xfader->onChange = [this](double v) { send(engine::ParamId::Crossfader, v); };
    masterLevel = std::make_unique<ValueControl>("MASTER", S::Knob, 0.5);
    masterLevel->onChange = [this](double v) { send(engine::ParamId::MasterGain, levelDb(v)); };
    masterLevel->valueText = [](double v) { return dbText(levelDb(v)); };
    cueMix = std::make_unique<ValueControl>("CUE/MST", S::Knob, 0.5);
    cueMix->setAccent(kCueColour);
    cueMix->onChange = [this](double v) { send(engine::ParamId::CueMix, v); };
    cueMix->valueText = [](double v)
    {
        return v <= 0.0 ? juce::String("cue") : v >= 1.0 ? juce::String("master") : juce::String(v, 2);
    };
    phonesLevel = std::make_unique<ValueControl>("PHONES", S::Knob, 0.5);
    phonesLevel->setAccent(kCueColour);
    phonesLevel->onChange = [this](double v) { send(engine::ParamId::HeadphoneLevel, levelDb(v)); };
    phonesLevel->valueText = [](double v) { return dbText(levelDb(v)); };
    masterMeter = std::make_unique<LevelMeterView>(
        [this]
        {
            const auto& s = engine.readState();
            return std::pair<float, float>{s.masterPeakL, s.masterPeakR};
        });
    for (auto* comp : std::initializer_list<juce::Component*>{xfader.get(), masterLevel.get(), cueMix.get(),
                                                              phonesLevel.get(), masterMeter.get()})
        addAndMakeVisible(comp);
    refresh();
}

MixerView::~MixerView() = default;

void MixerView::send(engine::ParamId id, double value)
{
    engine.send(engine::SetParam{id, static_cast<float>(value)});
}

void MixerView::show(ValueControl& c, double v)
{
    if (!c.isBeingChanged())
        c.setValue(v, false);
}

void MixerView::refresh()
{
    const auto& s = engine.readState();
    for (int ch = 0; ch < kChannels; ++ch)
    {
        auto& c = channels[static_cast<std::size_t>(ch)];
        const auto& st = s.channels[static_cast<std::size_t>(ch)];
        const bool iso = st.eqMode == 1;
        show(*c.trim, trimKnob(st.trimDb));
        show(*c.hi, eqKnob(st.eqDb[2], iso));
        show(*c.mid, eqKnob(st.eqDb[1], iso));
        show(*c.low, eqKnob(st.eqDb[0], iso));
        show(*c.filter, st.filter);
        show(*c.fader, st.fader);
        c.cue->setToggleState(st.cue, juce::dontSendNotification);
        c.meter->repaint();
    }
    show(*xfader, s.crossfader);
    show(*masterLevel, levelKnob(s.masterGainDb));
    show(*cueMix, s.cueMix);
    show(*phonesLevel, levelKnob(s.headphoneLevelDb));
    masterMeter->repaint();
    repaint(headers[0]);
    repaint(headers[1]);
}

void MixerView::resized()
{
    auto area = getLocalBounds().reduced(8);
    auto bottom = area.removeFromBottom(34);
    xfader->setBounds(bottom.withSizeKeepingCentre(std::min(240, bottom.getWidth()), 30));
    area.removeFromBottom(6);

    const int column = area.getWidth() / 3;
    for (int ch = 0; ch < kChannels; ++ch)
    {
        auto col = ch == 0 ? area.withWidth(column) : area.withLeft(area.getRight() - column);
        auto& c = channels[static_cast<std::size_t>(ch)];
        headers[static_cast<std::size_t>(ch)] = col.removeFromTop(14);
        for (auto* knob : {c.trim.get(), c.hi.get(), c.mid.get(), c.low.get(), c.filter.get()})
            knob->setBounds(col.removeFromTop(42).withSizeKeepingCentre(std::min(100, col.getWidth()), 40));
        col.removeFromTop(4);
        c.cue->setBounds(col.removeFromTop(24).reduced(10, 0));
        col.removeFromTop(4);
        // Fader with the channel meter on the side facing the middle.
        auto faderArea = col;
        auto meterArea = ch == 0 ? faderArea.removeFromRight(12) : faderArea.removeFromLeft(12);
        c.meter->setBounds(meterArea.withTrimmedBottom(13).reduced(1, 10));
        c.fader->setBounds(faderArea.withSizeKeepingCentre(48, faderArea.getHeight()));
    }

    auto centre = area.withTrimmedLeft(column).withWidth(area.getWidth() - 2 * column);
    centre.removeFromTop(14);
    masterLevel->setBounds(centre.removeFromTop(48).withSizeKeepingCentre(56, 48));
    auto knobsBottom = centre.removeFromBottom(100);
    phonesLevel->setBounds(knobsBottom.removeFromBottom(48).withSizeKeepingCentre(56, 48));
    cueMix->setBounds(knobsBottom.removeFromBottom(48).withSizeKeepingCentre(56, 48));
    centre.removeFromBottom(16); // room for the "phones not routed" note
    masterMeter->setBounds(centre.reduced(0, 6).withSizeKeepingCentre(18, centre.getHeight() - 12));
}

void MixerView::paint(juce::Graphics& g)
{
    g.setColour(kBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
    const auto& s = engine.readState();
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    for (int ch = 0; ch < kChannels; ++ch)
    {
        const bool iso = s.channels[static_cast<std::size_t>(ch)].eqMode == 1;
        g.setColour(kText);
        g.drawText(juce::String("CH ") + engine::deckLetter(static_cast<engine::DeckId>(ch)) + (iso ? "  ISO" : "  EQ"),
                   headers[static_cast<std::size_t>(ch)], juce::Justification::centred);
    }
    g.setColour(kText.withAlpha(0.6f));
    g.setFont(juce::FontOptions(10.0f));
    const auto xf = xfader->getBounds();
    g.drawText("A", xf.withWidth(14).translated(-16, 0), juce::Justification::centred);
    g.drawText("B", xf.withX(xf.getRight() + 2).withWidth(14), juce::Justification::centred);
    if (!s.cueRouted)
    {
        g.setColour(kCueColour.withAlpha(0.7f));
        g.drawText("phones not routed", cueMix->getBounds().translated(0, -15).withHeight(12).expanded(40, 0),
                   juce::Justification::centred);
    }
}

} // namespace devildj::ui
