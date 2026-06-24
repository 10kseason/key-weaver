#pragma once

#include <keyconv/chart.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace keyconv::onnx {

constexpr int kLanePolicyFeatureCount = 12;

struct LanePolicyRequest {
    const Chart* chart = nullptr;
    int sourceKeyCount = 0;
    int targetKeyCount = 0;
};

struct LanePolicyOptions {
    std::filesystem::path modelPath;
    int cudaDeviceId = 0;
    std::size_t maxBatchNotes = 0;
    bool disableCpuFallback = false;
};

struct LanePolicyResult {
    std::vector<std::vector<int>> lanesByChart;
    std::size_t totalNotes = 0;
    std::size_t runCount = 0;
    std::string inputName;
    std::string outputName;
};

LanePolicyResult runCudaLanePolicy(const std::vector<LanePolicyRequest>& requests,
                                   const LanePolicyOptions& options);

}  // namespace keyconv::onnx
