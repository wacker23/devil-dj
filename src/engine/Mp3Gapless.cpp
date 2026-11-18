#include "engine/Mp3Gapless.h"

#include <cstring>

namespace devildj::engine
{

namespace
{
std::uint32_t be32(const std::uint8_t* p) noexcept
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

constexpr std::uint32_t kSampleRates[3][3] = {
    {44100, 48000, 32000}, // MPEG-1
    {22050, 24000, 16000}, // MPEG-2
    {11025, 12000, 8000},  // MPEG-2.5
};
} // namespace

std::optional<Mp3GaplessInfo> parseMp3Gapless(std::span<const std::uint8_t> d) noexcept
{
    std::size_t pos = 0;

    // Skip an ID3v2 tag (size is a 28-bit "synchsafe" integer, plus a footer if flagged).
    if (d.size() >= 10 && std::memcmp(d.data(), "ID3", 3) == 0)
    {
        const std::size_t size = (static_cast<std::size_t>(d[6] & 0x7F) << 21) |
                                 (static_cast<std::size_t>(d[7] & 0x7F) << 14) |
                                 (static_cast<std::size_t>(d[8] & 0x7F) << 7) | (d[9] & 0x7F);
        pos = 10 + size + ((d[5] & 0x10) != 0 ? 10 : 0);
    }

    // Find the first frame sync (allow some junk between the tag and the first frame).
    const std::size_t searchEnd = std::min<std::size_t>(d.size(), pos + 64 * 1024);
    while (pos + 4 <= searchEnd && !(d[pos] == 0xFF && (d[pos + 1] & 0xE0) == 0xE0))
        ++pos;
    if (pos + 4 > d.size())
        return std::nullopt;

    const std::uint8_t b1 = d[pos + 1], b2 = d[pos + 2], b3 = d[pos + 3];
    const int versionBits = (b1 >> 3) & 3; // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5, 1 = reserved
    const int layerBits = (b1 >> 1) & 3;   // 1 = Layer III
    const int rateIndex = (b2 >> 2) & 3;
    const bool mono = ((b3 >> 6) & 3) == 3;
    if (versionBits == 1 || layerBits != 1 || rateIndex == 3)
        return std::nullopt;

    const bool mpeg1 = versionBits == 3;
    const int versionRow = mpeg1 ? 0 : (versionBits == 2 ? 1 : 2);

    // The Xing/Info tag sits right after the side information.
    const std::size_t sideInfo = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    std::size_t p = pos + 4 + sideInfo;
    if (p + 8 > d.size())
        return std::nullopt;
    if (std::memcmp(d.data() + p, "Xing", 4) != 0 && std::memcmp(d.data() + p, "Info", 4) != 0)
        return std::nullopt;

    const std::uint32_t flags = be32(d.data() + p + 4);
    p += 8;
    Mp3GaplessInfo info;
    if ((flags & 0x1) != 0) // frame count
    {
        if (p + 4 > d.size())
            return std::nullopt;
        info.frames = be32(d.data() + p);
        p += 4;
    }
    if ((flags & 0x2) != 0) // byte count
        p += 4;
    if ((flags & 0x4) != 0) // seek table
        p += 100;
    if ((flags & 0x8) != 0) // quality
        p += 4;

    // LAME extension: 9-byte encoder string, then delay/padding at offset 21 as two 12-bit values.
    if (info.frames == 0 || p + 24 > d.size())
        return std::nullopt;
    const std::uint8_t* lame = d.data() + p;
    if (std::memcmp(lame, "LAME", 4) != 0 && std::memcmp(lame, "Lavc", 4) != 0 && std::memcmp(lame, "Lavf", 4) != 0)
        return std::nullopt;
    info.encoderDelay = (static_cast<std::uint32_t>(lame[21]) << 4) | (lame[22] >> 4);
    info.encoderPadding = (static_cast<std::uint32_t>(lame[22] & 0x0F) << 8) | lame[23];
    info.samplesPerFrame = mpeg1 ? 1152 : 576;
    info.sampleRate = kSampleRates[versionRow][rateIndex];
    return info;
}

std::optional<TrimRange> mp3TrimRange(const Mp3GaplessInfo& info, std::int64_t decodedLength) noexcept
{
    const auto total = static_cast<std::int64_t>(info.frames) * info.samplesPerFrame;
    const auto exact = info.exactLength();
    const auto alreadyDropped = total - decodedLength; // what the decoder removed at the start itself
    if (exact <= 0 || alreadyDropped < 0 || alreadyDropped > static_cast<std::int64_t>(info.samplesPerFrame))
        return std::nullopt;

    TrimRange r;
    r.start = static_cast<std::int64_t>(info.encoderDelay) + Mp3GaplessInfo::kDecoderDelay - alreadyDropped;
    if (r.start < 0)
        r.start = 0;
    r.length = std::min(exact, decodedLength - r.start);
    if (r.length <= 0)
        return std::nullopt;
    return r;
}

} // namespace devildj::engine
