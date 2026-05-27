#pragma once

#include <optional>
#include <string>
#include <vector>

namespace keyconv {

enum class ConversionStyle {
    Direct,
    Expand,
    Compress,
    Playable,
    Faithful,
    Training,
    DP,
};

enum class CollisionPolicy {
    Keep,
    ShiftNearest,
    Merge,
    Drop,
};

enum class OptimizerKind {
    Greedy,
    Beam,
};

enum class CompressPolicy {
    Auto,
    PreserveStrict,
    NoOverlapDrop,
    NoOverlapRoll,
    NoOverlapHybrid,
    TrainingSimplify,
};

enum class DistancePolicy {
    Off,
    WarnOnly,
    AimodSafe,
    Strict,
};

enum class ExpansionPolicy {
    PreserveNoteCount,
    PreserveTapPlusMore,
    PreserveTapPlus,
    PreserveTapPlusLow,
    DeterministicChordFill,
    DeterministicEcho,
    TrainingScaffold,
    HarderRemix,
    SeededRandomRemix,
};

enum class EchoPolicy {
    Off,
    StairOnly,
    TrillOnly,
    StreamOnly,
    StairTrill,
    StairTrillStream,
    Auto,
};

enum class StreamEchoProfile {
    Conservative,
    Balanced,
    Training,
    Experimental,
};

enum class StreamTransformPolicy {
    Off,
    SuperRandom,
    FullJitter,
};

enum class JackPreservePolicy {
    PreserveStrict,
    PreservePlayable,
    AvoidNewJacks,
    SmoothAll,
};

struct TargetKFeatureStat {
    bool present = false;
    double mean = 0.0;
    double median = 0.0;
    double iqr = 0.0;
    double p10 = 0.0;
    double p25 = 0.0;
    double p75 = 0.0;
    double p90 = 0.0;
};

struct TargetKBucketProfile {
    bool present = false;
    int windowCount = 0;
    TargetKFeatureStat activeLaneRate;
    TargetKFeatureStat adjacentExpansion;
    TargetKFeatureStat chordRate;
    TargetKFeatureStat chordSpan;
    TargetKFeatureStat densityNps;
    TargetKFeatureStat edgeUsage;
    TargetKFeatureStat handBalance;
    TargetKFeatureStat holdRate;
    TargetKFeatureStat jackRisk;
    TargetKFeatureStat laneEntropy;
};

struct TargetKDensityBuckets {
    bool present = false;
    double lowMaxNps = 7.0;
    double midMaxNps = 15.0;
    TargetKBucketProfile all;
    TargetKBucketProfile low;
    TargetKBucketProfile mid;
    TargetKBucketProfile high;
    TargetKBucketProfile lnHeavy;
    TargetKBucketProfile chordHeavy;
    TargetKBucketProfile jackRisk;
};

struct TargetKProfile {
    int targetKeys = 10;
    int sampleCount = 0;
    int windowMs = 1000;
    std::string profileName = "builtin_10k_profile";
    std::string profileKind = "builtin";
    std::string sourceName = "builtin";
    std::string authorToken;
    double desiredLaneEntropy = 0.90;
    double desiredEdgeUsage = 0.34;
    double desiredActiveLaneRate = 0.80;
    double desiredChordSpan = 0.45;
    double desiredHandBalance = 0.88;
    double desiredAdjacentExpansion = 0.18;
    TargetKDensityBuckets densityBuckets;
};

struct ConversionReport {
    int sourceKeyCount = 0;
    int targetKeyCount = 0;
    int totalNotes = 0;
    int tapNotes = 0;
    int holdNotes = 0;
    int sameTimeCollisions = 0;
    int longNoteConflicts = 0;
    int shiftedNotes = 0;
    int droppedNotes = 0;
    int mergedNotes = 0;
    std::vector<int> laneDistribution;
    std::vector<std::string> warnings;

    struct QualityMetrics {
        int collisionCount = 0;
        int lnConflictCount = 0;
        double densityDelta = 0.0;
        double chordRateBefore = 0.0;
        double chordRateAfter = 0.0;
        double jackRateBefore = 0.0;
        double jackRateAfter = 0.0;
        std::vector<int> laneDistribution;
        double laneEntropy = 0.0;
        double laneCoverageBefore = 0.0;
        double laneCoverageAfter = 0.0;
        double laneEntropyBefore = 0.0;
        double laneEntropyAfter = 0.0;
        double lnAnchorPressureBefore = 0.0;
        double lnAnchorPressureAfter = 0.0;
        double handSpreadAfter = 0.0;
        int leftHandNotes = 0;
        int rightHandNotes = 0;
        double handBalanceRatio = 1.0;
        double kLikenessScore = 0.0;
        int targetProfileChartCount = 0;
        int targetProfileWindowMs = 0;
        std::string targetProfileName = "builtin_10k_profile";
        std::string targetProfileKind = "builtin";
        std::string targetProfileSource = "builtin";
        std::string targetProfileAuthor;
        double laneCoverageScore = 0.0;
        double laneEntropyScore = 0.0;
        double edgeUsageScore = 0.0;
        double activeLaneWindowScore = 0.0;
        double spatialSpanScore = 0.0;
        double adjacentExpansionScore = 0.0;
        double anchorPreserveScore = 1.0;
        double patternVocabularyScore = 1.0;
        double addedRatioFitScore = 0.0;
        double targetKSafetyScore = 1.0;
        std::vector<std::string> feelTags;
        double orderPreserveScore = 0.0;
        double spanPreserveScore = 0.0;
        double patternPreserveScore = 0.0;
        std::optional<double> dpHandBalance;
        double playabilityScore = 0.0;
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
        bool deterministic = true;
        std::string algorithmVersion = "v0.5.7";
        std::string expansionPolicy = "preserve";
        std::string streamEchoProfile = "conservative";
        std::string streamTransformPolicy = "off";
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
        std::string jackPreservePolicy = "preserve-playable";
        int sourceJackGroups = 0;
        int preservedJackGroups = 0;
        int splitJackGroups = 0;
        int createdJacks = 0;
        int preventedJacks = 0;
        int createdJacksFromBaseMapping = 0;
        int createdJacksFromRepair = 0;
        int createdJacksFromAddedNotes = 0;
        int preventedJacksByAssignment = 0;
        int preventedJacksByRepair = 0;
        int preventedJacksByExpansion = 0;
        int sanitizedCreatedJacks = 0;
        int unsolvedCreatedJacks = 0;
        int smoothedJacks = 0;
        double jackPreserveScore = 1.0;
        double createdJackRate = 0.0;
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
        int streamTransformedNotes = 0;
        int streamJitteredNotes = 0;
    } quality;
};

using QualityReport = ConversionReport::QualityMetrics;

std::string toString(ConversionStyle style);
std::string toString(CollisionPolicy policy);
std::string toString(OptimizerKind optimizer);
std::string toString(CompressPolicy policy);
std::string toString(DistancePolicy policy);
std::string toString(ExpansionPolicy policy);
std::string toString(EchoPolicy policy);
std::string toString(StreamEchoProfile profile);
std::string toString(StreamTransformPolicy policy);
std::string toString(JackPreservePolicy policy);
std::optional<ConversionStyle> parseConversionStyle(const std::string& value);
std::optional<CollisionPolicy> parseCollisionPolicy(const std::string& value);
std::optional<OptimizerKind> parseOptimizerKind(const std::string& value);
std::optional<CompressPolicy> parseCompressPolicy(const std::string& value);
std::optional<DistancePolicy> parseDistancePolicy(const std::string& value);
std::optional<ExpansionPolicy> parseExpansionPolicy(const std::string& value);
std::optional<EchoPolicy> parseEchoPolicy(const std::string& value);
std::optional<StreamEchoProfile> parseStreamEchoProfile(const std::string& value);
std::optional<StreamTransformPolicy> parseStreamTransformPolicy(const std::string& value);
std::optional<JackPreservePolicy> parseJackPreservePolicy(const std::string& value);
std::string reportToJson(const ConversionReport& report);
std::string reportToText(const ConversionReport& report);

}  // namespace keyconv
