#include "core/quality.hpp"

#include "core/collision.hpp"
#include "core/mapping.hpp"
#include "core/repeat.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>

namespace keyconv {

namespace {

std::vector<Note> sortedNotes(std::vector<Note> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.lane < b.lane;
    });
    return notes;
}

int signOf(int value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

double scoreAtLeast(double value, double desired) {
    if (desired <= 0.0) {
        return 1.0;
    }
    return clamp01(value / desired);
}

double scoreBand(double value, double low, double ideal, double high) {
    if (ideal <= low || high <= ideal) {
        return scoreAtLeast(value, ideal);
    }
    if (value <= low) {
        return 0.0;
    }
    if (value <= ideal) {
        return clamp01((value - low) / (ideal - low));
    }
    if (value <= high) {
        return clamp01((high - value) / (high - ideal));
    }
    return 0.0;
}

double jackRate(const std::vector<Note>& notes, int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    const auto sorted = sortedNotes(notes);
    if (window <= 0 || sorted.size() < 2) {
        return 0.0;
    }
    int jackCount = 0;
    int eligible = 0;
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const int dt = sorted[i].time - sorted[i - 1].time;
        if (dt > 0 && dt <= window) {
            ++eligible;
            if (sorted[i].lane == sorted[i - 1].lane) {
                ++jackCount;
            }
        }
    }
    if (eligible == 0) {
        return 0.0;
    }
    return static_cast<double>(jackCount) / static_cast<double>(eligible);
}

double laneEntropy(const std::vector<int>& distribution) {
    const int total = std::accumulate(distribution.begin(), distribution.end(), 0);
    if (total <= 0 || distribution.size() <= 1) {
        return 0.0;
    }

    double entropy = 0.0;
    for (const int count : distribution) {
        if (count <= 0) {
            continue;
        }
        const double p = static_cast<double>(count) / static_cast<double>(total);
        entropy -= p * std::log(p);
    }
    return entropy / std::log(static_cast<double>(distribution.size()));
}

double laneCoverage(const std::vector<int>& distribution) {
    if (distribution.empty()) {
        return 0.0;
    }
    const int used = static_cast<int>(std::count_if(distribution.begin(), distribution.end(), [](int count) {
        return count > 0;
    }));
    return static_cast<double>(used) / static_cast<double>(distribution.size());
}

double noteDensity(const std::vector<Note>& notes) {
    if (notes.empty()) {
        return 0.0;
    }
    int minTime = notes.front().time;
    int maxTime = notes.front().time;
    for (const auto& note : notes) {
        minTime = std::min(minTime, note.time);
        maxTime = std::max(maxTime, note.time);
    }
    const int spanMs = maxTime - minTime;
    if (spanMs <= 0) {
        return static_cast<double>(notes.size());
    }
    return static_cast<double>(notes.size()) * 1000.0 / static_cast<double>(spanMs);
}

double chordRate(const std::vector<Note>& notes) {
    if (notes.empty()) {
        return 0.0;
    }
    std::map<int, int> sliceCounts;
    for (const auto& note : notes) {
        ++sliceCounts[note.time];
    }
    int chordSlices = 0;
    for (const auto& entry : sliceCounts) {
        if (entry.second >= 2) {
            ++chordSlices;
        }
    }
    return static_cast<double>(chordSlices) / static_cast<double>(sliceCounts.size());
}

double handSpread(const std::vector<Note>& notes, int keyCount) {
    if (notes.empty() || keyCount <= 1) {
        return 0.0;
    }
    std::map<int, std::vector<int>> lanesByTime;
    for (const auto& note : notes) {
        lanesByTime[note.time].push_back(note.lane);
    }

    double total = 0.0;
    for (const auto& entry : lanesByTime) {
        const auto [minIt, maxIt] = std::minmax_element(entry.second.begin(), entry.second.end());
        total += static_cast<double>(*maxIt - *minIt) / static_cast<double>(keyCount - 1);
    }
    return lanesByTime.empty() ? 0.0 : total / static_cast<double>(lanesByTime.size());
}

std::pair<int, int> handCounts(const std::vector<int>& distribution) {
    int left = 0;
    int right = 0;
    const int boundary = std::max(1, static_cast<int>(distribution.size()) / 2);
    for (int lane = 0; lane < static_cast<int>(distribution.size()); ++lane) {
        if (lane < boundary) {
            left += distribution[static_cast<std::size_t>(lane)];
        } else {
            right += distribution[static_cast<std::size_t>(lane)];
        }
    }
    return {left, right};
}

double handBalanceRatio(int left, int right) {
    const int total = left + right;
    if (total <= 0) {
        return 1.0;
    }
    return 1.0 - static_cast<double>(std::abs(left - right)) / static_cast<double>(total);
}

double lnAnchorPressure(const std::vector<Note>& notes) {
    std::vector<Note> holds;
    std::vector<Note> taps;
    for (const auto& note : notes) {
        if (note.type == NoteType::Hold && note.endTime.has_value() && *note.endTime > note.time) {
            holds.push_back(note);
        } else if (note.type == NoteType::Tap) {
            taps.push_back(note);
        }
    }
    if (taps.empty() || holds.empty()) {
        return 0.0;
    }

    int pressuredTaps = 0;
    for (const auto& tap : taps) {
        const bool underAnchor = std::any_of(holds.begin(), holds.end(), [&](const Note& hold) {
            return hold.lane != tap.lane && tap.time > hold.time && tap.time < *hold.endTime;
        });
        if (underAnchor) {
            ++pressuredTaps;
        }
    }
    return static_cast<double>(pressuredTaps) / static_cast<double>(taps.size());
}

void addTag(std::vector<std::string>& tags, const std::string& tag) {
    if (std::find(tags.begin(), tags.end(), tag) == tags.end()) {
        tags.push_back(tag);
    }
}

std::vector<std::string> feelTags(const QualityReport& report,
                                  const Chart& original,
                                  const Chart& converted) {
    std::vector<std::string> tags;
    const double addedRatio = original.notes.empty()
                                  ? 0.0
                                  : std::max(0.0,
                                             static_cast<double>(converted.notes.size()) -
                                                 static_cast<double>(original.notes.size())) /
                                        static_cast<double>(original.notes.size());

    if (report.laneCoverageAfter > report.laneCoverageBefore + 0.05 ||
        report.laneEntropyAfter > report.laneEntropyBefore + 0.03) {
        addTag(tags, "more_spread");
    }
    if (report.chordRateAfter > report.chordRateBefore + 0.02) {
        addTag(tags, "more_chordy");
    }
    if (report.densityDelta > 0.50) {
        addTag(tags, "more_streamy");
    }
    if ((report.jackRateAfter + 0.01 < report.jackRateBefore ||
         report.lnAnchorPressureAfter + 0.01 < report.lnAnchorPressureBefore) &&
        report.collisionCount == 0 && report.lnConflictCount == 0) {
        addTag(tags, "safer_than_original");
    }
    if (addedRatio > 0.0 || report.chordRateAfter > report.chordRateBefore + 0.01 ||
        report.densityDelta > 0.10) {
        addTag(tags, "harder_than_original");
    }
    if (addedRatio >= 0.03 && addedRatio <= 0.09 && report.collisionCount == 0 &&
        report.lnConflictCount == 0) {
        addTag(tags, "training_friendly");
    }
    if (addedRatio > 0.12 || report.chordRateAfter > report.chordRateBefore + 0.15 ||
        report.lnAnchorPressureAfter > report.lnAnchorPressureBefore + 0.20) {
        addTag(tags, "possible_overfill");
    }

    return tags;
}

std::map<std::string, Note> byId(const std::vector<Note>& notes) {
    std::map<std::string, Note> result;
    for (const auto& note : notes) {
        result[note.id] = note;
    }
    return result;
}

double orderScore(const Chart& original, const Chart& converted) {
    const auto convertedById = byId(converted.notes);
    const auto sorted = sortedNotes(original.notes);
    double total = 0.0;
    double score = 0.0;

    for (std::size_t i = 1; i < sorted.size(); ++i) {
        const auto convertedPrev = convertedById.find(sorted[i - 1].id);
        const auto convertedNow = convertedById.find(sorted[i].id);
        if (convertedPrev == convertedById.end() || convertedNow == convertedById.end()) {
            continue;
        }

        const int sourceSign = signOf(sorted[i].lane - sorted[i - 1].lane);
        if (sourceSign == 0) {
            continue;
        }
        const int targetSign = signOf(convertedNow->second.lane - convertedPrev->second.lane);
        ++total;
        if (targetSign == sourceSign) {
            score += 1.0;
        } else if (targetSign == 0) {
            score += 0.5;
        }
    }

    return total <= 0.0 ? 1.0 : score / total;
}

double spanScore(const Chart& original, const Chart& converted, int sourceK, int targetK) {
    const auto convertedById = byId(converted.notes);
    std::map<int, std::vector<Note>> byTime;
    for (const auto& note : original.notes) {
        byTime[note.time].push_back(note);
    }

    double total = 0.0;
    double score = 0.0;
    for (const auto& entry : byTime) {
        if (entry.second.size() < 2) {
            continue;
        }

        int sourceMin = entry.second.front().lane;
        int sourceMax = entry.second.front().lane;
        int targetMin = targetK;
        int targetMax = 0;
        bool complete = true;

        for (const auto& note : entry.second) {
            sourceMin = std::min(sourceMin, note.lane);
            sourceMax = std::max(sourceMax, note.lane);
            const auto convertedNote = convertedById.find(note.id);
            if (convertedNote == convertedById.end()) {
                complete = false;
                break;
            }
            targetMin = std::min(targetMin, convertedNote->second.lane);
            targetMax = std::max(targetMax, convertedNote->second.lane);
        }

        if (!complete) {
            continue;
        }

        const double sourceSpan = static_cast<double>(sourceMax - sourceMin) / static_cast<double>(std::max(1, sourceK - 1));
        const double targetSpan = static_cast<double>(targetMax - targetMin) / static_cast<double>(std::max(1, targetK - 1));
        score += clamp01(1.0 - std::abs(sourceSpan - targetSpan));
        ++total;
    }

    return total <= 0.0 ? 1.0 : score / total;
}

int targetAnchorLaneForSource(int sourceLane, int sourceK, int targetK) {
    if (sourceK == 7 && targetK == 10) {
        static constexpr std::array<int, 7> anchors = {0, 1, 3, 4, 6, 8, 9};
        const int clamped = clampInt(sourceLane, 0, 6);
        return anchors[static_cast<std::size_t>(clamped)];
    }
    return mapLaneDirect(sourceLane, sourceK, targetK);
}

double edgeUsageRatio(const std::vector<int>& distribution) {
    if (distribution.empty()) {
        return 0.0;
    }
    const int total = std::accumulate(distribution.begin(), distribution.end(), 0);
    if (total <= 0) {
        return 0.0;
    }
    const int edgeWidth = std::min(2, static_cast<int>(distribution.size()) / 2);
    int edgeNotes = 0;
    for (int i = 0; i < edgeWidth; ++i) {
        edgeNotes += distribution[static_cast<std::size_t>(i)];
        edgeNotes += distribution[distribution.size() - 1 - static_cast<std::size_t>(i)];
    }
    return static_cast<double>(edgeNotes) / static_cast<double>(total);
}

double activeLaneWindowScore(const Chart& converted, int targetK, const TargetKProfile& profile) {
    if (converted.notes.empty() || targetK <= 0) {
        return 0.0;
    }

    int minTime = converted.notes.front().time;
    int maxTime = converted.notes.front().time;
    for (const auto& note : converted.notes) {
        minTime = std::min(minTime, note.time);
        maxTime = std::max(maxTime, note.time);
    }

    constexpr int windowMs = 2000;
    constexpr int stepMs = 1000;
    const double desiredActiveLanes =
        std::max(1.0, std::round(static_cast<double>(targetK) * profile.desiredActiveLaneRate));

    double score = 0.0;
    int windows = 0;
    for (int start = minTime; start <= maxTime; start += stepMs) {
        std::vector<bool> active(static_cast<std::size_t>(targetK), false);
        int noteCount = 0;
        for (const auto& note : converted.notes) {
            if (note.time < start || note.time >= start + windowMs) {
                continue;
            }
            if (note.lane >= 0 && note.lane < targetK) {
                active[static_cast<std::size_t>(note.lane)] = true;
                ++noteCount;
            }
        }
        if (noteCount <= 0) {
            continue;
        }
        const int activeCount =
            static_cast<int>(std::count(active.begin(), active.end(), true));
        score += clamp01(static_cast<double>(activeCount) / desiredActiveLanes);
        ++windows;
    }

    return windows <= 0 ? 0.0 : score / static_cast<double>(windows);
}

double averageChordSpan(const Chart& converted, int targetK) {
    if (converted.notes.empty() || targetK <= 1) {
        return 0.0;
    }

    std::map<int, std::vector<int>> lanesByTime;
    for (const auto& note : converted.notes) {
        lanesByTime[note.time].push_back(note.lane);
    }

    double totalSpan = 0.0;
    int chordSlices = 0;
    for (const auto& entry : lanesByTime) {
        if (entry.second.size() < 2) {
            continue;
        }
        const auto [minIt, maxIt] =
            std::minmax_element(entry.second.begin(), entry.second.end());
        totalSpan += static_cast<double>(*maxIt - *minIt) / static_cast<double>(targetK - 1);
        ++chordSlices;
    }
    return chordSlices <= 0 ? 0.0 : totalSpan / static_cast<double>(chordSlices);
}

double anchorPreserveScore(const Chart& original,
                           const Chart& converted,
                           int sourceK,
                           int targetK) {
    if (original.notes.empty() || targetK <= 1 || sourceK <= 0) {
        return 1.0;
    }

    const auto convertedById = byId(converted.notes);
    const double tolerance = std::max(1.0, static_cast<double>(targetK) * 0.30);
    double total = 0.0;
    double score = 0.0;
    for (const auto& sourceNote : original.notes) {
        const auto convertedNote = convertedById.find(sourceNote.id);
        if (convertedNote == convertedById.end()) {
            continue;
        }
        const int sourceLane = sourceNote.sourceLane.value_or(sourceNote.lane);
        const int anchor = targetAnchorLaneForSource(sourceLane, sourceK, targetK);
        const double distance = std::abs(static_cast<double>(convertedNote->second.lane - anchor));
        score += clamp01(1.0 - distance / tolerance);
        ++total;
    }

    return total <= 0.0 ? 1.0 : score / total;
}

double adjacentExpansionRatio(const Chart& converted, int sourceK, int targetK) {
    if (converted.notes.empty() || sourceK <= 0 || targetK <= sourceK) {
        return 0.0;
    }

    double total = 0.0;
    double adjacent = 0.0;
    for (const auto& note : converted.notes) {
        const int sourceLane = note.sourceLane.value_or(note.lane);
        if (sourceLane < 0 || sourceLane >= sourceK) {
            continue;
        }
        const int anchor = targetAnchorLaneForSource(sourceLane, sourceK, targetK);
        const int distance = std::abs(note.lane - anchor);
        if (distance == 1) {
            adjacent += 1.0;
        }
        total += 1.0;
    }

    return total <= 0.0 ? 0.0 : adjacent / total;
}

double addedRatioFitScore(double addedRatio, int sourceK, int targetK) {
    if (sourceK <= 0 || targetK <= sourceK) {
        return addedRatio <= 0.001 ? 1.0 : clamp01(1.0 - addedRatio / 0.20);
    }

    const double growthPotential =
        static_cast<double>(targetK) / static_cast<double>(sourceK) - 1.0;
    const double ideal = std::max(0.05, std::min(0.32, growthPotential * 0.55));
    const double low = std::max(0.0, ideal * 0.35);
    const double high = std::min(0.45, ideal * 1.65);
    return scoreBand(addedRatio, low, ideal, high);
}

double targetKSafetyScore(const QualityReport& report) {
    double safety = 1.0;
    safety -= std::min(0.75, static_cast<double>(report.collisionCount) * 0.25);
    safety -= std::min(0.75, static_cast<double>(report.lnConflictCount) * 0.25);
    safety -= std::min(0.80, static_cast<double>(report.createdJacks) * 0.20);
    safety -= std::min(0.90, static_cast<double>(report.unsolvedCreatedJacks) * 0.35);
    safety -= std::min(0.40, static_cast<double>(report.nearTimeConflicts) * 0.08);
    safety -= std::min(0.35, static_cast<double>(report.sameLaneNearConflicts) * 0.06);
    safety -= std::min(0.30, static_cast<double>(report.unsnappedAddedNotes) * 0.05);
    return clamp01(safety);
}

}  // namespace

TargetKProfile targetKProfileFor(int sourceKeyCount, int targetKeyCount) {
    TargetKProfile profile;
    profile.targetKeys = targetKeyCount;
    profile.profileName = "builtin_" + std::to_string(std::max(0, targetKeyCount)) + "k_profile";
    profile.profileKind = "builtin";
    profile.windowMs = 2000;
    if (targetKeyCount <= 0) {
        return profile;
    }

    const double growthPotential =
        sourceKeyCount > 0
            ? std::max(0.0,
                       static_cast<double>(targetKeyCount) / static_cast<double>(sourceKeyCount) - 1.0)
            : 0.0;
    profile.desiredLaneEntropy = targetKeyCount >= 10 ? 0.90 : 0.84;
    profile.desiredEdgeUsage = targetKeyCount >= 10 ? 0.34 : std::min(0.45, 2.0 / targetKeyCount);
    profile.desiredActiveLaneRate = targetKeyCount >= 10 ? 0.80 : 0.72;
    profile.desiredChordSpan = targetKeyCount >= 10 ? 0.45 : 0.38;
    profile.desiredHandBalance = targetKeyCount >= 9 ? 0.88 : 0.95;
    profile.desiredAdjacentExpansion =
        std::max(0.08, std::min(0.30, growthPotential * 0.45));
    return profile;
}

QualityReport computeQualityReport(const Chart& original,
                                   const Chart& converted,
                                   int sourceKeyCount,
                                   int targetKeyCount,
                                   int jackWindowMs) {
    QualityReport report;
    const auto scan = detectCollisions(converted.notes);
    report.collisionCount = scan.sameTimeCollisions;
    report.lnConflictCount = scan.longNoteConflicts;
    const auto originalDistribution = calculateLaneDistribution(original.notes, sourceKeyCount);
    report.jackRateBefore = jackRate(original.notes, jackWindowMs);
    report.jackRateAfter = jackRate(converted.notes, jackWindowMs);
    report.densityDelta = noteDensity(converted.notes) - noteDensity(original.notes);
    report.chordRateBefore = chordRate(original.notes);
    report.chordRateAfter = chordRate(converted.notes);
    report.laneDistribution = calculateLaneDistribution(converted.notes, targetKeyCount);
    report.laneEntropy = laneEntropy(report.laneDistribution);
    report.laneCoverageBefore = laneCoverage(originalDistribution);
    report.laneCoverageAfter = laneCoverage(report.laneDistribution);
    report.laneEntropyBefore = laneEntropy(originalDistribution);
    report.laneEntropyAfter = report.laneEntropy;
    report.lnAnchorPressureBefore = lnAnchorPressure(original.notes);
    report.lnAnchorPressureAfter = lnAnchorPressure(converted.notes);
    report.handSpreadAfter = handSpread(converted.notes, targetKeyCount);
    const auto [leftHandNotes, rightHandNotes] = handCounts(report.laneDistribution);
    report.leftHandNotes = leftHandNotes;
    report.rightHandNotes = rightHandNotes;
    report.handBalanceRatio = handBalanceRatio(leftHandNotes, rightHandNotes);
    report.orderPreserveScore = orderScore(original, converted);
    report.spanPreserveScore = spanScore(original, converted, sourceKeyCount, targetKeyCount);
    report.patternPreserveScore = (report.orderPreserveScore + report.spanPreserveScore) / 2.0;
    const auto jackValidation = validateJackPreservation(original, converted, jackWindowMs, 2);
    report.sourceJackGroups = jackValidation.sourceJackGroups;
    report.preservedJackGroups = jackValidation.preservedJackGroups;
    report.splitJackGroups = jackValidation.splitJackGroups;
    report.createdJacks = jackValidation.createdJacks;
    report.smoothedJacks = jackValidation.smoothedJacks;
    report.jackPreserveScore = jackValidation.jackPreserveScore;
    report.createdJackRate = jackValidation.createdJackRate;
    report.feelTags = feelTags(report, original, converted);

    double playability = 100.0;
    playability -= static_cast<double>(report.collisionCount) * 25.0;
    playability -= static_cast<double>(report.lnConflictCount) * 25.0;
    playability -= report.jackRateAfter * 20.0;
    playability += report.laneEntropy * 10.0;
    playability += report.orderPreserveScore * 5.0;
    report.playabilityScore = std::max(0.0, std::min(100.0, playability));
    return report;
}

void finalizeTargetKLikenessReport(QualityReport& report,
                                   const Chart& original,
                                   const Chart& converted,
                                   int sourceKeyCount,
                                   int targetKeyCount,
                                   const TargetKProfile* profileOverride) {
    if (targetKeyCount <= 1 || converted.notes.empty()) {
        report.kLikenessScore = 0.0;
        return;
    }

    const auto fallbackProfile = targetKProfileFor(sourceKeyCount, targetKeyCount);
    const auto& profile = profileOverride != nullptr ? *profileOverride : fallbackProfile;
    report.targetProfileChartCount = profile.sampleCount;
    report.targetProfileWindowMs = profile.windowMs;
    report.targetProfileName = profile.profileName;
    report.targetProfileKind = profile.profileKind;
    report.targetProfileSource = profile.sourceName;
    report.targetProfileAuthor = profile.authorToken;
    report.laneCoverageScore =
        scoreAtLeast(report.laneCoverageAfter, std::min(1.0, profile.desiredActiveLaneRate + 0.10));
    report.laneEntropyScore = scoreAtLeast(report.laneEntropyAfter, profile.desiredLaneEntropy);
    report.edgeUsageScore = scoreBand(edgeUsageRatio(report.laneDistribution),
                                      profile.desiredEdgeUsage * 0.35,
                                      profile.desiredEdgeUsage,
                                      std::min(0.75, profile.desiredEdgeUsage * 1.75));
    report.activeLaneWindowScore = activeLaneWindowScore(converted, targetKeyCount, profile);

    const double chordSpan = averageChordSpan(converted, targetKeyCount);
    const double spatialSpan = chordSpan > 0.0 ? chordSpan : report.handSpreadAfter;
    report.spatialSpanScore = scoreAtLeast(spatialSpan, profile.desiredChordSpan);

    const double adjacentRatio = adjacentExpansionRatio(converted, sourceKeyCount, targetKeyCount);
    report.adjacentExpansionScore = scoreBand(adjacentRatio,
                                             profile.desiredAdjacentExpansion * 0.25,
                                             profile.desiredAdjacentExpansion,
                                             std::min(0.60, profile.desiredAdjacentExpansion * 2.20));
    report.anchorPreserveScore =
        anchorPreserveScore(original, converted, sourceKeyCount, targetKeyCount);
    report.patternVocabularyScore =
        clamp01(report.patternPreserveScore * 0.40 + report.gesturePreservationScore * 0.40 +
                report.jackPreserveScore * 0.20);
    report.addedRatioFitScore =
        addedRatioFitScore(report.addedNoteRatio, sourceKeyCount, targetKeyCount);
    report.targetKSafetyScore = targetKSafetyScore(report);

    const double handBalanceScore = scoreAtLeast(report.handBalanceRatio, profile.desiredHandBalance);
    const double sourceIntegrity =
        clamp01(report.anchorPreserveScore * 0.55 + report.patternVocabularyScore * 0.45);
    const double targetKSpace =
        clamp01(report.laneCoverageScore * 0.22 + report.laneEntropyScore * 0.22 +
                report.edgeUsageScore * 0.18 + report.activeLaneWindowScore * 0.24 +
                report.spatialSpanScore * 0.14);
    const double raw =
        clamp01(sourceIntegrity * 0.32 + targetKSpace * 0.30 +
                report.adjacentExpansionScore * 0.16 + handBalanceScore * 0.12 +
                report.addedRatioFitScore * 0.10);
    const double adjacentGrowthGate = 0.50 + report.adjacentExpansionScore * 0.50;
    report.kLikenessScore =
        std::round(raw * adjacentGrowthGate * report.targetKSafetyScore * 1000.0) / 10.0;
}

}  // namespace keyconv
