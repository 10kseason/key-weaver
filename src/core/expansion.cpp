#include "core/expansion.hpp"

#include "core/collision.hpp"
#include "core/distance.hpp"
#include "core/gesture.hpp"
#include "core/mapping.hpp"
#include "core/pattern.hpp"
#include "core/repeat.hpp"
#include "core/slice.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace keyconv {

int rotateWithinZone(int mirrorLane, int sliceIndex, int zoneStart, int zoneWidth, int phaseStep) {
    if (zoneWidth <= 0) {
        return zoneStart;
    }
    const auto positiveModulo = [](int value, int modulo) {
        const int result = value % modulo;
        return result < 0 ? result + modulo : result;
    };
    const int base = mirrorLane - zoneStart;
    const int offset = positiveModulo(sliceIndex * phaseStep, zoneWidth);
    const int index = positiveModulo(base + offset, zoneWidth);
    return zoneStart + index;
}

namespace {

constexpr int kLnHeavyWindowMs = 2000;
constexpr int kLnHeavyMinStarts = 3;
constexpr int kLnHeavyMinHoldStarts = 2;
constexpr int kLnAddMaxAnchorLaneDistance = 2;
constexpr int kFallbackBeatLengthMs = 500;
constexpr double kLnHeavyStartRatio = 0.40;
constexpr double kLnHeavyActiveRatio = 0.40;

struct SliceView {
    int index = 0;
    int time = 0;
    std::vector<std::size_t> noteIndices;
};

struct ExpansionCandidate {
    Note note;
    std::string ruleName;
    std::string echoKind;
    std::vector<std::string> sourceNoteIds;
    int sourcePatternStart = 0;
    int sourceSliceIndex = 0;
    int sourceNoteIndex = 0;
    int ordinal = 0;
    double score = 0.0;
    int rhythmDriftAbs = 0;
    int snapPriority = 0;
    int laneMovement = 0;
};

struct StreamEchoProfileSettings {
    double maxAddedRatio = 0.04;
    double maxLocalNps = 10.0;
    double shortWindowNpsLimit = 14.0;
    double midWindowNpsLimit = 10.0;
    double longWindowNpsLimit = 8.0;
};

struct ExpansionComposerSettings {
    std::string profile = "preserve";
    double targetAddedRatio = 0.0;
};

struct LnAnchor {
    int lane = 0;
    int durationMs = 0;
};

struct LnWindowProfile {
    bool lnHeavy = false;
    std::vector<LnAnchor> anchors;
};

struct AdaptiveGrowthWindow {
    int bucket = 0;
    int startMs = 0;
    int endMs = 0;
    int originalNotes = 0;
    int activeLanes = 0;
    int holdNotes = 0;
    int jackRiskPairs = 0;
    int chordSlices = 0;
    int totalSlices = 0;
    double localRatio = 0.0;
    int maxAdds = 0;
    int added = 0;
};

enum class AdaptiveBucketKind {
    None,
    All,
    Low,
    Mid,
    High,
    ChordHeavy,
    JackRisk,
};

struct AdaptiveBucketChoice {
    const TargetKBucketProfile* bucket = nullptr;
    AdaptiveBucketKind kind = AdaptiveBucketKind::None;
};

struct ExpansionTimingGrid {
    int time = 0;
    double beatLength = 0.0;
};

int handBoundary(int targetKeyCount) {
    return std::max(1, targetKeyCount / 2);
}

int handForLane(int lane, int targetKeyCount) {
    return lane < handBoundary(targetKeyCount) ? 0 : 1;
}

std::pair<int, int> handUseCounts(const std::vector<int>& laneUse, int targetKeyCount) {
    int left = 0;
    int right = 0;
    const int boundary = handBoundary(targetKeyCount);
    for (int lane = 0; lane < targetKeyCount && lane < static_cast<int>(laneUse.size()); ++lane) {
        if (lane < boundary) {
            left += laneUse[static_cast<std::size_t>(lane)];
        } else {
            right += laneUse[static_cast<std::size_t>(lane)];
        }
    }
    return {left, right};
}

double handBalanceScore(const std::vector<int>& laneUse, int lane, int targetKeyCount) {
    if (targetKeyCount <= 1 || lane < 0 || lane >= targetKeyCount ||
        laneUse.size() != static_cast<std::size_t>(targetKeyCount)) {
        return 0.0;
    }

    const auto [leftUse, rightUse] = handUseCounts(laneUse, targetKeyCount);
    const int laneHand = handForLane(lane, targetKeyCount);
    const int currentHandUse = laneHand == 0 ? leftUse : rightUse;
    const int otherHandUse = laneHand == 0 ? rightUse : leftUse;
    const double normalizer = std::max(1.0, (static_cast<double>(leftUse + rightUse) / 2.0));
    return std::max(-1.0, std::min(1.0, static_cast<double>(otherHandUse - currentHandUse) / normalizer));
}

double preserveTapPlusTargetRatio(int sourceKeyCount, int targetKeyCount) {
    if (targetKeyCount <= sourceKeyCount || sourceKeyCount <= 0) {
        return 0.0;
    }
    return 0.15;
}

double preserveTapPlusMoreTargetRatio(int sourceKeyCount, int targetKeyCount) {
    if (targetKeyCount <= sourceKeyCount || sourceKeyCount <= 0) {
        return 0.0;
    }
    return 0.20;
}

double preserveTapPlusLowTargetRatio(int sourceKeyCount, int targetKeyCount) {
    if (targetKeyCount <= sourceKeyCount || sourceKeyCount <= 0) {
        return 0.0;
    }
    return 0.10;
}

StreamEchoProfileSettings streamEchoProfileSettings(StreamEchoProfile profile) {
    switch (profile) {
        case StreamEchoProfile::Conservative:
            return {0.04, 10.0, 14.0, 10.0, 8.0};
        case StreamEchoProfile::Balanced:
            return {0.06, 12.0, 16.0, 12.0, 9.0};
        case StreamEchoProfile::Training:
            return {0.08, 13.0, 17.0, 13.0, 10.0};
        case StreamEchoProfile::Experimental:
            return {0.10, 15.0, 20.0, 15.0, 11.0};
    }
    return {0.04, 10.0, 14.0, 10.0, 8.0};
}

ExpansionPolicy effectiveExpansionPolicy(const ConvertOptions& options) {
    if (options.targetKeyCount <= options.sourceKeyCount) {
        return ExpansionPolicy::PreserveNoteCount;
    }
    return options.expansionPolicy;
}

bool tenKFullFieldRemixActive(const ConvertOptions& options) {
    return options.targetKeyCount == 10 && options.tenKFullFieldRemix;
}

double tenKFullFieldRemixTargetRatio(const ConvertOptions& options) {
    return std::max(0.0, options.tenKFullFieldRemixDensityCeiling - 1.0);
}

ExpansionComposerSettings composerSettingsForPolicy(ExpansionPolicy policy,
                                                    StreamEchoProfile streamProfile,
                                                    int sourceKeyCount,
                                                    int targetKeyCount) {
    if (policy == ExpansionPolicy::PreserveNoteCount || policy == ExpansionPolicy::SeededRandomRemix) {
        return {"preserve", 0.0};
    }
    if (policy == ExpansionPolicy::PreserveTapPlusMore) {
        return {"tap-plus-more", preserveTapPlusMoreTargetRatio(sourceKeyCount, targetKeyCount)};
    }
    if (policy == ExpansionPolicy::PreserveTapPlus) {
        return {"tap-plus", preserveTapPlusTargetRatio(sourceKeyCount, targetKeyCount)};
    }
    if (policy == ExpansionPolicy::PreserveTapPlusLow) {
        return {"tap-plus-low", preserveTapPlusLowTargetRatio(sourceKeyCount, targetKeyCount)};
    }
    if (policy == ExpansionPolicy::HarderRemix) {
        return {"harder", 0.12};
    }
    if (policy == ExpansionPolicy::TrainingScaffold) {
        return {"training", 0.08};
    }
    if (policy == ExpansionPolicy::DeterministicChordFill) {
        return {"balanced", 0.04};
    }
    if (policy == ExpansionPolicy::DeterministicEcho) {
        switch (streamProfile) {
            case StreamEchoProfile::Conservative:
                return {"conservative", 0.01};
            case StreamEchoProfile::Balanced:
                return {"balanced", 0.04};
            case StreamEchoProfile::Training:
                return {"training", 0.08};
            case StreamEchoProfile::Experimental:
                return {"experimental", 0.10};
        }
    }
    return {"preserve", 0.0};
}

ExpansionComposerSettings composerSettingsForOptions(const ConvertOptions& options) {
    if (tenKFullFieldRemixActive(options)) {
        return {"full-field-remix", tenKFullFieldRemixTargetRatio(options)};
    }
    return composerSettingsForPolicy(effectiveExpansionPolicy(options),
                                     options.streamEchoProfile,
                                     options.sourceKeyCount,
                                     options.targetKeyCount);
}

double effectiveTargetAddedNoteRatio(const ConvertOptions& options) {
    if (tenKFullFieldRemixActive(options)) {
        return tenKFullFieldRemixTargetRatio(options);
    }
    const auto composer = composerSettingsForOptions(options);
    if (composer.targetAddedRatio <= 0.0) {
        return 0.0;
    }
    constexpr double defaultMaxAddedNoteRatio = 0.45;
    constexpr double epsilon = 1e-9;
    if (std::abs(options.maxAddedNoteRatio - defaultMaxAddedNoteRatio) > epsilon) {
        return options.maxAddedNoteRatio;
    }
    return std::min(options.maxAddedNoteRatio, composer.targetAddedRatio);
}

std::vector<Note> sortedNotes(std::vector<Note> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        if (a.lane != b.lane) {
            return a.lane < b.lane;
        }
        return a.id < b.id;
    });
    return notes;
}

std::vector<SliceView> buildSliceViews(const std::vector<Note>& notes, int epsilonMs) {
    std::vector<std::size_t> order(notes.size());
    std::iota(order.begin(), order.end(), static_cast<std::size_t>(0));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (notes[a].time != notes[b].time) {
            return notes[a].time < notes[b].time;
        }
        if (notes[a].lane != notes[b].lane) {
            return notes[a].lane < notes[b].lane;
        }
        return notes[a].id < notes[b].id;
    });

    std::vector<SliceView> slices;
    std::size_t cursor = 0;
    while (cursor < order.size()) {
        SliceView slice;
        slice.index = static_cast<int>(slices.size());
        slice.time = notes[order[cursor]].time;
        while (cursor < order.size() && notes[order[cursor]].time - slice.time <= epsilonMs) {
            slice.noteIndices.push_back(order[cursor]);
            ++cursor;
        }
        slices.push_back(std::move(slice));
    }
    return slices;
}

std::vector<int> orderedLaneCandidates(int baseLane, int keyCount) {
    std::vector<int> lanes;
    if (keyCount <= 0) {
        return lanes;
    }
    auto addUnique = [&](int lane) {
        const int clamped = clampInt(lane, 0, keyCount - 1);
        if (std::find(lanes.begin(), lanes.end(), clamped) == lanes.end()) {
            lanes.push_back(clamped);
        }
    };

    addUnique(baseLane);
    for (int distance = 1; distance < keyCount; ++distance) {
        addUnique(baseLane - distance);
        addUnique(baseLane + distance);
    }
    return lanes;
}

std::vector<std::string> sourceIdsForSlice(const SliceView& slice, const std::vector<Note>& notes) {
    std::vector<std::string> ids;
    for (const auto noteIndex : slice.noteIndices) {
        ids.push_back(notes[noteIndex].id.empty() ? "anon" + std::to_string(noteIndex) : notes[noteIndex].id);
    }
    std::stable_sort(ids.begin(), ids.end());
    return ids;
}

bool evenKeyGeneratedPolicyActive(const ConvertOptions& options);
double beatLengthAtOrFallback(int time, const std::vector<TimingPoint>& timingPoints);

std::optional<int> singleSourceLaneForSlice(const SliceView& slice, const std::vector<Note>& notes) {
    if (slice.noteIndices.size() != 1 || slice.noteIndices.front() >= notes.size()) {
        return std::nullopt;
    }
    const auto& note = notes[slice.noteIndices.front()];
    return note.sourceLane.value_or(note.lane);
}

std::set<int> fastStairSuppressedTimes(const std::vector<Note>& notes,
                                       const ConvertOptions& options,
                                       const std::vector<TimingPoint>& timingPoints) {
    std::set<int> suppressed;
    if (!evenKeyGeneratedPolicyActive(options) || notes.size() < 3) {
        return suppressed;
    }

    const auto slices = buildSliceViews(notes, options.sameTimeEpsilonMs);
    for (std::size_t index = 0; index + 2 < slices.size(); ++index) {
        const auto laneA = singleSourceLaneForSlice(slices[index], notes);
        const auto laneB = singleSourceLaneForSlice(slices[index + 1], notes);
        const auto laneC = singleSourceLaneForSlice(slices[index + 2], notes);
        if (!laneA.has_value() || !laneB.has_value() || !laneC.has_value()) {
            continue;
        }
        const int firstDelta = *laneB - *laneA;
        const int secondDelta = *laneC - *laneB;
        if (firstDelta == 0 || secondDelta == 0 ||
            (firstDelta > 0) != (secondDelta > 0)) {
            continue;
        }
        const int timeA = slices[index].time;
        const int timeB = slices[index + 1].time;
        const int timeC = slices[index + 2].time;
        const int gapAB = timeB - timeA;
        const int gapBC = timeC - timeB;
        if (gapAB <= 0 || gapBC <= 0) {
            continue;
        }
        const int maxGapAB = static_cast<int>(std::ceil(beatLengthAtOrFallback(timeB, timingPoints) / 8.0)) +
                             std::max(2, options.expansionSnapToleranceMs);
        const int maxGapBC = static_cast<int>(std::ceil(beatLengthAtOrFallback(timeC, timingPoints) / 8.0)) +
                             std::max(2, options.expansionSnapToleranceMs);
        if (gapAB <= maxGapAB && gapBC <= maxGapBC) {
            std::size_t runStart = index;
            while (runStart > 0 &&
                   singleSourceLaneForSlice(slices[runStart - 1], notes).has_value() &&
                   singleSourceLaneForSlice(slices[runStart], notes).has_value()) {
                const int leftTime = slices[runStart - 1].time;
                const int rightTime = slices[runStart].time;
                const int maxGap =
                    static_cast<int>(std::ceil(beatLengthAtOrFallback(rightTime, timingPoints) / 8.0)) +
                    std::max(2, options.expansionSnapToleranceMs);
                if (rightTime - leftTime <= 0 || rightTime - leftTime > maxGap) {
                    break;
                }
                --runStart;
            }

            std::size_t runEnd = index + 2;
            while (runEnd + 1 < slices.size() &&
                   singleSourceLaneForSlice(slices[runEnd], notes).has_value() &&
                   singleSourceLaneForSlice(slices[runEnd + 1], notes).has_value()) {
                const int leftTime = slices[runEnd].time;
                const int rightTime = slices[runEnd + 1].time;
                const int maxGap =
                    static_cast<int>(std::ceil(beatLengthAtOrFallback(rightTime, timingPoints) / 8.0)) +
                    std::max(2, options.expansionSnapToleranceMs);
                if (rightTime - leftTime <= 0 || rightTime - leftTime > maxGap) {
                    break;
                }
                ++runEnd;
            }

            for (std::size_t runIndex = runStart; runIndex <= runEnd; ++runIndex) {
                suppressed.insert(slices[runIndex].time);
            }
        }
    }
    return suppressed;
}

int sourceHandForIndices(const std::vector<std::size_t>& noteIndices,
                         const std::vector<Note>& notes,
                         int sourceKeyCount) {
    if (sourceKeyCount <= 1) {
        return -1;
    }
    bool left = false;
    bool right = false;
    const int boundary = std::max(1, sourceKeyCount / 2);
    for (const auto noteIndex : noteIndices) {
        if (noteIndex >= notes.size()) {
            continue;
        }
        const int sourceLane = notes[noteIndex].sourceLane.value_or(notes[noteIndex].lane);
        if (sourceLane < boundary) {
            left = true;
        } else {
            right = true;
        }
    }
    if (left == right) {
        return -1;
    }
    return left ? 0 : 1;
}

bool candidateRespectsEvenSourceHand(const std::vector<std::size_t>& noteIndices,
                                     const std::vector<Note>& notes,
                                     int targetLane,
                                     const ConvertOptions& options) {
    if (!evenKeyGeneratedPolicyActive(options)) {
        return true;
    }
    const int sourceHand = sourceHandForIndices(noteIndices, notes, options.sourceKeyCount);
    return sourceHand < 0 || handForLane(targetLane, options.targetKeyCount) == sourceHand;
}

int targetChordSizeForSlice(const SliceView& slice, const ConvertOptions& options) {
    if (options.sourceKeyCount <= 0 || options.targetKeyCount <= options.sourceKeyCount) {
        return static_cast<int>(slice.noteIndices.size()) + options.maxAddedPerSlice;
    }
    const double ratio = static_cast<double>(options.targetKeyCount) /
                         static_cast<double>(options.sourceKeyCount);
    return clampInt(static_cast<int>(std::ceil(static_cast<double>(slice.noteIndices.size()) * ratio)),
                    static_cast<int>(slice.noteIndices.size()),
                    options.targetKeyCount);
}

std::string joinedIds(const std::vector<std::string>& ids) {
    std::ostringstream out;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i > 0) {
            out << "_";
        }
        out << ids[i];
    }
    return out.str();
}

std::string generatedId(const ExpansionCandidate& candidate) {
    if (candidate.ruleName == "echo") {
        return "gen:echo:" + candidate.echoKind + ":" + std::to_string(candidate.sourcePatternStart) + ":" +
               joinedIds(candidate.sourceNoteIds) + ":" + std::to_string(candidate.ordinal);
    }
    return "gen:" + candidate.ruleName + ":" + std::to_string(candidate.sourceSliceIndex) + ":" +
           joinedIds(candidate.sourceNoteIds) + ":" + std::to_string(candidate.ordinal);
}

int maxAddedTotal(const Chart& original, const ConvertOptions& options) {
    const double targetRatio = effectiveTargetAddedNoteRatio(options);
    if (targetRatio <= 0.0 || original.notes.empty()) {
        return 0;
    }
    if (tenKFullFieldRemixActive(options)) {
        return static_cast<int>(std::floor(static_cast<double>(original.notes.size()) * targetRatio));
    }
    if (effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlusMore ||
        effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlus ||
        effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlusLow) {
        const int budget = static_cast<int>(std::floor(static_cast<double>(original.notes.size()) * targetRatio));
        if (options.sourceKeyCount == 4 && options.targetKeyCount == 6 && original.notes.size() >= 2) {
            return std::max(1, budget);
        }
        return budget;
    }
    return std::max(1, static_cast<int>(std::floor(static_cast<double>(original.notes.size()) *
                                                   targetRatio)));
}

int adaptiveBudgetWindowMs(const ConvertOptions& options) {
    int windowMs = options.targetKProfile.has_value() ? options.targetKProfile->windowMs : 1000;
    if (windowMs <= 0) {
        windowMs = 1000;
    }
    return std::max(500, std::min(4000, windowMs));
}

bool adaptiveBudgetEnabledFor(const ConvertOptions& options) {
    if (tenKFullFieldRemixActive(options)) {
        return options.targetKeyCount > options.sourceKeyCount;
    }
    return options.targetKProfile.has_value() &&
           (effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlusMore ||
            effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlus ||
            effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlusLow) &&
           options.targetKeyCount > options.sourceKeyCount;
}

bool highKeyGeneratedTuningActive(const ConvertOptions& options) {
    return options.targetKeyCount >= 8 && options.targetKeyCount > options.sourceKeyCount;
}

bool tenKeyQuarterEighthDensityActive(const ConvertOptions& options) {
    return options.targetKeyCount == 10 && options.targetKeyCount > options.sourceKeyCount;
}

bool evenKeyGeneratedPolicyActive(const ConvertOptions& options) {
    return options.targetKeyCount > options.sourceKeyCount &&
           options.sourceKeyCount >= 2 && options.targetKeyCount >= 2 &&
           options.sourceKeyCount % 2 == 0 && options.targetKeyCount % 2 == 0;
}

std::vector<ExpansionTimingGrid> explicitTimingGrids(std::vector<TimingPoint> timingPoints) {
    std::stable_sort(timingPoints.begin(), timingPoints.end(), [](const TimingPoint& lhs, const TimingPoint& rhs) {
        return lhs.time < rhs.time;
    });

    std::vector<ExpansionTimingGrid> grids;
    for (const auto& point : timingPoints) {
        if (point.beatLength <= 0.0) {
            continue;
        }
        if (point.uninherited.has_value() && !*point.uninherited) {
            continue;
        }
        grids.push_back({point.time, point.beatLength});
    }
    return grids;
}

std::optional<ExpansionTimingGrid> explicitGridAt(int time, const std::vector<TimingPoint>& timingPoints) {
    const auto grids = explicitTimingGrids(timingPoints);
    if (grids.empty()) {
        return std::nullopt;
    }
    ExpansionTimingGrid selected = grids.front();
    for (const auto& grid : grids) {
        if (grid.time <= time) {
            selected = grid;
        } else {
            break;
        }
    }
    return selected;
}

double beatLengthAtOrFallback(int time, const std::vector<TimingPoint>& timingPoints) {
    const auto grid = explicitGridAt(time, timingPoints);
    if (!grid.has_value()) {
        return static_cast<double>(kFallbackBeatLengthMs);
    }
    return grid->beatLength > 0.0 ? grid->beatLength : static_cast<double>(kFallbackBeatLengthMs);
}

std::optional<int> beatDivisionDeltaMs(int time, const std::vector<TimingPoint>& timingPoints, int divisor) {
    const auto grid = explicitGridAt(time, timingPoints);
    if (!grid.has_value() || divisor <= 0) {
        return std::nullopt;
    }
    const double step = grid->beatLength / static_cast<double>(divisor);
    if (step <= 0.0) {
        return std::nullopt;
    }
    const auto nearestIndex = static_cast<int>(std::lround((static_cast<double>(time) - grid->time) / step));
    const int snapped = static_cast<int>(std::lround(static_cast<double>(grid->time) +
                                                     static_cast<double>(nearestIndex) * step));
    return std::abs(snapped - time);
}

std::optional<int> quarterBeatDeltaMs(int time, const std::vector<TimingPoint>& timingPoints) {
    return beatDivisionDeltaMs(time, timingPoints, 1);
}

std::optional<int> eighthBeatDeltaMs(int time, const std::vector<TimingPoint>& timingPoints) {
    return beatDivisionDeltaMs(time, timingPoints, 2);
}

std::optional<int> sixteenthBeatDeltaMs(int time, const std::vector<TimingPoint>& timingPoints) {
    return beatDivisionDeltaMs(time, timingPoints, 4);
}

int generatedHoldMinDurationMs(int time, const std::vector<TimingPoint>& timingPoints) {
    const double beatLength = beatLengthAtOrFallback(time, timingPoints);
    return std::max(1, static_cast<int>(std::lround(beatLength / 4.0)));
}

int generatedHoldMaxDurationMs(int time, const std::vector<TimingPoint>& timingPoints) {
    const double beatLength = beatLengthAtOrFallback(time, timingPoints);
    return std::max(1, static_cast<int>(std::lround(beatLength / 2.0)));
}

bool generatedHoldDurationAllowed(int durationMs,
                                  int time,
                                  const std::vector<TimingPoint>& timingPoints,
                                  int toleranceMs = 2) {
    const int tolerance = std::max(0, toleranceMs);
    return durationMs > 0 &&
           durationMs >= generatedHoldMinDurationMs(time, timingPoints) - tolerance &&
           durationMs <= generatedHoldMaxDurationMs(time, timingPoints) + tolerance;
}

double adaptiveDensityRoom(double densityNps) {
    if (densityNps <= 8.0) {
        return 1.15;
    }
    if (densityNps <= 14.0) {
        return 0.95;
    }
    if (densityNps <= 20.0) {
        return 0.70;
    }
    return 0.25;
}

double adaptivePatternSafety(double holdRate, double chordRate) {
    double safety = 1.0;
    if (holdRate >= 0.35) {
        safety *= 0.60;
    } else if (holdRate >= 0.20) {
        safety *= 0.80;
    }
    if (chordRate >= 0.65) {
        safety *= 0.75;
    } else if (chordRate >= 0.35) {
        safety *= 0.90;
    }
    return safety;
}

double statMedianOr(const TargetKFeatureStat& stat, double fallback) {
    return stat.present ? stat.median : fallback;
}

double statP25Or(const TargetKFeatureStat& stat, double fallback) {
    return stat.present ? stat.p25 : fallback;
}

int activeLanesForRate(double rate, int targetKeyCount) {
    return std::max(1,
                    std::min(targetKeyCount,
                             static_cast<int>(std::lround(static_cast<double>(targetKeyCount) *
                                                          std::clamp(rate, 0.05, 1.0)))));
}

AdaptiveBucketChoice chooseAdaptiveBucket(const AdaptiveGrowthWindow& window,
                                          const ConvertOptions& options,
                                          double densityNps,
                                          double chordRate,
                                          double jackRiskRate) {
    if (!options.targetKProfile.has_value() ||
        !options.targetKProfile->densityBuckets.present) {
        return {};
    }

    const auto& buckets = options.targetKProfile->densityBuckets;
    const double jackThreshold =
        buckets.jackRisk.present
            ? std::max(0.05, statP25Or(buckets.jackRisk.jackRisk, 0.08) * 0.75)
            : 0.05;
    if (buckets.jackRisk.present && jackRiskRate >= jackThreshold) {
        return {&buckets.jackRisk, AdaptiveBucketKind::JackRisk};
    }

    const double chordHeavyThreshold =
        buckets.chordHeavy.present
            ? std::clamp(statP25Or(buckets.chordHeavy.chordRate, 0.85), 0.80, 0.95)
            : 0.85;
    if (buckets.chordHeavy.present && chordRate >= chordHeavyThreshold) {
        return {&buckets.chordHeavy, AdaptiveBucketKind::ChordHeavy};
    }

    if (densityNps <= buckets.lowMaxNps && buckets.low.present) {
        return {&buckets.low, AdaptiveBucketKind::Low};
    }
    if (densityNps <= buckets.midMaxNps && buckets.mid.present) {
        return {&buckets.mid, AdaptiveBucketKind::Mid};
    }
    if (buckets.high.present) {
        return {&buckets.high, AdaptiveBucketKind::High};
    }
    if (buckets.all.present) {
        return {&buckets.all, AdaptiveBucketKind::All};
    }
    (void)window;
    return {};
}

double adaptiveBucketBasePressure(AdaptiveBucketKind kind) {
    switch (kind) {
        case AdaptiveBucketKind::Low:
            return 0.50;
        case AdaptiveBucketKind::Mid:
            return 0.86;
        case AdaptiveBucketKind::High:
            return 0.96;
        case AdaptiveBucketKind::ChordHeavy:
            return 1.02;
        case AdaptiveBucketKind::JackRisk:
            return 0.62;
        case AdaptiveBucketKind::All:
            return 0.78;
        case AdaptiveBucketKind::None:
            return 0.75;
    }
    return 0.75;
}

double profiledDensityRoom(const TargetKBucketProfile& bucket, double densityNps) {
    if (!bucket.densityNps.present) {
        return adaptiveDensityRoom(densityNps);
    }

    const double median = std::max(0.1, bucket.densityNps.median);
    const double p75 = std::max(median, bucket.densityNps.p75);
    const double p90 = std::max(p75, bucket.densityNps.p90);
    if (densityNps <= median) {
        return 1.08;
    }
    if (densityNps <= p75 || p75 <= median) {
        return 1.00;
    }
    if (densityNps <= p90 || p90 <= p75) {
        return 0.88;
    }
    return 0.72;
}

double profiledPatternSafety(double holdRate,
                             double chordRate,
                             AdaptiveBucketKind kind) {
    double safety = adaptivePatternSafety(holdRate, chordRate);
    if (kind == AdaptiveBucketKind::ChordHeavy) {
        safety = std::max(safety, 0.88);
    } else if (kind == AdaptiveBucketKind::High || kind == AdaptiveBucketKind::Mid) {
        safety = std::max(safety, 0.78);
    } else if (kind == AdaptiveBucketKind::JackRisk) {
        safety *= 0.70;
    }
    return safety;
}

double profiledTargetKNeed(const AdaptiveGrowthWindow& window,
                           const ConvertOptions& options,
                           const TargetKBucketProfile& bucket,
                           AdaptiveBucketKind kind,
                           const TargetKProfile& profile) {
    const double fallbackActiveRate = std::clamp(profile.desiredActiveLaneRate, 0.10, 1.0);
    const double floorRate = std::clamp(statP25Or(bucket.activeLaneRate, fallbackActiveRate * 0.75),
                                        0.05,
                                        1.0);
    const double targetRate = std::clamp(statMedianOr(bucket.activeLaneRate, fallbackActiveRate),
                                         floorRate,
                                         1.0);
    const int floorActiveLanes = activeLanesForRate(floorRate, options.targetKeyCount);
    const int targetActiveLanes = activeLanesForRate(targetRate, options.targetKeyCount);
    const double floorGap =
        std::max(0.0, static_cast<double>(floorActiveLanes - window.activeLanes)) /
        static_cast<double>(std::max(1, floorActiveLanes));
    const double targetGap =
        std::max(0.0, static_cast<double>(targetActiveLanes - window.activeLanes)) /
        static_cast<double>(std::max(1, targetActiveLanes));

    const double pressure = adaptiveBucketBasePressure(kind) + floorGap * 1.15 + targetGap * 0.85;
    const double maxPressure =
        kind == AdaptiveBucketKind::Low ? 0.95 :
        kind == AdaptiveBucketKind::JackRisk ? 1.05 :
        1.55;
    return std::clamp(pressure, 0.35, maxPressure);
}

double adaptiveLocalRatio(const AdaptiveGrowthWindow& window,
                          const ConvertOptions& options,
                          double globalTargetRatio) {
    if (window.originalNotes <= 0 || globalTargetRatio <= 0.0) {
        return 0.0;
    }

    const double densityNps = static_cast<double>(window.originalNotes) * 1000.0 /
                              static_cast<double>(std::max(1, window.endMs - window.startMs));
    const double holdRate = static_cast<double>(window.holdNotes) /
                            static_cast<double>(std::max(1, window.originalNotes));
    const double chordRate = static_cast<double>(window.chordSlices) /
                             static_cast<double>(std::max(1, window.totalSlices));
    const double jackRiskRate = static_cast<double>(window.jackRiskPairs) /
                                static_cast<double>(std::max(1, window.originalNotes));
    if (!options.targetKProfile.has_value()) {
        const double densityRoom = adaptiveDensityRoom(densityNps);
        const double patternSafety = adaptivePatternSafety(holdRate, chordRate);
        const double hardMax =
            tenKFullFieldRemixActive(options) ? globalTargetRatio : std::min(options.maxAddedNoteRatio, 0.45);
        return std::clamp(globalTargetRatio * densityRoom * patternSafety, 0.0, hardMax);
    }

    const auto& profile = *options.targetKProfile;
    const auto bucketChoice =
        chooseAdaptiveBucket(window, options, densityNps, chordRate, jackRiskRate);

    double targetKNeed = 0.0;
    double densityRoom = 0.0;
    double patternSafety = 0.0;
    double adjacentPressure = 0.0;
    if (bucketChoice.bucket != nullptr) {
        targetKNeed = profiledTargetKNeed(window,
                                          options,
                                          *bucketChoice.bucket,
                                          bucketChoice.kind,
                                          profile);
        densityRoom = profiledDensityRoom(*bucketChoice.bucket, densityNps);
        patternSafety = profiledPatternSafety(holdRate, chordRate, bucketChoice.kind);
        const double adjacentTarget =
            statMedianOr(bucketChoice.bucket->adjacentExpansion, profile.desiredAdjacentExpansion);
        adjacentPressure = std::clamp(0.88 + adjacentTarget, 0.85, 1.28);
    } else {
        const double desiredActiveRate = std::clamp(profile.desiredActiveLaneRate, 0.10, 1.0);
        const int desiredActiveLanes = activeLanesForRate(desiredActiveRate, options.targetKeyCount);
        const double activeGap =
            std::max(0.0, static_cast<double>(desiredActiveLanes - window.activeLanes)) /
            static_cast<double>(desiredActiveLanes);
        targetKNeed = std::clamp(0.75 + activeGap * 0.95, 0.65, 1.45);
        densityRoom = adaptiveDensityRoom(densityNps);
        patternSafety = adaptivePatternSafety(holdRate, chordRate);
        adjacentPressure = std::clamp(0.85 + profile.desiredAdjacentExpansion, 0.85, 1.25);
    }
    const bool lowGrowth = effectiveExpansionPolicy(options) == ExpansionPolicy::PreserveTapPlusLow;
    const double hardMax = tenKFullFieldRemixActive(options)
                               ? globalTargetRatio
                               : lowGrowth ? std::min(options.maxAddedNoteRatio, globalTargetRatio)
                                           : std::min(options.maxAddedNoteRatio, 0.45);
    return std::clamp(globalTargetRatio *
                          densityRoom *
                          patternSafety *
                          targetKNeed *
                          adjacentPressure,
                      0.0,
                      hardMax);
}

std::vector<AdaptiveGrowthWindow> buildAdaptiveGrowthWindows(const Chart& original,
                                                             const ConvertOptions& options,
                                                             double globalTargetRatio,
                                                             int windowMs) {
    if (!adaptiveBudgetEnabledFor(options) || globalTargetRatio <= 0.0 || original.notes.empty()) {
        return {};
    }

    int minTime = original.notes.front().time;
    int maxTime = original.notes.front().time;
    for (const auto& note : original.notes) {
        minTime = std::min(minTime, note.time);
        maxTime = std::max(maxTime, note.time);
    }
    const int startBase = (minTime / windowMs) * windowMs;
    const int endBase = ((maxTime - startBase) / windowMs) * windowMs + startBase;

    std::vector<AdaptiveGrowthWindow> windows;
    for (int start = startBase; start <= endBase; start += windowMs) {
        AdaptiveGrowthWindow window;
        window.bucket = (start - startBase) / windowMs;
        window.startMs = start;
        window.endMs = start + windowMs;

        std::set<int> activeLanes;
        std::map<int, int> notesByTime;
        std::vector<std::pair<int, int>> timedSourceLanes;
        for (const auto& note : original.notes) {
            if (note.time < window.startMs || note.time >= window.endMs) {
                continue;
            }
            ++window.originalNotes;
            const int sourceLane = note.sourceLane.has_value() ? *note.sourceLane : note.lane;
            activeLanes.insert(sourceLane);
            timedSourceLanes.push_back({note.time, sourceLane});
            if (note.type == NoteType::Hold) {
                ++window.holdNotes;
            }
            ++notesByTime[note.time];
        }
        if (window.originalNotes <= 0) {
            continue;
        }
        window.activeLanes = static_cast<int>(activeLanes.size());
        window.totalSlices = static_cast<int>(notesByTime.size());
        for (const auto& [time, count] : notesByTime) {
            (void)time;
            if (count >= 2) {
                ++window.chordSlices;
            }
        }
        std::sort(timedSourceLanes.begin(), timedSourceLanes.end());
        for (std::size_t index = 1; index < timedSourceLanes.size(); ++index) {
            const auto& previous = timedSourceLanes[index - 1];
            const auto& current = timedSourceLanes[index];
            if (previous.second == current.second) {
                const int delta = current.first - previous.first;
                if (delta > 0 && delta <= options.jackWindowMs) {
                    ++window.jackRiskPairs;
                }
            }
        }
        window.localRatio = adaptiveLocalRatio(window, options, globalTargetRatio);
        window.maxAdds = static_cast<int>(std::floor(static_cast<double>(window.originalNotes) *
                                                     window.localRatio));
        if (window.maxAdds == 0 && window.localRatio >= 0.08 &&
            window.activeLanes < options.targetKeyCount) {
            window.maxAdds = 1;
        }
        windows.push_back(window);
    }
    return windows;
}

int measureIndexForTime(int time) {
    constexpr int fallbackMeasureMs = 2000;
    return time / fallbackMeasureMs;
}

ConvertOptions expansionDistanceOptions(const ConvertOptions& options) {
    ConvertOptions guard = options;
    guard.distancePolicy = DistancePolicy::AimodSafe;
    guard.minObjectGapMs = options.expansionMinGapMs;
    guard.sameLaneMinGapMs = options.expansionSameLaneMinGapMs;
    return guard;
}

ConvertOptions echoDistanceOptions(const ConvertOptions& options) {
    ConvertOptions guard = options;
    guard.distancePolicy = DistancePolicy::AimodSafe;
    guard.minObjectGapMs = options.echoMinGapMs;
    guard.sameLaneMinGapMs = options.echoSameLaneMinGapMs;
    return guard;
}

bool candidateLess(const ExpansionCandidate& lhs, const ExpansionCandidate& rhs) {
    if (lhs.score != rhs.score) {
        return lhs.score > rhs.score;
    }
    if (lhs.rhythmDriftAbs != rhs.rhythmDriftAbs) {
        return lhs.rhythmDriftAbs < rhs.rhythmDriftAbs;
    }
    if (lhs.note.time != rhs.note.time) {
        return lhs.note.time > rhs.note.time;
    }
    if (lhs.snapPriority != rhs.snapPriority) {
        return lhs.snapPriority > rhs.snapPriority;
    }
    if (lhs.laneMovement != rhs.laneMovement) {
        return lhs.laneMovement < rhs.laneMovement;
    }
    if (lhs.note.lane != rhs.note.lane) {
        return lhs.note.lane < rhs.note.lane;
    }
    if (lhs.sourcePatternStart != rhs.sourcePatternStart) {
        return lhs.sourcePatternStart < rhs.sourcePatternStart;
    }
    if (lhs.sourceSliceIndex != rhs.sourceSliceIndex) {
        return lhs.sourceSliceIndex < rhs.sourceSliceIndex;
    }
    if (lhs.sourceNoteIndex != rhs.sourceNoteIndex) {
        return lhs.sourceNoteIndex < rhs.sourceNoteIndex;
    }
    return lhs.ordinal < rhs.ordinal;
}

class ExpansionContext;
bool shouldRejectByDenimWebGuard(const ExpansionCandidate& candidate, const ExpansionContext& context);

class ExpansionContext {
public:
    ExpansionContext(Chart& convertedChart, const Chart& originalChart, const ConvertOptions& convertOptions)
        : chart(convertedChart),
          original(originalChart),
          options(convertOptions),
          maxAdded(maxAddedTotal(originalChart, convertOptions)),
          maxEchoAdded(maxEchoTotal(originalChart, convertOptions)),
          maxStreamEchoAdded(maxStreamEchoTotal(originalChart, convertOptions)),
          streamProfile(streamEchoProfileSettings(convertOptions.streamEchoProfile)),
          composerProfile(composerSettingsForOptions(convertOptions)),
          distanceOptions(expansionDistanceOptions(convertOptions)),
          echoDistanceOptionsForAdd(echoDistanceOptions(convertOptions)),
          noAddTimes(fastStairSuppressedTimes(convertedChart.notes,
                                              convertOptions,
                                              originalChart.timingPoints)) {
        laneUse = calculateLaneDistribution(chart.notes, options.targetKeyCount);
        stats.expansionComposerProfile = composerProfile.profile;
        stats.targetAddedNoteRatio = effectiveTargetAddedNoteRatio(options);
        adaptiveWindowMs = adaptiveBudgetWindowMs(options);
        adaptiveWindows =
            buildAdaptiveGrowthWindows(originalChart, convertOptions, stats.targetAddedNoteRatio, adaptiveWindowMs);
        configureAdaptiveBudgetStats();
    }

    bool tryAdd(ExpansionCandidate candidate) {
        if (maxAdded <= 0 || stats.addedNotes >= maxAdded ||
            perSliceAdded[candidate.sourceSliceIndex] >= options.maxAddedPerSlice ||
            perMeasureAdded[measureIndexForTime(candidate.note.time)] >= options.maxAddedPerMeasure) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByBudget;
            ++stats.rejectedByComposerBudget;
            return false;
        }
        if (rejectByAdaptiveBudget(candidate.note.time)) {
            return false;
        }

        if (suppressAddAtTime(candidate.note.time)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByComposerSafety;
            return false;
        }

        if (shouldRejectByDenimWebGuard(candidate, *this)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByComposerSafety;
            return false;
        }

        if (options.snapAddedNotes &&
            !isSnapAligned(candidate.note.time, original.timingPoints, options.expansionSnapToleranceMs)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedBySnap;
            ++stats.rejectedByComposerSafety;
            return false;
        }

        if (shouldRejectByJackGuard(candidate)) {
            rejectByJackGuard(false, false);
            return false;
        }

        if (hasSameTimeNote(chart.notes, candidate.note.time, candidate.note.lane) ||
            hasLongNoteConflict(chart.notes, candidate.note, candidate.note.lane)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByCollision;
            ++stats.rejectedByComposerSafety;
            return false;
        }

        if (hasDistanceConflict(chart.notes, candidate.note, distanceOptions, false)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByDistance;
            ++stats.rejectedByComposerSafety;
            return false;
        }

        commit(candidate);
        return true;
    }

    bool tryAddEcho(ExpansionCandidate candidate, const std::string& patternKey) {
        const bool isStreamEcho = candidate.echoKind == "stream";
        const int effectiveMaxEchoAdded = isStreamEcho ? std::max(maxEchoAdded, maxStreamEchoAdded) : maxEchoAdded;
        if (maxAdded <= 0 || stats.addedNotes >= maxAdded ||
            perSliceAdded[candidate.sourceSliceIndex] >= options.maxAddedPerSlice ||
            perMeasureAdded[measureIndexForTime(candidate.note.time)] >= options.maxAddedPerMeasure ||
            effectiveMaxEchoAdded <= 0 || stats.addedByEcho >= effectiveMaxEchoAdded ||
            (isStreamEcho && stats.addedByStreamEcho >= maxStreamEchoAdded) ||
            perPatternEchoAdded[patternKey] >= options.maxEchoPerPattern ||
            perEchoSliceAdded[candidate.sourceSliceIndex] >= options.maxEchoPerSlice ||
            perEchoMeasureAdded[measureIndexForTime(candidate.note.time)] >= options.maxEchoPerMeasure) {
            rejectEchoBudget(isStreamEcho);
            return false;
        }

        if (suppressAddAtTime(candidate.note.time)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedEchoCandidates;
            ++stats.rejectedByComposerSafety;
            if (isStreamEcho) {
                ++stats.rejectedStreamPrimaryByPatternLength;
            }
            return false;
        }

        const double maxLocalNps = isStreamEcho ? streamProfile.maxLocalNps : options.echoMaxLocalNps;
        if (options.echoAvoidHighDensity && localNps(candidate.note.time) > maxLocalNps) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedEchoCandidates;
            ++stats.rejectedEchoByDensity;
            ++stats.rejectedByComposerSafety;
            if (isStreamEcho) {
                ++stats.rejectedStreamEchoByLocalNps;
                ++stats.rejectedStreamPrimaryByLocalNps;
            }
            return false;
        }

        if (options.echoRequiresSnap &&
            !isSnapAligned(candidate.note.time, original.timingPoints, options.expansionSnapToleranceMs)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedBySnap;
            ++stats.rejectedEchoCandidates;
            ++stats.rejectedEchoBySnap;
            ++stats.rejectedByComposerSafety;
            if (isStreamEcho) {
                ++stats.rejectedStreamPrimaryBySnap;
            }
            return false;
        }

        if (shouldRejectByJackGuard(candidate)) {
            rejectByJackGuard(true, isStreamEcho);
            return false;
        }

        if (hasSameTimeNote(chart.notes, candidate.note.time, candidate.note.lane) ||
            hasLongNoteConflict(chart.notes, candidate.note, candidate.note.lane)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByCollision;
            ++stats.rejectedEchoCandidates;
            ++stats.rejectedByComposerSafety;
            if (isStreamEcho) {
                ++stats.rejectedStreamPrimaryByCollision;
            }
            return false;
        }

        if (hasDistanceConflict(chart.notes, candidate.note, echoDistanceOptionsForAdd, false)) {
            ++stats.rejectedExpansionCandidates;
            ++stats.rejectedByDistance;
            ++stats.rejectedEchoCandidates;
            ++stats.rejectedEchoByDistance;
            ++stats.rejectedByComposerSafety;
            if (isStreamEcho) {
                ++stats.rejectedStreamPrimaryByDistance;
            }
            return false;
        }

        if (isStreamEcho) {
            ++stats.streamSafeLaneCandidates;
        }
        commit(candidate);
        ++perPatternEchoAdded[patternKey];
        ++perEchoSliceAdded[candidate.sourceSliceIndex];
        ++perEchoMeasureAdded[measureIndexForTime(candidate.note.time)];
        if (candidate.echoKind == "stair_up" || candidate.echoKind == "stair_down") {
            ++stats.addedByStairEcho;
        } else if (candidate.echoKind == "trill") {
            ++stats.addedByTrillEcho;
        } else if (candidate.echoKind == "stream") {
            ++stats.addedByStreamEcho;
            ++stats.streamAcceptedCandidates;
        }
        return true;
    }

    Chart& chart;
    const Chart& original;
    const ConvertOptions& options;
    ExpansionPlanStats stats;
    int maxAdded = 0;
    int maxEchoAdded = 0;
    int maxStreamEchoAdded = 0;
    StreamEchoProfileSettings streamProfile;
    ExpansionComposerSettings composerProfile;
    std::vector<int> laneUse;

    bool suppressAddAtTime(int time) const {
        return noAddTimes.count(time) > 0;
    }

private:
    static int maxEchoTotal(const Chart& originalChart, const ConvertOptions& convertOptions) {
        if (convertOptions.maxEchoAddedRatio <= 0.0 || originalChart.notes.empty()) {
            return 0;
        }
        return std::max(0,
                        static_cast<int>(std::floor(static_cast<double>(originalChart.notes.size()) *
                                                    convertOptions.maxEchoAddedRatio)));
    }

    static int maxStreamEchoTotal(const Chart& originalChart, const ConvertOptions& convertOptions) {
        const auto profile = streamEchoProfileSettings(convertOptions.streamEchoProfile);
        if (profile.maxAddedRatio <= 0.0 || originalChart.notes.empty() || convertOptions.maxEchoAddedRatio <= 0.0) {
            return 0;
        }
        return std::max(1,
                        static_cast<int>(std::floor(static_cast<double>(originalChart.notes.size()) *
                                                    profile.maxAddedRatio)));
    }

    double localNps(int time) const {
        const int window = std::max(1, options.echoHighDensityWindowMs);
        const int start = time - window / 2;
        const int end = time + window / 2;
        int count = 0;
        for (const auto& note : chart.notes) {
            if (note.time >= start && note.time <= end) {
                ++count;
            }
        }
        return static_cast<double>(count) * 1000.0 / static_cast<double>(window);
    }

    bool shouldRejectByJackGuard(const ExpansionCandidate& candidate) const {
        if (options.jackPreservePolicy == JackPreservePolicy::SmoothAll) {
            return false;
        }
        return wouldCreateJackOnLane(chart.notes, candidate.note, options.jackWindowMs);
    }

    void rejectByJackGuard(bool isEcho, bool isStreamEcho) {
        ++stats.preventedJacks;
        ++stats.rejectedExpansionCandidates;
        ++stats.rejectedByComposerSafety;
        if (isEcho) {
            ++stats.rejectedEchoCandidates;
            if (isStreamEcho) {
                ++stats.rejectedStreamEchoByJack;
                ++stats.rejectedStreamPrimaryByJack;
            }
        }
    }

    void rejectEchoBudget(bool isStreamEcho) {
        ++stats.rejectedExpansionCandidates;
        ++stats.rejectedByBudget;
        ++stats.rejectedEchoCandidates;
        ++stats.rejectedEchoByBudget;
        ++stats.rejectedByComposerBudget;
        if (isStreamEcho) {
            ++stats.rejectedStreamPrimaryByBudget;
        }
    }

    bool rejectByAdaptiveBudget(int time) {
        const auto windowIndex = adaptiveWindowIndexForTime(time);
        if (!windowIndex.has_value()) {
            return false;
        }
        auto& window = adaptiveWindows[*windowIndex];
        if (window.added < window.maxAdds) {
            return false;
        }
        ++stats.rejectedExpansionCandidates;
        ++stats.rejectedByBudget;
        ++stats.rejectedByComposerBudget;
        ++stats.rejectedByAdaptiveBudget;
        return true;
    }

    void commit(ExpansionCandidate& candidate) {
        candidate.note.id = generatedId(candidate);
        chart.notes.push_back(candidate.note);
        ++stats.addedNotes;
        ++stats.acceptedByComposer;
        ++perSliceAdded[candidate.sourceSliceIndex];
        ++perMeasureAdded[measureIndexForTime(candidate.note.time)];
        if (candidate.note.lane >= 0 && candidate.note.lane < static_cast<int>(laneUse.size())) {
            ++laneUse[static_cast<std::size_t>(candidate.note.lane)];
        }
        const auto windowIndex = adaptiveWindowIndexForTime(candidate.note.time);
        if (windowIndex.has_value()) {
            ++adaptiveWindows[*windowIndex].added;
        }
        if (candidate.ruleName == "chord_fill") {
            ++stats.addedByChordFill;
        } else if (candidate.ruleName == "tap_plus") {
            ++stats.addedByTapPlus;
        } else if (candidate.ruleName == "training_scaffold") {
            ++stats.addedByTrainingScaffold;
        } else if (candidate.ruleName == "echo") {
            ++stats.addedByEcho;
        }

        GeneratedNoteInfo info;
        info.generatedId = candidate.note.id;
        info.ruleName = candidate.ruleName;
        info.sourceNoteIds = candidate.sourceNoteIds;
        info.sourceSliceIndex = candidate.sourceSliceIndex;
        info.originalTimeMs = candidate.note.time;
        info.generatedTimeMs = candidate.note.time;
        info.generatedLane = candidate.note.lane;
        stats.generatedNotes.push_back(std::move(info));
    }

    ConvertOptions distanceOptions;
    ConvertOptions echoDistanceOptionsForAdd;
    std::set<int> noAddTimes;
    int adaptiveWindowMs = 1000;
    std::vector<AdaptiveGrowthWindow> adaptiveWindows;
    std::map<int, std::size_t> adaptiveWindowByBucket;
    std::map<int, int> perSliceAdded;
    std::map<int, int> perMeasureAdded;
    std::map<int, int> perEchoSliceAdded;
    std::map<int, int> perEchoMeasureAdded;
    std::map<std::string, int> perPatternEchoAdded;

    void configureAdaptiveBudgetStats() {
        if (adaptiveWindows.empty()) {
            stats.adaptiveGrowthBudgetEnabled = false;
            stats.adaptiveBudgetWindowMs = adaptiveWindowMs;
            return;
        }

        stats.adaptiveGrowthBudgetEnabled = true;
        stats.adaptiveBudgetWindowMs = adaptiveWindowMs;
        stats.adaptiveBudgetWindows = static_cast<int>(adaptiveWindows.size());
        stats.adaptiveBudgetMinRatio = adaptiveWindows.front().localRatio;
        stats.adaptiveBudgetMaxRatio = adaptiveWindows.front().localRatio;

        double weightedRatioSum = 0.0;
        int totalNotes = 0;
        for (std::size_t index = 0; index < adaptiveWindows.size(); ++index) {
            const auto& window = adaptiveWindows[index];
            adaptiveWindowByBucket[window.bucket] = index;
            stats.adaptiveBudgetMinRatio = std::min(stats.adaptiveBudgetMinRatio, window.localRatio);
            stats.adaptiveBudgetMaxRatio = std::max(stats.adaptiveBudgetMaxRatio, window.localRatio);
            weightedRatioSum += window.localRatio * static_cast<double>(window.originalNotes);
            totalNotes += window.originalNotes;
        }
        stats.adaptiveBudgetAverageRatio =
            totalNotes <= 0 ? 0.0 : weightedRatioSum / static_cast<double>(totalNotes);
    }

    std::optional<std::size_t> adaptiveWindowIndexForTime(int time) const {
        if (adaptiveWindows.empty() || adaptiveWindowMs <= 0) {
            return std::nullopt;
        }
        const int firstStart = adaptiveWindows.front().startMs;
        const int bucket = (time - firstStart) / adaptiveWindowMs;
        const auto found = adaptiveWindowByBucket.find(bucket);
        if (found == adaptiveWindowByBucket.end()) {
            return std::nullopt;
        }
        return found->second;
    }
};

void tuneHighKeyGeneratedCandidate(ExpansionCandidate& candidate, const ExpansionContext& context);
void sortExpansionSlices(std::vector<SliceView>& slices, const ExpansionContext& context);

std::vector<ExpansionCandidate> chordFillCandidates(const SliceView& slice,
                                                    const std::vector<Note>& notes,
                                                    const ExpansionContext& context) {
    std::set<int> occupied;
    int minLane = context.options.targetKeyCount;
    int maxLane = 0;
    for (const auto noteIndex : slice.noteIndices) {
        occupied.insert(notes[noteIndex].lane);
        minLane = std::min(minLane, notes[noteIndex].lane);
        maxLane = std::max(maxLane, notes[noteIndex].lane);
    }
    if (slice.noteIndices.size() < 2 || minLane > maxLane || maxLane - minLane < 2) {
        return {};
    }

    const auto sourceIds = sourceIdsForSlice(slice, notes);
    const int center = static_cast<int>(std::lround((static_cast<double>(minLane) + maxLane) / 2.0));
    const auto laneOrder = orderedLaneCandidates(center, context.options.targetKeyCount);

    std::vector<ExpansionCandidate> candidates;
    int ordinal = 0;
    for (const int lane : laneOrder) {
        if (lane < minLane || lane > maxLane || occupied.count(lane) > 0) {
            continue;
        }
        if (!candidateRespectsEvenSourceHand(slice.noteIndices, notes, lane, context.options)) {
            continue;
        }

        ExpansionCandidate candidate;
        candidate.note.time = slice.time;
        candidate.note.lane = lane;
        candidate.note.type = NoteType::Tap;
        candidate.note.sourceLane = lane;
        candidate.ruleName = "chord_fill";
        candidate.sourceNoteIds = sourceIds;
        candidate.sourceSliceIndex = slice.index;
        candidate.sourceNoteIndex = 0;
        candidate.ordinal = ordinal++;
        candidate.score = 100.0 - std::abs(lane - center) * 0.5 -
                          static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 0.25;
        candidate.rhythmDriftAbs = 0;
        candidate.snapPriority = 1;
        candidate.laneMovement = std::abs(lane - center);
        tuneHighKeyGeneratedCandidate(candidate, context);
        candidates.push_back(std::move(candidate));
    }
    std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
    return candidates;
}

void applyChordFill(ExpansionContext& context) {
    auto slices = buildSliceViews(context.chart.notes, context.options.sameTimeEpsilonMs);
    sortExpansionSlices(slices, context);
    for (const auto& slice : slices) {
        int addedInThisSlice = 0;
        auto candidates = chordFillCandidates(slice, context.chart.notes, context);
        for (auto& candidate : candidates) {
            if (addedInThisSlice >= context.options.maxAddedPerSlice) {
                break;
            }
            if (context.tryAdd(candidate)) {
                ++addedInThisSlice;
            }
        }
    }
}

std::vector<int> underusedLaneOrder(const ExpansionContext& context) {
    std::vector<int> lanes(static_cast<std::size_t>(context.options.targetKeyCount));
    std::iota(lanes.begin(), lanes.end(), 0);
    const double center = static_cast<double>(std::max(0, context.options.targetKeyCount - 1)) / 2.0;
    std::stable_sort(lanes.begin(), lanes.end(), [&](int lhs, int rhs) {
        const int lhsUse = context.laneUse[static_cast<std::size_t>(lhs)];
        const int rhsUse = context.laneUse[static_cast<std::size_t>(rhs)];
        if (lhsUse != rhsUse) {
            return lhsUse < rhsUse;
        }
        const double lhsCenter = std::abs(static_cast<double>(lhs) - center);
        const double rhsCenter = std::abs(static_cast<double>(rhs) - center);
        if (lhsCenter != rhsCenter) {
            return lhsCenter < rhsCenter;
        }
        return lhs < rhs;
    });
    return lanes;
}

int localLaneUseAround(const std::vector<Note>& notes, int lane, int time, int windowMs) {
    const int halfWindow = std::max(250, windowMs / 2);
    const int start = time - halfWindow;
    const int end = time + halfWindow;
    int count = 0;
    for (const auto& note : notes) {
        if (note.lane != lane) {
            continue;
        }
        const int noteEnd = note.endTime.value_or(note.time);
        if (note.time <= end && noteEnd >= start) {
            ++count;
        }
    }
    return count;
}

bool outerLane(int lane, int targetKeyCount) {
    return targetKeyCount > 1 && (lane == 0 || lane == targetKeyCount - 1);
}

bool lightEdgeLane(int lane, int targetKeyCount) {
    if (targetKeyCount < 2) {
        return false;
    }
    const int edgeWidth =
        std::max(1, static_cast<int>(std::ceil(static_cast<double>(targetKeyCount) * 0.20)));
    return lane < edgeWidth || lane >= targetKeyCount - edgeWidth;
}

double lightEdgePenalty(int lane, int targetKeyCount) {
    if (!lightEdgeLane(lane, targetKeyCount)) {
        return 0.0;
    }
    return outerLane(lane, targetKeyCount) ? 10.0 : 8.0;
}

double outerEdgeTrillPenalty(const std::vector<Note>& notes, int lane, int time, int targetKeyCount) {
    if (!outerLane(lane, targetKeyCount)) {
        return 0.0;
    }

    const int opposite = lane == 0 ? targetKeyCount - 1 : 0;
    int oppositeNear = 0;
    int sameNear = 0;
    constexpr int kEdgeTrillWindowMs = 750;
    for (const auto& note : notes) {
        if (std::abs(note.time - time) > kEdgeTrillWindowMs) {
            continue;
        }
        if (note.lane == opposite) {
            ++oppositeNear;
        } else if (note.lane == lane) {
            ++sameNear;
        }
    }
    if (oppositeNear == 0) {
        return 0.0;
    }
    return std::min(30.0, 14.0 + static_cast<double>(oppositeNear) * 4.0 -
                              std::min(6.0, static_cast<double>(sameNear) * 2.0));
}

double mirrorSymmetryScore(const ExpansionContext& context, int lane) {
    const int targetKeyCount = context.options.targetKeyCount;
    const int mirror = targetKeyCount - 1 - lane;
    if (lane < 0 || lane >= targetKeyCount || mirror < 0 || mirror >= targetKeyCount || mirror == lane) {
        return 0.0;
    }
    const int laneUse = context.laneUse[static_cast<std::size_t>(lane)];
    const int mirrorUse = context.laneUse[static_cast<std::size_t>(mirror)];
    return std::max(-8.0, std::min(8.0, static_cast<double>(mirrorUse - laneUse) * 1.75));
}

bool stagedMirrorCompressExpansionActive(const ConvertOptions& options) {
    return options.sourceKeyCount == 7 &&
           options.targetKeyCount == 10 &&
           options.tenKeyPlannerPolicy == TenKeyPlannerPolicy::StagedMirrorCompress;
}

double stagedMirrorPanelCenterScore(const ExpansionContext& context, int lane) {
    if (!stagedMirrorCompressExpansionActive(context.options) ||
        (lane != 2 && lane != 7) ||
        context.laneUse.size() < 10) {
        return 0.0;
    }

    const int panelStart = lane < 5 ? 0 : 5;
    const int panelEnd = panelStart + 4;
    int panelUse = 0;
    for (int panelLane = panelStart; panelLane <= panelEnd; ++panelLane) {
        panelUse += context.laneUse[static_cast<std::size_t>(panelLane)];
    }
    const double panelAverage = static_cast<double>(panelUse) / 5.0;
    const double centerUse = static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]);
    const double centerNeed =
        std::clamp((panelAverage - centerUse) / std::max(1.0, panelAverage), 0.0, 1.0);
    return centerNeed * 32.0;
}

int positiveModulo(int value, int divisor) {
    if (divisor <= 0) {
        return 0;
    }
    const int result = value % divisor;
    return result < 0 ? result + divisor : result;
}

int circularDistance(int lhs, int rhs, int width) {
    if (width <= 0) {
        return 0;
    }
    const int delta = std::abs(lhs - rhs);
    return std::min(delta, width - delta);
}

int beatIndexAtOrFallback(int time, const std::vector<TimingPoint>& timingPoints) {
    const auto grid = explicitGridAt(time, timingPoints);
    const int gridTime = grid.has_value() ? grid->time : 0;
    const double beatLength = grid.has_value() && grid->beatLength > 0.0
                                  ? grid->beatLength
                                  : static_cast<double>(kFallbackBeatLengthMs);
    if (beatLength <= 0.0) {
        return 0;
    }
    return static_cast<int>(std::floor((static_cast<double>(time - gridTime) / beatLength) + 1e-6));
}

int sameTimeHandLaneCountForCandidate(const ExpansionCandidate& candidate, const ExpansionContext& context) {
    if (candidate.note.lane < 0 || candidate.note.lane >= context.options.targetKeyCount) {
        return 0;
    }
    const int hand = handForLane(candidate.note.lane, context.options.targetKeyCount);
    std::set<int> lanes;
    for (const auto& note : context.chart.notes) {
        if (std::abs(note.time - candidate.note.time) > context.options.sameTimeEpsilonMs) {
            continue;
        }
        if (note.lane < 0 || note.lane >= context.options.targetKeyCount ||
            handForLane(note.lane, context.options.targetKeyCount) != hand) {
            continue;
        }
        lanes.insert(note.lane);
    }
    return static_cast<int>(lanes.size());
}

double tenKeyDenimBeatShiftScore(const ExpansionCandidate& candidate, const ExpansionContext& context) {
    if (!tenKeyQuarterEighthDensityActive(context.options) ||
        candidate.ruleName != "tap_plus" ||
        candidate.note.type != NoteType::Tap ||
        sameTimeHandLaneCountForCandidate(candidate, context) < 2) {
        return 0.0;
    }

    const int handStart = handForLane(candidate.note.lane, context.options.targetKeyCount) == 0
                              ? 0
                              : handBoundary(context.options.targetKeyCount);
    const int handEnd = handForLane(candidate.note.lane, context.options.targetKeyCount) == 0
                            ? handBoundary(context.options.targetKeyCount)
                            : context.options.targetKeyCount;
    const int handWidth = handEnd - handStart;
    if (handWidth <= 1) {
        return 0.0;
    }
    const int localLane = candidate.note.lane - handStart;
    if (localLane < 0 || localLane >= handWidth) {
        return 0.0;
    }

    const int beatIndex = beatIndexAtOrFallback(candidate.note.time, context.original.timingPoints);
    const int globalPreferredLane = positiveModulo(2 + beatIndex, context.options.targetKeyCount);
    const int panelPreferredLane =
        handStart + positiveModulo(1 + beatIndex / 2 + (handStart == 0 ? 0 : 2), handWidth);

    const int globalDistance =
        circularDistance(candidate.note.lane, globalPreferredLane, context.options.targetKeyCount);
    const int panelDistance = circularDistance(localLane, panelPreferredLane - handStart, handWidth);
    const int recentWindow =
        static_cast<int>(std::lround(beatLengthAtOrFallback(candidate.note.time, context.original.timingPoints) * 2.0)) +
        std::max(2, context.options.expansionSnapToleranceMs);
    double recentGeneratedLanePenalty = 0.0;
    for (const auto& note : context.chart.notes) {
        if (note.lane != candidate.note.lane || note.id.rfind("gen:tap_plus:", 0) != 0) {
            continue;
        }
        const int delta = candidate.note.time - note.time;
        if (delta > context.options.sameTimeEpsilonMs && delta <= recentWindow) {
            recentGeneratedLanePenalty = 12.0;
            break;
        }
    }
    return 10.0 - static_cast<double>(globalDistance) * 2.5 +
           6.0 - static_cast<double>(panelDistance) * 3.0 -
           recentGeneratedLanePenalty;
}

void tuneHighKeyGeneratedCandidate(ExpansionCandidate& candidate, const ExpansionContext& context) {
    if (!highKeyGeneratedTuningActive(context.options)) {
        return;
    }

    const int tolerance = std::max(2, context.options.expansionSnapToleranceMs);
    const auto quarterDelta = quarterBeatDeltaMs(candidate.note.time, context.original.timingPoints);
    const auto eighthDelta = eighthBeatDeltaMs(candidate.note.time, context.original.timingPoints);
    const auto sixteenthDelta = sixteenthBeatDeltaMs(candidate.note.time, context.original.timingPoints);
    if (eighthDelta.has_value() && *eighthDelta <= tolerance) {
        candidate.rhythmDriftAbs = *eighthDelta;
        candidate.score += 10.0;
        candidate.snapPriority = std::max(candidate.snapPriority, 3);
    } else if (sixteenthDelta.has_value() && *sixteenthDelta <= tolerance) {
        candidate.rhythmDriftAbs = *sixteenthDelta;
        candidate.score += 4.5;
        candidate.snapPriority = std::max(candidate.snapPriority, 2);
    } else if (eighthDelta.has_value()) {
        candidate.rhythmDriftAbs = *eighthDelta;
        candidate.score -= std::min(10.0, static_cast<double>(*eighthDelta) / 8.0);
        candidate.snapPriority = std::min(candidate.snapPriority, 0);
    }

    if (tenKeyQuarterEighthDensityActive(context.options)) {
        if (quarterDelta.has_value() && *quarterDelta <= tolerance) {
            candidate.score += 4.0;
            candidate.snapPriority = std::max(candidate.snapPriority, 4);
        } else if (eighthDelta.has_value() && *eighthDelta <= tolerance) {
            candidate.score += 2.5;
            candidate.snapPriority = std::max(candidate.snapPriority, 4);
        } else if (sixteenthDelta.has_value() && *sixteenthDelta <= tolerance) {
            candidate.score -= 1.5;
        }
    }

    candidate.score += tenKeyDenimBeatShiftScore(candidate, context);
    candidate.score += mirrorSymmetryScore(context, candidate.note.lane);
    candidate.score += stagedMirrorPanelCenterScore(context, candidate.note.lane);
    candidate.score -= lightEdgePenalty(candidate.note.lane, context.options.targetKeyCount);
    candidate.score -= outerEdgeTrillPenalty(context.chart.notes,
                                             candidate.note.lane,
                                             candidate.note.time,
                                             context.options.targetKeyCount);
}

void sortExpansionSlices(std::vector<SliceView>& slices, const ExpansionContext& context) {
    std::stable_sort(slices.begin(), slices.end(), [&](const SliceView& lhs, const SliceView& rhs) {
        if (lhs.noteIndices.size() != rhs.noteIndices.size()) {
            return lhs.noteIndices.size() < rhs.noteIndices.size();
        }
        if (tenKeyQuarterEighthDensityActive(context.options)) {
            const int tolerance = std::max(2, context.options.expansionSnapToleranceMs);
            const auto leftQuarter = quarterBeatDeltaMs(lhs.time, context.original.timingPoints);
            const auto rightQuarter = quarterBeatDeltaMs(rhs.time, context.original.timingPoints);
            const auto leftEighth = eighthBeatDeltaMs(lhs.time, context.original.timingPoints);
            const auto rightEighth = eighthBeatDeltaMs(rhs.time, context.original.timingPoints);
            const bool leftQuarterAligned = leftQuarter.has_value() && *leftQuarter <= tolerance;
            const bool rightQuarterAligned = rightQuarter.has_value() && *rightQuarter <= tolerance;
            const bool leftEighthAligned = leftEighth.has_value() && *leftEighth <= tolerance;
            const bool rightEighthAligned = rightEighth.has_value() && *rightEighth <= tolerance;
            const int leftRank = leftQuarterAligned ? 2 : leftEighthAligned ? 1 : 0;
            const int rightRank = rightQuarterAligned ? 2 : rightEighthAligned ? 1 : 0;
            if (leftRank != rightRank) {
                return leftRank > rightRank;
            }
            if (leftRank > 0) {
                const int leftDelta = leftRank == 2 ? *leftQuarter : *leftEighth;
                const int rightDelta = rightRank == 2 ? *rightQuarter : *rightEighth;
                if (leftDelta != rightDelta) {
                    return leftDelta < rightDelta;
                }
            }
        }
        if (highKeyGeneratedTuningActive(context.options)) {
            const auto leftDelta = eighthBeatDeltaMs(lhs.time, context.original.timingPoints);
            const auto rightDelta = eighthBeatDeltaMs(rhs.time, context.original.timingPoints);
            if (leftDelta.has_value() && rightDelta.has_value()) {
                const int tolerance = std::max(2, context.options.expansionSnapToleranceMs);
                const bool leftAligned = *leftDelta <= tolerance;
                const bool rightAligned = *rightDelta <= tolerance;
                if (leftAligned != rightAligned) {
                    return leftAligned;
                }
                if (*leftDelta != *rightDelta) {
                    return *leftDelta < *rightDelta;
                }
            }
        }
        return lhs.time < rhs.time;
    });
}

std::pair<int, int> sliceHandCounts(const SliceView& slice,
                                    const std::vector<Note>& notes,
                                    int targetKeyCount) {
    int left = 0;
    int right = 0;
    for (const auto noteIndex : slice.noteIndices) {
        if (noteIndex >= notes.size()) {
            continue;
        }
        if (handForLane(notes[noteIndex].lane, targetKeyCount) == 0) {
            ++left;
        } else {
            ++right;
        }
    }
    return {left, right};
}

int counterHandForSlice(const SliceView& slice, const std::vector<Note>& notes, int targetKeyCount) {
    const auto [left, right] = sliceHandCounts(slice, notes, targetKeyCount);
    if (left > right) {
        return 1;
    }
    if (right > left) {
        return 0;
    }
    return -1;
}

bool denimWebGuardActive(const ConvertOptions& options) {
    return options.targetKeyCount == 8 && options.targetKeyCount > options.sourceKeyCount;
}

bool denimWebCandidateType(const ExpansionCandidate& candidate) {
    return candidate.ruleName == "tap_plus" && candidate.note.type == NoteType::Tap;
}

std::vector<int> sameTimeHandLanes(const std::vector<Note>& notes,
                                   int time,
                                   int hand,
                                   int targetKeyCount,
                                   int epsilonMs) {
    std::set<int> lanes;
    for (const auto& note : notes) {
        if (std::abs(note.time - time) > epsilonMs) {
            continue;
        }
        if (note.lane < 0 || note.lane >= targetKeyCount ||
            handForLane(note.lane, targetKeyCount) != hand) {
            continue;
        }
        lanes.insert(note.lane);
    }
    return {lanes.begin(), lanes.end()};
}

std::vector<int> sameTimeHandLanesWithCandidate(const std::vector<Note>& notes,
                                                const ExpansionCandidate& candidate,
                                                const ConvertOptions& options) {
    const int hand = handForLane(candidate.note.lane, options.targetKeyCount);
    std::set<int> lanes;
    const auto current =
        sameTimeHandLanes(notes, candidate.note.time, hand, options.targetKeyCount, options.sameTimeEpsilonMs);
    lanes.insert(current.begin(), current.end());
    if (candidate.note.lane >= 0 && candidate.note.lane < options.targetKeyCount) {
        lanes.insert(candidate.note.lane);
    }
    return {lanes.begin(), lanes.end()};
}

std::vector<int> previousSameHandChordLanes(const std::vector<Note>& notes,
                                            int time,
                                            int hand,
                                            const ConvertOptions& options,
                                            const std::vector<TimingPoint>& timingPoints) {
    const int tolerance = std::max(2, options.expansionSnapToleranceMs);
    const int windowMs =
        std::max(125, static_cast<int>(std::ceil(beatLengthAtOrFallback(time, timingPoints) / 2.0)) + tolerance);
    std::optional<int> bestTime;
    int bestDelta = std::numeric_limits<int>::max();
    for (const auto& note : notes) {
        if (note.lane < 0 || note.lane >= options.targetKeyCount ||
            handForLane(note.lane, options.targetKeyCount) != hand) {
            continue;
        }
        const int delta = time - note.time;
        if (delta <= options.sameTimeEpsilonMs || delta > windowMs || delta >= bestDelta) {
            continue;
        }
        bestTime = note.time;
        bestDelta = delta;
    }
    if (!bestTime.has_value()) {
        return {};
    }
    return sameTimeHandLanes(notes, *bestTime, hand, options.targetKeyCount, options.sameTimeEpsilonMs);
}

bool laneSetsInterleave(const std::vector<int>& lhs, const std::vector<int>& rhs) {
    if (lhs.size() < 2 || rhs.size() < 2) {
        return false;
    }
    std::set<int> left(lhs.begin(), lhs.end());
    std::set<int> right(rhs.begin(), rhs.end());
    std::vector<int> combined;
    std::set_union(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(combined));
    if (combined.size() < 4) {
        return false;
    }

    int previousMask = 0;
    int transitions = 0;
    for (const int lane : combined) {
        int mask = 0;
        if (left.count(lane) > 0) {
            mask |= 1;
        }
        if (right.count(lane) > 0) {
            mask |= 2;
        }
        if (mask == 3) {
            continue;
        }
        if (previousMask != 0 && previousMask != mask) {
            ++transitions;
        }
        previousMask = mask;
    }
    if (transitions >= 3) {
        return true;
    }

    const bool disjoint = std::none_of(left.begin(), left.end(), [&](int lane) {
        return right.count(lane) > 0;
    });
    if (!disjoint) {
        return false;
    }
    return *left.begin() < *right.rbegin() && *right.begin() < *left.rbegin();
}

bool shouldRejectByDenimWebGuard(const ExpansionCandidate& candidate, const ExpansionContext& context) {
    if (!denimWebGuardActive(context.options) || !denimWebCandidateType(candidate)) {
        return false;
    }
    if (candidate.note.lane < 0 || candidate.note.lane >= context.options.targetKeyCount) {
        return false;
    }

    const int hand = handForLane(candidate.note.lane, context.options.targetKeyCount);
    const auto current = sameTimeHandLanesWithCandidate(context.chart.notes, candidate, context.options);
    if (current.size() >= 3) {
        return true;
    }

    const auto previous =
        previousSameHandChordLanes(context.chart.notes,
                                   candidate.note.time,
                                   hand,
                                   context.options,
                                   context.original.timingPoints);
    return laneSetsInterleave(previous, current);
}

std::vector<LnAnchor> holdAnchorsForSlice(const SliceView& slice, const std::vector<Note>& notes) {
    std::vector<LnAnchor> anchors;
    for (const auto noteIndex : slice.noteIndices) {
        if (noteIndex >= notes.size()) {
            continue;
        }
        const auto& note = notes[noteIndex];
        if (note.type != NoteType::Hold || !note.endTime.has_value() || *note.endTime <= note.time) {
            continue;
        }
        anchors.push_back({note.lane, *note.endTime - note.time});
    }
    return anchors;
}

LnWindowProfile lnWindowProfileFor(const SliceView& slice, const std::vector<Note>& notes, int targetKeyCount) {
    LnWindowProfile profile;
    profile.anchors = holdAnchorsForSlice(slice, notes);
    if (profile.anchors.empty()) {
        return profile;
    }

    const int time = slice.time;
    const int halfWindow = kLnHeavyWindowMs / 2;
    int noteStarts = 0;
    int holdStarts = 0;
    std::set<int> activeHoldLanes;

    for (const auto& note : notes) {
        if (std::abs(note.time - time) <= halfWindow) {
            ++noteStarts;
            if (note.type == NoteType::Hold && note.endTime.has_value() && *note.endTime > note.time) {
                ++holdStarts;
            }
        }
        if (note.type == NoteType::Hold && note.endTime.has_value() && note.time <= time && *note.endTime >= time) {
            activeHoldLanes.insert(note.lane);
        }
    }

    const double startRatio = noteStarts <= 0 ? 0.0 : static_cast<double>(holdStarts) / static_cast<double>(noteStarts);
    const double activeRatio = targetKeyCount <= 0
                                   ? 0.0
                                   : static_cast<double>(activeHoldLanes.size()) / static_cast<double>(targetKeyCount);
    const bool startHeavy = noteStarts >= kLnHeavyMinStarts && holdStarts >= kLnHeavyMinHoldStarts &&
                            startRatio >= kLnHeavyStartRatio;
    const bool activeHeavy = activeRatio >= kLnHeavyActiveRatio;
    profile.lnHeavy = startHeavy || activeHeavy;
    return profile;
}

const LnAnchor* nearestAnchorForLane(const LnWindowProfile& profile, int lane) {
    const LnAnchor* best = nullptr;
    int bestDistance = std::numeric_limits<int>::max();
    for (const auto& anchor : profile.anchors) {
        const int distance = std::abs(anchor.lane - lane);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = &anchor;
        }
    }
    return best;
}

void applyTrainingScaffold(ExpansionContext& context) {
    auto slices = buildSliceViews(context.chart.notes, context.options.sameTimeEpsilonMs);
    sortExpansionSlices(slices, context);

    for (const auto& slice : slices) {
        if (slice.noteIndices.empty() ||
            slice.noteIndices.size() >= static_cast<std::size_t>(std::max(2, context.options.targetKeyCount / 2))) {
            continue;
        }

        const auto sourceIds = sourceIdsForSlice(slice, context.chart.notes);
        const auto lanes = underusedLaneOrder(context);
        std::vector<ExpansionCandidate> candidates;
        int ordinal = 0;
        const double center = static_cast<double>(std::max(0, context.options.targetKeyCount - 1)) / 2.0;
        for (const int lane : lanes) {
            if (!candidateRespectsEvenSourceHand(slice.noteIndices, context.chart.notes, lane, context.options)) {
                continue;
            }
            ExpansionCandidate candidate;
            candidate.note.time = slice.time;
            candidate.note.lane = lane;
            candidate.note.type = NoteType::Tap;
            candidate.note.sourceLane = lane;
            candidate.ruleName = "training_scaffold";
            candidate.sourceNoteIds = sourceIds;
            candidate.sourceSliceIndex = slice.index;
            candidate.sourceNoteIndex = 0;
            candidate.ordinal = ordinal++;
            candidate.score = 100.0 -
                              static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 2.0 -
                              static_cast<double>(slice.noteIndices.size()) * 0.25;
            candidate.rhythmDriftAbs = 0;
            candidate.snapPriority = 1;
            candidate.laneMovement = static_cast<int>(std::lround(std::abs(static_cast<double>(lane) - center) * 100.0));
            tuneHighKeyGeneratedCandidate(candidate, context);
            candidates.push_back(std::move(candidate));
        }

        std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
        for (auto& candidate : candidates) {
            if (context.tryAdd(candidate)) {
                break;
            }
        }
    }
}

void applyPreserveTapPlus(ExpansionContext& context) {
    const auto baseNotes = context.chart.notes;
    auto slices = buildSliceViews(baseNotes, context.options.sameTimeEpsilonMs);
    sortExpansionSlices(slices, context);

    for (const auto& slice : slices) {
        if (slice.noteIndices.empty() ||
            slice.noteIndices.size() >= static_cast<std::size_t>(std::max(2, context.options.targetKeyCount / 2))) {
            continue;
        }

        const auto sourceIds = sourceIdsForSlice(slice, baseNotes);
        const auto lanes = underusedLaneOrder(context);
        const auto lnProfile = lnWindowProfileFor(slice, baseNotes, context.options.targetKeyCount);
        const int targetChordSize = targetChordSizeForSlice(slice, context.options);
        const int maxAddsForSlice =
            std::max(0,
                     std::min(context.options.maxAddedPerSlice,
                              targetChordSize - static_cast<int>(slice.noteIndices.size())));
        if (maxAddsForSlice <= 0) {
            continue;
        }
        std::vector<ExpansionCandidate> candidates;
        int ordinal = 0;
        const double center = static_cast<double>(std::max(0, context.options.targetKeyCount - 1)) / 2.0;
        const int counterHand = counterHandForSlice(slice, baseNotes, context.options.targetKeyCount);
        for (const int lane : lanes) {
            if (!candidateRespectsEvenSourceHand(slice.noteIndices, baseNotes, lane, context.options)) {
                continue;
            }
            const int laneHand = handForLane(lane, context.options.targetKeyCount);
            const double counterHandScore = counterHand < 0 ? 0.0 : (laneHand == counterHand ? 6.0 : -3.0);
            const LnAnchor* lnAnchor = lnProfile.lnHeavy ? nearestAnchorForLane(lnProfile, lane) : nullptr;
            int lnAnchorDistance = 0;
            if (lnProfile.lnHeavy) {
                if (lnAnchor == nullptr) {
                    continue;
                }
                lnAnchorDistance = std::abs(lane - lnAnchor->lane);
                if (lnAnchorDistance > kLnAddMaxAnchorLaneDistance ||
                    handForLane(lnAnchor->lane, context.options.targetKeyCount) != laneHand) {
                    continue;
                }
            }
            const bool canGenerateHold =
                lnAnchor != nullptr &&
                generatedHoldDurationAllowed(lnAnchor->durationMs,
                                             slice.time,
                                             context.original.timingPoints,
                                             context.options.expansionSnapToleranceMs);
            ExpansionCandidate candidate;
            candidate.note.time = slice.time;
            candidate.note.lane = lane;
            candidate.note.type = canGenerateHold ? NoteType::Hold : NoteType::Tap;
            if (canGenerateHold) {
                candidate.note.endTime = slice.time + lnAnchor->durationMs;
            }
            candidate.note.sourceLane = lane;
            candidate.ruleName = "tap_plus";
            candidate.sourceNoteIds = sourceIds;
            candidate.sourceSliceIndex = slice.index;
            candidate.sourceNoteIndex = 0;
            candidate.ordinal = ordinal++;
            const int localLaneUse =
                localLaneUseAround(context.chart.notes, lane, slice.time, adaptiveBudgetWindowMs(context.options));
            const double vacancyBonus =
                localLaneUse == 0 ? 14.0 : -std::min(12.0, static_cast<double>(localLaneUse) * 4.0);
            candidate.score = 105.0 -
                              static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 2.0 -
                              static_cast<double>(slice.noteIndices.size()) * 0.25 +
                              handBalanceScore(context.laneUse, lane, context.options.targetKeyCount) * 10.0 +
                              counterHandScore +
                              vacancyBonus;
            if (lnAnchor != nullptr) {
                candidate.score += canGenerateHold
                                       ? 12.0 - static_cast<double>(lnAnchorDistance) * 8.0
                                       : 3.0 - static_cast<double>(lnAnchorDistance) * 2.0;
            }
            candidate.rhythmDriftAbs = 0;
            candidate.snapPriority = 1;
            candidate.laneMovement =
                lnAnchor != nullptr
                    ? lnAnchorDistance
                    : static_cast<int>(std::lround(std::abs(static_cast<double>(lane) - center) * 100.0));
            tuneHighKeyGeneratedCandidate(candidate, context);
            candidates.push_back(std::move(candidate));
        }

        std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
        int addedInThisSlice = 0;
        for (auto& candidate : candidates) {
            if (addedInThisSlice >= maxAddsForSlice ||
                context.stats.addedNotes >= context.maxAdded) {
                break;
            }
            if (context.tryAdd(candidate)) {
                ++addedInThisSlice;
            }
        }
    }
}

bool fullFieldEchoSuppressed(PatternKind kind) {
    return kind == PatternKind::Jack || kind == PatternKind::Trill || kind == PatternKind::Chord ||
           kind == PatternKind::AnchorLn || kind == PatternKind::ReleaseLn;
}

std::optional<int> deriveFullFieldEchoLane(const GestureHint* hint,
                                           int primaryLane,
                                           int sliceIndex,
                                           const ConvertOptions& options) {
    const PatternKind kind = hint != nullptr ? hint->kind : PatternKind::Single;
    if (fullFieldEchoSuppressed(kind) || primaryLane < 0 || primaryLane >= options.targetKeyCount ||
        options.targetKeyCount != 10) {
        return std::nullopt;
    }

    const int echoZoneStart = primaryLane < 5 ? 5 : 0;
    const int mirrorLane = options.targetKeyCount - 1 - primaryLane;
    const int phaseStep = options.tenKFullFieldRemixPhaseStep == 3 ? 3 : 2;
    return rotateWithinZone(mirrorLane, sliceIndex, echoZoneStart, 5, phaseStep);
}

void applyFullFieldMirrorRemix(ExpansionContext& context) {
    if (!tenKFullFieldRemixActive(context.options)) {
        return;
    }

    const auto rail = buildFullFieldRail(context.original,
                                         context.options.sourceKeyCount,
                                         context.options.targetKeyCount,
                                         context.options.sameTimeEpsilonMs,
                                         context.options.jackWindowMs,
                                         context.options.gestureRailEnabled);
    auto slices = buildSliceViews(context.chart.notes, context.options.sameTimeEpsilonMs);
    sortExpansionSlices(slices, context);
    int ordinal = 0;
    for (const auto& slice : slices) {
        if (context.stats.addedNotes >= context.maxAdded) {
            break;
        }
        if (slice.noteIndices.size() != 1) {
            continue;
        }
        const auto noteIndex = slice.noteIndices.front();
        if (noteIndex >= context.chart.notes.size()) {
            continue;
        }
        const auto& primary = context.chart.notes[noteIndex];
        const auto* hint = findGestureHint(&rail, primary.id);
        const auto echoLane = deriveFullFieldEchoLane(hint, primary.lane, slice.index, context.options);
        if (!echoLane.has_value()) {
            continue;
        }

        ExpansionCandidate candidate;
        candidate.note.time = primary.time;
        candidate.note.lane = *echoLane;
        candidate.note.type = NoteType::Tap;
        candidate.note.sourceLane = primary.sourceLane.value_or(primary.lane);
        candidate.ruleName = "echo";
        candidate.echoKind = "fullfield";
        candidate.sourceNoteIds = {primary.id};
        candidate.sourcePatternStart = hint != nullptr ? hint->motifId : slice.index;
        candidate.sourceSliceIndex = slice.index;
        candidate.sourceNoteIndex = static_cast<int>(noteIndex);
        candidate.ordinal = ordinal++;
        candidate.score =
            120.0 - static_cast<double>(context.laneUse[static_cast<std::size_t>(*echoLane)]) * 0.35;
        candidate.rhythmDriftAbs = 0;
        candidate.snapPriority = 1;
        candidate.laneMovement = std::abs(primary.lane - *echoLane);
        tuneHighKeyGeneratedCandidate(candidate, context);
        context.tryAdd(candidate);
    }
}

EchoPolicy resolveEchoPolicyForExpansion(const ConvertOptions& options, ExpansionPolicy expansionPolicy) {
    if (expansionPolicy == ExpansionPolicy::DeterministicEcho) {
        return options.echoPolicy == EchoPolicy::Off || options.echoPolicy == EchoPolicy::Auto
                   ? EchoPolicy::StairTrill
                   : options.echoPolicy;
    }
    if (expansionPolicy == ExpansionPolicy::HarderRemix) {
        return options.echoPolicy == EchoPolicy::Off || options.echoPolicy == EchoPolicy::Auto
                   ? EchoPolicy::StairTrill
                   : options.echoPolicy;
    }
    return options.echoPolicy;
}

bool echoAllowsStair(EchoPolicy policy) {
    return policy == EchoPolicy::StairOnly || policy == EchoPolicy::StairTrill ||
           policy == EchoPolicy::StairTrillStream || policy == EchoPolicy::Auto;
}

bool echoAllowsTrill(EchoPolicy policy) {
    return policy == EchoPolicy::TrillOnly || policy == EchoPolicy::StairTrill ||
           policy == EchoPolicy::StairTrillStream || policy == EchoPolicy::Auto;
}

bool echoAllowsStream(EchoPolicy policy) {
    return policy == EchoPolicy::StreamOnly || policy == EchoPolicy::StairTrillStream;
}

std::optional<std::size_t> firstNoteIndex(const TimeSlice& slice) {
    if (slice.noteIndices.empty()) {
        return std::nullopt;
    }
    return slice.noteIndices.front();
}

std::vector<std::string> sourceIdsForPattern(const PatternToken& token,
                                             const std::vector<TimeSlice>& slices,
                                             const std::vector<Note>& notes) {
    std::vector<std::string> ids;
    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                        sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        for (const auto noteIndex : slices[static_cast<std::size_t>(sliceIndex)].noteIndices) {
            ids.push_back(notes[noteIndex].id.empty() ? "anon" + std::to_string(noteIndex) : notes[noteIndex].id);
        }
    }
    std::stable_sort(ids.begin(), ids.end());
    return ids;
}

void addUniqueLane(std::vector<int>& lanes, int lane, int keyCount) {
    if (lane < 0 || lane >= keyCount) {
        return;
    }
    if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) {
        lanes.push_back(lane);
    }
}

std::vector<int> stairHelperLanes(int previousLane, int currentLane, int direction, int keyCount) {
    std::vector<int> lanes;
    const int gap = std::abs(currentLane - previousLane);
    if (gap < 2) {
        return lanes;
    }
    addUniqueLane(lanes, currentLane - direction, keyCount);
    addUniqueLane(lanes, previousLane + direction, keyCount);
    const int midpoint = static_cast<int>(std::lround((static_cast<double>(previousLane) + currentLane) / 2.0));
    addUniqueLane(lanes, midpoint, keyCount);
    return lanes;
}

std::vector<int> trillHelperLanes(int lane, int otherLane, int keyCount) {
    std::vector<int> lanes;
    addUniqueLane(lanes, lane + 1, keyCount);
    addUniqueLane(lanes, lane - 1, keyCount);
    lanes.erase(std::remove(lanes.begin(), lanes.end(), otherLane), lanes.end());
    return lanes;
}

int countNotesInWindow(const std::vector<Note>& notes, int time, int windowMs) {
    const int halfWindow = windowMs / 2;
    const int start = time - halfWindow;
    const int end = time + halfWindow;
    int count = 0;
    for (const auto& note : notes) {
        if (note.time >= start && note.time <= end) {
            ++count;
        }
    }
    return count;
}

double localNpsForWindow(const std::vector<Note>& notes, int time, int windowMs) {
    return static_cast<double>(countNotesInWindow(notes, time, windowMs)) * 1000.0 /
           static_cast<double>(std::max(1, windowMs));
}

double maxLocalNpsForNotes(const std::vector<Note>& notes, int windowMs) {
    double maxNps = 0.0;
    for (const auto& note : notes) {
        maxNps = std::max(maxNps, localNpsForWindow(notes, note.time, windowMs));
    }
    return maxNps;
}

bool exceedsStreamDensityGate(const PatternToken& token,
                              const std::vector<TimeSlice>& slices,
                              ExpansionContext& context,
                              int patternLength) {
    constexpr int shortWindowMs = 500;
    constexpr int midWindowMs = 1000;
    constexpr int longWindowMs = 2000;
    const auto& profile = context.streamProfile;

    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                            sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        const int time = slices[static_cast<std::size_t>(sliceIndex)].time;
        const double shortNps = localNpsForWindow(context.chart.notes, time, shortWindowMs);
        const double midNps = localNpsForWindow(context.chart.notes, time, midWindowMs);
        const double longNps = localNpsForWindow(context.chart.notes, time, longWindowMs);
        if (shortNps > profile.shortWindowNpsLimit || midNps > profile.midWindowNpsLimit ||
            longNps > profile.longWindowNpsLimit || midNps > profile.maxLocalNps) {
            context.stats.rejectedStreamEchoByLocalNps += patternLength;
            context.stats.rejectedStreamPrimaryByLocalNps += patternLength;
            context.stats.rejectedByComposerSafety += patternLength;
            ++context.stats.rejectedEchoByDensity;
            ++context.stats.rejectedEchoCandidates;
            ++context.stats.rejectedExpansionCandidates;
            return true;
        }
    }
    return false;
}

bool isJackHeavyStream(const PatternToken& token,
                       const std::vector<TimeSlice>& slices,
                       const std::vector<Note>& notes) {
    int repeats = 0;
    int transitions = 0;
    std::optional<int> previousLane;
    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                            sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        const auto noteIndex = firstNoteIndex(slices[static_cast<std::size_t>(sliceIndex)]);
        if (!noteIndex.has_value()) {
            continue;
        }
        const int lane = notes[*noteIndex].lane;
        if (previousLane.has_value()) {
            ++transitions;
            if (*previousLane == lane) {
                ++repeats;
            }
        }
        previousLane = lane;
    }
    return transitions > 0 && static_cast<double>(repeats) / static_cast<double>(transitions) >= 0.35;
}

bool isLnHeavyStream(const PatternToken& token,
                     const std::vector<TimeSlice>& slices,
                     int targetKeyCount) {
    const int threshold = std::max(1, static_cast<int>(std::ceil(static_cast<double>(targetKeyCount) * 0.40)));
    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                            sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        if (static_cast<int>(slices[static_cast<std::size_t>(sliceIndex)].activeHolds.size()) >= threshold) {
            return true;
        }
    }
    return false;
}

std::vector<int> streamHelperLanes(const ExpansionContext& context, int currentLane) {
    std::vector<int> lanes = underusedLaneOrder(context);
    lanes.erase(std::remove(lanes.begin(), lanes.end(), currentLane), lanes.end());
    return lanes;
}

bool streamLaneRoleAllowed(int currentLane, int helperLane, int keyCount) {
    const int maxMovement = std::max(2, static_cast<int>(std::ceil(static_cast<double>(keyCount) * 0.60)));
    return std::abs(helperLane - currentLane) <= maxMovement;
}

bool generatedHoldNeedsNeighborDuration(const Note& note) {
    return note.type == NoteType::Hold && note.id.rfind("gen:", 0) == 0;
}

std::optional<int> adjacentShortHoldDuration(const std::vector<Note>& notes,
                                             std::size_t noteIndex,
                                             const std::vector<TimingPoint>& timingPoints) {
    if (noteIndex >= notes.size()) {
        return std::nullopt;
    }
    const auto& target = notes[noteIndex];
    std::optional<int> bestDuration;
    int bestLaneDistance = std::numeric_limits<int>::max();
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (i == noteIndex) {
            continue;
        }
        const auto& neighbor = notes[i];
        if (neighbor.type != NoteType::Hold || !neighbor.endTime.has_value() ||
            *neighbor.endTime <= neighbor.time || neighbor.time != target.time) {
            continue;
        }
        const int duration = *neighbor.endTime - neighbor.time;
        if (!generatedHoldDurationAllowed(duration, target.time, timingPoints)) {
            continue;
        }
        const int laneDistance = std::abs(neighbor.lane - target.lane);
        if (laneDistance < bestLaneDistance) {
            bestLaneDistance = laneDistance;
            bestDuration = duration;
        }
    }
    return bestDuration;
}

void tapifyGeneratedHold(Note& note) {
    note.type = NoteType::Tap;
    note.endTime.reset();
}

void normalizeGeneratedHoldDurations(Chart& chart) {
    for (std::size_t i = 0; i < chart.notes.size(); ++i) {
        auto& note = chart.notes[i];
        if (!generatedHoldNeedsNeighborDuration(note)) {
            continue;
        }

        const int currentDuration =
            note.endTime.has_value() && *note.endTime > note.time ? *note.endTime - note.time : 0;
        const auto duration = adjacentShortHoldDuration(chart.notes, i, chart.timingPoints);
        if (duration.has_value() && *duration > 0) {
            note.endTime = note.time + *duration;
            continue;
        }
        if (!generatedHoldDurationAllowed(currentDuration, note.time, chart.timingPoints)) {
            tapifyGeneratedHold(note);
        }
    }
}

void applyStairEcho(const PatternToken& token,
                    const std::vector<TimeSlice>& slices,
                    ExpansionContext& context) {
    if (token.confidence < context.options.minPatternConfidence ||
        token.endSlice - token.startSlice + 1 < context.options.minEchoPatternLength) {
        return;
    }
    const int direction = token.kind == PatternKind::StairUp ? 1 : -1;
    const auto sourceIds = sourceIdsForPattern(token, slices, context.chart.notes);
    const std::string echoKind = toString(token.kind);
    const std::string patternKey = echoKind + ":" + std::to_string(token.startSlice);
    std::vector<ExpansionCandidate> candidates;
    int ordinal = 0;

    for (int sliceIndex = token.startSlice + 1; sliceIndex <= token.endSlice &&
                                                sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex <= 0) {
            continue;
        }
        const auto prevNoteIndex = firstNoteIndex(slices[static_cast<std::size_t>(sliceIndex - 1)]);
        const auto currNoteIndex = firstNoteIndex(slices[static_cast<std::size_t>(sliceIndex)]);
        if (!prevNoteIndex.has_value() || !currNoteIndex.has_value()) {
            continue;
        }
        const auto& prev = context.chart.notes[*prevNoteIndex];
        const auto& curr = context.chart.notes[*currNoteIndex];
        if ((curr.lane > prev.lane ? 1 : -1) != direction) {
            continue;
        }

        const auto helperLanes = stairHelperLanes(prev.lane, curr.lane, direction, context.options.targetKeyCount);
        for (const int lane : helperLanes) {
            if (!candidateRespectsEvenSourceHand(slices[static_cast<std::size_t>(sliceIndex)].noteIndices,
                                                 context.chart.notes,
                                                 lane,
                                                 context.options)) {
                continue;
            }
            ExpansionCandidate candidate;
            candidate.note.time = curr.time;
            candidate.note.lane = lane;
            candidate.note.type = NoteType::Tap;
            candidate.note.sourceLane = lane;
            candidate.ruleName = "echo";
            candidate.echoKind = echoKind;
            candidate.sourceNoteIds = sourceIds;
            candidate.sourcePatternStart = token.startSlice;
            candidate.sourceSliceIndex = sliceIndex;
            candidate.sourceNoteIndex = static_cast<int>(*currNoteIndex);
            candidate.ordinal = ordinal++;
            candidate.score = 120.0 - std::abs(curr.lane - lane) * 0.5 -
                              static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 0.2;
            candidate.rhythmDriftAbs = 0;
            candidate.snapPriority = 1;
            candidate.laneMovement = std::abs(curr.lane - lane);
            tuneHighKeyGeneratedCandidate(candidate, context);
            candidates.push_back(std::move(candidate));
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
    for (auto& candidate : candidates) {
        context.tryAddEcho(candidate, patternKey);
    }
}

void applyTrillEcho(const PatternToken& token,
                    const std::vector<TimeSlice>& slices,
                    ExpansionContext& context) {
    if (token.confidence < std::max(0.75, context.options.minPatternConfidence) ||
        token.endSlice - token.startSlice + 1 < std::max(4, context.options.minEchoPatternLength)) {
        return;
    }
    if (token.startSlice < 0 || token.startSlice + 1 >= static_cast<int>(slices.size())) {
        return;
    }
    const auto first = firstNoteIndex(slices[static_cast<std::size_t>(token.startSlice)]);
    const auto second = firstNoteIndex(slices[static_cast<std::size_t>(token.startSlice + 1)]);
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    const int targetA = context.chart.notes[*first].lane;
    const int targetB = context.chart.notes[*second].lane;

    const auto sourceIds = sourceIdsForPattern(token, slices, context.chart.notes);
    const std::string patternKey = std::string("trill:") + std::to_string(token.startSlice);
    std::vector<ExpansionCandidate> candidates;
    int ordinal = 0;

    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                            sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        const auto noteIndex = firstNoteIndex(slices[static_cast<std::size_t>(sliceIndex)]);
        if (!noteIndex.has_value()) {
            continue;
        }
        const auto& note = context.chart.notes[*noteIndex];
        const int otherLane = note.lane == targetA ? targetB : targetA;
        const auto helperLanes = trillHelperLanes(note.lane, otherLane, context.options.targetKeyCount);
        for (const int lane : helperLanes) {
            if (!candidateRespectsEvenSourceHand(slices[static_cast<std::size_t>(sliceIndex)].noteIndices,
                                                 context.chart.notes,
                                                 lane,
                                                 context.options)) {
                continue;
            }
            ExpansionCandidate candidate;
            candidate.note.time = note.time;
            candidate.note.lane = lane;
            candidate.note.type = NoteType::Tap;
            candidate.note.sourceLane = lane;
            candidate.ruleName = "echo";
            candidate.echoKind = "trill";
            candidate.sourceNoteIds = sourceIds;
            candidate.sourcePatternStart = token.startSlice;
            candidate.sourceSliceIndex = sliceIndex;
            candidate.sourceNoteIndex = static_cast<int>(*noteIndex);
            candidate.ordinal = ordinal++;
            candidate.score = 110.0 - std::abs(note.lane - lane) * 0.5 -
                              static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 0.25;
            candidate.rhythmDriftAbs = 0;
            candidate.snapPriority = 1;
            candidate.laneMovement = std::abs(note.lane - lane);
            tuneHighKeyGeneratedCandidate(candidate, context);
            candidates.push_back(std::move(candidate));
        }
    }

    std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
    for (auto& candidate : candidates) {
        context.tryAddEcho(candidate, patternKey);
    }
}

void applyStreamEcho(const PatternToken& token,
                     const std::vector<TimeSlice>& slices,
                     ExpansionContext& context) {
    const int patternLength = std::max(1, token.endSlice - token.startSlice + 1);
    context.stats.streamEchoCandidates += patternLength;
    context.stats.streamRawPatternCandidates += patternLength;

    if (token.kind == PatternKind::Burst) {
        context.stats.rejectedStreamEchoByBurst += patternLength;
        context.stats.rejectedStreamPrimaryByBurst += patternLength;
        ++context.stats.rejectedEchoCandidates;
        ++context.stats.rejectedExpansionCandidates;
        return;
    }
    if (token.kind != PatternKind::Stream) {
        return;
    }
    if (token.confidence < context.options.minPatternConfidence) {
        context.stats.rejectedStreamEchoByPatternConfidence += patternLength;
        context.stats.rejectedStreamPrimaryByPatternConfidence += patternLength;
        ++context.stats.rejectedEchoCandidates;
        ++context.stats.rejectedExpansionCandidates;
        return;
    }
    if (patternLength < std::max(5, context.options.minEchoPatternLength)) {
        context.stats.rejectedStreamEchoByPatternLength += patternLength;
        context.stats.rejectedStreamPrimaryByPatternLength += patternLength;
        ++context.stats.rejectedEchoCandidates;
        ++context.stats.rejectedExpansionCandidates;
        return;
    }
    if (exceedsStreamDensityGate(token, slices, context, patternLength)) {
        return;
    }
    if (isJackHeavyStream(token, slices, context.chart.notes)) {
        context.stats.rejectedStreamEchoByJack += patternLength;
        context.stats.rejectedStreamPrimaryByJack += patternLength;
        ++context.stats.rejectedEchoCandidates;
        ++context.stats.rejectedExpansionCandidates;
        return;
    }
    if (isLnHeavyStream(token, slices, context.options.targetKeyCount)) {
        context.stats.rejectedStreamEchoByLNHeavy += patternLength;
        context.stats.rejectedStreamPrimaryByLNHeavy += patternLength;
        ++context.stats.rejectedEchoCandidates;
        ++context.stats.rejectedExpansionCandidates;
        return;
    }

    context.stats.streamEligiblePatternCandidates += patternLength;
    const auto sourceIds = sourceIdsForPattern(token, slices, context.chart.notes);
    const std::string patternKey = std::string("stream:") + std::to_string(token.startSlice);
    std::vector<ExpansionCandidate> candidates;
    int ordinal = 0;

    for (int sliceIndex = token.startSlice; sliceIndex <= token.endSlice &&
                                            sliceIndex < static_cast<int>(slices.size());
         ++sliceIndex) {
        if (sliceIndex < 0) {
            continue;
        }
        const auto noteIndex = firstNoteIndex(slices[static_cast<std::size_t>(sliceIndex)]);
        if (!noteIndex.has_value()) {
            continue;
        }
        if (slices[static_cast<std::size_t>(sliceIndex)].noteIndices.size() >=
            static_cast<std::size_t>(context.options.targetKeyCount)) {
            ++context.stats.rejectedStreamEchoBySliceChordFull;
            ++context.stats.rejectedStreamPrimaryBySliceChordFull;
            continue;
        }
        const auto& note = context.chart.notes[*noteIndex];
        const auto helperLanes = streamHelperLanes(context, note.lane);
        if (helperLanes.empty()) {
            ++context.stats.rejectedStreamEchoByNoUnderusedLane;
            ++context.stats.rejectedStreamPrimaryByNoUnderusedLane;
            continue;
        }
        bool sawRoleLane = false;
        for (const int lane : helperLanes) {
            ++context.stats.streamRawLaneCandidates;
            if (!streamLaneRoleAllowed(note.lane, lane, context.options.targetKeyCount)) {
                ++context.stats.rejectedStreamEchoByLaneRole;
                ++context.stats.rejectedStreamPrimaryByLaneRole;
                continue;
            }
            if (!candidateRespectsEvenSourceHand(slices[static_cast<std::size_t>(sliceIndex)].noteIndices,
                                                 context.chart.notes,
                                                 lane,
                                                 context.options)) {
                ++context.stats.rejectedStreamEchoByLaneRole;
                ++context.stats.rejectedStreamPrimaryByLaneRole;
                continue;
            }
            sawRoleLane = true;
            ExpansionCandidate candidate;
            candidate.note.time = note.time;
            candidate.note.lane = lane;
            candidate.note.type = NoteType::Tap;
            candidate.note.sourceLane = lane;
            candidate.ruleName = "echo";
            candidate.echoKind = "stream";
            candidate.sourceNoteIds = sourceIds;
            candidate.sourcePatternStart = token.startSlice;
            candidate.sourceSliceIndex = sliceIndex;
            candidate.sourceNoteIndex = static_cast<int>(*noteIndex);
            candidate.ordinal = ordinal++;
            candidate.score = 85.0 -
                              static_cast<double>(context.laneUse[static_cast<std::size_t>(lane)]) * 2.0 -
                              std::abs(note.lane - lane) * 0.15 -
                              localNpsForWindow(context.chart.notes, note.time, 1000) * 0.5;
            candidate.rhythmDriftAbs = 0;
            candidate.snapPriority = 1;
            candidate.laneMovement = std::abs(note.lane - lane);
            tuneHighKeyGeneratedCandidate(candidate, context);
            candidates.push_back(std::move(candidate));
        }
        (void)sawRoleLane;
    }

    std::stable_sort(candidates.begin(), candidates.end(), candidateLess);
    for (auto& candidate : candidates) {
        context.tryAddEcho(candidate, patternKey);
    }
}

void applyDeterministicEcho(ExpansionContext& context, EchoPolicy echoPolicy) {
    if (echoPolicy == EchoPolicy::Off) {
        return;
    }

    const auto slices = buildTimeSlices(context.chart, context.options.targetKeyCount, context.options.sameTimeEpsilonMs);
    auto tokens = detectPatternTokens(slices, context.options.jackWindowMs);
    std::stable_sort(tokens.begin(), tokens.end(), [](const PatternToken& lhs, const PatternToken& rhs) {
        if (lhs.startSlice != rhs.startSlice) {
            return lhs.startSlice < rhs.startSlice;
        }
        if (lhs.endSlice != rhs.endSlice) {
            return lhs.endSlice < rhs.endSlice;
        }
        return toString(lhs.kind) < toString(rhs.kind);
    });

    for (const auto& token : tokens) {
        if (echoAllowsStair(echoPolicy) &&
            (token.kind == PatternKind::StairUp || token.kind == PatternKind::StairDown)) {
            applyStairEcho(token, slices, context);
        } else if (echoAllowsTrill(echoPolicy) && token.kind == PatternKind::Trill) {
            applyTrillEcho(token, slices, context);
        } else if (echoAllowsStream(echoPolicy) &&
                   (token.kind == PatternKind::Stream || token.kind == PatternKind::Burst)) {
            applyStreamEcho(token, slices, context);
        }
    }
}

void finishStats(ExpansionContext& context) {
    normalizeGeneratedHoldDurations(context.chart);
    context.chart.notes = sortedNotes(std::move(context.chart.notes));
    context.stats.streamEchoCandidates = context.stats.streamRawPatternCandidates;
    context.stats.streamAcceptedCandidates = context.stats.addedByStreamEcho;
    context.stats.addedNoteRatio = context.original.notes.empty()
                                       ? 0.0
                                       : static_cast<double>(context.stats.addedNotes) /
                                             static_cast<double>(context.original.notes.size());
    context.stats.budgetUsedRatio = context.stats.targetAddedNoteRatio <= 0.0
                                        ? 0.0
                                        : context.stats.addedNoteRatio / context.stats.targetAddedNoteRatio;
    context.stats.echoAddedRatio = context.original.notes.empty()
                                       ? 0.0
                                       : static_cast<double>(context.stats.addedByEcho) /
                                             static_cast<double>(context.original.notes.size());
    context.stats.streamEchoAddedRatio = context.original.notes.empty()
                                             ? 0.0
                                             : static_cast<double>(context.stats.addedByStreamEcho) /
                                                   static_cast<double>(context.original.notes.size());
    context.stats.maxObservedLocalNpsAfterEcho = maxLocalNpsForNotes(context.chart.notes, 1000);

    for (const auto& info : context.stats.generatedNotes) {
        if (!isSnapAligned(static_cast<int>(std::lround(info.generatedTimeMs)),
                           context.original.timingPoints,
                           context.options.expansionSnapToleranceMs)) {
            ++context.stats.unsnappedAddedNotes;
        }
    }
}

}  // namespace

ExpansionPolicy resolveExpansionPolicy(const ConvertOptions& options) {
    return effectiveExpansionPolicy(options);
}

void applyExpansionComposer(ExpansionContext& context, ExpansionPolicy policy) {
    if (tenKFullFieldRemixActive(context.options)) {
        applyFullFieldMirrorRemix(context);
        return;
    }

    if (policy == ExpansionPolicy::PreserveTapPlusMore ||
        policy == ExpansionPolicy::PreserveTapPlus ||
        policy == ExpansionPolicy::PreserveTapPlusLow) {
        applyPreserveTapPlus(context);
        return;
    }

    if (policy == ExpansionPolicy::DeterministicEcho) {
        applyDeterministicEcho(context, resolveEchoPolicyForExpansion(context.options, policy));
        return;
    }

    if (policy == ExpansionPolicy::DeterministicChordFill) {
        applyChordFill(context);
        return;
    }

    if (policy == ExpansionPolicy::TrainingScaffold) {
        applyTrainingScaffold(context);
        if (context.options.echoPolicy != EchoPolicy::Off) {
            applyDeterministicEcho(context, resolveEchoPolicyForExpansion(context.options, policy));
        }
        return;
    }

    if (policy == ExpansionPolicy::HarderRemix) {
        const int totalMaxAdded = context.maxAdded;
        auto applyWithStageLimit = [&](double fraction, auto applyStage) {
            const int savedMaxAdded = context.maxAdded;
            int stageLimit = static_cast<int>(std::floor(static_cast<double>(totalMaxAdded) * fraction));
            if (totalMaxAdded > context.stats.addedNotes && stageLimit <= context.stats.addedNotes) {
                stageLimit = context.stats.addedNotes + 1;
            }
            context.maxAdded = std::min(savedMaxAdded, std::max(0, stageLimit));
            applyStage();
            context.maxAdded = savedMaxAdded;
        };

        applyWithStageLimit(0.25, [&]() {
            applyDeterministicEcho(context, resolveEchoPolicyForExpansion(context.options, policy));
        });
        applyWithStageLimit(0.70, [&]() {
            applyChordFill(context);
        });
        context.maxAdded = totalMaxAdded;
        applyTrainingScaffold(context);
        return;
    }
}

ExpansionPlanStats applyExpansionPlanner(Chart& converted,
                                         const Chart& original,
                                         const ConvertOptions& options) {
    ExpansionContext context(converted, original, options);
    const auto policy = resolveExpansionPolicy(options);
    context.stats.policy = policy;
    context.stats.streamEchoProfile = options.streamEchoProfile;
    context.stats.deterministic = options.deterministicExpansion &&
                                  (tenKFullFieldRemixActive(options) ||
                                   policy != ExpansionPolicy::SeededRandomRemix);

    if (!options.deterministicExpansion) {
        context.stats.warnings.push_back(
            "Warning: non-deterministic expansion is disabled; using deterministic planner behavior.");
        context.stats.deterministic = true;
    }

    if (policy == ExpansionPolicy::PreserveNoteCount && !tenKFullFieldRemixActive(options)) {
        finishStats(context);
        return context.stats;
    }

    if (policy == ExpansionPolicy::SeededRandomRemix && !tenKFullFieldRemixActive(options)) {
        context.stats.warnings.push_back(
            "Warning: seeded-random expansion is reserved but not implemented in this version; no random notes added.");
        finishStats(context);
        return context.stats;
    }

    applyExpansionComposer(context, policy);
    finishStats(context);
    return context.stats;
}

}  // namespace keyconv
