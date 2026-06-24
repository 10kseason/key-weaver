#include "onnx/onnx_cuda_policy.hpp"

#include "core/mapping.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace keyconv::onnx {

namespace {

struct IndexedNote {
    std::size_t index = 0;
    const Note* note = nullptr;
};

struct RowRef {
    std::size_t chartIndex = 0;
    std::size_t noteIndex = 0;
    int targetKeyCount = 0;
};

std::string joinProviders(const std::vector<std::string>& providers) {
    std::ostringstream out;
    for (std::size_t i = 0; i < providers.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << providers[i];
    }
    return out.str();
}

void requireCudaProvider() {
    const auto providers = Ort::GetAvailableProviders();
    if (std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") == providers.end()) {
        throw std::runtime_error("ONNX Runtime CUDAExecutionProvider is unavailable. Available providers: " +
                                 joinProviders(providers));
    }
}

std::vector<IndexedNote> sortedIndexedNotes(const Chart& chart) {
    std::vector<IndexedNote> notes;
    notes.reserve(chart.notes.size());
    for (std::size_t index = 0; index < chart.notes.size(); ++index) {
        notes.push_back({index, &chart.notes[index]});
    }
    std::stable_sort(notes.begin(), notes.end(), [](const IndexedNote& lhs, const IndexedNote& rhs) {
        if (lhs.note->time != rhs.note->time) {
            return lhs.note->time < rhs.note->time;
        }
        return lhs.note->lane < rhs.note->lane;
    });
    return notes;
}

int sourceLaneOf(const Note& note) {
    return note.sourceLane.value_or(note.lane);
}

float normalizedLane(int lane, int keyCount) {
    return static_cast<float>(lane) / static_cast<float>(std::max(1, keyCount - 1));
}

int holdDuration(const Note& note) {
    if (!note.endTime.has_value()) {
        return 0;
    }
    return std::max(0, *note.endTime - note.time);
}

std::map<int, int> chordSizes(const std::vector<IndexedNote>& notes) {
    std::map<int, int> sizes;
    for (const auto& item : notes) {
        ++sizes[item.note->time];
    }
    return sizes;
}

std::vector<int> sortedHoldStarts(const std::vector<IndexedNote>& notes) {
    std::vector<int> starts;
    for (const auto& item : notes) {
        if (item.note->type == NoteType::Hold && item.note->endTime.has_value()) {
            starts.push_back(item.note->time);
        }
    }
    std::sort(starts.begin(), starts.end());
    return starts;
}

std::vector<int> sortedHoldEnds(const std::vector<IndexedNote>& notes) {
    std::vector<int> ends;
    for (const auto& item : notes) {
        if (item.note->type == NoteType::Hold && item.note->endTime.has_value()) {
            ends.push_back(*item.note->endTime);
        }
    }
    std::sort(ends.begin(), ends.end());
    return ends;
}

int activeHoldsAt(const std::vector<int>& holdStarts,
                  const std::vector<int>& holdEnds,
                  int time) {
    const auto started = std::distance(holdStarts.begin(), std::lower_bound(holdStarts.begin(), holdStarts.end(), time));
    const auto ended = std::distance(holdEnds.begin(), std::lower_bound(holdEnds.begin(), holdEnds.end(), time));
    return std::max(0, static_cast<int>(started - ended));
}

void appendFeaturesForChart(const LanePolicyRequest& request,
                            std::size_t chartIndex,
                            std::vector<float>& features,
                            std::vector<RowRef>& rows) {
    if (request.chart == nullptr) {
        throw std::runtime_error("ONNX lane policy request has a null chart");
    }
    if (request.sourceKeyCount < 1 || request.targetKeyCount < 1) {
        throw std::runtime_error("ONNX lane policy requires positive source and target key counts");
    }

    const auto notes = sortedIndexedNotes(*request.chart);
    if (notes.empty()) {
        return;
    }

    const auto sizes = chordSizes(notes);
    const auto holdStarts = sortedHoldStarts(notes);
    const auto holdEnds = sortedHoldEnds(notes);
    const int minTime = notes.front().note->time;
    int maxTime = minTime;
    for (const auto& item : notes) {
        maxTime = std::max(maxTime, item.note->endTime.value_or(item.note->time));
    }
    const int duration = std::max(1, maxTime - minTime);

    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& note = *notes[i].note;
        const int sourceLane = sourceLaneOf(note);
        const int previousGap = i == 0 ? 2000 : std::max(0, note.time - notes[i - 1].note->time);
        const int nextGap = i + 1 >= notes.size() ? 2000 : std::max(0, notes[i + 1].note->time - note.time);
        const int direct = mapLaneDirect(sourceLane, request.sourceKeyCount, request.targetKeyCount);
        const int activeHolds = activeHoldsAt(holdStarts, holdEnds, note.time);
        const auto sizeIt = sizes.find(note.time);
        const int chordSize = sizeIt == sizes.end() ? 1 : sizeIt->second;

        const std::array<float, kLanePolicyFeatureCount> row{
            normalizedLane(sourceLane, request.sourceKeyCount),
            normalizedLane(direct, request.targetKeyCount),
            static_cast<float>(request.sourceKeyCount) / 32.0f,
            static_cast<float>(request.targetKeyCount) / 32.0f,
            static_cast<float>(note.time - minTime) / static_cast<float>(duration),
            std::min(1.0f, static_cast<float>(previousGap) / 2000.0f),
            std::min(1.0f, static_cast<float>(nextGap) / 2000.0f),
            std::min(1.0f, static_cast<float>(chordSize) / static_cast<float>(std::max(1, request.sourceKeyCount))),
            note.type == NoteType::Hold ? 1.0f : 0.0f,
            std::min(1.0f, static_cast<float>(holdDuration(note)) / 4000.0f),
            static_cast<float>(sourceLane) < static_cast<float>(request.sourceKeyCount) / 2.0f ? 0.0f : 1.0f,
            std::min(1.0f, static_cast<float>(activeHolds) / static_cast<float>(std::max(1, request.sourceKeyCount))),
        };
        features.insert(features.end(), row.begin(), row.end());
        rows.push_back({chartIndex, notes[i].index, request.targetKeyCount});
    }
}

void validateInputContract(const Ort::Session& session, const std::string& inputName) {
    auto typeInfo = session.GetInputTypeInfo(0);
    auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        throw std::runtime_error("ONNX lane policy input '" + inputName + "' must be float32");
    }
    const auto shape = tensorInfo.GetShape();
    if (shape.size() != 2) {
        throw std::runtime_error("ONNX lane policy input '" + inputName + "' must have rank 2");
    }
    if (shape[1] != -1 && shape[1] != kLanePolicyFeatureCount) {
        throw std::runtime_error("ONNX lane policy input feature count must be 12");
    }
}

std::string allocatedName(Ort::AllocatedStringPtr name) {
    if (name == nullptr || name.get() == nullptr) {
        return {};
    }
    return std::string(name.get());
}

int argmaxLane(const float* logits, int targetKeyCount) {
    int best = 0;
    float bestValue = logits[0];
    for (int lane = 1; lane < targetKeyCount; ++lane) {
        if (logits[lane] > bestValue) {
            bestValue = logits[lane];
            best = lane;
        }
    }
    return best;
}

void copyFloatLogits(const Ort::Value& output,
                     const std::vector<RowRef>& rows,
                     std::size_t rowOffset,
                     std::size_t rowCount,
                     std::vector<std::vector<int>>& lanesByChart) {
    const auto info = output.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (shape.size() != 2 || shape[0] != static_cast<int64_t>(rowCount)) {
        throw std::runtime_error("ONNX lane policy output lane_logits must have shape [notes, targetKeys]");
    }
    const int64_t outputWidth = shape[1];
    if (outputWidth <= 0) {
        throw std::runtime_error("ONNX lane policy output targetKeys dimension must be static and positive");
    }
    const auto* logits = output.GetTensorData<float>();
    for (std::size_t i = 0; i < rowCount; ++i) {
        const auto& row = rows[rowOffset + i];
        if (outputWidth < row.targetKeyCount) {
            throw std::runtime_error("ONNX lane policy output targetKeys dimension is smaller than --target");
        }
        lanesByChart[row.chartIndex][row.noteIndex] =
            argmaxLane(logits + (i * static_cast<std::size_t>(outputWidth)), row.targetKeyCount);
    }
}

void copyIntegerLanes(const Ort::Value& output,
                      const std::vector<RowRef>& rows,
                      std::size_t rowOffset,
                      std::size_t rowCount,
                      std::vector<std::vector<int>>& lanesByChart) {
    const auto info = output.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (shape.size() != 1 || shape[0] != static_cast<int64_t>(rowCount)) {
        throw std::runtime_error("ONNX lane policy integer output must have shape [notes]");
    }

    const auto type = info.GetElementType();
    for (std::size_t i = 0; i < rowCount; ++i) {
        int lane = 0;
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
            lane = static_cast<int>(output.GetTensorData<int64_t>()[i]);
        } else if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
            lane = output.GetTensorData<int32_t>()[i];
        } else {
            throw std::runtime_error("ONNX lane policy output must be float32 logits, int64 lanes, or int32 lanes");
        }
        const auto& row = rows[rowOffset + i];
        if (lane < 0 || lane >= row.targetKeyCount) {
            throw std::runtime_error("ONNX lane policy predicted a lane outside --target");
        }
        lanesByChart[row.chartIndex][row.noteIndex] = lane;
    }
}

}  // namespace

LanePolicyResult runCudaLanePolicy(const std::vector<LanePolicyRequest>& requests,
                                   const LanePolicyOptions& options) {
    if (requests.empty()) {
        return {};
    }
    if (options.modelPath.empty()) {
        throw std::runtime_error("ONNX lane policy model path is required");
    }

    LanePolicyResult result;
    result.lanesByChart.resize(requests.size());
    for (std::size_t chartIndex = 0; chartIndex < requests.size(); ++chartIndex) {
        if (requests[chartIndex].chart == nullptr) {
            throw std::runtime_error("ONNX lane policy request has a null chart");
        }
        result.lanesByChart[chartIndex].assign(requests[chartIndex].chart->notes.size(), 0);
    }

    std::vector<float> features;
    std::vector<RowRef> rows;
    for (std::size_t chartIndex = 0; chartIndex < requests.size(); ++chartIndex) {
        appendFeaturesForChart(requests[chartIndex], chartIndex, features, rows);
    }
    result.totalNotes = rows.size();
    if (rows.empty()) {
        return result;
    }

    requireCudaProvider();

    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "KeyWeaverOnnxCuda");
    Ort::SessionOptions sessionOptions;
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    sessionOptions.SetIntraOpNumThreads(1);
    if (options.disableCpuFallback) {
        sessionOptions.AddConfigEntry("session.disable_cpu_ep_fallback", "1");
    }
    OrtCUDAProviderOptions cudaOptions{};
    cudaOptions.device_id = options.cudaDeviceId;
    sessionOptions.AppendExecutionProvider_CUDA(cudaOptions);

#if defined(_WIN32)
    const auto modelPath = options.modelPath.wstring();
#else
    const auto modelPath = options.modelPath.string();
#endif
    Ort::Session session(env, modelPath.c_str(), sessionOptions);
    Ort::AllocatorWithDefaultOptions allocator;
    result.inputName = allocatedName(session.GetInputNameAllocated(0, allocator));
    result.outputName = allocatedName(session.GetOutputNameAllocated(0, allocator));
    if (result.inputName.empty()) {
        result.inputName = "features";
    }
    if (result.outputName.empty()) {
        result.outputName = "lane_logits";
    }
    validateInputContract(session, result.inputName);

    const auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const std::size_t maxBatchNotes = options.maxBatchNotes == 0
                                          ? rows.size()
                                          : std::max<std::size_t>(1, options.maxBatchNotes);
    for (std::size_t offset = 0; offset < rows.size(); offset += maxBatchNotes) {
        const std::size_t rowCount = std::min(maxBatchNotes, rows.size() - offset);
        std::array<int64_t, 2> inputShape{
            static_cast<int64_t>(rowCount),
            static_cast<int64_t>(kLanePolicyFeatureCount),
        };
        auto inputTensor = Ort::Value::CreateTensor<float>(
            memoryInfo,
            features.data() + (offset * static_cast<std::size_t>(kLanePolicyFeatureCount)),
            rowCount * static_cast<std::size_t>(kLanePolicyFeatureCount),
            inputShape.data(),
            inputShape.size());

        const char* inputNames[] = {result.inputName.c_str()};
        const char* outputNames[] = {result.outputName.c_str()};
        Ort::RunOptions runOptions{nullptr};
        auto outputs = session.Run(runOptions, inputNames, &inputTensor, 1, outputNames, 1);
        if (outputs.empty() || !outputs.front().IsTensor()) {
            throw std::runtime_error("ONNX lane policy did not return a tensor output");
        }

        const auto elementType = outputs.front().GetTensorTypeAndShapeInfo().GetElementType();
        if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            copyFloatLogits(outputs.front(), rows, offset, rowCount, result.lanesByChart);
        } else {
            copyIntegerLanes(outputs.front(), rows, offset, rowCount, result.lanesByChart);
        }
        ++result.runCount;
    }

    return result;
}

}  // namespace keyconv::onnx
