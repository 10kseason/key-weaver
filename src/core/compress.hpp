#pragma once

#include "core/convert.hpp"

#include <set>
#include <string>
#include <vector>

namespace keyconv {

struct SliceCapacity {
    int sliceIndex = 0;
    int timeMs = 0;
    int targetKeyCount = 0;
    int activeHoldCount = 0;
    int freeLaneCount = 0;
    int tapStartCount = 0;
    int holdStartCount = 0;
    int requiredLaneCount = 0;
    bool impossible = false;
};

struct OverlapValidationResult {
    int sameTimeCollisions = 0;
    int longNoteConflicts = 0;
    int impossibleSlices = 0;
    bool noOverlapGuaranteed = false;
};

struct CompressionPlanStats {
    int impossibleSlices = 0;
    int droppedByCompression = 0;
    int rolledByCompression = 0;
    int shortenedHolds = 0;
    int tapifiedHolds = 0;
    bool noOverlapGuaranteed = false;
    int nearTimeConflicts = 0;
    int sameLaneNearConflicts = 0;
    int unsnappedNotes = 0;
    int unsnappedRolledNotes = 0;
    int minPositiveDeltaMs = 0;
    int droppedByDistanceGuard = 0;
    int rerolledByDistanceGuard = 0;
    std::set<std::string> rolledNoteIds;
    std::vector<std::string> warnings;
};

CompressPolicy resolveCompressPolicy(const ConvertOptions& options);
CompressionPlanStats applyCompressPlanner(std::vector<Note>& notes,
                                          const ConvertOptions& options,
                                          const std::vector<TimingPoint>& timingPoints);
OverlapValidationResult validateNoOverlap(const std::vector<Note>& notes, int targetKeyCount);

}  // namespace keyconv
