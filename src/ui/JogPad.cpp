#include "JogPad.h"

#include <cmath>

namespace devildj::ui
{

JogPad::JogPad(engine::AudioEngine& audioEngine, engine::DeckId deckId)
    : engine(audioEngine)
    , deck(deckId)
    , vblank(this, [this] { repaint(); })
{
    setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
}

double JogPad::samplesPerSecond() const
{
    const double rate = engine.readState().sampleRate;
    return rate > 0.0 ? rate : 48000.0;
}

double JogPad::uiClockSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() / 1000.0;
}

void JogPad::mouseDown(const juce::MouseEvent& e)
{
    held = true;
    lastX = e.position.x;
    engine.send(engine::ScratchBegin{deck, uiClockSeconds()});
}

void JogPad::mouseDrag(const juce::MouseEvent& e)
{
    const float dx = e.position.x - lastX;
    lastX = e.position.x;
    if (dx != 0.0f) // grab-and-move: dragging left moves the track forward
        engine.send(engine::ScratchMove{deck, -dx * kSecondsPerPixel * samplesPerSecond(), uiClockSeconds()});
}

void JogPad::mouseUp(const juce::MouseEvent&)
{
    held = false;
    engine.send(engine::ScratchEnd{deck});
}

void JogPad::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    // JUCE reports about 0.25 per wheel notch; up / right = forward.
    const float amount = std::abs(wheel.deltaY) >= std::abs(wheel.deltaX) ? wheel.deltaY : wheel.deltaX;
    const double notches = (wheel.isReversed ? -amount : amount) / 0.25;
    if (notches != 0.0)
        engine.send(engine::Nudge{deck, notches * kNudgeSecondsPerNotch * samplesPerSecond()});
}

void JogPad::paint(juce::Graphics& g)
{
    const auto& s = engine.readState().decks[deck];
    const auto area = getLocalBounds().toFloat().reduced(2.0f);

    g.setColour(juce::Colour(0xff1c1c22));
    g.fillRoundedRectangle(area, 8.0f);

    // Stripes that move with the track, so scratching and nudging are visible.
    if (s.loaded)
    {
        const double pxPerSample = 1.0 / (kSecondsPerPixel * samplesPerSecond());
        constexpr float spacing = 24.0f;
        const auto offset = static_cast<float>(std::fmod(s.positionSamples * pxPerSample, spacing));
        g.saveState();
        g.reduceClipRegion(area.reduced(4.0f).toNearestInt());
        g.setColour(juce::Colour(s.scratching ? 0xffe0483c : 0xff5a5a68));
        for (float x = area.getX() - offset; x < area.getRight(); x += spacing)
            g.fillRect(x, area.getY() + 6.0f, 2.0f, area.getHeight() - 12.0f);
        g.restoreState();
    }

    g.setColour(held ? juce::Colour(0xffe0483c) : juce::Colours::white.withAlpha(0.15f));
    g.drawRoundedRectangle(area, 8.0f, held ? 2.0f : 1.0f);

    g.setColour(juce::Colours::white.withAlpha(0.75f));
    g.setFont(juce::FontOptions(13.0f));
    const juce::String text = juce::String("Deck ") + engine::deckLetter(deck) +
                              (s.loaded ? "  jog: drag = scratch / scrub, wheel = nudge" : "  (no track)");
    g.drawText(text, area.reduced(10.0f, 6.0f), juce::Justification::topLeft);
}

} // namespace devildj::ui
