#include "TagReader.h"

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace devildj::library
{

namespace
{
std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string removeSpaces(std::string s)
{
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    return s;
}

std::string firstProperty(const TagLib::PropertyMap& properties, const char* name)
{
    const auto found = properties.find(name);
    if (found == properties.end() || found->second.isEmpty())
        return {};
    return trim(found->second.front().to8Bit(true));
}

// Camelot number (1..12) of a major key's pitch class: C = 8B, G = 9B, F = 7B ...
int majorFromCamelot(int number)
{
    return ((7 * (number - 8)) % 12 + 12) % 12;
}
} // namespace

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

std::optional<int> parseKeyTag(const std::string& text)
{
    const auto t = trim(text);
    if (t.empty())
        return std::nullopt;

    // Camelot ("8A") or Open Key ("1m" / "1d"): a number 1..12 and one letter.
    if (std::isdigit(static_cast<unsigned char>(t[0])))
    {
        int number = 0;
        const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), number);
        const auto suffix = lower(removeSpaces(std::string(end, t.data() + t.size())));
        if (ec != std::errc{} || number < 1 || number > 12 || suffix.size() != 1)
            return std::nullopt;
        int camelot = number;
        bool minor = false;
        switch (suffix[0])
        {
            case 'a': minor = true; break;
            case 'b': minor = false; break;
            case 'm': minor = true; camelot = (number + 6) % 12 + 1; break;  // Open Key 1m = Camelot 8A
            case 'd': minor = false; camelot = (number + 6) % 12 + 1; break; // Open Key 1d = Camelot 8B
            default: return std::nullopt;
        }
        const int major = majorFromCamelot(camelot);
        return minor ? 12 + (major + 9) % 12 : major; // relative minor is a minor third below
    }

    // Note name: letter, optional sharp / flat, then the mode.
    static constexpr int kPitch[] = {9, 11, 0, 2, 4, 5, 7}; // a b c d e f g
    const char letter = static_cast<char>(std::tolower(static_cast<unsigned char>(t[0])));
    if (letter < 'a' || letter > 'g')
        return std::nullopt;
    int pitch = kPitch[letter - 'a'];
    std::size_t i = 1;
    if (t.compare(i, 1, "#") == 0)
        pitch += 1, i += 1;
    else if (t.compare(i, 3, "\xE2\x99\xAF") == 0) // ♯
        pitch += 1, i += 3;
    else if (t.compare(i, 3, "\xE2\x99\xAD") == 0) // ♭
        pitch -= 1, i += 3;
    else if (i < t.size() && t[i] == 'b')
        pitch -= 1, i += 1;
    pitch = (pitch + 12) % 12;

    const auto mode = removeSpaces(t.substr(i));
    const auto m = lower(mode);
    if (mode == "M" || m.empty() || m == "maj" || m == "major" || m == "ma")
        return pitch;
    if (mode == "m" || m == "min" || m == "minor" || m == "mi")
        return 12 + pitch;
    return std::nullopt;
}

std::string artworkExtension(const std::string& mimeType, const std::vector<std::uint8_t>& data)
{
    const auto m = lower(mimeType);
    if (m == "image/jpeg" || m == "image/jpg")
        return ".jpg";
    if (m == "image/png")
        return ".png";
    if (m == "image/gif")
        return ".gif";
    if (m == "image/bmp")
        return ".bmp";
    if (m == "image/webp")
        return ".webp";
    const auto starts = [&](std::initializer_list<std::uint8_t> magic)
    { return data.size() >= magic.size() && std::equal(magic.begin(), magic.end(), data.begin()); };
    if (starts({0xFF, 0xD8, 0xFF}))
        return ".jpg";
    if (starts({0x89, 'P', 'N', 'G'}))
        return ".png";
    if (starts({'G', 'I', 'F', '8'}))
        return ".gif";
    if (starts({'B', 'M'}))
        return ".bmp";
    if (data.size() >= 12 && std::equal(data.begin() + 8, data.begin() + 12, "WEBP"))
        return ".webp";
    return ".img";
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
        tags.bpm = parseBpmTag(firstProperty(ref.properties(), "BPM"));
    }
    catch (...) // TagLib shouldn't throw, but a broken file must never stop a load
    {
        return {};
    }
    return tags;
}

TrackTags readTrackTags(const std::filesystem::path& file, bool withArtwork)
{
    TrackTags tags;
    try
    {
        const TagLib::FileRef ref(file.c_str(), true, TagLib::AudioProperties::Fast);
        // TagLib opens any file by its extension; only a file with audio in it (a sample rate) counts.
        if (ref.isNull() || ref.audioProperties() == nullptr || ref.audioProperties()->sampleRate() <= 0)
            return tags;
        tags.readable = true;
        if (const auto* tag = ref.tag())
        {
            tags.title = trim(tag->title().to8Bit(true));
            tags.artist = trim(tag->artist().to8Bit(true));
            tags.album = trim(tag->album().to8Bit(true));
            tags.genre = trim(tag->genre().to8Bit(true));
            tags.comment = trim(tag->comment().to8Bit(true));
            if (const auto year = tag->year(); year > 0 && year < 10000)
                tags.year = static_cast<int>(year);
        }
        const auto properties = ref.properties();
        tags.bpm = parseBpmTag(firstProperty(properties, "BPM"));
        tags.keyText = firstProperty(properties, "INITIALKEY");
        tags.musicalKey = parseKeyTag(tags.keyText);

        if (const auto* audio = ref.audioProperties())
        {
            if (audio->lengthInMilliseconds() > 0)
                tags.durationMs = audio->lengthInMilliseconds();
            if (audio->sampleRate() > 0)
                tags.sampleRate = audio->sampleRate();
            if (audio->bitrate() > 0)
                tags.bitrateKbps = audio->bitrate();
        }

        if (withArtwork)
        {
            const auto pictures = ref.complexProperties("PICTURE");
            const TagLib::VariantMap* chosen = nullptr;
            for (const auto& picture : pictures)
            {
                if (chosen == nullptr)
                    chosen = &picture;
                const auto type = picture.find("pictureType");
                if (type != picture.end() && type->second.toString() == "Front Cover")
                {
                    chosen = &picture;
                    break;
                }
            }
            if (chosen != nullptr)
            {
                const auto data = chosen->find("data");
                if (data != chosen->end())
                {
                    const auto bytes = data->second.toByteVector();
                    tags.artwork.assign(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                        reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size());
                }
                const auto mime = chosen->find("mimeType");
                if (mime != chosen->end())
                    tags.artworkMimeType = mime->second.toString().to8Bit(true);
            }
        }
    }
    catch (...) // a broken file must never stop an import
    {
        TrackTags none;
        return none;
    }
    return tags;
}

} // namespace devildj::library
