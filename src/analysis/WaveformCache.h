#pragma once

#include "analysis/WaveformBuilder.h"

#include <filesystem>
#include <optional>
#include <string>

namespace devildj::analysis
{

// The waveform cache (P2-17; ARCHITECTURE §8): one `<content_hash>.wfm` per track in
// %LOCALAPPDATA%\Devil-DJ\cache, written by the analysis service (and by the loader when it had to build a
// waveform itself), read by the loader as a load starts so the waveform shows before the audio is decoded.
//
// Format (little-endian): magic "DDJW", u16 version, u32 sample rate, u16 points per second, u32 detail points,
// u32 samples per point, u16 overview points; then the detail as 6 u8 arrays (low / mid / high x peak / RMS) and
// the overview as 8 u8 arrays (low / mid / high x peak / RMS, full band peak / RMS). Values are stored as
// sqrt(level) x 255, so quiet passages keep their shape (decoded: (v / 255)^2). ~0.9 KB per second of audio.
inline constexpr std::uint16_t kWaveformCacheVersion = 1;

std::filesystem::path waveformCachePath(const std::filesystem::path& directory, const std::string& contentHash);

// Writes through a temporary file and a rename, so a reader never sees half a file. False on any I/O error.
bool writeWaveformCache(const std::filesystem::path& file, const Waveform& waveform);

// nullopt when the file is missing, not ours, another version, or cut short.
std::optional<Waveform> readWaveformCache(const std::filesystem::path& file);

// Re-labels a waveform for a buffer at another sample rate (a cache written at the file's 44.1 kHz shown on a
// 48 kHz deck): possible when both rates give the same points per second (150 for all the usual rates). False
// (and unchanged) otherwise.
bool retimeWaveform(Waveform& waveform, double sampleRate);

} // namespace devildj::analysis
