#pragma once

#include <optional>

#include <keyconv/quality_report.hpp>

namespace keyconv {

struct ConvertOptions {
    int sourceKeyCount = 0;
    int targetKeyCount = 0;
    ConversionStyle style = ConversionStyle::Playable;
    OptimizerKind optimizer = OptimizerKind::Greedy;
    CompressPolicy compressPolicy = CompressPolicy::Auto;
    DistancePolicy distancePolicy = DistancePolicy::AimodSafe;
    ExpansionPolicy expansionPolicy = ExpansionPolicy::PreserveNoteCount;
    EchoPolicy echoPolicy = EchoPolicy::Off;
    StreamEchoProfile streamEchoProfile = StreamEchoProfile::Conservative;
    StreamTransformPolicy streamTransformPolicy = StreamTransformPolicy::Off;
    JackPreservePolicy jackPreservePolicy = JackPreservePolicy::PreservePlayable;
    TenKeyPlannerPolicy tenKeyPlannerPolicy = TenKeyPlannerPolicy::Auto;
    bool gestureRailEnabled = true;
    bool preserveLaneDrift = false;
    bool echoDiagnostics = false;
    bool dpMode = false;
    bool tenKFullFieldRemix = false;
    double tenKFullFieldRemixDensityCeiling = 1.6;
    int tenKFullFieldRemixPhaseStep = 2;
    int beamWidth = 16;
    int sameTimeEpsilonMs = 2;
    int minObjectGapMs = 16;
    int sameLaneMinGapMs = 20;
    int jackWindowMs = 500;
    int strictJackWindowMs = 500;
    bool allowPlayableJackSplit = true;
    int maxJackSplitLanes = 2;
    bool snapRolledNotes = true;
    int snapToleranceMs = 2;
    int maxRollMs = 64;
    double maxAddedNoteRatio = 0.45;
    int maxAddedPerSlice = 2;
    int maxAddedPerMeasure = 16;
    bool deterministicExpansion = true;
    int expansionMinGapMs = 16;
    int expansionSameLaneMinGapMs = 20;
    bool snapAddedNotes = true;
    int expansionSnapToleranceMs = 2;
    double maxEchoAddedRatio = 0.08;
    int maxEchoPerPattern = 4;
    int maxEchoPerMeasure = 8;
    int maxEchoPerSlice = 1;
    int minEchoPatternLength = 3;
    double minPatternConfidence = 0.70;
    bool echoRequiresSnap = true;
    int echoMinGapMs = 16;
    int echoSameLaneMinGapMs = 20;
    bool echoAvoidHighDensity = true;
    int echoHighDensityWindowMs = 1000;
    double echoMaxLocalNps = 12.0;
    std::optional<TargetKProfile> targetKProfile;
    CollisionPolicy collisionPolicy = CollisionPolicy::ShiftNearest;
    unsigned int seed = 0;
};

}  // namespace keyconv
