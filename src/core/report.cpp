#include "core/report.hpp"

#include <sstream>

namespace keyconv {

namespace {

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << ch;
                break;
        }
    }
    return out.str();
}

}  // namespace

std::string toString(ConversionStyle style) {
    switch (style) {
        case ConversionStyle::Direct:
            return "direct";
        case ConversionStyle::Expand:
            return "expand";
        case ConversionStyle::Compress:
            return "compress";
        case ConversionStyle::Playable:
            return "playable";
        case ConversionStyle::Faithful:
            return "faithful";
        case ConversionStyle::Training:
            return "training";
        case ConversionStyle::DP:
            return "dp";
    }
    return "playable";
}

std::string toString(CollisionPolicy policy) {
    switch (policy) {
        case CollisionPolicy::Keep:
            return "keep";
        case CollisionPolicy::ShiftNearest:
            return "shift-nearest";
        case CollisionPolicy::Merge:
            return "merge";
        case CollisionPolicy::Drop:
            return "drop";
    }
    return "shift-nearest";
}

std::string toString(OptimizerKind optimizer) {
    switch (optimizer) {
        case OptimizerKind::Greedy:
            return "greedy";
        case OptimizerKind::Beam:
            return "beam";
    }
    return "greedy";
}

std::string toString(CompressPolicy policy) {
    switch (policy) {
        case CompressPolicy::Auto:
            return "auto";
        case CompressPolicy::PreserveStrict:
            return "preserve-strict";
        case CompressPolicy::NoOverlapDrop:
            return "no-overlap-drop";
        case CompressPolicy::NoOverlapRoll:
            return "no-overlap-roll";
        case CompressPolicy::NoOverlapHybrid:
            return "no-overlap-hybrid";
        case CompressPolicy::TrainingSimplify:
            return "training-simplify";
    }
    return "auto";
}

std::string toString(DistancePolicy policy) {
    switch (policy) {
        case DistancePolicy::Off:
            return "off";
        case DistancePolicy::WarnOnly:
            return "warn";
        case DistancePolicy::AimodSafe:
            return "aimod-safe";
        case DistancePolicy::Strict:
            return "strict";
    }
    return "aimod-safe";
}

std::string toString(ExpansionPolicy policy) {
    switch (policy) {
        case ExpansionPolicy::PreserveNoteCount:
            return "preserve";
        case ExpansionPolicy::PreserveTapPlusMore:
            return "preserve-tap-plus-more";
        case ExpansionPolicy::PreserveTapPlus:
            return "preserve-tap-plus";
        case ExpansionPolicy::PreserveTapPlusLow:
            return "preserve-tap-plus-low";
        case ExpansionPolicy::DeterministicChordFill:
            return "chord-fill";
        case ExpansionPolicy::DeterministicEcho:
            return "echo";
        case ExpansionPolicy::TrainingScaffold:
            return "training-scaffold";
        case ExpansionPolicy::HarderRemix:
            return "harder-remix";
        case ExpansionPolicy::SeededRandomRemix:
            return "seeded-random";
    }
    return "preserve";
}

std::string toString(EchoPolicy policy) {
    switch (policy) {
        case EchoPolicy::Off:
            return "off";
        case EchoPolicy::StairOnly:
            return "stair";
        case EchoPolicy::TrillOnly:
            return "trill";
        case EchoPolicy::StreamOnly:
            return "stream";
        case EchoPolicy::StairTrill:
            return "stair-trill";
        case EchoPolicy::StairTrillStream:
            return "stair-trill-stream";
        case EchoPolicy::Auto:
            return "auto";
    }
    return "off";
}

std::string toString(StreamEchoProfile profile) {
    switch (profile) {
        case StreamEchoProfile::Conservative:
            return "conservative";
        case StreamEchoProfile::Balanced:
            return "balanced";
        case StreamEchoProfile::Training:
            return "training";
        case StreamEchoProfile::Experimental:
            return "experimental";
    }
    return "conservative";
}

std::string toString(StreamTransformPolicy policy) {
    switch (policy) {
        case StreamTransformPolicy::Off:
            return "off";
        case StreamTransformPolicy::SuperRandom:
            return "superrandom";
        case StreamTransformPolicy::FullJitter:
            return "full-jitter";
    }
    return "off";
}

std::string toString(JackPreservePolicy policy) {
    switch (policy) {
        case JackPreservePolicy::PreserveStrict:
            return "preserve-strict";
        case JackPreservePolicy::PreservePlayable:
            return "preserve-playable";
        case JackPreservePolicy::AvoidNewJacks:
            return "avoid-new-jacks";
        case JackPreservePolicy::SmoothAll:
            return "smooth-all";
    }
    return "preserve-playable";
}

std::string toString(TenKeyPlannerPolicy policy) {
    switch (policy) {
        case TenKeyPlannerPolicy::Auto:
            return "auto";
        case TenKeyPlannerPolicy::Legacy:
            return "legacy";
        case TenKeyPlannerPolicy::StagedNative:
            return "staged-7-9-10";
        case TenKeyPlannerPolicy::StagedMirrorCompress:
            return "staged-7-14-10";
    }
    return "auto";
}

std::optional<ConversionStyle> parseConversionStyle(const std::string& value) {
    if (value == "direct") {
        return ConversionStyle::Direct;
    }
    if (value == "expand") {
        return ConversionStyle::Expand;
    }
    if (value == "compress") {
        return ConversionStyle::Compress;
    }
    if (value == "playable") {
        return ConversionStyle::Playable;
    }
    if (value == "faithful") {
        return ConversionStyle::Faithful;
    }
    if (value == "training") {
        return ConversionStyle::Training;
    }
    if (value == "dp") {
        return ConversionStyle::DP;
    }
    return std::nullopt;
}

std::optional<CollisionPolicy> parseCollisionPolicy(const std::string& value) {
    if (value == "keep") {
        return CollisionPolicy::Keep;
    }
    if (value == "shift-nearest") {
        return CollisionPolicy::ShiftNearest;
    }
    if (value == "merge") {
        return CollisionPolicy::Merge;
    }
    if (value == "drop") {
        return CollisionPolicy::Drop;
    }
    return std::nullopt;
}

std::optional<OptimizerKind> parseOptimizerKind(const std::string& value) {
    if (value == "greedy") {
        return OptimizerKind::Greedy;
    }
    if (value == "beam") {
        return OptimizerKind::Beam;
    }
    return std::nullopt;
}

std::optional<CompressPolicy> parseCompressPolicy(const std::string& value) {
    if (value == "auto") {
        return CompressPolicy::Auto;
    }
    if (value == "preserve-strict") {
        return CompressPolicy::PreserveStrict;
    }
    if (value == "no-overlap-drop") {
        return CompressPolicy::NoOverlapDrop;
    }
    if (value == "no-overlap-roll") {
        return CompressPolicy::NoOverlapRoll;
    }
    if (value == "no-overlap-hybrid") {
        return CompressPolicy::NoOverlapHybrid;
    }
    if (value == "training-simplify") {
        return CompressPolicy::TrainingSimplify;
    }
    return std::nullopt;
}

std::optional<DistancePolicy> parseDistancePolicy(const std::string& value) {
    if (value == "off") {
        return DistancePolicy::Off;
    }
    if (value == "warn" || value == "warn-only") {
        return DistancePolicy::WarnOnly;
    }
    if (value == "aimod-safe") {
        return DistancePolicy::AimodSafe;
    }
    if (value == "strict") {
        return DistancePolicy::Strict;
    }
    return std::nullopt;
}

std::optional<ExpansionPolicy> parseExpansionPolicy(const std::string& value) {
    if (value == "preserve" || value == "preserve-note-count") {
        return ExpansionPolicy::PreserveNoteCount;
    }
    if (value == "preserve-tap-plus-more" || value == "tap-plus-more" || value == "auto-more") {
        return ExpansionPolicy::PreserveTapPlusMore;
    }
    if (value == "preserve-tap-plus" || value == "tap-plus" || value == "auto-normal") {
        return ExpansionPolicy::PreserveTapPlus;
    }
    if (value == "preserve-tap-plus-low" || value == "tap-plus-low" || value == "auto-low") {
        return ExpansionPolicy::PreserveTapPlusLow;
    }
    if (value == "chord-fill") {
        return ExpansionPolicy::DeterministicChordFill;
    }
    if (value == "echo") {
        return ExpansionPolicy::DeterministicEcho;
    }
    if (value == "training-scaffold") {
        return ExpansionPolicy::TrainingScaffold;
    }
    if (value == "harder-remix") {
        return ExpansionPolicy::HarderRemix;
    }
    if (value == "seeded-random") {
        return ExpansionPolicy::SeededRandomRemix;
    }
    return std::nullopt;
}

std::optional<TenKeyPlannerPolicy> parseTenKeyPlannerPolicy(const std::string& value) {
    if (value == "auto") {
        return TenKeyPlannerPolicy::Auto;
    }
    if (value == "legacy") {
        return TenKeyPlannerPolicy::Legacy;
    }
    if (value == "staged" || value == "staged-native" || value == "staged-7-9-10" ||
        value == "native") {
        return TenKeyPlannerPolicy::StagedNative;
    }
    if (value == "staged-7-14-10" || value == "mirror-compress" ||
        value == "staged-mirror-compress") {
        return TenKeyPlannerPolicy::StagedMirrorCompress;
    }
    return std::nullopt;
}

std::optional<EchoPolicy> parseEchoPolicy(const std::string& value) {
    if (value == "off") {
        return EchoPolicy::Off;
    }
    if (value == "stair" || value == "stair-only") {
        return EchoPolicy::StairOnly;
    }
    if (value == "trill" || value == "trill-only") {
        return EchoPolicy::TrillOnly;
    }
    if (value == "stream" || value == "stream-only") {
        return EchoPolicy::StreamOnly;
    }
    if (value == "stair-trill") {
        return EchoPolicy::StairTrill;
    }
    if (value == "stair-trill-stream") {
        return EchoPolicy::StairTrillStream;
    }
    if (value == "auto") {
        return EchoPolicy::Auto;
    }
    return std::nullopt;
}

std::optional<StreamEchoProfile> parseStreamEchoProfile(const std::string& value) {
    if (value == "conservative") {
        return StreamEchoProfile::Conservative;
    }
    if (value == "balanced") {
        return StreamEchoProfile::Balanced;
    }
    if (value == "training") {
        return StreamEchoProfile::Training;
    }
    if (value == "experimental") {
        return StreamEchoProfile::Experimental;
    }
    return std::nullopt;
}

std::optional<StreamTransformPolicy> parseStreamTransformPolicy(const std::string& value) {
    if (value == "off" || value == "none") {
        return StreamTransformPolicy::Off;
    }
    if (value == "superrandom" || value == "super-random") {
        return StreamTransformPolicy::SuperRandom;
    }
    if (value == "full-jitter" || value == "all-jitter" || value == "zure" || value == "jitter") {
        return StreamTransformPolicy::FullJitter;
    }
    return std::nullopt;
}

std::optional<JackPreservePolicy> parseJackPreservePolicy(const std::string& value) {
    if (value == "preserve-strict" || value == "strict") {
        return JackPreservePolicy::PreserveStrict;
    }
    if (value == "preserve-playable" || value == "playable") {
        return JackPreservePolicy::PreservePlayable;
    }
    if (value == "avoid-new-jacks" || value == "avoid-new" || value == "no-new-jacks") {
        return JackPreservePolicy::AvoidNewJacks;
    }
    if (value == "smooth-all" || value == "smooth") {
        return JackPreservePolicy::SmoothAll;
    }
    return std::nullopt;
}

std::string reportToJson(const ConversionReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"sourceKeyCount\": " << report.sourceKeyCount << ",\n";
    out << "  \"targetKeyCount\": " << report.targetKeyCount << ",\n";
    out << "  \"totalNotes\": " << report.totalNotes << ",\n";
    out << "  \"tapNotes\": " << report.tapNotes << ",\n";
    out << "  \"holdNotes\": " << report.holdNotes << ",\n";
    out << "  \"sameTimeCollisions\": " << report.sameTimeCollisions << ",\n";
    out << "  \"longNoteConflicts\": " << report.longNoteConflicts << ",\n";
    out << "  \"shiftedNotes\": " << report.shiftedNotes << ",\n";
    out << "  \"droppedNotes\": " << report.droppedNotes << ",\n";
    out << "  \"mergedNotes\": " << report.mergedNotes << ",\n";
    out << "  \"laneDistribution\": [";
    for (std::size_t i = 0; i < report.laneDistribution.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << report.laneDistribution[i];
    }
    out << "],\n";
    out << "  \"quality\": {\n";
    out << "    \"collisionCount\": " << report.quality.collisionCount << ",\n";
    out << "    \"lnConflictCount\": " << report.quality.lnConflictCount << ",\n";
    out << "    \"densityDelta\": " << report.quality.densityDelta << ",\n";
    out << "    \"chordRateBefore\": " << report.quality.chordRateBefore << ",\n";
    out << "    \"chordRateAfter\": " << report.quality.chordRateAfter << ",\n";
    out << "    \"jackRateBefore\": " << report.quality.jackRateBefore << ",\n";
    out << "    \"jackRateAfter\": " << report.quality.jackRateAfter << ",\n";
    out << "    \"laneDistribution\": [";
    for (std::size_t i = 0; i < report.quality.laneDistribution.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << report.quality.laneDistribution[i];
    }
    out << "],\n";
    out << "    \"laneEntropy\": " << report.quality.laneEntropy << ",\n";
    out << "    \"laneCoverageBefore\": " << report.quality.laneCoverageBefore << ",\n";
    out << "    \"laneCoverageAfter\": " << report.quality.laneCoverageAfter << ",\n";
    out << "    \"laneEntropyBefore\": " << report.quality.laneEntropyBefore << ",\n";
    out << "    \"laneEntropyAfter\": " << report.quality.laneEntropyAfter << ",\n";
    out << "    \"lnAnchorPressureBefore\": " << report.quality.lnAnchorPressureBefore << ",\n";
    out << "    \"lnAnchorPressureAfter\": " << report.quality.lnAnchorPressureAfter << ",\n";
    out << "    \"handSpreadAfter\": " << report.quality.handSpreadAfter << ",\n";
    out << "    \"leftHandNotes\": " << report.quality.leftHandNotes << ",\n";
    out << "    \"rightHandNotes\": " << report.quality.rightHandNotes << ",\n";
    out << "    \"handBalanceRatio\": " << report.quality.handBalanceRatio << ",\n";
    out << "    \"centerBridgeRate\": " << report.quality.centerBridgeRate << ",\n";
    out << "    \"centerSplitBalance\": " << report.quality.centerSplitBalance << ",\n";
    out << "    \"splitChordRate\": " << report.quality.splitChordRate << ",\n";
    out << "    \"kLikenessScore\": " << report.quality.kLikenessScore << ",\n";
    out << "    \"targetProfileChartCount\": " << report.quality.targetProfileChartCount << ",\n";
    out << "    \"targetProfileWindowMs\": " << report.quality.targetProfileWindowMs << ",\n";
    out << "    \"targetProfileName\": \"" << jsonEscape(report.quality.targetProfileName) << "\",\n";
    out << "    \"targetProfileKind\": \"" << jsonEscape(report.quality.targetProfileKind) << "\",\n";
    out << "    \"targetProfileSource\": \"" << jsonEscape(report.quality.targetProfileSource) << "\",\n";
    out << "    \"targetProfileAuthor\": \"" << jsonEscape(report.quality.targetProfileAuthor) << "\",\n";
    out << "    \"laneCoverageScore\": " << report.quality.laneCoverageScore << ",\n";
    out << "    \"laneEntropyScore\": " << report.quality.laneEntropyScore << ",\n";
    out << "    \"edgeUsageScore\": " << report.quality.edgeUsageScore << ",\n";
    out << "    \"activeLaneWindowScore\": " << report.quality.activeLaneWindowScore << ",\n";
    out << "    \"spatialSpanScore\": " << report.quality.spatialSpanScore << ",\n";
    out << "    \"adjacentExpansionScore\": " << report.quality.adjacentExpansionScore << ",\n";
    out << "    \"centerBridgeScore\": " << report.quality.centerBridgeScore << ",\n";
    out << "    \"centerSplitBalanceScore\": " << report.quality.centerSplitBalanceScore << ",\n";
    out << "    \"splitChordScore\": " << report.quality.splitChordScore << ",\n";
    out << "    \"anchorPreserveScore\": " << report.quality.anchorPreserveScore << ",\n";
    out << "    \"patternVocabularyScore\": " << report.quality.patternVocabularyScore << ",\n";
    out << "    \"addedRatioFitScore\": " << report.quality.addedRatioFitScore << ",\n";
    out << "    \"targetKSafetyScore\": " << report.quality.targetKSafetyScore << ",\n";
    out << "    \"feelTags\": [";
    for (std::size_t i = 0; i < report.quality.feelTags.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << jsonEscape(report.quality.feelTags[i]) << "\"";
    }
    out << "],\n";
    out << "    \"orderPreserveScore\": " << report.quality.orderPreserveScore << ",\n";
    out << "    \"spanPreserveScore\": " << report.quality.spanPreserveScore << ",\n";
    out << "    \"patternPreserveScore\": " << report.quality.patternPreserveScore << ",\n";
    if (report.quality.dpHandBalance.has_value()) {
        out << "    \"dpHandBalance\": " << *report.quality.dpHandBalance << ",\n";
    } else {
        out << "    \"dpHandBalance\": null,\n";
    }
    out << "    \"playabilityScore\": " << report.quality.playabilityScore << ",\n";
    out << "    \"impossibleSlices\": " << report.quality.impossibleSlices << ",\n";
    out << "    \"droppedByCompression\": " << report.quality.droppedByCompression << ",\n";
    out << "    \"rolledByCompression\": " << report.quality.rolledByCompression << ",\n";
    out << "    \"shortenedHolds\": " << report.quality.shortenedHolds << ",\n";
    out << "    \"tapifiedHolds\": " << report.quality.tapifiedHolds << ",\n";
    out << "    \"noOverlapGuaranteed\": " << (report.quality.noOverlapGuaranteed ? "true" : "false") << ",\n";
    out << "    \"nearTimeConflicts\": " << report.quality.nearTimeConflicts << ",\n";
    out << "    \"sameLaneNearConflicts\": " << report.quality.sameLaneNearConflicts << ",\n";
    out << "    \"unsnappedNotes\": " << report.quality.unsnappedNotes << ",\n";
    out << "    \"unsnappedRolledNotes\": " << report.quality.unsnappedRolledNotes << ",\n";
    out << "    \"minPositiveDeltaMs\": " << report.quality.minPositiveDeltaMs << ",\n";
    out << "    \"droppedByDistanceGuard\": " << report.quality.droppedByDistanceGuard << ",\n";
    out << "    \"rerolledByDistanceGuard\": " << report.quality.rerolledByDistanceGuard << ",\n";
    out << "    \"deterministic\": " << (report.quality.deterministic ? "true" : "false") << ",\n";
    out << "    \"algorithmVersion\": \"" << jsonEscape(report.quality.algorithmVersion) << "\",\n";
    out << "    \"expansionPolicy\": \"" << jsonEscape(report.quality.expansionPolicy) << "\",\n";
    out << "    \"tenKeyPlanner\": \"" << jsonEscape(report.quality.tenKeyPlanner) << "\",\n";
    out << "    \"streamEchoProfile\": \"" << jsonEscape(report.quality.streamEchoProfile) << "\",\n";
    out << "    \"streamTransformPolicy\": \"" << jsonEscape(report.quality.streamTransformPolicy) << "\",\n";
    out << "    \"expansionComposerProfile\": \"" << jsonEscape(report.quality.expansionComposerProfile) << "\",\n";
    out << "    \"targetAddedNoteRatio\": " << report.quality.targetAddedNoteRatio << ",\n";
    out << "    \"budgetUsedRatio\": " << report.quality.budgetUsedRatio << ",\n";
    out << "    \"adaptiveGrowthBudgetEnabled\": "
        << (report.quality.adaptiveGrowthBudgetEnabled ? "true" : "false") << ",\n";
    out << "    \"adaptiveBudgetWindowMs\": " << report.quality.adaptiveBudgetWindowMs << ",\n";
    out << "    \"adaptiveBudgetWindows\": " << report.quality.adaptiveBudgetWindows << ",\n";
    out << "    \"adaptiveBudgetAverageRatio\": " << report.quality.adaptiveBudgetAverageRatio << ",\n";
    out << "    \"adaptiveBudgetMinRatio\": " << report.quality.adaptiveBudgetMinRatio << ",\n";
    out << "    \"adaptiveBudgetMaxRatio\": " << report.quality.adaptiveBudgetMaxRatio << ",\n";
    out << "    \"acceptedByComposer\": " << report.quality.acceptedByComposer << ",\n";
    out << "    \"rejectedByComposerBudget\": " << report.quality.rejectedByComposerBudget << ",\n";
    out << "    \"rejectedByComposerSafety\": " << report.quality.rejectedByComposerSafety << ",\n";
    out << "    \"rejectedByAdaptiveBudget\": " << report.quality.rejectedByAdaptiveBudget << ",\n";
    out << "    \"addedNotes\": " << report.quality.addedNotes << ",\n";
    out << "    \"addedByTapPlus\": " << report.quality.addedByTapPlus << ",\n";
    out << "    \"addedByChordFill\": " << report.quality.addedByChordFill << ",\n";
    out << "    \"addedByEcho\": " << report.quality.addedByEcho << ",\n";
    out << "    \"addedByStairEcho\": " << report.quality.addedByStairEcho << ",\n";
    out << "    \"addedByTrillEcho\": " << report.quality.addedByTrillEcho << ",\n";
    out << "    \"addedByStreamEcho\": " << report.quality.addedByStreamEcho << ",\n";
    out << "    \"addedByTrainingScaffold\": " << report.quality.addedByTrainingScaffold << ",\n";
    out << "    \"rejectedExpansionCandidates\": " << report.quality.rejectedExpansionCandidates << ",\n";
    out << "    \"rejectedByCollision\": " << report.quality.rejectedByCollision << ",\n";
    out << "    \"rejectedByDistance\": " << report.quality.rejectedByDistance << ",\n";
    out << "    \"rejectedBySnap\": " << report.quality.rejectedBySnap << ",\n";
    out << "    \"rejectedByBudget\": " << report.quality.rejectedByBudget << ",\n";
    out << "    \"unsnappedAddedNotes\": " << report.quality.unsnappedAddedNotes << ",\n";
    out << "    \"addedNoteRatio\": " << report.quality.addedNoteRatio << ",\n";
    out << "    \"jackPreservePolicy\": \"" << jsonEscape(report.quality.jackPreservePolicy) << "\",\n";
    out << "    \"sourceJackGroups\": " << report.quality.sourceJackGroups << ",\n";
    out << "    \"preservedJackGroups\": " << report.quality.preservedJackGroups << ",\n";
    out << "    \"splitJackGroups\": " << report.quality.splitJackGroups << ",\n";
    out << "    \"createdJacks\": " << report.quality.createdJacks << ",\n";
    out << "    \"preventedJacks\": " << report.quality.preventedJacks << ",\n";
    out << "    \"createdJacksFromBaseMapping\": " << report.quality.createdJacksFromBaseMapping << ",\n";
    out << "    \"createdJacksFromRepair\": " << report.quality.createdJacksFromRepair << ",\n";
    out << "    \"createdJacksFromAddedNotes\": " << report.quality.createdJacksFromAddedNotes << ",\n";
    out << "    \"preventedJacksByAssignment\": " << report.quality.preventedJacksByAssignment << ",\n";
    out << "    \"preventedJacksByRepair\": " << report.quality.preventedJacksByRepair << ",\n";
    out << "    \"preventedJacksByExpansion\": " << report.quality.preventedJacksByExpansion << ",\n";
    out << "    \"sanitizedCreatedJacks\": " << report.quality.sanitizedCreatedJacks << ",\n";
    out << "    \"unsolvedCreatedJacks\": " << report.quality.unsolvedCreatedJacks << ",\n";
    out << "    \"smoothedJacks\": " << report.quality.smoothedJacks << ",\n";
    out << "    \"jackPreserveScore\": " << report.quality.jackPreserveScore << ",\n";
    out << "    \"createdJackRate\": " << report.quality.createdJackRate << ",\n";
    out << "    \"detectedStairs\": " << report.quality.detectedStairs << ",\n";
    out << "    \"preservedStairs\": " << report.quality.preservedStairs << ",\n";
    out << "    \"brokenStairs\": " << report.quality.brokenStairs << ",\n";
    out << "    \"detectedTrills\": " << report.quality.detectedTrills << ",\n";
    out << "    \"preservedTrills\": " << report.quality.preservedTrills << ",\n";
    out << "    \"brokenTrills\": " << report.quality.brokenTrills << ",\n";
    out << "    \"detectedJacks\": " << report.quality.detectedJacks << ",\n";
    out << "    \"preservedJacks\": " << report.quality.preservedJacks << ",\n";
    out << "    \"brokenJacks\": " << report.quality.brokenJacks << ",\n";
    out << "    \"handZoneBreaks\": " << report.quality.handZoneBreaks << ",\n";
    out << "    \"motifDirectionFlips\": " << report.quality.motifDirectionFlips << ",\n";
    out << "    \"motifLaneScatterCount\": " << report.quality.motifLaneScatterCount << ",\n";
    out << "    \"gesturePreservationScore\": " << report.quality.gesturePreservationScore << ",\n";
    out << "    \"gestureRailEnabled\": " << (report.quality.gestureRailEnabled ? "true" : "false") << ",\n";
    out << "    \"rejectedEchoCandidates\": " << report.quality.rejectedEchoCandidates << ",\n";
    out << "    \"rejectedEchoByDensity\": " << report.quality.rejectedEchoByDensity << ",\n";
    out << "    \"rejectedEchoByDistance\": " << report.quality.rejectedEchoByDistance << ",\n";
    out << "    \"rejectedEchoBySnap\": " << report.quality.rejectedEchoBySnap << ",\n";
    out << "    \"rejectedEchoByBudget\": " << report.quality.rejectedEchoByBudget << ",\n";
    out << "    \"echoAddedRatio\": " << report.quality.echoAddedRatio << ",\n";
    out << "    \"rejectedStreamEchoByBurst\": " << report.quality.rejectedStreamEchoByBurst << ",\n";
    out << "    \"rejectedStreamEchoByJack\": " << report.quality.rejectedStreamEchoByJack << ",\n";
    out << "    \"rejectedStreamEchoByLNHeavy\": " << report.quality.rejectedStreamEchoByLNHeavy << ",\n";
    out << "    \"rejectedStreamEchoByLocalNps\": " << report.quality.rejectedStreamEchoByLocalNps << ",\n";
    out << "    \"streamEchoCandidates\": " << report.quality.streamEchoCandidates << ",\n";
    out << "    \"streamRawPatternCandidates\": " << report.quality.streamRawPatternCandidates << ",\n";
    out << "    \"streamEligiblePatternCandidates\": " << report.quality.streamEligiblePatternCandidates << ",\n";
    out << "    \"streamRawLaneCandidates\": " << report.quality.streamRawLaneCandidates << ",\n";
    out << "    \"streamSafeLaneCandidates\": " << report.quality.streamSafeLaneCandidates << ",\n";
    out << "    \"streamAcceptedCandidates\": " << report.quality.streamAcceptedCandidates << ",\n";
    out << "    \"rejectedStreamEchoByNoUnderusedLane\": "
        << report.quality.rejectedStreamEchoByNoUnderusedLane << ",\n";
    out << "    \"rejectedStreamEchoByPatternConfidence\": "
        << report.quality.rejectedStreamEchoByPatternConfidence << ",\n";
    out << "    \"rejectedStreamEchoByPatternLength\": "
        << report.quality.rejectedStreamEchoByPatternLength << ",\n";
    out << "    \"rejectedStreamEchoBySliceChordFull\": "
        << report.quality.rejectedStreamEchoBySliceChordFull << ",\n";
    out << "    \"rejectedStreamEchoByLaneRole\": " << report.quality.rejectedStreamEchoByLaneRole << ",\n";
    out << "    \"rejectedStreamPrimaryByPatternConfidence\": "
        << report.quality.rejectedStreamPrimaryByPatternConfidence << ",\n";
    out << "    \"rejectedStreamPrimaryByPatternLength\": "
        << report.quality.rejectedStreamPrimaryByPatternLength << ",\n";
    out << "    \"rejectedStreamPrimaryByBurst\": " << report.quality.rejectedStreamPrimaryByBurst << ",\n";
    out << "    \"rejectedStreamPrimaryByJack\": " << report.quality.rejectedStreamPrimaryByJack << ",\n";
    out << "    \"rejectedStreamPrimaryByLNHeavy\": " << report.quality.rejectedStreamPrimaryByLNHeavy << ",\n";
    out << "    \"rejectedStreamPrimaryByLocalNps\": "
        << report.quality.rejectedStreamPrimaryByLocalNps << ",\n";
    out << "    \"rejectedStreamPrimaryByNoUnderusedLane\": "
        << report.quality.rejectedStreamPrimaryByNoUnderusedLane << ",\n";
    out << "    \"rejectedStreamPrimaryBySliceChordFull\": "
        << report.quality.rejectedStreamPrimaryBySliceChordFull << ",\n";
    out << "    \"rejectedStreamPrimaryByLaneRole\": "
        << report.quality.rejectedStreamPrimaryByLaneRole << ",\n";
    out << "    \"rejectedStreamPrimaryByCollision\": "
        << report.quality.rejectedStreamPrimaryByCollision << ",\n";
    out << "    \"rejectedStreamPrimaryByDistance\": "
        << report.quality.rejectedStreamPrimaryByDistance << ",\n";
    out << "    \"rejectedStreamPrimaryBySnap\": " << report.quality.rejectedStreamPrimaryBySnap << ",\n";
    out << "    \"rejectedStreamPrimaryByBudget\": " << report.quality.rejectedStreamPrimaryByBudget << ",\n";
    out << "    \"streamEchoAddedRatio\": " << report.quality.streamEchoAddedRatio << ",\n";
    out << "    \"maxObservedLocalNpsAfterEcho\": " << report.quality.maxObservedLocalNpsAfterEcho << ",\n";
    out << "    \"streamTransformedNotes\": " << report.quality.streamTransformedNotes << ",\n";
    out << "    \"streamJitteredNotes\": " << report.quality.streamJitteredNotes << "\n";
    out << "  },\n";
    out << "  \"warnings\": [";
    for (std::size_t i = 0; i < report.warnings.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << "\"" << jsonEscape(report.warnings[i]) << "\"";
    }
    out << "]\n";
    out << "}\n";
    return out.str();
}

std::string reportToText(const ConversionReport& report) {
    std::ostringstream out;
    out << "Notes: " << report.totalNotes << "\n";
    out << "Tap notes: " << report.tapNotes << "\n";
    out << "Hold notes: " << report.holdNotes << "\n";
    out << "Same-time collisions: " << report.sameTimeCollisions << "\n";
    out << "LN conflicts: " << report.longNoteConflicts << "\n";
    out << "Shifted notes: " << report.shiftedNotes << "\n";
    out << "Dropped notes: " << report.droppedNotes << "\n";
    out << "Merged notes: " << report.mergedNotes << "\n";
    out << "Lane distribution: [";
    for (std::size_t i = 0; i < report.laneDistribution.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << report.laneDistribution[i];
    }
    out << "]\n";
    out << "Quality:\n";
    out << "- Density delta: " << report.quality.densityDelta << "\n";
    out << "- Chord rate before: " << report.quality.chordRateBefore << "\n";
    out << "- Chord rate after: " << report.quality.chordRateAfter << "\n";
    out << "- Jack rate before: " << report.quality.jackRateBefore << "\n";
    out << "- Jack rate after: " << report.quality.jackRateAfter << "\n";
    out << "- Lane entropy: " << report.quality.laneEntropy << "\n";
    out << "- Lane coverage before: " << report.quality.laneCoverageBefore << "\n";
    out << "- Lane coverage after: " << report.quality.laneCoverageAfter << "\n";
    out << "- Lane entropy before: " << report.quality.laneEntropyBefore << "\n";
    out << "- Lane entropy after: " << report.quality.laneEntropyAfter << "\n";
    out << "- LN anchor pressure before: " << report.quality.lnAnchorPressureBefore << "\n";
    out << "- LN anchor pressure after: " << report.quality.lnAnchorPressureAfter << "\n";
    out << "- Hand spread after: " << report.quality.handSpreadAfter << "\n";
    out << "- Left hand notes: " << report.quality.leftHandNotes << "\n";
    out << "- Right hand notes: " << report.quality.rightHandNotes << "\n";
    out << "- Hand balance ratio: " << report.quality.handBalanceRatio << "\n";
    out << "- Center bridge rate: " << report.quality.centerBridgeRate << "\n";
    out << "- Center split balance: " << report.quality.centerSplitBalance << "\n";
    out << "- Split chord rate: " << report.quality.splitChordRate << "\n";
    out << "- K-likeness score: " << report.quality.kLikenessScore << "\n";
    out << "- Target profile charts: " << report.quality.targetProfileChartCount << "\n";
    out << "- Target profile window: " << report.quality.targetProfileWindowMs << " ms\n";
    out << "- Target profile name: " << report.quality.targetProfileName << "\n";
    out << "- Target profile kind: " << report.quality.targetProfileKind << "\n";
    out << "- Target profile source: " << report.quality.targetProfileSource << "\n";
    out << "- Target profile author: " << report.quality.targetProfileAuthor << "\n";
    out << "- Lane coverage score: " << report.quality.laneCoverageScore << "\n";
    out << "- Lane entropy score: " << report.quality.laneEntropyScore << "\n";
    out << "- Edge usage score: " << report.quality.edgeUsageScore << "\n";
    out << "- Active lane window score: " << report.quality.activeLaneWindowScore << "\n";
    out << "- Spatial span score: " << report.quality.spatialSpanScore << "\n";
    out << "- Adjacent expansion score: " << report.quality.adjacentExpansionScore << "\n";
    out << "- Center bridge score: " << report.quality.centerBridgeScore << "\n";
    out << "- Center split balance score: " << report.quality.centerSplitBalanceScore << "\n";
    out << "- Split chord score: " << report.quality.splitChordScore << "\n";
    out << "- Anchor preserve score: " << report.quality.anchorPreserveScore << "\n";
    out << "- Pattern vocabulary score: " << report.quality.patternVocabularyScore << "\n";
    out << "- Added ratio fit score: " << report.quality.addedRatioFitScore << "\n";
    out << "- Target-K safety score: " << report.quality.targetKSafetyScore << "\n";
    out << "- Feel tags: [";
    for (std::size_t i = 0; i < report.quality.feelTags.size(); ++i) {
        if (i > 0) {
            out << ", ";
        }
        out << report.quality.feelTags[i];
    }
    out << "]\n";
    out << "- Order preserve: " << report.quality.orderPreserveScore << "\n";
    out << "- Span preserve: " << report.quality.spanPreserveScore << "\n";
    out << "- Pattern preserve: " << report.quality.patternPreserveScore << "\n";
    out << "- Playability score: " << report.quality.playabilityScore << "\n";
    out << "- Impossible slices: " << report.quality.impossibleSlices << "\n";
    out << "- Dropped by compression: " << report.quality.droppedByCompression << "\n";
    out << "- Rolled by compression: " << report.quality.rolledByCompression << "\n";
    out << "- Tapified holds: " << report.quality.tapifiedHolds << "\n";
    out << "- No-overlap guaranteed: " << (report.quality.noOverlapGuaranteed ? "yes" : "no") << "\n";
    out << "- Near-time conflicts: " << report.quality.nearTimeConflicts << "\n";
    out << "- Same-lane near conflicts: " << report.quality.sameLaneNearConflicts << "\n";
    out << "- Unsnapped notes: " << report.quality.unsnappedNotes << "\n";
    out << "- Unsnapped rolled notes: " << report.quality.unsnappedRolledNotes << "\n";
    out << "- Min positive delta: " << report.quality.minPositiveDeltaMs << " ms\n";
    out << "- Dropped by distance guard: " << report.quality.droppedByDistanceGuard << "\n";
    out << "- Rerolled by distance guard: " << report.quality.rerolledByDistanceGuard << "\n";
    out << "- Deterministic: " << (report.quality.deterministic ? "yes" : "no") << "\n";
    out << "- Algorithm version: " << report.quality.algorithmVersion << "\n";
    out << "- Expansion policy: " << report.quality.expansionPolicy << "\n";
    out << "- 10K planner: " << report.quality.tenKeyPlanner << "\n";
    out << "- Stream echo profile: " << report.quality.streamEchoProfile << "\n";
    out << "- Stream transform policy: " << report.quality.streamTransformPolicy << "\n";
    out << "- Expansion composer profile: " << report.quality.expansionComposerProfile << "\n";
    out << "- Target added note ratio: " << report.quality.targetAddedNoteRatio << "\n";
    out << "- Budget used ratio: " << report.quality.budgetUsedRatio << "\n";
    out << "- Adaptive growth budget: " << (report.quality.adaptiveGrowthBudgetEnabled ? "yes" : "no") << "\n";
    out << "- Adaptive budget window: " << report.quality.adaptiveBudgetWindowMs << " ms\n";
    out << "- Adaptive budget windows: " << report.quality.adaptiveBudgetWindows << "\n";
    out << "- Adaptive budget average ratio: " << report.quality.adaptiveBudgetAverageRatio << "\n";
    out << "- Adaptive budget min ratio: " << report.quality.adaptiveBudgetMinRatio << "\n";
    out << "- Adaptive budget max ratio: " << report.quality.adaptiveBudgetMaxRatio << "\n";
    out << "- Accepted by composer: " << report.quality.acceptedByComposer << "\n";
    out << "- Rejected by composer budget: " << report.quality.rejectedByComposerBudget << "\n";
    out << "- Rejected by composer safety: " << report.quality.rejectedByComposerSafety << "\n";
    out << "- Rejected by adaptive budget: " << report.quality.rejectedByAdaptiveBudget << "\n";
    out << "- Added notes: " << report.quality.addedNotes << "\n";
    out << "- Added by tap plus: " << report.quality.addedByTapPlus << "\n";
    out << "- Added by chord fill: " << report.quality.addedByChordFill << "\n";
    out << "- Added by echo: " << report.quality.addedByEcho << "\n";
    out << "- Added by stair echo: " << report.quality.addedByStairEcho << "\n";
    out << "- Added by trill echo: " << report.quality.addedByTrillEcho << "\n";
    out << "- Added by stream echo: " << report.quality.addedByStreamEcho << "\n";
    out << "- Added by training scaffold: " << report.quality.addedByTrainingScaffold << "\n";
    out << "- Added note ratio: " << report.quality.addedNoteRatio << "\n";
    out << "- Jack preserve policy: " << report.quality.jackPreservePolicy << "\n";
    out << "- Source jack groups: " << report.quality.sourceJackGroups << "\n";
    out << "- Preserved jack groups: " << report.quality.preservedJackGroups << "\n";
    out << "- Split jack groups: " << report.quality.splitJackGroups << "\n";
    out << "- Created jacks: " << report.quality.createdJacks << "\n";
    out << "- Prevented jacks: " << report.quality.preventedJacks << "\n";
    out << "- Created jacks from base mapping: " << report.quality.createdJacksFromBaseMapping << "\n";
    out << "- Created jacks from repair: " << report.quality.createdJacksFromRepair << "\n";
    out << "- Created jacks from added notes: " << report.quality.createdJacksFromAddedNotes << "\n";
    out << "- Prevented jacks by assignment: " << report.quality.preventedJacksByAssignment << "\n";
    out << "- Prevented jacks by repair: " << report.quality.preventedJacksByRepair << "\n";
    out << "- Prevented jacks by expansion: " << report.quality.preventedJacksByExpansion << "\n";
    out << "- Sanitized created jacks: " << report.quality.sanitizedCreatedJacks << "\n";
    out << "- Unsolved created jacks: " << report.quality.unsolvedCreatedJacks << "\n";
    out << "- Smoothed jacks: " << report.quality.smoothedJacks << "\n";
    out << "- Jack preserve score: " << report.quality.jackPreserveScore << "\n";
    out << "- Created jack rate: " << report.quality.createdJackRate << "\n";
    out << "- Gesture rail enabled: " << (report.quality.gestureRailEnabled ? "yes" : "no") << "\n";
    out << "- Detected stairs: " << report.quality.detectedStairs << "\n";
    out << "- Preserved stairs: " << report.quality.preservedStairs << "\n";
    out << "- Broken stairs: " << report.quality.brokenStairs << "\n";
    out << "- Detected trills: " << report.quality.detectedTrills << "\n";
    out << "- Preserved trills: " << report.quality.preservedTrills << "\n";
    out << "- Broken trills: " << report.quality.brokenTrills << "\n";
    out << "- Detected jacks: " << report.quality.detectedJacks << "\n";
    out << "- Preserved jacks: " << report.quality.preservedJacks << "\n";
    out << "- Broken jacks: " << report.quality.brokenJacks << "\n";
    out << "- Hand zone breaks: " << report.quality.handZoneBreaks << "\n";
    out << "- Motif direction flips: " << report.quality.motifDirectionFlips << "\n";
    out << "- Motif lane scatter count: " << report.quality.motifLaneScatterCount << "\n";
    out << "- Gesture preservation score: " << report.quality.gesturePreservationScore << "\n";
    out << "- Rejected expansion candidates: " << report.quality.rejectedExpansionCandidates << "\n";
    out << "- Rejected by collision: " << report.quality.rejectedByCollision << "\n";
    out << "- Rejected by distance: " << report.quality.rejectedByDistance << "\n";
    out << "- Rejected by snap: " << report.quality.rejectedBySnap << "\n";
    out << "- Rejected by budget: " << report.quality.rejectedByBudget << "\n";
    out << "- Unsnapped added notes: " << report.quality.unsnappedAddedNotes << "\n";
    out << "- Rejected echo candidates: " << report.quality.rejectedEchoCandidates << "\n";
    out << "- Rejected echo by density: " << report.quality.rejectedEchoByDensity << "\n";
    out << "- Rejected echo by distance: " << report.quality.rejectedEchoByDistance << "\n";
    out << "- Rejected echo by snap: " << report.quality.rejectedEchoBySnap << "\n";
    out << "- Rejected echo by budget: " << report.quality.rejectedEchoByBudget << "\n";
    out << "- Echo added ratio: " << report.quality.echoAddedRatio << "\n";
    out << "- Rejected stream echo by burst: " << report.quality.rejectedStreamEchoByBurst << "\n";
    out << "- Rejected stream echo by jack: " << report.quality.rejectedStreamEchoByJack << "\n";
    out << "- Rejected stream echo by LN-heavy: " << report.quality.rejectedStreamEchoByLNHeavy << "\n";
    out << "- Rejected stream echo by local NPS: " << report.quality.rejectedStreamEchoByLocalNps << "\n";
    out << "- Stream echo candidates: " << report.quality.streamEchoCandidates << "\n";
    out << "- Stream raw pattern candidates: " << report.quality.streamRawPatternCandidates << "\n";
    out << "- Stream eligible pattern candidates: " << report.quality.streamEligiblePatternCandidates << "\n";
    out << "- Stream raw lane candidates: " << report.quality.streamRawLaneCandidates << "\n";
    out << "- Stream safe lane candidates: " << report.quality.streamSafeLaneCandidates << "\n";
    out << "- Stream accepted candidates: " << report.quality.streamAcceptedCandidates << "\n";
    out << "- Rejected stream echo by no underused lane: "
        << report.quality.rejectedStreamEchoByNoUnderusedLane << "\n";
    out << "- Rejected stream echo by pattern confidence: "
        << report.quality.rejectedStreamEchoByPatternConfidence << "\n";
    out << "- Rejected stream echo by pattern length: "
        << report.quality.rejectedStreamEchoByPatternLength << "\n";
    out << "- Rejected stream echo by slice chord full: "
        << report.quality.rejectedStreamEchoBySliceChordFull << "\n";
    out << "- Rejected stream echo by lane role: " << report.quality.rejectedStreamEchoByLaneRole << "\n";
    out << "- Rejected stream primary by pattern confidence: "
        << report.quality.rejectedStreamPrimaryByPatternConfidence << "\n";
    out << "- Rejected stream primary by pattern length: "
        << report.quality.rejectedStreamPrimaryByPatternLength << "\n";
    out << "- Rejected stream primary by burst: " << report.quality.rejectedStreamPrimaryByBurst << "\n";
    out << "- Rejected stream primary by jack: " << report.quality.rejectedStreamPrimaryByJack << "\n";
    out << "- Rejected stream primary by LN-heavy: " << report.quality.rejectedStreamPrimaryByLNHeavy << "\n";
    out << "- Rejected stream primary by local NPS: "
        << report.quality.rejectedStreamPrimaryByLocalNps << "\n";
    out << "- Rejected stream primary by no underused lane: "
        << report.quality.rejectedStreamPrimaryByNoUnderusedLane << "\n";
    out << "- Rejected stream primary by slice chord full: "
        << report.quality.rejectedStreamPrimaryBySliceChordFull << "\n";
    out << "- Rejected stream primary by lane role: " << report.quality.rejectedStreamPrimaryByLaneRole << "\n";
    out << "- Rejected stream primary by collision: " << report.quality.rejectedStreamPrimaryByCollision << "\n";
    out << "- Rejected stream primary by distance: " << report.quality.rejectedStreamPrimaryByDistance << "\n";
    out << "- Rejected stream primary by snap: " << report.quality.rejectedStreamPrimaryBySnap << "\n";
    out << "- Rejected stream primary by budget: " << report.quality.rejectedStreamPrimaryByBudget << "\n";
    out << "- Stream echo added ratio: " << report.quality.streamEchoAddedRatio << "\n";
    out << "- Max observed local NPS after echo: " << report.quality.maxObservedLocalNpsAfterEcho << "\n";
    out << "- Stream transformed notes: " << report.quality.streamTransformedNotes << "\n";
    out << "- Stream jittered notes: " << report.quality.streamJitteredNotes << "\n";
    if (!report.warnings.empty()) {
        out << "Warnings:\n";
        constexpr std::size_t maxTextWarnings = 20;
        const auto shownWarnings = std::min(maxTextWarnings, report.warnings.size());
        for (std::size_t i = 0; i < shownWarnings; ++i) {
            out << "- " << report.warnings[i] << "\n";
        }
        if (report.warnings.size() > shownWarnings) {
            out << "- ... " << (report.warnings.size() - shownWarnings)
                << " more warnings omitted from console output; see JSON report for full details.\n";
        }
    }
    return out.str();
}

}  // namespace keyconv
