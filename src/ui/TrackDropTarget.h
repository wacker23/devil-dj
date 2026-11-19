#pragma once

#include "engine/TrackDecoder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace devildj::ui
{

// Drag-and-drop onto a deck: audio files from Windows Explorer (P1-18) and rows dragged from the library table
// (P2-04: the drag description is the track's file path; the main window is the DragAndDropContainer).
// Components that show a deck (DeckView, WaveformView) derive from this and draw `isDropHovering()` as a
// highlight.
class TrackDropTarget : public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    std::function<void(const juce::File& file)> onTrackDropped;

    // The first supported audio file among the dragged ones (none = not interested).
    static juce::File firstTrack(const juce::StringArray& paths)
    {
        for (const auto& path : paths)
            if (const juce::File f(path); f.existsAsFile() && engine::isSupportedTrackFile(f))
                return f;
        return {};
    }

    bool isInterestedInFileDrag(const juce::StringArray& paths) override { return firstTrack(paths) != juce::File(); }
    void fileDragEnter(const juce::StringArray&, int, int) override { setDropHover(true); }
    void fileDragExit(const juce::StringArray&) override { setDropHover(false); }
    void filesDropped(const juce::StringArray& paths, int, int) override
    {
        setDropHover(false);
        if (const auto file = firstTrack(paths); file != juce::File() && onTrackDropped)
            onTrackDropped(file);
    }

    // Library rows.
    static juce::File trackFromDrag(const SourceDetails& details)
    {
        // A file path, or {path, trackIds} from the library table (P2-06).
        const auto* o = details.description.getDynamicObject();
        const auto path = o != nullptr ? o->getProperty("path").toString() : details.description.toString();
        if (path.isEmpty() || !juce::File::isAbsolutePath(path))
            return {};
        const juce::File f(path);
        return engine::isSupportedTrackFile(f) ? f : juce::File();
    }
    bool isInterestedInDragSource(const SourceDetails& details) override { return trackFromDrag(details) != juce::File(); }
    void itemDragEnter(const SourceDetails&) override { setDropHover(true); }
    void itemDragExit(const SourceDetails&) override { setDropHover(false); }
    void itemDropped(const SourceDetails& details) override
    {
        setDropHover(false);
        if (const auto file = trackFromDrag(details); file != juce::File() && onTrackDropped)
            onTrackDropped(file);
    }

    bool isDropHovering() const noexcept { return dropHover; }

protected:
    virtual void dropHoverChanged() {}

private:
    void setDropHover(bool on)
    {
        if (dropHover != on)
        {
            dropHover = on;
            dropHoverChanged();
        }
    }
    bool dropHover = false;
};

// Highlight for a drop target: accent outline and a "Drop to load" label.
inline void drawDropHighlight(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text)
{
    g.setColour(juce::Colour(0xffe0483c).withAlpha(0.18f));
    g.fillRoundedRectangle(area, 8.0f);
    g.setColour(juce::Colour(0xffe0483c));
    g.drawRoundedRectangle(area.reduced(1.5f), 8.0f, 3.0f);
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(20.0f, juce::Font::bold));
    g.drawText(text, area.toNearestInt(), juce::Justification::centred);
}

} // namespace devildj::ui
