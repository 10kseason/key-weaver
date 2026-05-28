#pragma once

#include "core/chart.hpp"

#include <keyconv/convert_options.hpp>

#include <string>
#include <vector>

namespace keyconv {

struct GeneratedNoteInfo {
    std::string generatedId;
    std::string ruleName;
    std::vector<std::string> sourceNoteIds;
    int sourceSliceIndex = -1;
    double originalTimeMs = 0.0;
    double generatedTimeMs = 0.0;
    int generatedLane = 0;
};

struct ExpansionPlanStats {
    int addedNotes = 0;
    int addedByTapPlus = 0;
    int addedByChordFill = 0;
    int addedByEcho = 0;
    int addedByStairEcho = 0;
    int addedByTrillEcho = 0;
    int addedByStreamEcho = 0;
    int addedByTrainingScaffold = 0;
    int rejectedExpansionCandidates = 0;
    int rejectedByCollision = 0;
    int rejectedByDistance = 0;
    int rejectedBySnap = 0;
    int rejectedByBudget = 0;
    int unsnappedAddedNotes = 0;
    double addedNoteRatio = 0.0;
    int preventedJacks = 0;
    std::string expansionComposerProfile = "preserve";
    double targetAddedNoteRatio = 0.0;
    double budgetUsedRatio = 0.0;
    bool adaptiveGrowthBudgetEnabled = false;
    int adaptiveBudgetWindowMs = 1000;
    int adaptiveBudgetWindows = 0;
    double adaptiveBudgetAverageRatio = 0.0;
    double adaptiveBudgetMinRatio = 0.0;
    double adaptiveBudgetMaxRatio = 0.0;
    int acceptedByComposer = 0;
    int rejectedByComposerBudget = 0;
    int rejectedByComposerSafety = 0;
    int rejectedByAdaptiveBudget = 0;
    int rejectedEchoCandidates = 0;
    int rejectedEchoByDensity = 0;
    int rejectedEchoByDistance = 0;
    int rejectedEchoBySnap = 0;
    int rejectedEchoByBudget = 0;
    double echoAddedRatio = 0.0;
    int rejectedStreamEchoByBurst = 0;
    int rejectedStreamEchoByJack = 0;
    int rejectedStreamEchoByLNHeavy = 0;
    int rejectedStreamEchoByLocalNps = 0;
    int streamEchoCandidates = 0;
    int streamRawPatternCandidates = 0;
    int streamEligiblePatternCandidates = 0;
    int streamRawLaneCandidates = 0;
    int streamSafeLaneCandidates = 0;
    int streamAcceptedCandidates = 0;
    int rejectedStreamEchoByNoUnderusedLane = 0;
    int rejectedStreamEchoByPatternConfidence = 0;
    int rejectedStreamEchoByPatternLength = 0;
    int rejectedStreamEchoBySliceChordFull = 0;
    int rejectedStreamEchoByLaneRole = 0;
    int rejectedStreamPrimaryByPatternConfidence = 0;
    int rejectedStreamPrimaryByPatternLength = 0;
    int rejectedStreamPrimaryByBurst = 0;
    int rejectedStreamPrimaryByJack = 0;
    int rejectedStreamPrimaryByLNHeavy = 0;
    int rejectedStreamPrimaryByLocalNps = 0;
    int rejectedStreamPrimaryByNoUnderusedLane = 0;
    int rejectedStreamPrimaryBySliceChordFull = 0;
    int rejectedStreamPrimaryByLaneRole = 0;
    int rejectedStreamPrimaryByCollision = 0;
    int rejectedStreamPrimaryByDistance = 0;
    int rejectedStreamPrimaryBySnap = 0;
    int rejectedStreamPrimaryByBudget = 0;
    double streamEchoAddedRatio = 0.0;
    double maxObservedLocalNpsAfterEcho = 0.0;
    bool deterministic = true;
    std::string algorithmVersion = "v0.6.0";
    StreamEchoProfile streamEchoProfile = StreamEchoProfile::Conservative;
    ExpansionPolicy policy = ExpansionPolicy::PreserveNoteCount;
    std::vector<GeneratedNoteInfo> generatedNotes;
    std::vector<std::string> warnings;
};

ExpansionPolicy resolveExpansionPolicy(const ConvertOptions& options);
ExpansionPlanStats applyExpansionPlanner(Chart& converted,
                                         const Chart& original,
                                         const ConvertOptions& options);

}  // namespace keyconv
