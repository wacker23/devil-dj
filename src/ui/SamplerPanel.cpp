#include "ui/SamplerPanel.h"

#include "dsp/fx/ColorFx.h"

#include <cmath>

namespace devildj::ui
{

namespace
{
const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kSamplerColour(0xff3cc4b4);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kCueColour(0xfff0a030);

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kSamplerColour);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::black);
    b.setWantsKeyboardFocus(false);
}

const char* modeText(int mode)
{
    return mode == 1 ? "LOOP" : mode == 2 ? "GATE" : "ONE";
}
} // namespace

SamplerPanel::SamplerPanel(engine::AudioEngine& e)
    : engine(e)
    , vblank(this, [this] { refresh(); })
{
    for (int b = 0; b < engine::Sampler::kBanks; ++b)
    {
        auto& button = banks[static_cast<std::size_t>(b)];
        style(button);
        button.setButtonText(juce::String(b + 1));
        button.setTooltip("Sampler bank " + juce::String(b + 1) + " (in Sampler pad mode: Shift + pad 1-4)");
        button.onClick = [this, b] { setBank(b); };
        addAndMakeVisible(button);
    }
    for (int i = 0; i < engine::Sampler::kSlotsPerBank; ++i)
    {
        auto& t = tiles[static_cast<std::size_t>(i)];
        for (auto* b : {static_cast<juce::TextButton*>(&t.play), &t.mode, &t.sync})
        {
            style(*b);
            addAndMakeVisible(*b);
        }
        t.play.setButtonText(juce::String(i + 1));
        t.play.setTooltip("Play the slot (one-shot / loop: press; gate: hold). Right-click the slot to load it.");
        t.play.onHold = [this, i](bool down)
        { engine.send(engine::SamplerPad{static_cast<std::uint8_t>(slotOf(i)), !down}); };
        t.mode.setTooltip("One-shot, loop or gate");
        t.mode.onClick = [this, i]
        {
            const int now = engine.readState().sampler[static_cast<std::size_t>(slotOf(i))].mode;
            engine.send(engine::SamplerSet{static_cast<std::uint8_t>(slotOf(i)), engine::SamplerOp::Mode,
                                           static_cast<float>((now + 1) % 3)});
        };
        t.sync.setTooltip("Sync to the master tempo (time-stretched; needs the sample's BPM)");
        t.sync.onClick = [this, i]
        {
            const bool now = engine.readState().sampler[static_cast<std::size_t>(slotOf(i))].sync;
            engine.send(engine::SamplerSet{static_cast<std::uint8_t>(slotOf(i)), engine::SamplerOp::Sync,
                                           now ? 0.0f : 1.0f});
        };
        t.gain = std::make_unique<ValueControl>("", ValueControl::Style::Knob, 0.5);
        t.gain->setAccent(kSamplerColour);
        t.gain->valueText = [](double v) { return juce::String(juce::roundToInt(v * 200.0)) + " %"; };
        t.gain->onChange = [this, i](double v)
        {
            engine.send(engine::SamplerSet{static_cast<std::uint8_t>(slotOf(i)), engine::SamplerOp::Gain,
                                           static_cast<float>(2.0 * v)});
        };
        addAndMakeVisible(*t.gain);
    }
    for (auto* b : {&cue, &assign})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    cue.setColour(juce::TextButton::buttonOnColourId, kCueColour);
    cue.setTooltip("The sampler in the headphones");
    cue.onClick = [this]
    {
        const bool on = engine.readState().channels[engine::kSamplerChannel].cue;
        sendParam(engine::ChannelParam::Cue, on ? 0.0f : 1.0f);
    };
    assign.setTooltip("The sampler's crossfader side: A, THRU (not on the crossfader), B");
    assign.onClick = [this]
    {
        const int now = engine.readState().channels[engine::kSamplerChannel].crossfaderAssign;
        sendParam(engine::ChannelParam::CrossfaderAssign, static_cast<float>((now + 1) % 3));
    };
    level = std::make_unique<ValueControl>("LEVEL", ValueControl::Style::VerticalFader, 1.0);
    level->setAccent(kSamplerColour);
    level->valueText = [](double v) { return juce::String(juce::roundToInt(100.0 * v)) + " %"; };
    level->onChange = [this](double v) { sendParam(engine::ChannelParam::Fader, static_cast<float>(v)); };
    addAndMakeVisible(*level);
    filter = std::make_unique<ValueControl>("FILTER", ValueControl::Style::Knob, 0.5);
    filter->setAccent(juce::Colour(0xff4aa8e0));
    filter->onChange = [this](double v) { sendParam(engine::ChannelParam::Filter, static_cast<float>(v)); };
    addAndMakeVisible(*filter);
    setBank(0);
}

SamplerPanel::~SamplerPanel() = default;

void SamplerPanel::sendParam(engine::ChannelParam p, float value)
{
    engine.send(engine::SetParam{engine::channelParam(engine::kSamplerChannel, p), value});
}

void SamplerPanel::setSlotName(int slot, const juce::String& name)
{
    if (slot >= 0 && slot < engine::Sampler::kSlots)
        names[static_cast<std::size_t>(slot)] = name;
    repaint();
}

void SamplerPanel::setBank(int b)
{
    currentBank = std::clamp(b, 0, engine::Sampler::kBanks - 1);
    for (int i = 0; i < engine::Sampler::kBanks; ++i)
        banks[static_cast<std::size_t>(i)].setToggleState(i == currentBank, juce::dontSendNotification);
    if (onBank)
        onBank(currentBank);
    refresh();
    repaint();
}

void SamplerPanel::showSlotMenu(int tileIndex)
{
    const int slot = slotOf(tileIndex);
    juce::PopupMenu m;
    m.addSectionHeader("Sampler " + juce::String(currentBank + 1) + "-" + juce::String(tileIndex + 1));
    m.addItem("Load a file...", [this, slot] { if (onLoadFile) onLoadFile(slot); });
    m.addItem("Load the browser's selected track", [this, slot] { if (onLoadSelected) onLoadSelected(slot); });
    m.addItem("Empty the slot", engine.readState().sampler[static_cast<std::size_t>(slot)].loaded, false,
              [this, slot] { if (onClear) onClear(slot); });
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&tiles[static_cast<std::size_t>(tileIndex)].play));
}

void SamplerPanel::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < engine::Sampler::kSlotsPerBank; ++i)
        if (tiles[static_cast<std::size_t>(i)].area.contains(e.getPosition()))
        {
            showSlotMenu(i);
            return;
        }
}

void SamplerPanel::refresh()
{
    const auto& s = engine.readState();
    for (int i = 0; i < engine::Sampler::kSlotsPerBank; ++i)
    {
        auto& t = tiles[static_cast<std::size_t>(i)];
        const auto& st = s.sampler[static_cast<std::size_t>(slotOf(i))];
        t.play.setToggleState(st.playing, juce::dontSendNotification);
        t.play.setEnabled(st.loaded);
        t.mode.setButtonText(modeText(st.mode));
        t.mode.setEnabled(st.loaded);
        t.sync.setToggleState(st.sync, juce::dontSendNotification);
        t.sync.setEnabled(st.loaded && st.bpm > 0.0f);
        if (!t.gain->isBeingChanged() && std::abs(t.gain->getValue() - st.gain / 2.0) > 1e-4)
            t.gain->setValue(st.gain / 2.0, false);
    }
    const auto& ch = s.channels[engine::kSamplerChannel];
    cue.setToggleState(ch.cue, juce::dontSendNotification);
    assign.setButtonText(ch.crossfaderAssign == 0 ? "A" : ch.crossfaderAssign == 2 ? "B" : "THRU");
    if (!level->isBeingChanged())
        level->setValue(ch.fader, false);
    if (!filter->isBeingChanged())
        filter->setValue(ch.filter, false);
    filter->setLabel(juce::String(dsp::fx::ColorFx::typeName(static_cast<dsp::fx::ColorFx::Type>(s.colorFx))).toUpperCase());
    repaint();
}

void SamplerPanel::resized()
{
    auto area = getLocalBounds().reduced(6);
    // Right: the channel.
    auto channel = area.removeFromRight(150);
    level->setBounds(channel.removeFromRight(40));
    channel.removeFromRight(4);
    auto buttons = channel.removeFromRight(48);
    cue.setBounds(buttons.removeFromTop(buttons.getHeight() / 2).reduced(0, 2));
    assign.setBounds(buttons.reduced(0, 2));
    filter->setBounds(channel.withSizeKeepingCentre(std::min(channel.getWidth(), 56), std::min(channel.getHeight(), 60)));
    area.removeFromRight(8);
    // Left: the banks.
    auto bankColumn = area.removeFromLeft(28);
    const int bh = bankColumn.getHeight() / engine::Sampler::kBanks;
    for (auto& b : banks)
        b.setBounds(bankColumn.removeFromTop(bh).reduced(0, 1));
    area.removeFromLeft(6);
    // The slots: 8 tiles in a row.
    const int w = area.getWidth() / engine::Sampler::kSlotsPerBank;
    for (int i = 0; i < engine::Sampler::kSlotsPerBank; ++i)
    {
        auto& t = tiles[static_cast<std::size_t>(i)];
        t.area = area.withX(area.getX() + i * w).withWidth(w - 4);
        auto tile = t.area.reduced(2);
        tile.removeFromTop(14); // the name
        auto bottom = tile.removeFromBottom(20);
        t.mode.setBounds(bottom.removeFromLeft(bottom.getWidth() / 2).reduced(1, 0));
        t.sync.setBounds(bottom.reduced(1, 0));
        tile.removeFromBottom(2);
        t.gain->setBounds(tile.removeFromRight(std::min(36, tile.getWidth() / 2)));
        t.play.setBounds(tile.reduced(1));
    }
}

void SamplerPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    const auto& s = engine.readState();
    g.setFont(juce::FontOptions(10.0f));
    for (int i = 0; i < engine::Sampler::kSlotsPerBank; ++i)
    {
        const auto& t = tiles[static_cast<std::size_t>(i)];
        const int slot = slotOf(i);
        const auto& st = s.sampler[static_cast<std::size_t>(slot)];
        g.setColour(kPanel.brighter(0.04f));
        g.fillRoundedRectangle(t.area.toFloat(), 4.0f);
        if (st.playing && st.progress >= 0.0f) // how far through it is
        {
            g.setColour(kSamplerColour.withAlpha(0.25f));
            g.fillRect(t.area.withHeight(3).withWidth(static_cast<int>(t.area.getWidth() * st.progress)).toFloat());
        }
        const auto& name = names[static_cast<std::size_t>(slot)];
        g.setColour(st.loaded ? kText : kDim);
        juce::String label = st.loaded ? (name.isNotEmpty() ? name : juce::String("sample")) : juce::String("empty");
        if (st.loaded && st.bpm > 0.0f)
            label << "  " << juce::String(st.bpm, 1);
        g.drawText(label, t.area.reduced(4, 1).withHeight(14), juce::Justification::centredLeft, true);
    }
}

} // namespace devildj::ui
