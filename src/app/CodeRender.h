#pragma once

// Code deck render (P5-17): N cycles of a buffer's code rendered offline - faster than real time - through a private
// AudioEngine of its own, fed by a CodeDeckProducer of its own on a simulated clock that ticks every ~25 ms of audio,
// as playback does: the same evaluation, windows, sounds, voices, sends, channel and master path. With the same
// block size the result is sample for sample what playback of the same code produces.
//
// Runs the JavaScript, so call it on a thread with a big stack (QuickJS: see codebeat::JsRuntime), never on the
// message thread or the audio thread. Sample folders' files are decoded here, for this render only.

#include "engine/TrackBuffer.h"

#include <juce_core/juce_core.h>

#include <memory>
#include <string>
#include <vector>

namespace devildj::app
{

struct CodeRenderOptions
{
    int cycles = 4;
    double bpm = 120.0;          // the tempo (the master's when called from the app)
    double sampleRate = 48000.0;
    int blockSize = 256;
    double tailSeconds = 2.0;    // after the end, for decays and the sends (0 for a seamless loop)
    juce::File samplesFolder;    // the sound bank's folder (Documents\Devil-DJ\Samples in the app)
};

struct CodeRenderResult
{
    std::unique_ptr<engine::TrackBuffer> audio; // stereo, the private engine's master; null when the code failed
    std::string error;                          // the code's error (nothing rendered)
    std::vector<std::string> warnings;          // unknown sounds and the like
    double bpm = 120.0;
    double beatsPerCycle = 4.0; // setcpb() in the code, else 4
    int loopSamples = 0;        // the cycles' length (without the tail)
};

CodeRenderResult renderCode(const std::string& code, const CodeRenderOptions& options);

} // namespace devildj::app
