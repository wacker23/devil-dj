#include "util/CrashHandler.h"
#include "util/Log.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

namespace
{
constexpr const char* kCrashDirEnv = "DEVILDJ_CRASH_TEST_DIR";

fs::path makeTempDir(const char* prefix)
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto p = fs::temp_directory_path() / (std::string(prefix) + std::to_string(stamp));
    fs::create_directories(p);
    return p;
}

std::string readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path findDump(const fs::path& dir)
{
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".dmp")
            return e.path();
    return {};
}

bool hasMinidumpSignature(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    char magic[4] = {};
    in.read(magic, 4);
    return std::string(magic, 4) == "MDMP";
}
} // namespace

#if defined(_WIN32)

TEST_CASE("writeMinidump writes a valid dump without crashing", "[util][CrashHandler]")
{
    const auto dir = makeTempDir("devildj-dumptest-");
    const auto dump = devildj::util::writeMinidump(dir);
    REQUIRE_FALSE(dump.empty());
    CHECK(fs::exists(dump));
    CHECK(dump.filename().string().starts_with("devil-dj-crash-"));
    CHECK(fs::file_size(dump) > 1'000);
    CHECK(hasMinidumpSignature(dump));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Runs this test executable again with the hidden "[.crash-child]" test, which really crashes.
TEST_CASE("A real crash writes a minidump and a FATAL log line", "[util][CrashHandler]")
{
    const auto dir = makeTempDir("devildj-crashtest-");
    REQUIRE(_putenv_s(kCrashDirEnv, dir.string().c_str()) == 0); // inherited by the child

    const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::ChildProcess child;
    REQUIRE(child.start(juce::StringArray{self.getFullPathName(), "[.crash-child]"}, 0));
    REQUIRE(child.waitForProcessToFinish(60'000));
    const auto exitCode = child.getExitCode();
    _putenv_s(kCrashDirEnv, "");

    CHECK(exitCode == 0xC0000005u); // process ended by the access violation, not a normal exit

    const auto dump = findDump(dir);
    REQUIRE_FALSE(dump.empty());
    CHECK(hasMinidumpSignature(dump));

    const auto log = readAll(dir / "devil-dj.log");
    CHECK(log.find("crash child started") != std::string::npos);
    CHECK(log.find("[FATAL]") != std::string::npos);
    CHECK(log.find("0xC0000005") != std::string::npos);
    CHECK(log.find(dump.filename().string()) != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif

// Hidden: only run as the child of the test above. Crashes on a worker thread.
TEST_CASE("Crash child", "[.crash-child]")
{
    const auto dirText = juce::SystemStats::getEnvironmentVariable(kCrashDirEnv, {}).toStdString();
    REQUIRE_FALSE(dirText.empty());

    devildj::util::LogOptions options;
    options.directory = dirText;
    REQUIRE(devildj::util::Log::start(options));
    REQUIRE(devildj::util::installCrashHandler({fs::path(dirText)}));
    devildj::util::logInfo("crash child started");
    devildj::util::Log::flush();

    std::thread worker([] { devildj::util::crashForTesting(); });
    worker.join(); // never returns
}
