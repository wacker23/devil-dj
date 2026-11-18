// P1-12: headphone cue. Done when: cueing deck B while A plays to master verified in all three output layouts.

#include "engine/AudioEngine.h"
#include "util/RealtimeGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace devildj::engine;
using Catch::Approx;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// DC tracks make each source easy to recognise in a mix: A = 0.2, B = 0.1 (both channels).
std::unique_ptr<TrackBuffer> dcTrack(float value)
{
    auto t = std::make_unique<TrackBuffer>();
    t->sampleRate = kRate;
    t->audio.setSize(2, static_cast<int>(kRate * 4));
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < t->audio.getNumSamples(); ++i)
            t->audio.setSample(ch, i, value);
    return t;
}

struct Rig
{
    std::unique_ptr<TrackBuffer> a = dcTrack(0.2f), b = dcTrack(0.1f);
    AudioEngine engine;
    std::vector<std::vector<float>> out;
    std::vector<float*> ptrs;

    explicit Rig(OutputLayout layout, int channels)
        : out(static_cast<std::size_t>(channels), std::vector<float>(kBlock))
    {
        for (auto& ch : out)
            ptrs.push_back(ch.data());
        engine.prepare(kRate, kBlock);
        engine.send(SetOutputLayout{layout});
        engine.send(LoadTrack{0, a.get(), 1});
        engine.send(LoadTrack{1, b.get(), 2});
        engine.send(SetParam{ParamId::Crossfader, 0.0f}); // master hears deck A only
        engine.send(Play{0});
        engine.send(Play{1});
        engine.send(SetParam{channelParam(1, ChannelParam::Cue), 1.0f}); // CUE on deck B
        engine.send(SetParam{ParamId::CueMix, 0.0f});                    // headphones: cue only
        run(20);
    }

    void run(int blocks)
    {
        for (int i = 0; i < blocks; ++i)
            engine.process(ptrs.data(), static_cast<int>(ptrs.size()), kBlock);
    }
    float at(int channel) const { return out[static_cast<std::size_t>(channel)].back(); }
};
} // namespace

TEST_CASE("Cue deck B while A plays: 4-channel layout", "[engine][cue]")
{
    Rig rig(OutputLayout::FourChannel, 4);
    CHECK(rig.at(0) == Approx(0.2f)); // master L = A
    CHECK(rig.at(1) == Approx(0.2f)); // master R
    CHECK(rig.at(2) == Approx(0.1f)); // cue L = B
    CHECK(rig.at(3) == Approx(0.1f)); // cue R
    CHECK(rig.engine.readState().channels[1].cue);
    CHECK_FALSE(rig.engine.readState().channels[0].cue);

    // Mix knob: centre = both at full level, right = master only.
    rig.engine.send(SetParam{ParamId::CueMix, 0.5f});
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.3f));
    rig.engine.send(SetParam{ParamId::CueMix, 1.0f});
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.2f));
    CHECK(rig.at(0) == Approx(0.2f));                  // master untouched by the headphone controls
    rig.engine.send(SetParam{ParamId::CueMix, 0.75f}); // master full, cue half
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.25f));

    // Headphone level -6 dB scales the headphones only.
    rig.engine.send(SetParam{ParamId::HeadphoneLevel, -6.0f});
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.25 * std::pow(10.0, -6.0 / 20.0)).margin(1e-5));
    CHECK(rig.at(0) == Approx(0.2f));
    CHECK(rig.engine.readState().headphoneLevelDb == -6.0f);
}

TEST_CASE("Cue deck B while A plays: split layout (master mono left, cue mono right)", "[engine][cue]")
{
    Rig rig(OutputLayout::SplitMono, 2);
    CHECK(rig.at(0) == Approx(0.2f)); // left = master (A)
    CHECK(rig.at(1) == Approx(0.1f)); // right = cue (B)
    // The cue knob works in split mode: centre = cue + master in the right ear.
    rig.engine.send(SetParam{ParamId::CueMix, 0.5f});
    rig.run(10);
    CHECK(rig.at(0) == Approx(0.2f));
    CHECK(rig.at(1) == Approx(0.3f));
    rig.engine.send(SetParam{ParamId::CueMix, 1.0f});
    rig.run(10);
    CHECK(rig.at(1) == Approx(0.2f));
}

TEST_CASE("Cue deck B while A plays: stereo layout plays the master only", "[engine][cue]")
{
    Rig rig(OutputLayout::StereoMaster, 2);
    CHECK(rig.at(0) == Approx(0.2f));
    CHECK(rig.at(1) == Approx(0.2f));
    CHECK_FALSE(rig.engine.readState().cueRouted); // the UI says the headphones aren't routed
    // Cue on/off changes nothing on the master.
    rig.engine.send(SetParam{channelParam(1, ChannelParam::Cue), 0.0f});
    rig.run(10);
    CHECK(rig.at(0) == Approx(0.2f));
}

TEST_CASE("Cue is pre-fader and pre-crossfader; cueing several channels sums them", "[engine][cue]")
{
    Rig rig(OutputLayout::FourChannel, 4);
    rig.engine.send(SetParam{channelParam(1, ChannelParam::Fader), 0.0f}); // B fader closed: still in the cue
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.1f));
    rig.engine.send(SetParam{channelParam(0, ChannelParam::Cue), 1.0f}); // A too
    rig.run(10);
    CHECK(rig.at(2) == Approx(0.3f));
    // The EQ is before the cue tap: killing B's low band removes DC from the cue.
    rig.engine.send(SetParam{channelParam(1, ChannelParam::EqMode), 1.0f});
    rig.engine.send(SetParam{channelParam(1, ChannelParam::EqLow), -1000.0f});
    rig.run(40);
    CHECK(rig.at(2) == Approx(0.2f).margin(1e-3));
}

TEST_CASE("Cue switching ramps (no click) and doesn't allocate", "[engine][cue]")
{
    Rig rig(OutputLayout::FourChannel, 4);
    rig.engine.send(SetParam{channelParam(1, ChannelParam::Cue), 0.0f});
    rig.run(10);
    CHECK(rig.at(2) == 0.0f);

    const auto violations = devildj::util::realtimeViolationCount();
    rig.engine.send(SetParam{channelParam(1, ChannelParam::Cue), 1.0f});
    float maxStep = 0.0f, previous = 0.0f;
    for (int b = 0; b < 8; ++b)
    {
        rig.run(1);
        for (float v : rig.out[2])
        {
            maxStep = std::max(maxStep, std::abs(v - previous));
            previous = v;
        }
    }
    CHECK(rig.at(2) == Approx(0.1f));
    CHECK(maxStep < 0.1f / 900.0f); // 0.1 over ~960 samples
    if (devildj::util::kRealtimeGuardEnabled)
        CHECK(devildj::util::realtimeViolationCount() == violations);
}

TEST_CASE("Headphones are limited too", "[engine][cue]")
{
    Rig rig(OutputLayout::FourChannel, 4);
    rig.engine.send(SetParam{ParamId::CueMix, 0.5f});
    rig.engine.send(SetParam{ParamId::HeadphoneLevel, 6.0f});
    rig.engine.send(SetParam{channelParam(1, ChannelParam::Trim), 12.0f});
    float worst = 0.0f;
    for (int b = 0; b < 40; ++b)
    {
        rig.run(1);
        for (float v : rig.out[2])
            worst = std::max(worst, std::abs(v));
    }
    CHECK(worst <= static_cast<float>(std::pow(10.0, -0.3 / 20.0)));
    CHECK(rig.engine.readState().cuePeakL > 0.9f);
}
