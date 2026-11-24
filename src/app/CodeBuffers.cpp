#include "app/CodeBuffers.h"

#include <algorithm>

namespace devildj::app
{

namespace
{
constexpr const char* kTabsFile = "tabs.json";
constexpr const char* kClosedFolder = "Closed";

juce::String legalName(const juce::String& name)
{
    auto n = juce::File::createLegalFileName(name.trim()).trim();
    while (n.endsWithIgnoreCase(".js"))
        n = n.dropLastCharacters(3).trim();
    return n;
}
} // namespace

CodeBuffers::CodeBuffers(juce::File folder)
    : dir(std::move(folder))
{
}

juce::File CodeBuffers::defaultFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Devil-DJ").getChildFile("Code");
}

juce::String CodeBuffers::starter()
{
    return "// Code Beat: the last value is what plays. Ctrl+Enter starts it at the next bar, Ctrl+. stops.\n"
           "stack(\n"
           "  s(\"bd*4\"),\n"
           "  s(\"~ cp ~ cp\"),\n"
           "  s(\"hh*8\").gain(sine.range(0.4, 0.8)),\n"
           "  n(\"<0 3 5 7>\").scale(\"A:minor\").s(\"saw\").lpf(900)\n"
           ")\n";
}

juce::File CodeBuffers::fileOf(const juce::String& name) const
{
    return dir.getChildFile(name + ".js");
}

juce::String CodeBuffers::uniqueName(juce::String base, int except) const
{
    base = legalName(base);
    if (base.isEmpty())
        base = "Code";
    const auto taken = [&](const juce::String& n)
    {
        for (std::size_t i = 0; i < buffers.size(); ++i)
            if (static_cast<int>(i) != except && buffers[i].name.equalsIgnoreCase(n))
                return true;
        return false;
    };
    if (!taken(base) && base != "Code")
        return base;
    for (int i = base == "Code" ? 1 : 2;; ++i)
        if (const auto n = base + " " + juce::String(i); !taken(n))
            return n;
}

void CodeBuffers::load()
{
    buffers.clear();
    shown = 0;
    dir.createDirectory();
    // The tab order and the one shown, then any other file (sorted) after them.
    juce::StringArray order;
    int current = 0;
    if (const auto tabs = juce::JSON::parse(dir.getChildFile(kTabsFile)); tabs.isObject())
    {
        if (const auto* names = tabs.getProperty("order", {}).getArray())
            for (const auto& n : *names)
                order.add(n.toString());
        current = static_cast<int>(tabs.getProperty("current", 0));
    }
    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.js");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b)
              { return a.getFileName().compareNatural(b.getFileName()) < 0; });
    for (const auto& name : order)
        if (const auto f = fileOf(name); f.existsAsFile())
            buffers.push_back({name, f.loadFileAsString(), false});
    for (const auto& f : files)
    {
        const auto name = f.getFileNameWithoutExtension();
        if (!order.contains(name))
            buffers.push_back({name, f.loadFileAsString(), false});
    }
    if (buffers.empty())
    {
        buffers.push_back({"Code 1", starter(), true});
        orderDirty = true;
    }
    shown = std::clamp(current, 0, count() - 1);
}

juce::String CodeBuffers::save()
{
    dir.createDirectory();
    juce::String failed;
    for (auto& b : buffers)
    {
        if (!b.dirty)
            continue;
        if (fileOf(b.name).replaceWithText(b.text, false, false, "\n"))
            b.dirty = false;
        else
            failed << "can't write " << fileOf(b.name).getFullPathName() << "\n";
    }
    if (orderDirty)
    {
        juce::Array<juce::var> names;
        for (const auto& b : buffers)
            names.add(b.name);
        auto* tabs = new juce::DynamicObject();
        tabs->setProperty("order", names);
        tabs->setProperty("current", shown);
        if (dir.getChildFile(kTabsFile).replaceWithText(juce::JSON::toString(juce::var(tabs))))
            orderDirty = false;
        else
            failed << "can't write " << dir.getChildFile(kTabsFile).getFullPathName() << "\n";
    }
    return failed.trim();
}

void CodeBuffers::setCurrent(int index)
{
    const int i = std::clamp(index, 0, count() - 1);
    if (i != shown)
    {
        shown = i;
        orderDirty = true;
    }
}

int CodeBuffers::add(juce::String name, const juce::String& text)
{
    buffers.push_back({uniqueName(name.isEmpty() ? juce::String("Code") : name), text, true});
    shown = count() - 1;
    orderDirty = true;
    return shown;
}

juce::String CodeBuffers::rename(int index, const juce::String& newName)
{
    if (index < 0 || index >= count())
        return "no such buffer";
    auto& b = buffers[static_cast<std::size_t>(index)];
    const auto name = legalName(newName);
    if (name.isEmpty())
        return "a buffer needs a name";
    if (name == b.name)
        return {};
    for (std::size_t i = 0; i < buffers.size(); ++i)
        if (static_cast<int>(i) != index && buffers[i].name.equalsIgnoreCase(name))
            return "there is a buffer called \"" + name + "\" already";
    const auto from = fileOf(b.name), to = fileOf(name);
    if (from.existsAsFile() && !from.moveFileTo(to))
        return "can't rename " + from.getFullPathName();
    b.name = name;
    b.dirty = b.dirty || !to.existsAsFile();
    orderDirty = true;
    return {};
}

void CodeBuffers::remove(int index)
{
    if (index < 0 || index >= count() || count() == 1)
        return;
    const auto& b = buffers[static_cast<std::size_t>(index)];
    const auto closed = dir.getChildFile(kClosedFolder);
    closed.createDirectory();
    const auto file = fileOf(b.name);
    if (b.dirty || !file.existsAsFile())
        file.replaceWithText(b.text, false, false, "\n"); // the latest text goes with it
    file.moveFileTo(closed.getNonexistentChildFile(b.name, ".js", false));
    buffers.erase(buffers.begin() + index);
    shown = std::clamp(shown > index ? shown - 1 : shown, 0, count() - 1);
    orderDirty = true;
}

void CodeBuffers::setText(int index, const juce::String& text)
{
    if (index < 0 || index >= count())
        return;
    auto& b = buffers[static_cast<std::size_t>(index)];
    if (b.text == text)
        return;
    b.text = text;
    b.dirty = true;
}

} // namespace devildj::app
