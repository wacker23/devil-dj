// P6-01: ONNX Runtime with DirectML. Done when: a toy model runs on CPU and DirectML.

#include "analysis/InferenceSession.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using devildj::analysis::InferenceDevice;
using devildj::analysis::InferenceOptions;
using devildj::analysis::InferenceSession;
using devildj::analysis::Tensor;

namespace
{
// y = x * 2 + 1 over float[1, 8] (tools/make_toy_onnx.py).
const std::filesystem::path kToy = std::filesystem::path(DEVILDJ_SOURCE_DIR) / "tests" / "data" / "toy_scale_add.onnx";

void checkToy(InferenceSession& s)
{
    CHECK(s.inputNames() == std::vector<std::string>{"x"});
    CHECK(s.outputNames() == std::vector<std::string>{"y"});
    const Tensor x{{1, 8}, {0.0f, 1.0f, -1.0f, 0.5f, 10.0f, -3.25f, 100.0f, 1e-3f}};
    const auto r = s.run({x});
    INFO((r ? std::string() : r.error()));
    REQUIRE(r);
    REQUIRE(r.value().size() == 1);
    const auto& y = r.value()[0];
    CHECK(y.shape == std::vector<std::int64_t>{1, 8});
    REQUIRE(y.data.size() == 8);
    for (std::size_t i = 0; i < 8; ++i)
        CHECK(y.data[i] == x.data[i] * 2.0f + 1.0f);
}
} // namespace

TEST_CASE("Inference: the toy model runs on the CPU", "[analysis][inference]")
{
    InferenceOptions o;
    o.preferGpu = false;
    auto s = InferenceSession::open(kToy, o);
    INFO((s ? std::string() : s.error()));
    REQUIRE(s);
    CHECK(s.value()->device() == InferenceDevice::Cpu);
    CHECK(s.value()->fallbackReason().empty());
    checkToy(*s.value());
    WARN("ONNX Runtime " << InferenceSession::runtimeVersion());
    CHECK(InferenceSession::runtimeVersion() == "1.24.4");
}

TEST_CASE("Inference: the toy model runs on DirectML (or says why it fell back to the CPU)", "[analysis][inference]")
{
    auto s = InferenceSession::open(kToy); // the GPU preferred
    INFO((s ? std::string() : s.error()));
    REQUIRE(s);
    // Every Windows 10/11 machine with a DirectX 12 GPU (integrated ones too) runs it on DirectML; elsewhere the
    // CPU fallback is taken and its reason kept.
    if (s.value()->device() == InferenceDevice::DirectML)
        WARN("toy model on DirectML");
    else
        WARN("DirectML unavailable here, on the CPU: " << s.value()->fallbackReason());
    CHECK((s.value()->device() == InferenceDevice::DirectML || !s.value()->fallbackReason().empty()));
    checkToy(*s.value());
    // A second session on the same device, and runs reusing it.
    for (int i = 0; i < 3; ++i)
        checkToy(*s.value());
}

TEST_CASE("Inference: errors - no model, a broken model, wrong inputs", "[analysis][inference]")
{
    CHECK_FALSE(InferenceSession::open(kToy.parent_path() / "no-such-model.onnx"));
    const auto broken = std::filesystem::temp_directory_path() / "devildj-broken.onnx";
    std::ofstream(broken, std::ios::binary) << "not a model";
    auto b = InferenceSession::open(broken, {false});
    CHECK_FALSE(b);
    if (!b)
        CHECK(b.error().find("didn't load") != std::string::npos);
    std::filesystem::remove(broken);

    auto s = InferenceSession::open(kToy, {false});
    REQUIRE(s);
    CHECK_FALSE(s.value()->run({}));                                   // no input
    CHECK_FALSE(s.value()->run({Tensor{{1, 8}, {1.0f, 2.0f}}}));        // values don't fill the shape
    CHECK_FALSE(s.value()->run({Tensor{{1, 4}, {1.0f, 2.0f, 3.0f, 4.0f}}})); // the wrong shape for the model
}
