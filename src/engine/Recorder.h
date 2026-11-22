#pragma once

#include "engine/RecordTap.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace devildj::engine
{

// Mix recording (P3-16; ARCHITECTURE §3.9): a writer thread that drains the RecordTap into 24-bit WAV or FLAC
// files. Optional auto start (the file begins at the first sample above -60 dBFS) and auto split (after 20 s
// under it the file is closed; the next sound starts a new one). Nothing here runs on the audio thread.
class Recorder final : private juce::Thread
{
public:
    enum class Format
    {
        Wav24,
        Flac24,
    };
    struct Options
    {
        juce::File folder;
        Format format = Format::Wav24;
        double sampleRate = 48000.0;
        bool autoStart = false;
        bool autoSplit = false;
        double silenceSeconds = 20.0;
        float threshold = 0.001f; // -60 dBFS
    };
    struct FileInfo
    {
        juce::File file;
        juce::Time started;        // wall clock at its first sample
        std::uint64_t frames = 0;  // samples per channel written
    };

    explicit Recorder(RecordTap& tap);
    ~Recorder() override;

    // Message thread. start: false if already running or the folder can't be made. stop: waits for what the tap
    // still holds to be written, then closes the file.
    bool start(const Options& options);
    void stop();
    bool isRunning() const noexcept { return isThreadRunning(); }

    // Status, any thread.
    bool isWriting() const noexcept { return writing.load(); }       // a file is open
    bool isWaiting() const noexcept { return isRunning() && !writing.load(); } // for sound (auto start / split)
    std::uint64_t framesInFile() const noexcept { return fileFrames.load(); }
    std::uint64_t framesTotal() const noexcept { return totalFrames.load(); }
    int filesWritten() const noexcept { return files.load(); }
    juce::File currentFile() const;

    // Called on the writer thread when a file is finished (stop, or an auto split).
    std::function<void(const FileInfo&)> onFileClosed;

    static juce::String extension(Format format) { return format == Format::Flac24 ? ".flac" : ".wav"; }

private:
    void run() override;
    void process(const float* left, const float* right, int n);
    bool openFile(int samplesBehind);
    void closeFile();

    RecordTap& tap;
    Options options;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    FileInfo info;
    mutable std::mutex fileMutex; // guards info.file for currentFile()
    std::atomic<bool> writing{false};
    std::atomic<std::uint64_t> fileFrames{0}, totalFrames{0};
    std::atomic<int> files{0};
    std::uint64_t silentRun = 0;
};

} // namespace devildj::engine
