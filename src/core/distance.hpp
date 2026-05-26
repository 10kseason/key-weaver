#pragma once

#include "core/chart.hpp"

#include <keyconv/convert_options.hpp>

#include <set>
#include <string>
#include <vector>

namespace keyconv {

struct DistanceValidationResult {
    int nearTimeConflicts = 0;
    int sameLaneNearConflicts = 0;
    int unsnappedNotes = 0;
    int unsnappedRolledNotes = 0;
    int minPositiveDeltaMs = 0;
};

bool distancePolicyRejects(DistancePolicy policy);
bool isSnapAligned(int time,
                   const std::vector<TimingPoint>& timingPoints,
                   int toleranceMs);
std::vector<int> generateSnapTimesNear(int originalTimeMs,
                                       const std::vector<TimingPoint>& timingPoints,
                                       int maxRollMs);
bool hasDistanceConflict(const std::vector<Note>& placed,
                         const Note& candidate,
                         const ConvertOptions& options,
                         bool includeHoldEdges);
DistanceValidationResult validateDistance(const std::vector<Note>& notes,
                                          const ConvertOptions& options,
                                          const std::vector<TimingPoint>& timingPoints,
                                          const std::set<std::string>& rolledNoteIds);

}  // namespace keyconv
