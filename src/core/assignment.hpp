#pragma once

#include "core/convert.hpp"
#include "core/gesture.hpp"
#include "core/slice.hpp"

#include <vector>

namespace keyconv {

struct PpgWeights {
    double position = 1.0;
    double order = 2.0;
    double shape = 1.5;
    double collision = 120.0;
    double lnConflict = 100.0;
    double jack = 8.0;
    double movement = 5.0;
    double density = 4.0;
    double handBalance = 5.0;
    double gesture = 8.0;
};

struct LaneCandidateSet {
    int sourceLane = 0;
    int baseLane = 0;
    int radius = 1;
    bool hasPreferredZone = false;
    int preferredZoneStart = 0;
    int preferredZoneEnd = 0;
    std::vector<int> candidates;
};

struct SliceAssignment {
    std::vector<int> targetLanes;
    double score = 0.0;
};

struct AssignmentContext {
    std::vector<Note> placed;
    std::vector<int> laneUse;
    int sourceKeyCount = 0;
    int targetKeyCount = 0;
    int jackWindowMs = 500;
    ConversionStyle style = ConversionStyle::Playable;
    Native10KPreset native10KPreset = Native10KPreset::Off;
    PpgWeights weights;
    bool preserveLaneDrift = false;
    int* preventedJacksByAssignment = nullptr;
    const GestureRail* gestureRail = nullptr;
};

PpgWeights weightsForStyle(ConversionStyle style);
LaneCandidateSet generateCandidateLanes(int sourceLane,
                                        int sourceK,
                                        int targetK,
                                        ConversionStyle style,
                                        Native10KPreset native10KPreset = Native10KPreset::Off);
std::vector<SliceAssignment> generateSliceAssignments(const TimeSlice& slice,
                                                      const std::vector<Note>& sourceNotes,
                                                      const AssignmentContext& context);
double scoreAssignment(const TimeSlice& slice,
                       const std::vector<Note>& sourceNotes,
                       const SliceAssignment& assignment,
                       const AssignmentContext& context);

}  // namespace keyconv
