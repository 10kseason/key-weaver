#include "core/model_policy.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef KEYWEAVER_WITH_ONNXRUNTIME
#define KEYWEAVER_WITH_ONNXRUNTIME 0
#endif

#if KEYWEAVER_WITH_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace keyconv {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalizeOnnxProvider(std::string provider) {
    provider = lowerAscii(std::move(provider));
    if (provider.empty() || provider == "gpu") {
        return "auto";
    }
    if (provider == "directml") {
        return "dml";
    }
    return provider;
}

#if KEYWEAVER_WITH_ONNXRUNTIME

constexpr int kFeatureCount = 12;
constexpr int kMaxCandidateLanes = 5;

enum class LaneCandidateDecision {
    Safe,
    OutOfRange,
    SameTimeCollision,
    LnConflict,
    CreatedJack,
};

using LaneCandidateList = std::vector<std::vector<int>>;

struct CachedOnnxSession {
    std::string modelPath;
    std::string providerRequested;
    std::string providerActive = "off";
    std::vector<std::string> availableProviders;
    std::vector<std::string> providerWarnings;
    std::string inputName;
    std::string outputName;
    std::unique_ptr<Ort::Session> session;
};

Ort::Env& onnxEnvironment() {
    static auto* env = new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "KeyWeaverOnnxPolicy");
    return *env;
}

double clampUnit(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double safeRatio(int value, int denominator) {
    return denominator <= 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(denominator);
}

int chartDurationMs(const Chart& chart) {
    if (chart.notes.empty()) {
        return 1;
    }
    int minTime = chart.notes.front().time;
    int maxTime = chart.notes.front().time;
    for (const auto& note : chart.notes) {
        minTime = std::min(minTime, note.time);
        maxTime = std::max(maxTime, note.time);
        if (note.endTime.has_value()) {
            maxTime = std::max(maxTime, *note.endTime);
        }
    }
    return std::max(1, maxTime - minTime);
}

std::map<int, int> chordSizesByTime(const std::vector<Note>& notes) {
    std::map<int, int> chordSizes;
    for (const auto& note : notes) {
        ++chordSizes[note.time];
    }
    return chordSizes;
}

int activeHoldCountAt(const std::vector<Note>& notes, int time) {
    int active = 0;
    for (const auto& note : notes) {
        if (note.type != NoteType::Hold || !note.endTime.has_value()) {
            continue;
        }
        if (note.time < time && time <= *note.endTime) {
            ++active;
        }
    }
    return active;
}

std::vector<Note> sortedNotes(std::vector<Note> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& lhs, const Note& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        return lhs.lane < rhs.lane;
    });
    return notes;
}

std::vector<float> buildPolicyFeatures(const Chart& original,
                                       const std::vector<Note>& placed,
                                       const ConvertOptions& options) {
    const auto sortedOriginal = sortedNotes(original.notes);
    const auto sourceChordSizes = chordSizesByTime(original.notes);
    const int durationMs = chartDurationMs(original);
    const int minTime = original.notes.empty()
                            ? 0
                            : std::min_element(original.notes.begin(),
                                               original.notes.end(),
                                               [](const Note& lhs, const Note& rhs) {
                                                   return lhs.time < rhs.time;
                                               })
                                  ->time;

    std::vector<float> features;
    features.reserve(placed.size() * kFeatureCount);
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const auto& note = placed[index];
        const int sourceLane = note.sourceLane.value_or(note.lane);
        const int previousGap = index == 0 ? 2000 : std::max(0, note.time - placed[index - 1].time);
        const int nextGap = index + 1 >= placed.size() ? 2000 : std::max(0, placed[index + 1].time - note.time);
        const auto chordFound = sourceChordSizes.find(note.time);
        const int chordSize = chordFound == sourceChordSizes.end() ? 1 : chordFound->second;
        const int holdDuration = note.endTime.has_value() ? std::max(0, *note.endTime - note.time) : 0;
        const int activeHolds = activeHoldCountAt(sortedOriginal, note.time);

        features.push_back(static_cast<float>(clampUnit(safeRatio(sourceLane, options.sourceKeyCount - 1))));
        features.push_back(static_cast<float>(clampUnit(safeRatio(note.lane, options.targetKeyCount - 1))));
        features.push_back(static_cast<float>(clampUnit(safeRatio(options.sourceKeyCount, 32))));
        features.push_back(static_cast<float>(clampUnit(safeRatio(options.targetKeyCount, 32))));
        features.push_back(static_cast<float>(clampUnit(safeRatio(note.time - minTime, durationMs))));
        features.push_back(static_cast<float>(clampUnit(static_cast<double>(previousGap) / 2000.0)));
        features.push_back(static_cast<float>(clampUnit(static_cast<double>(nextGap) / 2000.0)));
        features.push_back(static_cast<float>(clampUnit(safeRatio(chordSize, options.sourceKeyCount))));
        features.push_back(note.type == NoteType::Hold ? 1.0f : 0.0f);
        features.push_back(static_cast<float>(clampUnit(static_cast<double>(holdDuration) / 4000.0)));
        features.push_back(sourceLane < options.sourceKeyCount / 2 ? 0.0f : 1.0f);
        features.push_back(static_cast<float>(clampUnit(safeRatio(activeHolds, options.sourceKeyCount))));
    }
    return features;
}

bool createsDifferentSourceRepeat(const std::vector<Note>& notes,
                                  std::size_t movingIndex,
                                  const Note& moved,
                                  int lane,
                                  int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0) {
        return false;
    }

    const int sourceLane = moved.sourceLane.value_or(moved.lane);
    for (std::size_t index = 0; index < notes.size(); ++index) {
        if (index == movingIndex) {
            continue;
        }
        const auto& other = notes[index];
        if (other.lane != lane) {
            continue;
        }
        const int delta = std::abs(other.time - moved.time);
        if (delta > 0 && delta <= window && other.sourceLane.value_or(other.lane) != sourceLane) {
            return true;
        }
    }
    return false;
}

LaneCandidateDecision evaluatePredictedLane(const std::vector<Note>& notes,
                                            std::size_t movingIndex,
                                            int predictedLane,
                                            const ConvertOptions& options) {
    if (predictedLane < 0 || predictedLane >= options.targetKeyCount || movingIndex >= notes.size()) {
        return LaneCandidateDecision::OutOfRange;
    }

    Note moved = notes[movingIndex];
    moved.lane = predictedLane;
    for (std::size_t index = 0; index < notes.size(); ++index) {
        if (index == movingIndex) {
            continue;
        }
        const auto& other = notes[index];
        if (other.lane != predictedLane) {
            continue;
        }
        if (other.time == moved.time) {
            return LaneCandidateDecision::SameTimeCollision;
        }
        if (other.type == NoteType::Hold && other.endTime.has_value() &&
            moved.time > other.time && moved.time <= *other.endTime) {
            return LaneCandidateDecision::LnConflict;
        }
        if (moved.type == NoteType::Hold && moved.endTime.has_value() &&
            other.time > moved.time && other.time <= *moved.endTime) {
            return LaneCandidateDecision::LnConflict;
        }
    }
    if (createsDifferentSourceRepeat(notes, movingIndex, moved, predictedLane, options.jackWindowMs)) {
        return LaneCandidateDecision::CreatedJack;
    }
    return LaneCandidateDecision::Safe;
}

void recordCandidateDecision(OnnxPolicyResult& result, LaneCandidateDecision decision) {
    switch (decision) {
        case LaneCandidateDecision::Safe:
            return;
        case LaneCandidateDecision::OutOfRange:
            ++result.rejectedByOutOfRange;
            return;
        case LaneCandidateDecision::SameTimeCollision:
            ++result.rejectedByCollision;
            return;
        case LaneCandidateDecision::LnConflict:
            ++result.rejectedByLnConflict;
            return;
        case LaneCandidateDecision::CreatedJack:
            ++result.rejectedByCreatedJack;
            return;
    }
}

void applyPredictedLaneCandidates(std::vector<Note>& placed,
                                  std::size_t index,
                                  const std::vector<int>& predictedLanes,
                                  const ConvertOptions& options,
                                  OnnxPolicyResult& result) {
    if (index >= placed.size() || predictedLanes.empty()) {
        return;
    }

    const int currentLane = placed[index].lane;
    if (predictedLanes.front() == currentLane) {
        ++result.sameLaneNoops;
        return;
    }

    ++result.attemptedNotes;
    int rejectedBeforeAccept = 0;
    for (const int predictedLane : predictedLanes) {
        if (predictedLane == currentLane) {
            ++result.sameLaneNoops;
            break;
        }

        ++result.evaluatedCandidates;
        const auto decision = evaluatePredictedLane(placed, index, predictedLane, options);
        if (decision == LaneCandidateDecision::Safe) {
            placed[index].lane = predictedLane;
            ++result.acceptedRelanes;
            if (rejectedBeforeAccept > 0) {
                ++result.fallbackRelanes;
            }
            return;
        }

        ++rejectedBeforeAccept;
        recordCandidateDecision(result, decision);
    }

    ++result.rejectedRelanes;
}

LaneCandidateList predictedLaneCandidatesFromFloatTensor(const Ort::Value& output,
                                                         std::size_t noteCount,
                                                         int targetKeyCount) {
    const auto info = output.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    const float* values = output.GetTensorData<float>();
    LaneCandidateList candidates(noteCount);
    if (shape.size() == 1 && static_cast<std::size_t>(shape[0]) == noteCount) {
        for (std::size_t index = 0; index < noteCount; ++index) {
            candidates[index].push_back(static_cast<int>(std::lround(values[index])));
        }
        return candidates;
    }
    if (shape.size() == 2 && static_cast<std::size_t>(shape[0]) == noteCount && shape[1] > 0) {
        const std::size_t stride = static_cast<std::size_t>(shape[1]);
        const std::size_t laneCount = std::min<std::size_t>(stride, static_cast<std::size_t>(targetKeyCount));
        for (std::size_t note = 0; note < noteCount; ++note) {
            std::vector<std::pair<float, int>> ranked;
            ranked.reserve(laneCount);
            for (std::size_t lane = 0; lane < laneCount; ++lane) {
                ranked.push_back({values[note * stride + lane], static_cast<int>(lane)});
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& lhs, const auto& rhs) {
                if (lhs.first != rhs.first) {
                    return lhs.first > rhs.first;
                }
                return lhs.second < rhs.second;
            });
            const std::size_t limit =
                std::min<std::size_t>(ranked.size(), static_cast<std::size_t>(kMaxCandidateLanes));
            candidates[note].reserve(limit);
            for (std::size_t index = 0; index < limit; ++index) {
                candidates[note].push_back(ranked[index].second);
            }
        }
        return candidates;
    }
    throw std::runtime_error("ONNX policy output must be [notes] lane ids or [notes, lanes] logits");
}

template <typename T>
LaneCandidateList predictedLaneCandidatesFromIntTensor(const Ort::Value& output, std::size_t noteCount) {
    const auto info = output.GetTensorTypeAndShapeInfo();
    const auto shape = info.GetShape();
    if (shape.size() != 1 || static_cast<std::size_t>(shape[0]) != noteCount) {
        throw std::runtime_error("ONNX policy integer output must be [notes] lane ids");
    }
    const T* values = output.GetTensorData<T>();
    LaneCandidateList candidates(noteCount);
    for (std::size_t index = 0; index < noteCount; ++index) {
        candidates[index].push_back(static_cast<int>(values[index]));
    }
    return candidates;
}

bool providerAvailable(const std::vector<std::string>& availableProviders, std::string_view providerName) {
    return std::find(availableProviders.begin(), availableProviders.end(), providerName) !=
           availableProviders.end();
}

std::string joinProviders(const std::vector<std::string>& providers) {
    if (providers.empty()) {
        return "none";
    }
    std::ostringstream out;
    for (std::size_t index = 0; index < providers.size(); ++index) {
        if (index > 0) {
            out << ", ";
        }
        out << providers[index];
    }
    return out.str();
}

bool hasCudaProvider(const std::vector<std::string>& providers) {
    return providerAvailable(providers, "CUDAExecutionProvider");
}

bool hasDmlProvider(const std::vector<std::string>& providers) {
    return providerAvailable(providers, "DmlExecutionProvider") ||
           providerAvailable(providers, "DMLExecutionProvider");
}

void appendCudaProvider(Ort::SessionOptions& sessionOptions) {
    Ort::CUDAProviderOptions cudaOptions;
    cudaOptions.Update(std::unordered_map<std::string, std::string>{{"device_id", "0"}});
    sessionOptions.AppendExecutionProvider_CUDA_V2(*cudaOptions);
}

void appendDmlProvider(Ort::SessionOptions& sessionOptions) {
    try {
        sessionOptions.AppendExecutionProvider(
            "DML",
            std::unordered_map<std::string, std::string>{{"device_id", "0"}});
        return;
    } catch (const std::exception& dmlError) {
        try {
            sessionOptions.AppendExecutionProvider(
                "DmlExecutionProvider",
                std::unordered_map<std::string, std::string>{{"device_id", "0"}});
            return;
        } catch (const std::exception& providerNameError) {
            throw std::runtime_error(std::string(dmlError.what()) + "; " + providerNameError.what());
        }
    }
}

void configureExecutionProvider(Ort::SessionOptions& sessionOptions,
                                const ConvertOptions& options,
                                OnnxPolicyResult& result) {
    result.providerRequested = normalizeOnnxProvider(options.onnxPolicyProvider);
    result.availableProviders = Ort::GetAvailableProviders();

    const auto& requested = result.providerRequested;
    if (requested == "cpu") {
        result.providerActive = "CPUExecutionProvider";
        return;
    }
    if (requested != "auto" && requested != "cuda" && requested != "dml") {
        throw std::runtime_error("Unsupported ONNX execution provider: " + options.onnxPolicyProvider);
    }

    if (requested == "cuda") {
        if (!hasCudaProvider(result.availableProviders)) {
            throw std::runtime_error("Requested ONNX CUDA provider is not available; available providers: " +
                                     joinProviders(result.availableProviders));
        }
        appendCudaProvider(sessionOptions);
        result.providerActive = "CUDAExecutionProvider";
        return;
    }

    if (requested == "dml") {
        if (!hasDmlProvider(result.availableProviders)) {
            throw std::runtime_error("Requested ONNX DirectML provider is not available; available providers: " +
                                     joinProviders(result.availableProviders));
        }
        appendDmlProvider(sessionOptions);
        result.providerActive = "DmlExecutionProvider";
        return;
    }

    std::vector<std::string> gpuErrors;
    if (hasCudaProvider(result.availableProviders)) {
        try {
            appendCudaProvider(sessionOptions);
            result.providerActive = "CUDAExecutionProvider";
            return;
        } catch (const std::exception& error) {
            gpuErrors.push_back("CUDA: " + std::string(error.what()));
        }
    }
    if (hasDmlProvider(result.availableProviders)) {
        try {
            appendDmlProvider(sessionOptions);
            result.providerActive = "DmlExecutionProvider";
            return;
        } catch (const std::exception& error) {
            gpuErrors.push_back("DirectML: " + std::string(error.what()));
        }
    }

    result.providerActive = "CPUExecutionProvider";
    if (!gpuErrors.empty()) {
        result.warnings.push_back("Warning: ONNX provider auto could not attach a GPU provider (" +
                                  joinProviders(gpuErrors) + "); using CPUExecutionProvider.");
    } else {
        result.warnings.push_back(
            "Warning: ONNX provider auto found no CUDA/DirectML provider; using CPUExecutionProvider.");
    }
}

CachedOnnxSession& cachedOnnxSession(const ConvertOptions& options, OnnxPolicyResult& result) {
    thread_local CachedOnnxSession cache;

    const std::string requestedModelPath = *options.onnxPolicyModelPath;
    const std::string requestedProvider = normalizeOnnxProvider(options.onnxPolicyProvider);
    const bool cacheHit = cache.session && cache.modelPath == requestedModelPath &&
                          cache.providerRequested == requestedProvider;
    if (cacheHit) {
        result.providerRequested = cache.providerRequested;
        result.providerActive = cache.providerActive;
        result.availableProviders = cache.availableProviders;
        result.warnings.insert(result.warnings.end(),
                               cache.providerWarnings.begin(),
                               cache.providerWarnings.end());
        return cache;
    }

    CachedOnnxSession next;
    next.modelPath = requestedModelPath;
    next.providerRequested = requestedProvider;

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
    configureExecutionProvider(sessionOptions, options, result);

    if (result.providerActive == "DmlExecutionProvider") {
        sessionOptions.DisableMemPattern();
        sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    }

#if defined(_WIN32)
    const std::wstring modelPath = std::filesystem::path(requestedModelPath).wstring();
    next.session = std::make_unique<Ort::Session>(onnxEnvironment(), modelPath.c_str(), sessionOptions);
#else
    next.session = std::make_unique<Ort::Session>(onnxEnvironment(), requestedModelPath.c_str(), sessionOptions);
#endif

    Ort::AllocatorWithDefaultOptions allocator;
    auto inputName = next.session->GetInputNameAllocated(0, allocator);
    auto outputName = next.session->GetOutputNameAllocated(0, allocator);
    next.inputName = inputName.get();
    next.outputName = outputName.get();
    next.providerActive = result.providerActive;
    next.availableProviders = result.availableProviders;
    next.providerWarnings = result.warnings;

    cache = std::move(next);
    return cache;
}

LaneCandidateList runOnnxPolicy(const Chart& original,
                                const std::vector<Note>& placed,
                                const ConvertOptions& options,
                                OnnxPolicyResult& result) {
    auto& session = cachedOnnxSession(options, result);
    const char* inputNames[] = {session.inputName.c_str()};
    const char* outputNames[] = {session.outputName.c_str()};

    auto features = buildPolicyFeatures(original, placed, options);
    std::array<int64_t, 2> inputShape{
        static_cast<int64_t>(placed.size()),
        static_cast<int64_t>(kFeatureCount),
    };
    auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(memoryInfo,
                                                       features.data(),
                                                       features.size(),
                                                       inputShape.data(),
                                                       inputShape.size());

    auto outputs = session.session->Run(Ort::RunOptions{nullptr},
                                        inputNames,
                                        &inputTensor,
                                        1,
                                        outputNames,
                                        1);
    if (outputs.empty() || !outputs.front().IsTensor()) {
        throw std::runtime_error("ONNX policy output is not a tensor");
    }

    const auto tensorInfo = outputs.front().GetTensorTypeAndShapeInfo();
    const auto elementType = tensorInfo.GetElementType();
    if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        return predictedLaneCandidatesFromFloatTensor(outputs.front(), placed.size(), options.targetKeyCount);
    }
    if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
        return predictedLaneCandidatesFromIntTensor<int64_t>(outputs.front(), placed.size());
    }
    if (elementType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
        return predictedLaneCandidatesFromIntTensor<int32_t>(outputs.front(), placed.size());
    }
    throw std::runtime_error("ONNX policy output type must be float, int64, or int32");
}

#endif

}  // namespace

OnnxPolicyResult applyOnnxLanePolicy(const Chart& original,
                                     std::vector<Note>& placed,
                                     const ConvertOptions& options) {
#if !KEYWEAVER_WITH_ONNXRUNTIME
    static_cast<void>(original);
#endif
    OnnxPolicyResult result;
    result.requested = options.onnxPolicyModelPath.has_value();
    result.providerRequested = normalizeOnnxProvider(options.onnxPolicyProvider);
    if (!result.requested || placed.empty()) {
        return result;
    }

#if !KEYWEAVER_WITH_ONNXRUNTIME
    const std::string message =
        "Warning: ONNX lane policy requested but this build was compiled without ONNX Runtime; "
        "falling back to deterministic lane placement.";
    if (options.onnxPolicyStrict) {
        throw std::runtime_error(message);
    }
    result.warnings.push_back(message);
    return result;
#else
    try {
        const auto predictedLaneCandidates = runOnnxPolicy(original, placed, options, result);
        result.modelLoaded = true;
        for (std::size_t index = 0; index < predictedLaneCandidates.size() && index < placed.size(); ++index) {
            applyPredictedLaneCandidates(placed, index, predictedLaneCandidates[index], options, result);
        }
        result.warnings.push_back("ONNX lane policy applied via " + result.providerActive +
                                  ": attempted=" + std::to_string(result.attemptedNotes) +
                                  ", accepted=" +
                                  std::to_string(result.acceptedRelanes) +
                                  ", fallback=" + std::to_string(result.fallbackRelanes) +
                                  ", rejected=" + std::to_string(result.rejectedRelanes) +
                                  ", candidates=" + std::to_string(result.evaluatedCandidates) +
                                  " (outOfRange=" + std::to_string(result.rejectedByOutOfRange) +
                                  ", collision=" + std::to_string(result.rejectedByCollision) +
                                  ", ln=" + std::to_string(result.rejectedByLnConflict) +
                                  ", createdJack=" + std::to_string(result.rejectedByCreatedJack) + ").");
    } catch (const std::exception& error) {
        const std::string message =
            "Warning: ONNX lane policy failed (" + std::string(error.what()) +
            "); falling back to deterministic lane placement.";
        if (options.onnxPolicyStrict) {
            throw;
        }
        result.warnings.push_back(message);
    }
    return result;
#endif
}

}  // namespace keyconv
