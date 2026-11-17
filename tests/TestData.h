#pragma once

#include <juce_core/juce_core.h>

// Access to the generated test signals (tools/gen_test_signals.py, task P0-13).
// ctest generates them first (fixture "test_signals"); when running DevilDJTests.exe directly, run
//   python tools/gen_test_signals.py
// once, or tests that need them are skipped.

#ifndef DEVILDJ_TEST_DATA_DIR
#error "DEVILDJ_TEST_DATA_DIR must be defined by tests/CMakeLists.txt"
#endif

namespace devildj::test
{

inline juce::File generatedDataDir()
{
    return juce::File(juce::String::fromUTF8(DEVILDJ_TEST_DATA_DIR));
}

inline juce::File generatedFile(const juce::String& relativePath)
{
    return generatedDataDir().getChildFile(relativePath);
}

// Parsed manifest.json, or a void var if the signals haven't been generated.
inline juce::var generatedManifest()
{
    const auto file = generatedFile("manifest.json");
    return file.existsAsFile() ? juce::JSON::parse(file) : juce::var();
}

} // namespace devildj::test
