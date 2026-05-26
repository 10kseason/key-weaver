#pragma once

#include "core/convert.hpp"
#include "core/pattern.hpp"

#include <vector>

namespace keyconv {

struct OptimizationResult {
    std::vector<Note> notes;
    std::vector<PatternToken> patterns;
    int localRepairShifted = 0;
    int preventedJacksByAssignment = 0;
    int preventedJacksByRepair = 0;
};

OptimizationResult greedyOptimizeSlices(const Chart& chart, const ConvertOptions& options);
int localRepairAssignments(std::vector<Note>& notes,
                           int targetKeyCount,
                           int jackWindowMs = 180,
                           int* preventedJacksByRepair = nullptr);

}  // namespace keyconv
