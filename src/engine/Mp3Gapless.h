#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace devildj::engine
{

// Gapless information from an MP3's Xing/Info + LAME header (P1-04).
//
// MP3 encoders add `encoderDelay` samples of silence at the start and `encoderPadding` at the end; decoders add
// another kDecoderDelay (529) at the start. Without trimming, a track starts ~25 ms late and ends with silence,
// which shifts beatgrids and cue points. Windows' MP3 decoder ignores this header, so we apply it ourselves.
struct Mp3GaplessInfo
{
    std::uint32_t frames = 0;          // audio frames (excluding the Xing/Info frame itself)
    std::uint32_t samplesPerFrame = 0; // 1152 (MPEG-1 Layer III) or 576 (MPEG-2/2.5)
    std::uint32_t sampleRate = 0;
    std::uint32_t encoderDelay = 0;
    std::uint32_t encoderPadding = 0;

    static constexpr std::uint32_t kDecoderDelay = 529;

    // Samples of real audio: frames x samplesPerFrame - delay - padding.
    std::int64_t exactLength() const noexcept
    {
        return static_cast<std::int64_t>(frames) * samplesPerFrame - encoderDelay - encoderPadding;
    }
};

// Parses the start of an MP3 file (ID3v2 tag allowed). Returns nothing if there is no Xing/Info frame with a
// LAME extension (e.g. plain CBR files from some encoders).
std::optional<Mp3GaplessInfo> parseMp3Gapless(std::span<const std::uint8_t> fileStart) noexcept;

// Given how many samples a decoder produced, returns {first sample to keep, number to keep} that removes the
// encoder delay, decoder delay and padding. `decodedLength` lets this work whether or not the decoder already
// dropped its own 529-sample delay. Returns nothing if the numbers don't fit (then keep the decode as is).
struct TrimRange
{
    std::int64_t start = 0;
    std::int64_t length = 0;
};
std::optional<TrimRange> mp3TrimRange(const Mp3GaplessInfo& info, std::int64_t decodedLength) noexcept;

} // namespace devildj::engine
