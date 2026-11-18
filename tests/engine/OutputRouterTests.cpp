#include "engine/OutputRouter.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <string>
#include <vector>

using devildj::engine::OutputLayout;
using devildj::engine::routeOutputs;

namespace
{
constexpr int kN = 8;

// Distinct constant signals per bus channel so we can see where each one lands.
struct Buses
{
    std::array<float, kN> mL{}, mR{}, cL{}, cR{};
    Buses()
    {
        mL.fill(0.1f);
        mR.fill(0.3f);
        cL.fill(0.5f);
        cR.fill(0.7f);
    }
};

struct Outputs
{
    explicit Outputs(int channels)
        : data(static_cast<std::size_t>(channels), std::vector<float>(kN, 9.0f)) // garbage to be overwritten
    {
        for (auto& ch : data)
            ptrs.push_back(ch.data());
    }
    float at(int ch) const { return data[static_cast<std::size_t>(ch)][kN - 1]; }
    std::vector<std::vector<float>> data;
    std::vector<float*> ptrs;
};

void route(OutputLayout layout, Outputs& out)
{
    Buses b;
    routeOutputs(layout, b.mL.data(), b.mR.data(), b.cL.data(), b.cR.data(), out.ptrs.data(),
                 static_cast<int>(out.ptrs.size()), kN);
}

bool near(float a, float b)
{
    return std::abs(a - b) < 1e-6f;
}
} // namespace

TEST_CASE("Stereo layout: master on 1/2, cue dropped, extra outputs silent", "[engine][OutputRouter]")
{
    Outputs out(4);
    route(OutputLayout::StereoMaster, out);
    CHECK(near(out.at(0), 0.1f));
    CHECK(near(out.at(1), 0.3f));
    CHECK(out.at(2) == 0.0f);
    CHECK(out.at(3) == 0.0f);
}

TEST_CASE("4-channel layout: master on 1/2, cue on 3/4", "[engine][OutputRouter]")
{
    Outputs out(4);
    route(OutputLayout::FourChannel, out);
    CHECK(near(out.at(0), 0.1f));
    CHECK(near(out.at(1), 0.3f));
    CHECK(near(out.at(2), 0.5f));
    CHECK(near(out.at(3), 0.7f));
}

TEST_CASE("4-channel layout on a stereo device plays the master only", "[engine][OutputRouter]")
{
    Outputs out(2);
    route(OutputLayout::FourChannel, out);
    CHECK(near(out.at(0), 0.1f));
    CHECK(near(out.at(1), 0.3f));
    CHECK_FALSE(devildj::engine::cueIsRouted(OutputLayout::FourChannel, 2));
    CHECK(devildj::engine::cueIsRouted(OutputLayout::FourChannel, 4));
}

TEST_CASE("Split layout: master mono in left, cue mono in right", "[engine][OutputRouter]")
{
    Outputs out(2);
    route(OutputLayout::SplitMono, out);
    CHECK(near(out.at(0), 0.5f * (0.1f + 0.3f)));
    CHECK(near(out.at(1), 0.5f * (0.5f + 0.7f)));
    CHECK(devildj::engine::cueIsRouted(OutputLayout::SplitMono, 2));

    Outputs wide(6); // extra outputs stay silent
    route(OutputLayout::SplitMono, wide);
    for (int ch = 2; ch < 6; ++ch)
        CHECK(wide.at(ch) == 0.0f);
}

TEST_CASE("Mono device plays the master summed to mono in every layout", "[engine][OutputRouter]")
{
    for (auto layout : {OutputLayout::StereoMaster, OutputLayout::FourChannel, OutputLayout::SplitMono})
    {
        Outputs out(1);
        route(layout, out);
        CHECK(near(out.at(0), 0.2f));
    }
}

TEST_CASE("Split mono sum cannot clip a full-scale stereo signal", "[engine][OutputRouter]")
{
    std::array<float, kN> one{}, minusOne{};
    one.fill(1.0f);
    minusOne.fill(-1.0f);
    Outputs out(2);
    routeOutputs(OutputLayout::SplitMono, one.data(), one.data(), minusOne.data(), minusOne.data(), out.ptrs.data(), 2,
                 kN);
    CHECK(out.at(0) == 1.0f);
    CHECK(out.at(1) == -1.0f);
}

TEST_CASE("Router skips null output pointers", "[engine][OutputRouter]")
{
    Buses b;
    std::vector<float> right(kN, 9.0f);
    float* outs[] = {nullptr, right.data()};
    routeOutputs(OutputLayout::StereoMaster, b.mL.data(), b.mR.data(), b.cL.data(), b.cR.data(), outs, 2, kN);
    CHECK(near(right[kN - 1], 0.3f));
}

TEST_CASE("Layout helpers", "[engine][OutputRouter]")
{
    CHECK(devildj::engine::outputChannelsFor(OutputLayout::StereoMaster) == 2);
    CHECK(devildj::engine::outputChannelsFor(OutputLayout::FourChannel) == 4);
    CHECK(devildj::engine::outputChannelsFor(OutputLayout::SplitMono) == 2);
    CHECK_FALSE(devildj::engine::cueIsRouted(OutputLayout::StereoMaster, 8));
    CHECK(std::string(devildj::engine::outputLayoutName(OutputLayout::SplitMono)).find("Split") == 0);
}
