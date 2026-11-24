#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace devildj::dsp
{

// Real-time variable-rate reading of a sample buffer (P1-07; ARCHITECTURE §4 "Resampler").
//
// read(data, length, position, rate) returns the signal at a fractional position. Samples outside
// [0, length) count as silence. `rate` (input samples per output sample) only matters for the sinc mode,
// which narrows its low-pass when playing faster so it doesn't alias.
//
//   Hermite: 4-point cubic Hermite. Cheap, the default for tempo changes without key lock.
//   Sinc16:  16-tap Kaiser-windowed sinc from precomputed tables ("high quality" setting).
//
// prepare() builds the sinc tables (allocates); read() is audio-thread safe (no allocation, no locks).
class Resampler
{
public:
    enum class Quality : std::uint8_t
    {
        Hermite,
        Sinc16,
    };

    void prepare();

    float read(const float* data, int length, double position, double rate) const noexcept
    {
        return quality == Quality::Sinc16 ? readSinc(data, length, position, rate)
                                          : readHermite(data, length, position);
    }

    // 16-bit data (P6-04: stems), read as samples / 32768.
    float read(const std::int16_t* data, int length, double position, double rate) const noexcept
    {
        constexpr float kScale = 1.0f / 32768.0f;
        return kScale * (quality == Quality::Sinc16 ? readSincT(data, length, position, rate)
                                                    : readHermiteT(data, length, position));
    }

    void setQuality(Quality q) noexcept { quality = q; }
    Quality getQuality() const noexcept { return quality; }

    static float readHermite(const float* data, int length, double position) noexcept;

private:
    static constexpr int kTaps = 16;
    static constexpr int kPhases = 256;
    // Kernels for playback rates up to these values (the next one above the current rate is used).
    static constexpr std::array<double, 7> kRateBuckets{1.0, 1.06, 1.1, 1.16, 1.25, 1.5, 2.0};

    float readSinc(const float* data, int length, double position, double rate) const noexcept;
    template <typename T>
    static float readHermiteT(const T* data, int length, double position) noexcept;
    template <typename T>
    float readSincT(const T* data, int length, double position, double rate) const noexcept;

    Quality quality = Quality::Hermite;
    // [bucket][phase 0..kPhases][tap], kTaps per row.
    std::vector<float> sincTables;
};

} // namespace devildj::dsp
