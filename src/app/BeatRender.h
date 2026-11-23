#pragma once

// Beat Maker export (P4-12): the pattern or the song rendered offline - faster than real time - through a private
// AudioEngine of its own: the same voices, sends, channel and master path as playback, fed the sequencer's events at
// the same samples, block by block. With the same block size the result is bit for bit what playback produces.

#include "beatmaker/Model.h"
#include "engine/TrackBuffer.h"

#include <memory>

namespace devildj::app
{

struct BeatRenderOptions
{
    bool song = false;  // the song list (once round); else `pattern`
    int pattern = 0;
    int passes = 1;     // times through the pattern
    double tailSeconds = 2.0; // after the end, for the sounds' decay and the sends (0 for a seamless loop)
    double masterBpm = 120.0; // the tempo when the project follows the master
    double sampleRate = 48000.0;
    int blockSize = 256;
};

struct BeatRenderResult
{
    std::unique_ptr<engine::TrackBuffer> audio; // stereo, the master of the private engine
    double bpm = 120.0;
    double beats = 0.0;  // the musical length (without the tail)
    int loopSamples = 0; // the same in samples
};

BeatRenderResult renderBeat(const beatmaker::Project& project, const BeatRenderOptions& options);

// The musical length of what would be rendered, in beats.
double renderBeats(const beatmaker::Project& project, const BeatRenderOptions& options);

} // namespace devildj::app
