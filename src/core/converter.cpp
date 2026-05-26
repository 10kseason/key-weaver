#include "core/converter.hpp"

#include <stdexcept>
#include <vector>

#include "core/convert.hpp"

namespace keyconv {

ConvertResult Converter::convert(const Chart& input, const ConvertOptions& options) const {
    ConvertOptions effective = options;
    std::vector<std::string> facadeWarnings;

    if (effective.sourceKeyCount < 1 || effective.sourceKeyCount > 32) {
        throw std::runtime_error("sourceKeyCount must be between 1 and 32");
    }
    if (effective.targetKeyCount < 1 || effective.targetKeyCount > 32) {
        throw std::runtime_error("targetKeyCount must be between 1 and 32");
    }
    if (effective.beamWidth < 1) {
        throw std::runtime_error("beamWidth must be at least 1");
    }
    if (effective.sameTimeEpsilonMs < 0) {
        throw std::runtime_error("sameTimeEpsilonMs must be non-negative");
    }
    if (effective.jackWindowMs < 0) {
        throw std::runtime_error("jackWindowMs must be non-negative");
    }
    if (effective.strictJackWindowMs < 0) {
        throw std::runtime_error("strictJackWindowMs must be non-negative");
    }
    if (effective.maxJackSplitLanes < 1) {
        throw std::runtime_error("maxJackSplitLanes must be at least 1");
    }
    if (effective.maxAddedNoteRatio < 0.0) {
        throw std::runtime_error("maxAddedNoteRatio must be non-negative");
    }
    if (effective.maxAddedPerSlice < 0) {
        throw std::runtime_error("maxAddedPerSlice must be non-negative");
    }
    if (effective.maxAddedPerMeasure < 0) {
        throw std::runtime_error("maxAddedPerMeasure must be non-negative");
    }
    if (effective.expansionMinGapMs < 0) {
        throw std::runtime_error("expansionMinGapMs must be non-negative");
    }
    if (effective.expansionSameLaneMinGapMs < 0) {
        throw std::runtime_error("expansionSameLaneMinGapMs must be non-negative");
    }
    if (effective.expansionSnapToleranceMs < 0) {
        throw std::runtime_error("expansionSnapToleranceMs must be non-negative");
    }
    if (effective.maxEchoAddedRatio < 0.0) {
        throw std::runtime_error("maxEchoAddedRatio must be non-negative");
    }
    if (effective.maxEchoPerPattern < 0) {
        throw std::runtime_error("maxEchoPerPattern must be non-negative");
    }
    if (effective.maxEchoPerMeasure < 0) {
        throw std::runtime_error("maxEchoPerMeasure must be non-negative");
    }
    if (effective.maxEchoPerSlice < 0) {
        throw std::runtime_error("maxEchoPerSlice must be non-negative");
    }
    if (effective.minEchoPatternLength < 0) {
        throw std::runtime_error("minEchoPatternLength must be non-negative");
    }
    if (effective.minPatternConfidence < 0.0) {
        throw std::runtime_error("minPatternConfidence must be non-negative");
    }
    if (effective.echoMinGapMs < 0) {
        throw std::runtime_error("echoMinGapMs must be non-negative");
    }
    if (effective.echoSameLaneMinGapMs < 0) {
        throw std::runtime_error("echoSameLaneMinGapMs must be non-negative");
    }
    if (effective.echoHighDensityWindowMs < 1) {
        throw std::runtime_error("echoHighDensityWindowMs must be at least 1");
    }
    if (effective.echoMaxLocalNps < 0.0) {
        throw std::runtime_error("echoMaxLocalNps must be non-negative");
    }

    if (effective.optimizer == OptimizerKind::Beam) {
        facadeWarnings.push_back(
            "Warning: beam optimizer is reserved but not implemented in this version; falling back to greedy optimizer.");
        effective.optimizer = OptimizerKind::Greedy;
    }
    if (effective.dpMode || effective.style == ConversionStyle::DP) {
        facadeWarnings.push_back(
            "Warning: DP conversion is reserved but not implemented in this version; falling back to SP PPG conversion.");
        effective.dpMode = false;
        if (effective.style == ConversionStyle::DP) {
            effective.style = ConversionStyle::Playable;
        }
    }

    auto result = convertChart(input, effective);
    result.report.warnings.insert(result.report.warnings.begin(), facadeWarnings.begin(), facadeWarnings.end());
    result.chart.warnings.insert(result.chart.warnings.begin(), facadeWarnings.begin(), facadeWarnings.end());
    return result;
}

}  // namespace keyconv
