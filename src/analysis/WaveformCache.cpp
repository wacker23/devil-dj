#include "WaveformCache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <vector>

namespace devildj::analysis
{

namespace
{
constexpr char kMagic[4] = {'D', 'D', 'J', 'W'};
constexpr std::size_t kHeaderBytes = 4 + 2 + 4 + 2 + 4 + 4 + 2;

std::uint8_t encode(float level)
{
    const double v = std::sqrt(std::clamp(static_cast<double>(level), 0.0, 1.0));
    return static_cast<std::uint8_t>(std::lround(v * 255.0));
}

float decode(std::uint8_t v)
{
    const double x = v / 255.0;
    return static_cast<float>(x * x);
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    put16(out, v & 0xffff);
    put16(out, v >> 16);
}

std::uint32_t get(const std::vector<std::uint8_t>& in, std::size_t at, int bytes)
{
    std::uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; --i)
        v = (v << 8) | in[at + static_cast<std::size_t>(i)];
    return v;
}

void putArray(std::vector<std::uint8_t>& out, const std::vector<float>& values, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(i < values.size() ? encode(values[i]) : 0);
}

std::vector<float> getArray(const std::vector<std::uint8_t>& in, std::size_t& at, std::size_t count)
{
    std::vector<float> v(count);
    for (std::size_t i = 0; i < count; ++i)
        v[i] = decode(in[at + i]);
    at += count;
    return v;
}
} // namespace

std::filesystem::path waveformCachePath(const std::filesystem::path& directory, const std::string& contentHash)
{
    return directory / (contentHash + ".wfm");
}

bool writeWaveformCache(const std::filesystem::path& file, const Waveform& w)
{
    if (w.empty() || w.samplesPerPoint <= 0 || w.sampleRate <= 0.0)
        return false;
    const auto count = static_cast<std::size_t>(w.numPoints());
    const auto overview = w.overviewPeak[0].size();
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderBytes + 6 * count + 8 * overview);
    out.insert(out.end(), kMagic, kMagic + 4);
    put16(out, kWaveformCacheVersion);
    put32(out, static_cast<std::uint32_t>(std::lround(w.sampleRate)));
    put16(out, static_cast<std::uint32_t>(std::lround(Waveform::kPointsPerSecond)));
    put32(out, static_cast<std::uint32_t>(count));
    put32(out, static_cast<std::uint32_t>(w.samplesPerPoint));
    put16(out, static_cast<std::uint32_t>(overview));
    for (int b = 0; b < Waveform::kBands; ++b)
    {
        putArray(out, w.peak[static_cast<std::size_t>(b)], count);
        putArray(out, w.rms[static_cast<std::size_t>(b)], count);
    }
    for (int b = 0; b < Waveform::kBands; ++b)
    {
        putArray(out, w.overviewPeak[static_cast<std::size_t>(b)], overview);
        putArray(out, w.overviewRms[static_cast<std::size_t>(b)], overview);
    }
    putArray(out, w.overviewTotalPeak, overview);
    putArray(out, w.overviewTotalRms, overview);

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream f(temporary, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        if (!f)
            return false;
    }
    std::filesystem::rename(temporary, file, ec);
    if (ec)
    {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::optional<Waveform> readWaveformCache(const std::filesystem::path& file)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size < kHeaderBytes || size > (64u << 20))
        return std::nullopt;
    std::ifstream f(file, std::ios::binary);
    if (!f)
        return std::nullopt;
    std::vector<std::uint8_t> in(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(in.data()), static_cast<std::streamsize>(in.size())))
        return std::nullopt;
    if (in.size() < kHeaderBytes || !std::equal(kMagic, kMagic + 4, in.begin()))
        return std::nullopt;
    if (get(in, 4, 2) != kWaveformCacheVersion)
        return std::nullopt;
    const auto rate = get(in, 6, 4);
    const auto pointsPerSecond = get(in, 10, 2);
    const auto count = static_cast<std::size_t>(get(in, 12, 4));
    const auto samplesPerPoint = get(in, 16, 4);
    const auto overview = static_cast<std::size_t>(get(in, 20, 2));
    const bool usualRate = static_cast<long>(pointsPerSecond) == std::lround(Waveform::kPointsPerSecond);
    if (rate == 0 || samplesPerPoint == 0 || count == 0 || !usualRate ||
        in.size() != kHeaderBytes + 6 * count + 8 * overview)
        return std::nullopt;

    Waveform w;
    w.sampleRate = rate;
    w.samplesPerPoint = static_cast<int>(samplesPerPoint);
    std::size_t at = kHeaderBytes;
    for (int b = 0; b < Waveform::kBands; ++b)
    {
        w.peak[static_cast<std::size_t>(b)] = getArray(in, at, count);
        w.rms[static_cast<std::size_t>(b)] = getArray(in, at, count);
    }
    for (int b = 0; b < Waveform::kBands; ++b)
    {
        w.overviewPeak[static_cast<std::size_t>(b)] = getArray(in, at, overview);
        w.overviewRms[static_cast<std::size_t>(b)] = getArray(in, at, overview);
    }
    w.overviewTotalPeak = getArray(in, at, overview);
    w.overviewTotalRms = getArray(in, at, overview);
    return w;
}

bool retimeWaveform(Waveform& w, double sampleRate)
{
    if (w.samplesPerPoint <= 0 || sampleRate <= 0.0)
        return false;
    const int samplesPerPoint = static_cast<int>(std::lround(sampleRate / Waveform::kPointsPerSecond));
    if (samplesPerPoint <= 0 || std::abs(sampleRate / samplesPerPoint - w.pointsPerSecond()) > 1e-9)
        return false;
    w.sampleRate = sampleRate;
    w.samplesPerPoint = samplesPerPoint;
    return true;
}

} // namespace devildj::analysis
