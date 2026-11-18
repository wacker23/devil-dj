#include "TagReader.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>

#include <charconv>

namespace devildj::library
{

double parseBpmTag(const std::string& text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return 0.0;
    double value = 0.0;
    const char* begin = text.data() + first;
    const auto [end, ec] = std::from_chars(begin, text.data() + text.size(), value);
    if (ec != std::errc{} || end == begin)
        return 0.0;
    return value >= 20.0 && value <= 999.0 ? value : 0.0;
}

BasicTags readBasicTags(const std::filesystem::path& file)
{
    BasicTags tags;
    try
    {
        const TagLib::FileRef ref(file.c_str(), false); // no audio properties: tags only, fast
        if (ref.isNull())
            return tags;
        if (const auto* tag = ref.tag())
        {
            tags.title = tag->title().to8Bit(true);
            tags.artist = tag->artist().to8Bit(true);
        }
        const auto properties = ref.properties();
        const auto bpm = properties.find("BPM");
        if (bpm != properties.end() && !bpm->second.isEmpty())
            tags.bpm = parseBpmTag(bpm->second.front().to8Bit(true));
    }
    catch (...) // TagLib shouldn't throw, but a broken file must never stop a load
    {
        return {};
    }
    return tags;
}

} // namespace devildj::library
