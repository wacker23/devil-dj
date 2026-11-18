#include "engine/Mp3Gapless.h"

#include "TestData.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using devildj::engine::Mp3GaplessInfo;
using devildj::engine::mp3TrimRange;
using devildj::engine::parseMp3Gapless;

namespace
{
std::vector<std::uint8_t> readHead(const juce::File& f)
{
    juce::MemoryBlock block;
    f.loadFileAsData(block);
    const auto* p = static_cast<const std::uint8_t*>(block.getData());
    return {p, p + std::min<std::size_t>(block.getSize(), 256 * 1024)};
}
} // namespace

TEST_CASE("Mp3Gapless reads the LAME header of the generated MP3", "[engine][mp3gapless]")
{
    const auto file = devildj::test::generatedFile("formats/ref.mp3");
    if (!file.existsAsFile())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    const auto info = parseMp3Gapless(readHead(file));
    REQUIRE(info.has_value());
    CHECK(info->samplesPerFrame == 1152);
    CHECK(info->sampleRate == 44100);
    CHECK(info->frames == 193);
    CHECK(info->encoderDelay == 576);
    CHECK(info->encoderPadding == 1260);
    CHECK(info->exactLength() == 220500); // = the 5 s reference
}

TEST_CASE("Mp3Gapless skips an ID3v2 tag", "[engine][mp3gapless]")
{
    const auto file = devildj::test::generatedFile("formats/ref.mp3");
    if (!file.existsAsFile())
        SKIP("Test signals not generated; run: python tools/gen_test_signals.py");

    auto mp3 = readHead(file);
    // ID3v2.3 header with a 300-byte (synchsafe) body of padding in front of the audio.
    std::vector<std::uint8_t> tagged{'I', 'D', '3', 3, 0, 0, 0, 0, 0x02, 0x2C};
    tagged.resize(10 + 300, 0);
    tagged.insert(tagged.end(), mp3.begin(), mp3.end());

    const auto info = parseMp3Gapless(tagged);
    REQUIRE(info.has_value());
    CHECK(info->encoderDelay == 576);
}

TEST_CASE("Mp3Gapless rejects files without a usable header", "[engine][mp3gapless]")
{
    CHECK_FALSE(parseMp3Gapless({}).has_value());
    const std::vector<std::uint8_t> noise(4096, 0x5A);
    CHECK_FALSE(parseMp3Gapless(noise).has_value());
    // A frame header with no Xing/Info tag.
    std::vector<std::uint8_t> plain(1024, 0);
    plain[0] = 0xFF;
    plain[1] = 0xFB;
    plain[2] = 0x90;
    plain[3] = 0x64;
    CHECK_FALSE(parseMp3Gapless(plain).has_value());
}

TEST_CASE("Mp3Gapless trim works whether or not the decoder removed its own delay", "[engine][mp3gapless]")
{
    Mp3GaplessInfo info;
    info.frames = 193;
    info.samplesPerFrame = 1152;
    info.sampleRate = 44100;
    info.encoderDelay = 576;
    info.encoderPadding = 1260;
    const std::int64_t total = 193 * 1152;

    // Decoder that outputs everything: skip delay + 529.
    auto r = mp3TrimRange(info, total);
    REQUIRE(r.has_value());
    CHECK(r->start == 576 + 529);
    CHECK(r->length == 220500);

    // Windows' decoder (drops 528 itself): skip delay + 1.
    r = mp3TrimRange(info, total - 528);
    REQUIRE(r.has_value());
    CHECK(r->start == 577);
    CHECK(r->length == 220500);

    // Decoder that already applied the full 529.
    r = mp3TrimRange(info, total - 529);
    REQUIRE(r.has_value());
    CHECK(r->start == 576);

    // Nonsense: decoded more than the header says, or far less.
    CHECK_FALSE(mp3TrimRange(info, total + 5000).has_value());
    CHECK_FALSE(mp3TrimRange(info, total - 50000).has_value());
}
