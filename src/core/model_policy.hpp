#pragma once

#include "core/chart.hpp"

#include <keyconv/convert_options.hpp>

#include <string>
#include <vector>

namespace keyconv {

struct OnnxPolicyResult {
    bool requested = false;
    bool modelLoaded = false;
    std::string providerRequested = "auto";
    std::string providerActive = "off";
    std::vector<std::string> availableProviders;
    int attemptedNotes = 0;
    int evaluatedCandidates = 0;
    int acceptedRelanes = 0;
    int fallbackRelanes = 0;
    int rejectedRelanes = 0;
    int sameLaneNoops = 0;
    int rejectedByOutOfRange = 0;
    int rejectedByCollision = 0;
    int rejectedByLnConflict = 0;
    int rejectedByCreatedJack = 0;
    std::vector<std::string> warnings;
};

OnnxPolicyResult applyOnnxLanePolicy(const Chart& original,
                                     std::vector<Note>& placed,
                                     const ConvertOptions& options);

}  // namespace keyconv
