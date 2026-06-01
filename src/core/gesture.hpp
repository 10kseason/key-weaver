#pragma once

#include "core/chart.hpp"
#include "core/pattern.hpp"
#include "core/slice.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace keyconv {

enum class PhraseRole {
    Neutral,
    LeftHandVoice,
    RightHandVoice,
};

struct GestureHint {
    int motifId = -1;
    PatternKind kind = PatternKind::Single;
    PhraseRole role = PhraseRole::Neutral;
    int preferredLane = 0;
    int zoneStart = 0;
    int zoneEnd = 0;
    int direction = 0;
    int motifHitCount = 0;
    int motifDurationMs = 0;
};

struct GestureRail {
    bool enabled = true;
    std::map<std::string, GestureHint> hintsByNoteId;
};

struct GestureReport {
    int detectedStairs = 0;
    int preservedStairs = 0;
    int brokenStairs = 0;
    int detectedTrills = 0;
    int preservedTrills = 0;
    int brokenTrills = 0;
    int detectedJacks = 0;
    int preservedJacks = 0;
    int brokenJacks = 0;
    int handZoneBreaks = 0;
    int motifDirectionFlips = 0;
    int motifLaneScatterCount = 0;
    double gesturePreservationScore = 1.0;
    bool gestureRailEnabled = true;
};

GestureRail buildGestureRail(const Chart& chart,
                             int sourceKeyCount,
                             int targetKeyCount,
                             int sameTimeEpsilonMs,
                             int jackWindowMs,
                             bool enabled,
                             bool fullTenKeyGestureZone = false);

GestureRail buildFullFieldRail(const Chart& chart,
                               int sourceKeyCount,
                               int targetKeyCount,
                               int sameTimeEpsilonMs,
                               int jackWindowMs,
                               bool enabled);

const GestureHint* findGestureHint(const GestureRail* rail, const std::string& noteId);

GestureReport evaluateGesturePreservation(const Chart& original,
                                          const Chart& converted,
                                          int sourceKeyCount,
                                          int targetKeyCount,
                                          int sameTimeEpsilonMs,
                                          bool gestureRailEnabled,
                                          int jackWindowMs = 500);

}  // namespace keyconv
