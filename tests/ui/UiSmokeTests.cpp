// Smoke tests for src/ui.

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

TEST_CASE("Korean text round-trips through juce::String as UTF-8", "[smoke][ui]")
{
    // The UI ships in English and Korean (P8-05); source files are compiled with /utf-8.
    const juce::String text = juce::String::fromUTF8(reinterpret_cast<const char*>(u8"데빌 DJ"));
    CHECK(text.length() == 5);
    CHECK(juce::String::fromUTF8(text.toRawUTF8()) == text);
}
