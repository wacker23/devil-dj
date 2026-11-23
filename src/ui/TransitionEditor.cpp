#include "ui/TransitionEditor.h"

#include "dsp/fx/BeatFx.h"

#include <algorithm>
#include <cmath>

namespace devildj::ui
{

namespace
{
using engine::Lane;
using GA = GlobalAction;

const juce::Colour kBackground(0xff16161b);
const juce::Colour kPanel(0xff1f1f26);
const juce::Colour kGrid(0xff2c2c36);
const juce::Colour kText(0xffe8e8ee);
const juce::Colour kDim(0xff8a8a96);
const juce::Colour kAccent(0xffe0483c);
// Each lane its own colour (our own palette).
const juce::Colour kLaneColours[engine::kLaneCount] = {juce::Colour(0xfff0f0f0), juce::Colour(0xffe0483c), juce::Colour(0xffe8a23c),
                                                       juce::Colour(0xff4fb3e8), juce::Colour(0xff3cc46a), juce::Colour(0xffb57cf0)};
const char* kLaneNames[engine::kLaneCount] = {"VOLUME", "LOW", "MID", "HIGH", "FILTER", "FX"};
constexpr double kBefore = 8.0, kAfter = 4.0; // beats shown around it

void style(juce::TextButton& b)
{
    b.setColour(juce::TextButton::buttonColourId, kPanel);
    b.setColour(juce::TextButton::buttonOnColourId, kAccent);
    b.setColour(juce::TextButton::textColourOffId, kText);
    b.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    b.setWantsKeyboardFocus(false);
}

const char* shapeName(engine::Shape s)
{
    switch (s)
    {
        case engine::Shape::EaseIn: return "ease in";
        case engine::Shape::EaseOut: return "ease out";
        case engine::Shape::SCurve: return "S-curve";
        case engine::Shape::Hold: return "hold";
        case engine::Shape::Linear: break;
    }
    return "linear";
}
} // namespace

const std::vector<int>& TransitionEditor::fxChoices()
{
    using T = dsp::fx::EffectType;
    static const std::vector<int> c{-1, static_cast<int>(T::Echo), static_cast<int>(T::PingPong), static_cast<int>(T::Reverb),
                                    static_cast<int>(T::Spiral), static_cast<int>(T::FilterLfo), static_cast<int>(T::Flanger),
                                    static_cast<int>(T::Phaser), static_cast<int>(T::BitCrush)};
    return c;
}

const std::vector<int>& TransitionEditor::lengths()
{
    static const std::vector<int> l{1, 2, 4, 8, 12, 16, 24, 32};
    return l;
}

TransitionEditor::TransitionEditor()
{
    setWantsKeyboardFocus(true);
    for (int i = 0; i < engine::kLaneCount; ++i)
    {
        auto& b = laneButtons[static_cast<std::size_t>(i)];
        style(b);
        b.setButtonText(kLaneNames[i]);
        b.setColour(juce::TextButton::textColourOffId, kLaneColours[i]);
        b.setTooltip(juce::String("Edit the ") + kLaneNames[i] + " lane (" + juce::String(i + 1) + ")");
        b.onClick = [this, i]
        {
            selectedLane = static_cast<Lane>(i);
            refresh();
        };
        addAndMakeVisible(b);
    }
    for (const int bars : lengths())
        length.addItem(juce::String(bars) + (bars == 1 ? " bar" : " bars"), bars);
    length.setTooltip("The transition's length; the curves stretch with it (L)");
    length.onChange = [this] { setLength(length.getSelectedId()); };
    presets.setTooltip("Start from a preset (P / Shift+P)");
    presets.onChange = [this]
    {
        const int id = presets.getSelectedId();
        if (id > 0 && onPreset)
            onPreset(id - 1);
    };
    for (auto* c : {&length, &presets})
    {
        c->setWantsKeyboardFocus(false);
        addAndMakeVisible(*c);
    }
    for (auto* b : {&autoNow, &preview, &save, &fxType, &fxTime, &loop})
    {
        style(*b);
        addAndMakeVisible(*b);
    }
    fxType.setTooltip("The effect this deck's FX lane sends into (F)");
    fxType.onClick = [this] { perform(GA::TransFxType); };
    fxTime.setTooltip("The effect's time in beats (Shift+F)");
    fxTime.onClick = [this] { perform(GA::TransFxTime); };
    loop.setTooltip("Loop out: the old track loops its last 1, 2 or 4 bars through the transition (O)");
    loop.onClick = [this] { perform(GA::TransLoop); };
    autoNow.setTooltip("Plan it from the two tracks: where their phrases, intro and outro are, keys, tempo, energy (A)");
    autoNow.onClick = [this] { perform(GA::TransAuto); };
    preview.setTooltip("Play it, from 2 bars before (Space)");
    preview.onClick = [this] { perform(GA::TransPreview); };
    save.setTooltip("Save these curves as a preset of your own (Ctrl+S)");
    save.onClick = [this] { perform(GA::TransSavePreset); };
    refresh();
}

void TransitionEditor::setTracks(const Track& o, const Track& i)
{
    outTrack = o;
    inTrack = i;
    repaint();
}

void TransitionEditor::setEdit(const Edit& e)
{
    current = e;
    engine::normalize(current.curves);
    cursorBeat = std::clamp(cursorBeat, 0.0, static_cast<double>(current.curves.beats));
    refresh();
}

void TransitionEditor::setPresetNames(const juce::StringArray& builtIn, const juce::StringArray& user)
{
    presets.clear(juce::dontSendNotification);
    builtInCount = builtIn.size();
    for (int i = 0; i < builtIn.size(); ++i)
        presets.addItem(builtIn[i], i + 1);
    if (!user.isEmpty())
    {
        presets.addSeparator();
        for (int i = 0; i < user.size(); ++i)
            presets.addItem(user[i], builtInCount + i + 1);
    }
    refresh();
}

void TransitionEditor::setMessage(const juce::String& text)
{
    message = text;
    repaint();
}

int TransitionEditor::bars() const noexcept
{
    return std::max(1, static_cast<int>(std::lround(current.curves.beats / 4.0f)));
}

void TransitionEditor::changed(bool custom)
{
    engine::normalize(current.curves);
    if (custom)
        current.preset = "Custom";
    refresh();
    if (onChanged)
        onChanged(current);
}

void TransitionEditor::refresh()
{
    for (int i = 0; i < engine::kLaneCount; ++i)
        laneButtons[static_cast<std::size_t>(i)].setToggleState(static_cast<int>(selectedLane) == i, juce::dontSendNotification);
    length.setSelectedId(bars(), juce::dontSendNotification);
    {
        const int fx = current.curves.fx[incoming ? 1u : 0u];
        fxType.setButtonText(juce::String("FX: ") + (fx < 0 ? juce::String("NONE")
                                                            : juce::String(dsp::fx::effectTypeName(static_cast<dsp::fx::EffectType>(fx))).toUpperCase()));
        const float b = current.curves.fxBeats;
        fxTime.setButtonText(b == 0.75f ? juce::String("3/4") : b < 1.0f ? "1/" + juce::String(juce::roundToInt(1.0f / b)) : juce::String(b, 0));
        const float lb = current.curves.loopBeats;
        loop.setButtonText(lb <= 0.0f ? juce::String("NO LOOP") : "LOOP " + juce::String(juce::roundToInt(lb / 4.0f)) + (lb > 4.0f ? " BARS" : " BAR"));
        loop.setToggleState(lb > 0.0f, juce::dontSendNotification);
    }
    int presetId = 0;
    for (int i = 0; i < presets.getNumItems(); ++i)
        if (presets.getItemText(i) == current.preset)
            presetId = presets.getItemId(i);
    presets.setSelectedId(presetId, juce::dontSendNotification);
    if (presetId == 0)
        presets.setText(current.preset.isEmpty() ? juce::String("Custom") : current.preset, juce::dontSendNotification);
    repaint();
}

void TransitionEditor::setLength(int newBars)
{
    if (newBars <= 0 || newBars == bars())
        return;
    // The curves stretch with it.
    const float k = 4.0f * static_cast<float>(newBars) / current.curves.beats;
    for (auto* side : {&current.curves.from, &current.curves.to})
        for (auto& lc : *side)
            for (auto& pt : lc.points)
                pt.beat *= k;
    current.curves.beats = 4.0f * static_cast<float>(newBars);
    cursorBeat = std::min(cursorBeat * k, static_cast<double>(current.curves.beats));
    changed(false);
}

// ---- geometry ----

juce::Rectangle<int> TransitionEditor::half(bool side) const noexcept
{
    auto v = view;
    return side ? v.removeFromBottom(v.getHeight() / 2) : v.removeFromTop(v.getHeight() / 2);
}

int TransitionEditor::xOf(double beat) const noexcept
{
    const double span = kBefore + current.curves.beats + kAfter;
    return view.getX() + static_cast<int>(std::lround((beat + kBefore) / span * view.getWidth()));
}

double TransitionEditor::beatAt(int x) const noexcept
{
    const double span = kBefore + current.curves.beats + kAfter;
    return (x - view.getX()) * span / std::max(1, view.getWidth()) - kBefore;
}

void TransitionEditor::rangeOf(Lane l, float& lo, float& hi)
{
    switch (l)
    {
        case Lane::EqLow:
        case Lane::EqMid:
        case Lane::EqHigh: lo = -26.0f; hi = 6.0f; return;
        case Lane::Filter: lo = -1.0f; hi = 1.0f; return;
        case Lane::Volume:
        case Lane::FxSend: break;
    }
    lo = 0.0f;
    hi = 1.0f;
}

float TransitionEditor::step(Lane l)
{
    return l == Lane::EqLow || l == Lane::EqMid || l == Lane::EqHigh ? 1.0f : 0.05f;
}

int TransitionEditor::yOf(bool side, Lane l, float value) const noexcept
{
    float lo = 0.0f, hi = 1.0f;
    rangeOf(l, lo, hi);
    const auto area = half(side).reduced(0, 8);
    const float t = std::clamp((value - lo) / (hi - lo), 0.0f, 1.0f);
    return area.getBottom() - static_cast<int>(std::lround(t * area.getHeight()));
}

float TransitionEditor::valueAt(bool side, Lane l, int y) const noexcept
{
    float lo = 0.0f, hi = 1.0f;
    rangeOf(l, lo, hi);
    const auto area = half(side).reduced(0, 8);
    const float t = std::clamp(static_cast<float>(area.getBottom() - y) / std::max(1, area.getHeight()), 0.0f, 1.0f);
    const float v = lo + t * (hi - lo);
    if ((l == Lane::EqLow || l == Lane::EqMid || l == Lane::EqHigh) && t < 0.02f)
        return -1000.0f; // the bottom of an EQ lane: kill
    return v;
}

int TransitionEditor::pointNear(bool side, Lane l, juce::Point<int> p) const
{
    const auto& pts = current.curves.lane(side, l).points;
    int best = -1;
    int bestDistance = 10 * 10;
    for (std::size_t i = 0; i < pts.size(); ++i)
    {
        const juce::Point<int> q(xOf(pts[i].beat), yOf(side, l, pts[i].value));
        const int d = (q - p).getX() * (q - p).getX() + (q - p).getY() * (q - p).getY();
        if (d <= bestDistance)
        {
            bestDistance = d;
            best = static_cast<int>(i);
        }
    }
    return best;
}

int TransitionEditor::pointAtCursor() const
{
    const auto& pts = current.curves.lane(incoming, selectedLane).points;
    for (std::size_t i = 0; i < pts.size(); ++i)
        if (std::abs(pts[i].beat - cursorBeat) < 1e-3)
            return static_cast<int>(i);
    return -1;
}

// ---- keyboard ----

bool TransitionEditor::perform(GlobalAction a)
{
    const int ai = static_cast<int>(a);
    if (ai >= static_cast<int>(GA::TransLane1) && ai <= static_cast<int>(GA::TransLane6))
    {
        selectedLane = static_cast<Lane>(ai - static_cast<int>(GA::TransLane1));
        refresh();
        return true;
    }
    const double len = current.curves.beats;
    auto& pts = laneCurve(incoming, selectedLane).points;
    switch (a)
    {
        case GA::TransFxType:
        {
            const auto& c = fxChoices();
            auto& fx = current.curves.fx[incoming ? 1u : 0u];
            const auto it = std::find(c.begin(), c.end(), static_cast<int>(fx));
            const auto next = it == c.end() || it + 1 == c.end() ? c.front() : *(it + 1);
            fx = static_cast<std::int8_t>(next);
            if (next >= 0 && !laneCurve(incoming, Lane::FxSend).used()) // something to send: a lane to start from
                laneCurve(incoming, Lane::FxSend).points = {{0.0f, 0.0f}, {static_cast<float>(len), 0.5f}};
            changed();
            return true;
        }
        case GA::TransFxTime:
        {
            static const float times[] = {0.25f, 0.5f, 0.75f, 1.0f, 2.0f, 4.0f};
            auto& t = current.curves.fxBeats;
            const auto it = std::find(std::begin(times), std::end(times), t);
            t = it == std::end(times) || it + 1 == std::end(times) ? times[0] : *(it + 1);
            changed();
            return true;
        }
        case GA::TransLoop:
        {
            auto& lb = current.curves.loopBeats;
            lb = lb <= 0.0f ? 4.0f : lb <= 4.0f ? 8.0f : lb <= 8.0f ? 16.0f : 0.0f;
            changed();
            return true;
        }
        case GA::TransSide:
            incoming = !incoming;
            refresh();
            return true;
        case GA::TransCursorLeft:
        case GA::TransCursorRight:
        case GA::TransCursorBarLeft:
        case GA::TransCursorBarRight:
        {
            const double d = a == GA::TransCursorLeft ? -1.0 : a == GA::TransCursorRight ? 1.0 : a == GA::TransCursorBarLeft ? -4.0 : 4.0;
            cursorBeat = std::clamp(std::round(cursorBeat + d), 0.0, len);
            repaint();
            return true;
        }
        case GA::TransPointPrevious:
        case GA::TransPointNext:
        {
            const bool next = a == GA::TransPointNext;
            double best = next ? 1e9 : -1e9;
            for (const auto& p : pts)
                if (next ? (p.beat > cursorBeat + 1e-3 && p.beat < best) : (p.beat < cursorBeat - 1e-3 && p.beat > best))
                    best = p.beat;
            if (std::abs(best) < 1e8)
                cursorBeat = best;
            repaint();
            return true;
        }
        case GA::TransAddPoint:
            if (pointAtCursor() < 0)
            {
                const float v = pts.empty() ? engine::neutralValue(selectedLane) : engine::evaluate(laneCurve(incoming, selectedLane), cursorBeat);
                pts.push_back({static_cast<float>(cursorBeat), v, engine::Shape::Linear});
                changed();
            }
            return true;
        case GA::TransDeletePoint:
            if (const int i = pointAtCursor(); i >= 0)
            {
                pts.erase(pts.begin() + i);
                changed();
            }
            return true;
        case GA::TransValueUp:
        case GA::TransValueDown:
        {
            if (pointAtCursor() < 0)
                perform(GA::TransAddPoint);
            const int i = pointAtCursor();
            if (i < 0)
                return true;
            float lo = 0.0f, hi = 1.0f;
            rangeOf(selectedLane, lo, hi);
            auto& v = pts[static_cast<std::size_t>(i)].value;
            if (v < lo) // a kill: up comes back to the bottom of the range
                v = lo;
            else
                v = std::clamp(v + (a == GA::TransValueUp ? step(selectedLane) : -step(selectedLane)), lo, hi);
            if (a == GA::TransValueDown && v <= lo && (selectedLane == Lane::EqLow || selectedLane == Lane::EqMid || selectedLane == Lane::EqHigh))
                v = -1000.0f; // down past the bottom: kill
            changed();
            return true;
        }
        case GA::TransShape:
            if (const int i = pointAtCursor(); i >= 0)
            {
                auto& s = pts[static_cast<std::size_t>(i)].shape;
                s = static_cast<engine::Shape>((static_cast<int>(s) + 1) % 5);
                changed();
            }
            return true;
        case GA::TransLength:
        {
            const auto& l = lengths();
            auto it = std::upper_bound(l.begin(), l.end(), bars());
            setLength(it == l.end() ? l.front() : *it);
            return true;
        }
        case GA::TransPresetNext:
        case GA::TransPresetPrevious:
        {
            const int n = presets.getNumItems();
            if (n == 0)
                return true;
            int at = -1;
            for (int i = 0; i < n; ++i)
                if (presets.getItemId(i) == presets.getSelectedId())
                    at = i;
            const int next = at < 0 ? 0 : (at + (a == GA::TransPresetNext ? 1 : n - 1)) % n;
            if (onPreset)
                onPreset(presets.getItemId(next) - 1);
            return true;
        }
        case GA::TransAuto:
            if (onAuto)
                onAuto();
            return true;
        case GA::TransMoveInEarlier:
        case GA::TransMoveInLater:
            current.inBeat += a == GA::TransMoveInLater ? -1.0 : 1.0; // later in: it starts from an earlier beat
            changed();
            return true;
        case GA::TransMoveOutEarlier:
        case GA::TransMoveOutLater:
            current.outBeat += a == GA::TransMoveOutLater ? 4.0 : -4.0;
            changed();
            return true;
        case GA::TransPreview:
            if (onPreview)
                onPreview(current);
            return true;
        case GA::TransSavePreset:
            if (isShowing())
            {
                auto* w = new juce::AlertWindow("Save the transition", "A name for this preset:", juce::MessageBoxIconType::NoIcon);
                w->addTextEditor("name", current.preset == "Custom" ? juce::String("My transition") : current.preset + " (mine)");
                w->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
                w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
                w->enterModalState(true, juce::ModalCallbackFunction::create(
                                             [this, w](int result)
                                             {
                                                 if (result == 1 && onSavePreset)
                                                     onSavePreset(w->getTextEditorContents("name"));
                                             }),
                                   true);
            }
            return true;
        default:
            return false;
    }
}

// ---- mouse ----

void TransitionEditor::pressAt(juce::Point<int> p, juce::ModifierKeys mods)
{
    drag = Drag::None;
    if (!view.contains(p))
        return;
    const bool side = half(true).contains(p);
    if (side != incoming)
    {
        incoming = side;
        refresh();
    }
    if (mods.isShiftDown())
    {
        drag = Drag::MoveTrack;
        dragSide = side;
        dragStartX = p.x;
        dragStartBeat = side ? current.inBeat : current.outBeat;
        return;
    }
    auto& pts = laneCurve(side, selectedLane).points;
    const int hit = pointNear(side, selectedLane, p);
    if (hit >= 0 && mods.isPopupMenu())
    {
        pts.erase(pts.begin() + hit);
        changed();
        return;
    }
    if (mods.isPopupMenu())
        return;
    if (hit >= 0)
    {
        drag = Drag::Point;
        dragIndex = hit;
        dragSide = side;
        cursorBeat = pts[static_cast<std::size_t>(hit)].beat;
        repaint();
        return;
    }
    // A new point there.
    const double beat = std::clamp(mods.isAltDown() ? beatAt(p.x) : std::round(beatAt(p.x) * 4.0) / 4.0, 0.0,
                                   static_cast<double>(current.curves.beats));
    engine::CurvePoint pt{static_cast<float>(beat), valueAt(side, selectedLane, p.y), engine::Shape::Linear};
    auto it = std::upper_bound(pts.begin(), pts.end(), pt, [](const auto& a, const auto& b) { return a.beat < b.beat; });
    if (it != pts.begin())
        pt.shape = std::prev(it)->shape;
    dragIndex = static_cast<int>(pts.insert(it, pt) - pts.begin());
    dragSide = side;
    drag = Drag::Point;
    cursorBeat = beat;
    changed();
}

void TransitionEditor::dragTo(juce::Point<int> p, juce::ModifierKeys mods)
{
    if (drag == Drag::MoveTrack)
    {
        // Moved by the drag: the incoming track by beats, the outgoing by bars (it sits on a phrase).
        const double beats = beatAt(p.x) - beatAt(dragStartX);
        if (dragSide)
            current.inBeat = dragStartBeat - std::round(beats);
        else
            current.outBeat = dragStartBeat - 4.0 * std::round(beats / 4.0);
        changed();
        return;
    }
    if (drag != Drag::Point)
        return;
    auto& pts = laneCurve(dragSide, selectedLane).points;
    if (dragIndex < 0 || dragIndex >= static_cast<int>(pts.size()))
        return;
    // Between its neighbours (the order stays), snapped to quarter beats unless Alt is held.
    const double lo = dragIndex > 0 ? pts[static_cast<std::size_t>(dragIndex - 1)].beat : 0.0;
    const double hi = dragIndex + 1 < static_cast<int>(pts.size()) ? pts[static_cast<std::size_t>(dragIndex + 1)].beat
                                                                   : current.curves.beats;
    double beat = mods.isAltDown() ? beatAt(p.x) : std::round(beatAt(p.x) * 4.0) / 4.0;
    beat = std::clamp(beat, lo, hi);
    auto& pt = pts[static_cast<std::size_t>(dragIndex)];
    pt.beat = static_cast<float>(beat);
    pt.value = valueAt(dragSide, selectedLane, p.y);
    cursorBeat = beat;
    changed();
}

void TransitionEditor::release()
{
    drag = Drag::None;
    dragIndex = -1;
}

void TransitionEditor::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    pressAt(e.getPosition(), e.mods);
}

void TransitionEditor::mouseDrag(const juce::MouseEvent& e)
{
    dragTo(e.getPosition(), e.mods);
}

void TransitionEditor::mouseUp(const juce::MouseEvent&)
{
    release();
}

void TransitionEditor::mouseDoubleClick(const juce::MouseEvent& e)
{
    // A point's shape: linear -> ease in -> ease out -> S-curve -> hold.
    const bool side = half(true).contains(e.getPosition());
    if (const int i = pointNear(side, selectedLane, e.getPosition()); i >= 0)
    {
        auto& s = laneCurve(side, selectedLane).points[static_cast<std::size_t>(i)].shape;
        s = static_cast<engine::Shape>((static_cast<int>(s) + 1) % 5);
        changed();
    }
}

// ---- layout and drawing ----

void TransitionEditor::resized()
{
    auto area = getLocalBounds().reduced(6);
    auto bar = area.removeFromTop(28);
    autoNow.setBounds(bar.removeFromLeft(64).reduced(2));
    presets.setBounds(bar.removeFromLeft(170).reduced(2));
    length.setBounds(bar.removeFromLeft(90).reduced(2));
    bar.removeFromLeft(8);
    for (auto& b : laneButtons)
        b.setBounds(bar.removeFromLeft(64).reduced(2));
    bar.removeFromLeft(8);
    fxType.setBounds(bar.removeFromLeft(110).reduced(2));
    fxTime.setBounds(bar.removeFromLeft(44).reduced(2));
    loop.setBounds(bar.removeFromLeft(84).reduced(2));
    preview.setBounds(bar.removeFromLeft(80).reduced(2));
    save.setBounds(bar.removeFromLeft(100).reduced(2));
    area.removeFromTop(4);
    reasonsArea = area.removeFromBottom(36);
    view = area;
}

void TransitionEditor::drawWaveform(juce::Graphics& g, const Track& t, double startBeat, juce::Rectangle<int> area) const
{
    if (t.waveform == nullptr || t.waveform->empty() || t.grid.empty() || t.waveform->samplesPerPoint <= 0)
        return;
    const auto& w = *t.waveform;
    const float mid = static_cast<float>(area.getCentreY());
    const float scale = area.getHeight() * 0.45f;
    const juce::Colour bands[3] = {juce::Colour(0xff8a3a2e), juce::Colour(0xff9a7a3a), juce::Colour(0xff3a6a8a)};
    for (int x = area.getX(); x < area.getRight(); ++x)
    {
        const double s0 = t.grid.sampleAt(startBeat + beatAt(x)), s1 = t.grid.sampleAt(startBeat + beatAt(x + 1));
        const int p0 = static_cast<int>(s0 / w.samplesPerPoint), p1 = std::max(p0 + 1, static_cast<int>(s1 / w.samplesPerPoint));
        if (p0 < 0 || p0 >= w.numPoints())
            continue;
        for (int b = 2; b >= 0; --b)
        {
            float peak = 0.0f;
            for (int p = p0; p < std::min(p1, w.numPoints()); ++p)
                peak = std::max(peak, w.peak[static_cast<std::size_t>(b)][static_cast<std::size_t>(p)]);
            const float h = std::min(1.0f, peak) * scale;
            g.setColour(bands[b]);
            g.drawVerticalLine(x, mid - h, mid + h);
        }
    }
}

void TransitionEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);
    g.setColour(kPanel);
    g.fillRect(view);
    if (outTrack.waveform == nullptr || inTrack.waveform == nullptr)
    {
        g.setColour(kDim);
        g.setFont(juce::FontOptions(14.0f));
        g.drawFittedText(message, view.reduced(20), juce::Justification::centred, 3);
        return;
    }
    const double len = current.curves.beats;
    // The waveforms: the outgoing track from (its start - 2 bars), the incoming from its start.
    drawWaveform(g, outTrack, current.outBeat, half(false));
    drawWaveform(g, inTrack, current.inBeat, half(true));
    // The grid: bars and beats; the transition's span lit.
    for (int b = static_cast<int>(-kBefore); b <= static_cast<int>(len + kAfter); ++b)
    {
        g.setColour(b % 4 == 0 ? kDim.withAlpha(0.6f) : kGrid);
        g.drawVerticalLine(xOf(b), static_cast<float>(view.getY()), static_cast<float>(view.getBottom()));
    }
    g.setColour(kAccent.withAlpha(0.08f));
    g.fillRect(juce::Rectangle<int>::leftTopRightBottom(xOf(0.0), view.getY(), xOf(len), view.getBottom()));
    g.setColour(kDim);
    g.drawHorizontalLine(view.getCentreY(), static_cast<float>(view.getX()), static_cast<float>(view.getRight()));
    // The lanes: the chosen one bold with its points, the others faint.
    for (const bool side : {false, true})
        for (int l = 0; l < engine::kLaneCount; ++l)
        {
            const auto lane = static_cast<Lane>(l);
            const auto& lc = current.curves.lane(side, lane);
            if (!lc.used())
                continue;
            const bool chosen = lane == selectedLane && side == incoming;
            juce::Path path;
            for (int x = xOf(0.0); x <= xOf(len); x += 2)
            {
                const float y = static_cast<float>(yOf(side, lane, engine::evaluate(lc, beatAt(x))));
                x == xOf(0.0) ? path.startNewSubPath(static_cast<float>(x), y) : path.lineTo(static_cast<float>(x), y);
            }
            g.setColour(kLaneColours[l].withAlpha(chosen ? 1.0f : lane == selectedLane ? 0.6f : 0.3f));
            g.strokePath(path, juce::PathStrokeType(chosen ? 2.5f : 1.2f));
            if (lane == selectedLane)
                for (const auto& p : lc.points)
                {
                    const float x = static_cast<float>(xOf(p.beat)), y = static_cast<float>(yOf(side, lane, p.value));
                    g.fillEllipse(x - 5.0f, y - 5.0f, 10.0f, 10.0f);
                }
        }
    // The keyboard cursor.
    g.setColour(kAccent);
    g.drawVerticalLine(xOf(cursorBeat), static_cast<float>(half(incoming).getY()), static_cast<float>(half(incoming).getBottom()));
    // Titles, the chosen point, the reasons.
    g.setFont(juce::FontOptions(12.0f));
    g.setColour(kText);
    g.drawText("OUT  " + outTrack.title, half(false).reduced(6, 4), juce::Justification::topLeft);
    g.drawText("IN  " + inTrack.title, half(true).reduced(6, 4), juce::Justification::topLeft);
    juce::String info = juce::String(kLaneNames[static_cast<int>(selectedLane)]) + (incoming ? " (in)" : " (out)") + "  beat " +
                        juce::String(cursorBeat, 2);
    if (const int i = pointAtCursor(); i >= 0)
    {
        const auto& p = current.curves.lane(incoming, selectedLane).points[static_cast<std::size_t>(i)];
        info << "  value " << (p.value <= -100.0f ? juce::String("kill") : juce::String(p.value, 2)) << "  " << shapeName(p.shape);
    }
    g.setColour(kDim);
    g.drawText(info, half(incoming).reduced(6, 4), juce::Justification::topRight);
    juce::String reasons;
    for (const auto& r : current.reasons)
        reasons << (reasons.isEmpty() ? "" : "  -  ") << juce::String::fromUTF8(r.c_str());
    g.setColour(kText.withAlpha(0.85f));
    g.drawFittedText(current.preset + (reasons.isNotEmpty() ? ": " + reasons : juce::String()), reasonsArea, juce::Justification::centredLeft, 2);
}

} // namespace devildj::ui
