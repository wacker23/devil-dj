#include "util/Log.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using devildj::util::Log;
using devildj::util::LogOptions;

namespace
{
// Fresh temporary log directory, removed afterwards. Stops the logger so the files can be deleted.
struct TempLogDir
{
    TempLogDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("devildj-logtest-" + std::to_string(stamp));
    }
    ~TempLogDir()
    {
        Log::stop();
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    LogOptions options() const
    {
        LogOptions o;
        o.directory = path;
        return o;
    }

    fs::path path;
};

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::size_t countLines(const std::string& s)
{
    std::size_t n = 0;
    for (char c : s)
        n += c == '\n' ? 1 : 0;
    return n;
}
} // namespace

TEST_CASE("Log writes formatted lines to <dir>/devil-dj.log", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));
    CHECK(Log::currentFile() == dir.path / "devil-dj.log");

    devildj::util::logInfo("hello {}", 42);
    devildj::util::logError("bad thing: {}", "disk full");
    Log::flush();

    const auto text = readAll(Log::currentFile());
    CHECK(text.find("[INFO ]") != std::string::npos);
    CHECK(text.find("hello 42") != std::string::npos);
    CHECK(text.find("[ERROR]") != std::string::npos);
    CHECK(text.find("bad thing: disk full") != std::string::npos);
    CHECK(countLines(text) == 2);
}

TEST_CASE("Log keeps messages logged before start", "[util][Log]")
{
    TempLogDir dir;
    devildj::util::logInfo("early bird");
    REQUIRE(Log::start(dir.options()));
    Log::flush();
    CHECK(readAll(Log::currentFile()).find("early bird") != std::string::npos);
}

TEST_CASE("Log stop writes everything still queued", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));
    for (int i = 0; i < 500; ++i)
        devildj::util::logDebug("line {}", i);
    Log::stop();
    const auto text = readAll(dir.path / "devil-dj.log");
    CHECK(countLines(text) == 500);
    CHECK(text.find("line 499") != std::string::npos);
}

TEST_CASE("Log starts a new file each session and keeps the previous one", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));
    devildj::util::logInfo("session one");
    Log::stop();

    REQUIRE(Log::start(dir.options()));
    devildj::util::logInfo("session two");
    Log::flush();

    CHECK(readAll(dir.path / "devil-dj.log").find("session two") != std::string::npos);
    CHECK(readAll(dir.path / "devil-dj.log").find("session one") == std::string::npos);
    CHECK(readAll(dir.path / "devil-dj.1.log").find("session one") != std::string::npos);
}

TEST_CASE("Log rotates by size and keeps at most maxFiles", "[util][Log]")
{
    TempLogDir dir;
    auto options = dir.options();
    options.maxFileBytes = 2'000;
    options.maxFiles = 3;
    REQUIRE(Log::start(options));

    const std::string payload(100, 'x');
    for (int i = 0; i < 200; ++i)
        devildj::util::logInfo("{} {}", i, payload);
    Log::flush();

    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir.path))
        files.push_back(entry.path());

    CHECK(files.size() == 3);
    CHECK(fs::exists(dir.path / "devil-dj.log"));
    CHECK(fs::exists(dir.path / "devil-dj.1.log"));
    CHECK(fs::exists(dir.path / "devil-dj.2.log"));
    for (const auto& f : files)
        CHECK(fs::file_size(f) <= options.maxFileBytes);
    CHECK(readAll(dir.path / "devil-dj.log").find("199 ") != std::string::npos); // newest in current file
}

TEST_CASE("Log from several threads loses nothing", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));

    constexpr int kThreads = 4;
    constexpr int kLinesEach = 1'000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back(
            [t]
            {
                for (int i = 0; i < kLinesEach; ++i)
                    devildj::util::logInfo("thread {} line {}", t, i);
            });
    for (auto& th : threads)
        th.join();
    Log::flush();

    CHECK(countLines(readAll(Log::currentFile())) == kThreads * kLinesEach);
}

TEST_CASE("Log drops messages from a real-time context", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));
    const auto droppedBefore = Log::droppedMessages();
    {
        devildj::util::ScopedRealtimeContext rt;
        devildj::util::logInfo("from the audio thread {}", 1); // dropped before formatting: no allocation
    }
    Log::flush();
    CHECK(Log::droppedMessages() - droppedBefore == (devildj::util::kRealtimeGuardEnabled ? 1u : 0u));
    if (devildj::util::kRealtimeGuardEnabled)
        CHECK(readAll(Log::currentFile()).find("audio thread") == std::string::npos);
}

TEST_CASE("Log::writeEmergency appends directly", "[util][Log]")
{
    TempLogDir dir;
    REQUIRE(Log::start(dir.options()));
    Log::writeEmergency("emergency line\n");
    Log::stop();
    CHECK(readAll(dir.path / "devil-dj.log").find("emergency line") != std::string::npos);
}
