// Smoke tests for src/app.

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

TEST_CASE("JUCE 8 core is linked", "[smoke][app]")
{
    CHECK(JUCE_MAJOR_VERSION == 8);
    CHECK(juce::SystemStats::getJUCEVersion().startsWith("JUCE v8"));
}

TEST_CASE("Settings folder location resolves", "[smoke][app]")
{
    // Settings live in %APPDATA%\Devil-DJ\settings.xml (ARCHITECTURE §8).
    const auto appData = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory);
    CHECK(appData.isDirectory());
    CHECK(appData.getChildFile("Devil-DJ").getFullPathName().endsWith("Devil-DJ"));
}
