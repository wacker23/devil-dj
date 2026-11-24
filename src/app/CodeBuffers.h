#pragma once

// The code deck's buffers (P5-13): the editor's tabs, each a file in Documents\Devil-DJ\Code ("<name>.js"), and
// tabs.json for their order and the one shown. Saved as they change (the editor calls save() a second after
// typing stops, and on the way out), read back at start - so the code survives a restart. The first time there is
// one buffer with a starter pattern. Closing a buffer moves its file to Code\Closed (nothing typed is ever lost).

#include <juce_core/juce_core.h>

#include <vector>

namespace devildj::app
{

class CodeBuffers
{
public:
    struct Buffer
    {
        juce::String name; // its file's name without ".js"
        juce::String text;
        bool dirty = false; // changed since it was saved
    };

    explicit CodeBuffers(juce::File folder = defaultFolder());

    void load();             // every buffer from the folder (a starter one if there are none)
    juce::String save();     // the changed buffers and the tab order; "" when it worked, else what failed

    int count() const noexcept { return static_cast<int>(buffers.size()); }
    const Buffer& at(int index) const { return buffers[static_cast<std::size_t>(index)]; }
    int current() const noexcept { return shown; }
    void setCurrent(int index);

    int add(juce::String name = {}, const juce::String& text = {}); // its index; the name made unique ("Code 2")
    juce::String rename(int index, const juce::String& newName);    // "" when it worked, else why not
    void remove(int index);                                          // into Closed; the last one is never removed
    void setText(int index, const juce::String& text);

    const juce::File& folder() const noexcept { return dir; }
    static juce::File defaultFolder(); // Documents\Devil-DJ\Code
    static juce::String starter();     // the first buffer's code

private:
    juce::String uniqueName(juce::String base, int except = -1) const;
    juce::File fileOf(const juce::String& name) const;

    juce::File dir;
    std::vector<Buffer> buffers;
    int shown = 0;
    bool orderDirty = false;
};

} // namespace devildj::app
