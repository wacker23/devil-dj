#include "analysis/InferenceSession.h"

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <numeric>

namespace devildj::analysis
{

namespace
{
// One ONNX Runtime environment for the process (made on first use; it is thread-safe).
Ort::Env& environment()
{
    // Errors only: its warnings (e.g. "shape ops run on the CPU" under DirectML) are expected and not ours to fix.
    static Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "devil-dj");
    return env;
}

std::int64_t elements(const std::vector<std::int64_t>& shape)
{
    return std::accumulate(shape.begin(), shape.end(), std::int64_t{1}, [](std::int64_t a, std::int64_t b) { return a * b; });
}
} // namespace

struct InferenceSession::Impl
{
    std::unique_ptr<Ort::Session> session;
    InferenceDevice device = InferenceDevice::Cpu;
    std::string fallbackReason;
    std::vector<std::string> inputs, outputs;
};

InferenceSession::InferenceSession()
    : impl(std::make_unique<Impl>())
{
}

InferenceSession::~InferenceSession() = default;

util::Result<std::unique_ptr<InferenceSession>> InferenceSession::open(const std::filesystem::path& model, InferenceOptions o)
{
    using R = util::Result<std::unique_ptr<InferenceSession>>;
    if (!std::filesystem::exists(model))
        return R::fail("no model at " + model.string());
    std::unique_ptr<InferenceSession> s(new InferenceSession());
    const auto make = [&](bool gpu) -> std::unique_ptr<Ort::Session>
    {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(o.fullOptimization ? GraphOptimizationLevel::ORT_ENABLE_ALL
                                                             : GraphOptimizationLevel::ORT_ENABLE_BASIC);
        if (o.cpuThreads > 0)
            options.SetIntraOpNumThreads(o.cpuThreads);
        if (gpu)
        {
            // DirectML wants memory patterns off and sequential execution (its provider's documented needs).
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, o.gpuDevice));
        }
        return std::make_unique<Ort::Session>(environment(), model.c_str(), options);
    };
    try
    {
        if (o.preferGpu)
        {
            try
            {
                s->impl->session = make(true);
                s->impl->device = InferenceDevice::DirectML;
            }
            catch (const Ort::Exception& e)
            {
                s->impl->fallbackReason = std::string("DirectML not used: ") + e.what();
            }
        }
        if (s->impl->session == nullptr)
        {
            s->impl->session = make(false);
            s->impl->device = InferenceDevice::Cpu;
        }
        Ort::AllocatorWithDefaultOptions alloc;
        for (std::size_t i = 0; i < s->impl->session->GetInputCount(); ++i)
            s->impl->inputs.emplace_back(s->impl->session->GetInputNameAllocated(i, alloc).get());
        for (std::size_t i = 0; i < s->impl->session->GetOutputCount(); ++i)
            s->impl->outputs.emplace_back(s->impl->session->GetOutputNameAllocated(i, alloc).get());
    }
    catch (const Ort::Exception& e)
    {
        return R::fail(std::string("the model didn't load: ") + e.what());
    }
    return R::ok(std::move(s));
}

InferenceDevice InferenceSession::device() const noexcept
{
    return impl->device;
}

const std::string& InferenceSession::fallbackReason() const noexcept
{
    return impl->fallbackReason;
}

std::vector<std::string> InferenceSession::inputNames() const
{
    return impl->inputs;
}

std::vector<std::string> InferenceSession::outputNames() const
{
    return impl->outputs;
}

util::Result<std::vector<Tensor>> InferenceSession::run(const std::vector<Tensor>& inputs)
{
    using R = util::Result<std::vector<Tensor>>;
    if (inputs.size() != impl->inputs.size())
        return R::fail("the model takes " + std::to_string(impl->inputs.size()) + " inputs, got " + std::to_string(inputs.size()));
    try
    {
        const auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        std::vector<Ort::Value> values;
        std::vector<const char*> inNames, outNames;
        for (std::size_t i = 0; i < inputs.size(); ++i)
        {
            const auto& t = inputs[i];
            if (elements(t.shape) != static_cast<std::int64_t>(t.data.size()))
                return R::fail("input " + impl->inputs[i] + ": " + std::to_string(t.data.size()) + " values for its shape");
            // The data isn't changed by a run; ONNX Runtime's API just doesn't take a const pointer.
            values.push_back(Ort::Value::CreateTensor<float>(memory, const_cast<float*>(t.data.data()), t.data.size(),
                                                             t.shape.data(), t.shape.size()));
            inNames.push_back(impl->inputs[i].c_str());
        }
        for (const auto& n : impl->outputs)
            outNames.push_back(n.c_str());
        auto results = impl->session->Run(Ort::RunOptions{nullptr}, inNames.data(), values.data(), values.size(),
                                          outNames.data(), outNames.size());
        std::vector<Tensor> out;
        for (auto& v : results)
        {
            Tensor t;
            const auto info = v.GetTensorTypeAndShapeInfo();
            if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                return R::fail("an output isn't float");
            t.shape = info.GetShape();
            const auto* p = v.GetTensorData<float>();
            t.data.assign(p, p + info.GetElementCount());
            out.push_back(std::move(t));
        }
        return R::ok(std::move(out));
    }
    catch (const Ort::Exception& e)
    {
        return R::fail(std::string("inference failed: ") + e.what());
    }
}

std::string InferenceSession::runtimeVersion()
{
    return Ort::GetVersionString();
}

} // namespace devildj::analysis
