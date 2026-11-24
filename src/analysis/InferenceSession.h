#pragma once

// Neural-network inference for analysis jobs (P6-01): an ONNX model run through ONNX Runtime, on the GPU through
// DirectML (any DirectX 12 card) when asked for and available, else on the CPU - the fallback is automatic and
// its reason kept. Float tensors in and out. For analysis threads only (never the audio thread): a session
// allocates, may take long, and may be used from one thread at a time.

#include "util/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace devildj::analysis
{

enum class InferenceDevice
{
    Cpu,
    DirectML,
};

struct InferenceOptions
{
    bool preferGpu = true;   // DirectML when it can be used
    int gpuDevice = 0;       // the DirectX adapter (0 = the default one)
    int cpuThreads = 0;      // 0 = ONNX Runtime's choice
};

struct Tensor
{
    std::vector<std::int64_t> shape;
    std::vector<float> data; // row-major, the product of shape long
};

class InferenceSession
{
public:
    static util::Result<std::unique_ptr<InferenceSession>> open(const std::filesystem::path& model,
                                                                InferenceOptions options = {});
    ~InferenceSession();
    InferenceSession(const InferenceSession&) = delete;
    InferenceSession& operator=(const InferenceSession&) = delete;

    InferenceDevice device() const noexcept;
    const std::string& fallbackReason() const noexcept; // why not DirectML, when asked for it ("" otherwise)
    std::vector<std::string> inputNames() const;
    std::vector<std::string> outputNames() const;

    // The inputs in the model's input order; the outputs in its output order.
    util::Result<std::vector<Tensor>> run(const std::vector<Tensor>& inputs);

    static std::string runtimeVersion(); // e.g. "1.24.4"

private:
    InferenceSession();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace devildj::analysis
