#include "core/assignment.hpp"

#include "core/collision.hpp"
#include "core/mapping.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace keyconv {

namespace {

constexpr int kSourceLaneAnchorWindowMs = 4000;

double normalizedLane(int lane, int keyCount) {
    if (keyCount <= 1) {
        return 0.5;
    }
    return static_cast<double>(lane) / static_cast<double>(keyCount - 1);
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

void addUnique(std::vector<int>& values, int value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

void addGestureCandidates(LaneCandidateSet& set, const GestureHint* hint, int targetKeyCount) {
    if (hint == nullptr || targetKeyCount <= 0) {
        return;
    }
    set.hasPreferredZone = true;
    set.preferredZoneStart = clampInt(hint->zoneStart, 0, targetKeyCount - 1);
    set.preferredZoneEnd = clampInt(hint->zoneEnd, 0, targetKeyCount - 1);
    if (set.preferredZoneStart > set.preferredZoneEnd) {
        std::swap(set.preferredZoneStart, set.preferredZoneEnd);
    }
    addUnique(set.candidates, clampInt(hint->preferredLane, 0, targetKeyCount - 1));
    if (hint->kind == PatternKind::Trill || hint->kind == PatternKind::Jack) {
        for (int lane = hint->zoneStart; lane <= hint->zoneEnd; ++lane) {
            addUnique(set.candidates, clampInt(lane, 0, targetKeyCount - 1));
        }
    } else {
        addUnique(set.candidates, clampInt(hint->preferredLane - 1, 0, targetKeyCount - 1));
        addUnique(set.candidates, clampInt(hint->preferredLane + 1, 0, targetKeyCount - 1));
    }
}

bool candidateInPreferredZone(const LaneCandidateSet& set, int lane) {
    return !set.hasPreferredZone || (lane >= set.preferredZoneStart && lane <= set.preferredZoneEnd);
}

void keepPreferredZoneCandidates(LaneCandidateSet& set) {
    if (!set.hasPreferredZone) {
        return;
    }

    std::vector<int> filtered;
    for (const int lane : set.candidates) {
        if (candidateInPreferredZone(set, lane)) {
            addUnique(filtered, lane);
        }
    }
    if (!filtered.empty()) {
        set.candidates = std::move(filtered);
    }
}

double recentJackPenalty(const std::vector<Note>& placed, int time, int lane, int thresholdMs) {
    double penalty = 0.0;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > thresholdMs) {
            break;
        }
        if (dt > 0 && it->lane == lane) {
            penalty += 1.0 - static_cast<double>(dt) / static_cast<double>(thresholdMs);
        }
    }
    return penalty;
}

double unwantedCreatedJackPenalty(const std::vector<Note>& placed,
                                  int sourceLane,
                                  int time,
                                  int targetLane,
                                  int thresholdMs) {
    double penalty = 0.0;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > thresholdMs) {
            break;
        }
        if (dt > 0 && it->lane == targetLane && it->sourceLane.value_or(it->lane) != sourceLane) {
            penalty += 1.0 - static_cast<double>(dt) / static_cast<double>(thresholdMs);
        }
    }
    return penalty;
}

bool createsSourceDifferentRepeat(const std::vector<Note>& placed,
                                  int sourceLane,
                                  int time,
                                  int targetLane,
                                  int thresholdMs) {
    const int window = std::max(0, thresholdMs);
    if (window <= 0) {
        return false;
    }

    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > window) {
            break;
        }
        if (dt > 0 && it->lane == targetLane && it->sourceLane.value_or(it->lane) != sourceLane) {
            return true;
        }
    }
    return false;
}

std::optional<Note> lastPlacedBefore(const std::vector<Note>& placed, int time) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        if (it->time < time) {
            return *it;
        }
    }
    return std::nullopt;
}

std::optional<Note> recentSameSourceLane(const std::vector<Note>& placed,
                                         int sourceLane,
                                         int time,
                                         int thresholdMs) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > thresholdMs) {
            break;
        }
        if (dt > 0 && it->sourceLane.value_or(it->lane) == sourceLane) {
            return *it;
        }
    }
    return std::nullopt;
}

std::set<int> recentSameSourceTargetLanes(const std::vector<Note>& placed,
                                          int sourceLane,
                                          int time,
                                          int thresholdMs) {
    std::set<int> lanes;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > thresholdMs) {
            break;
        }
        if (dt > 0 && it->sourceLane.value_or(it->lane) == sourceLane) {
            lanes.insert(it->lane);
        }
    }
    return lanes;
}

std::optional<Note> previousInMotif(const std::vector<Note>& placed,
                                    const GestureRail* rail,
                                    int motifId,
                                    int time) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        if (it->time >= time) {
            continue;
        }
        const auto* previousHint = findGestureHint(rail, it->id);
        if (previousHint != nullptr && previousHint->motifId == motifId) {
            return *it;
        }
    }
    return std::nullopt;
}

bool useDualFiveSplit(int sourceKeyCount, int targetKeyCount) {
    return sourceKeyCount == 7 && targetKeyCount == 10;
}

std::pair<int, int> dualFiveZoneForSource(int sourceLane) {
    return sourceLane <= 3 ? std::pair<int, int>{0, 4} : std::pair<int, int>{5, 9};
}

int dualFiveBaseLaneForSource(int sourceLane, int targetKeyCount) {
    const auto [zoneStart, zoneEnd] = dualFiveZoneForSource(sourceLane);
    const bool leftZone = zoneStart < 5;
    const int sourceMin = leftZone ? 0 : 3;
    const int sourceMax = leftZone ? 3 : 6;
    const double t = static_cast<double>(clampInt(sourceLane, sourceMin, sourceMax) - sourceMin) /
                     static_cast<double>(std::max(1, sourceMax - sourceMin));
    const int lane = static_cast<int>(std::lround(static_cast<double>(zoneStart) +
                                                  t * static_cast<double>(zoneEnd - zoneStart)));
    return clampInt(lane, 0, targetKeyCount - 1);
}

std::pair<int, int> balanceZoneForSource(int sourceLane, int sourceKeyCount, int targetKeyCount) {
    if (targetKeyCount <= 1) {
        return {0, 0};
    }
    if (useDualFiveSplit(sourceKeyCount, targetKeyCount)) {
        return dualFiveZoneForSource(sourceLane);
    }
    if (sourceKeyCount <= 1) {
        return {0, targetKeyCount - 1};
    }

    const double sourceMid = static_cast<double>(sourceKeyCount - 1) / 2.0;
    const int targetMid = std::max(1, targetKeyCount / 2);
    if (static_cast<double>(sourceLane) < sourceMid - 0.25) {
        return {0, targetMid - 1};
    }
    if (static_cast<double>(sourceLane) > sourceMid + 0.25) {
        return {targetMid, targetKeyCount - 1};
    }
    return {0, targetKeyCount - 1};
}

int handBoundary(int targetKeyCount) {
    return std::max(1, targetKeyCount / 2);
}

int handForLane(int lane, int targetKeyCount) {
    return lane < handBoundary(targetKeyCount) ? 0 : 1;
}

double dualFivePanelScore(int sourceLane,
                          int targetLane,
                          const GestureHint* hint,
                          const AssignmentContext& context) {
    if (!useDualFiveSplit(context.sourceKeyCount, context.targetKeyCount)) {
        return 0.0;
    }

    int zoneStart = 0;
    int zoneEnd = 0;
    if (hint != nullptr &&
        (hint->role == PhraseRole::LeftHandVoice || hint->role == PhraseRole::RightHandVoice)) {
        zoneStart = hint->zoneStart;
        zoneEnd = hint->zoneEnd;
    } else {
        const auto zone = dualFiveZoneForSource(sourceLane);
        zoneStart = zone.first;
        zoneEnd = zone.second;
    }
    return targetLane >= zoneStart && targetLane <= zoneEnd ? context.weights.shape * 0.5
                                                            : -context.weights.shape * 4.0;
}

std::optional<int> recentSourceLaneAnchor(const std::vector<Note>& placed,
                                          int sourceLane,
                                          int time,
                                          int targetKeyCount) {
    if (targetKeyCount <= 0) {
        return std::nullopt;
    }

    std::vector<int> counts(static_cast<std::size_t>(targetKeyCount), 0);
    std::vector<int> latestTimes(static_cast<std::size_t>(targetKeyCount), -1);
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt > kSourceLaneAnchorWindowMs) {
            break;
        }
        if (dt <= 0 || it->sourceLane.value_or(it->lane) != sourceLane ||
            it->lane < 0 || it->lane >= targetKeyCount) {
            continue;
        }
        ++counts[static_cast<std::size_t>(it->lane)];
        latestTimes[static_cast<std::size_t>(it->lane)] =
            std::max(latestTimes[static_cast<std::size_t>(it->lane)], it->time);
    }

    int bestLane = -1;
    int bestCount = 0;
    int bestLatestTime = -1;
    for (int lane = 0; lane < targetKeyCount; ++lane) {
        const int count = counts[static_cast<std::size_t>(lane)];
        const int latestTime = latestTimes[static_cast<std::size_t>(lane)];
        if (count > bestCount || (count == bestCount && latestTime > bestLatestTime)) {
            bestLane = lane;
            bestCount = count;
            bestLatestTime = latestTime;
        }
    }

    if (bestLane < 0 || bestCount == 0) {
        return std::nullopt;
    }
    return bestLane;
}

double sourceLaneAnchorScore(int targetLane, int anchorLane, int targetKeyCount) {
    if (targetLane == anchorLane) {
        return 1.0;
    }

    const int delta = std::abs(targetLane - anchorLane);
    if (handForLane(targetLane, targetKeyCount) == handForLane(anchorLane, targetKeyCount) && delta == 1) {
        return 0.25;
    }
    return -std::min(1.0, static_cast<double>(delta) / 3.0);
}

bool roleHasHandVoice(PhraseRole role) {
    return role == PhraseRole::LeftHandVoice || role == PhraseRole::RightHandVoice;
}

int expectedRoleHand(PhraseRole role) {
    return role == PhraseRole::RightHandVoice ? 1 : 0;
}

double roleVoiceLeadingScore(const Note& previous,
                             const GestureHint* previousHint,
                             const Note& source,
                             int targetLane,
                             const GestureHint& hint,
                             const AssignmentContext& context) {
    if (!useDualFiveSplit(context.sourceKeyCount, context.targetKeyCount) || !roleHasHandVoice(hint.role) ||
        hint.kind == PatternKind::Jack || previousHint == nullptr || previousHint->role != hint.role) {
        return 0.0;
    }

    const int roleHand = expectedRoleHand(hint.role);
    if (handForLane(targetLane, context.targetKeyCount) != roleHand ||
        handForLane(previous.lane, context.targetKeyCount) != roleHand) {
        return 0.0;
    }

    const int sourceLane = source.sourceLane.value_or(source.lane);
    const int previousSourceLane = previous.sourceLane.value_or(previous.lane);
    const int sourceDelta = std::abs(sourceLane - previousSourceLane);
    const int targetDelta = std::abs(targetLane - previous.lane);
    const int sourceSign = signOf(sourceLane - previousSourceLane);
    const int targetSign = signOf(targetLane - previous.lane);
    if (sourceSign != 0) {
        if (targetSign == 0) {
            return 0.0;
        }
        if (targetSign != sourceSign) {
            return 0.0;
        }
    }

    const int expectedDelta = std::max(1, sourceDelta + 1);
    if (targetDelta <= expectedDelta) {
        return context.weights.gesture * 0.08 *
               (1.0 - std::min(1.0, static_cast<double>(targetDelta) / 5.0));
    }

    return 0.0;
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

double handBalanceScore(const std::vector<int>& laneUse, int targetLane, int targetKeyCount) {
    if (targetKeyCount <= 1 || targetLane < 0 || targetLane >= targetKeyCount ||
        laneUse.size() != static_cast<std::size_t>(targetKeyCount)) {
        return 0.0;
    }

    const auto [leftUse, rightUse] = handUseCounts(laneUse, targetKeyCount);
    const int laneHand = handForLane(targetLane, targetKeyCount);
    const int currentHandUse = laneHand == 0 ? leftUse : rightUse;
    const int otherHandUse = laneHand == 0 ? rightUse : leftUse;
    const double normalizer = std::max(1.0, (static_cast<double>(leftUse + rightUse) / 2.0));
    return std::max(-1.0, std::min(1.0, static_cast<double>(otherHandUse - currentHandUse) / normalizer));
}

double expandedLaneBalanceScore(const std::vector<int>& laneUse,
                                int sourceLane,
                                int sourceKeyCount,
                                int targetLane,
                                int targetKeyCount) {
    if (targetKeyCount <= sourceKeyCount || targetLane < 0 || targetLane >= targetKeyCount ||
        laneUse.size() != static_cast<std::size_t>(targetKeyCount)) {
        return 0.0;
    }

    const auto [zoneStart, zoneEnd] = balanceZoneForSource(sourceLane, sourceKeyCount, targetKeyCount);
    if (zoneStart > zoneEnd) {
        return 0.0;
    }

    int total = 0;
    for (int lane = zoneStart; lane <= zoneEnd; ++lane) {
        total += laneUse[static_cast<std::size_t>(lane)];
    }
    const int zoneSize = std::max(1, zoneEnd - zoneStart + 1);
    const double averageUse = static_cast<double>(total) / static_cast<double>(zoneSize);
    const double laneUseValue = static_cast<double>(laneUse[static_cast<std::size_t>(targetLane)]);
    const double normalizer = std::max(1.0, averageUse);
    return std::max(-1.0, std::min(1.0, (averageUse - laneUseValue) / normalizer));
}

double gestureScore(const Note& source,
                    int targetLane,
                    const GestureHint* hint,
                    const AssignmentContext& context) {
    if (hint == nullptr) {
        return 0.0;
    }

    double score = 0.0;
    const int preferredDelta = std::abs(targetLane - hint->preferredLane);
    if (hint->kind == PatternKind::Jack &&
        (context.style == ConversionStyle::Playable || context.style == ConversionStyle::Training)) {
        score += (targetLane >= hint->zoneStart && targetLane <= hint->zoneEnd) ? context.weights.gesture * 0.25
                                                                                : -context.weights.gesture;
    } else {
        score += context.weights.gesture * std::max(0.0, 1.0 - static_cast<double>(preferredDelta) / 3.0);
    }
    if (targetLane < hint->zoneStart || targetLane > hint->zoneEnd) {
        score -= context.weights.gesture * 1.5;
    }

    const auto previous = previousInMotif(context.placed, context.gestureRail, hint->motifId, source.time);
    if (!previous.has_value()) {
        return score;
    }

    const auto* previousHint = findGestureHint(context.gestureRail, previous->id);
    const int sourceLane = source.sourceLane.value_or(source.lane);
    const int previousSourceLane = previous->sourceLane.value_or(previous->lane);
    const int sourceSign = signOf(sourceLane - previousSourceLane);
    const int targetSign = signOf(targetLane - previous->lane);
    score += roleVoiceLeadingScore(*previous, previousHint, source, targetLane, *hint, context);

    if (hint->kind == PatternKind::StairUp || hint->kind == PatternKind::StairDown) {
        if (sourceSign != 0) {
            if (targetSign == sourceSign) {
                score += context.weights.gesture * 1.25;
            } else if (targetSign == 0) {
                score -= context.weights.gesture * 0.5;
            } else {
                score -= context.weights.gesture * 2.0;
            }
        }
    } else if (hint->kind == PatternKind::Trill) {
        if (sourceSign != 0) {
            score += targetLane != previous->lane ? context.weights.gesture : -context.weights.gesture * 1.5;
        } else {
            score += targetLane == previous->lane ? context.weights.gesture * 0.5 : -context.weights.gesture;
        }
        if (previousHint != nullptr) {
            const bool onRail = targetLane == hint->preferredLane || targetLane == previousHint->preferredLane;
            score += onRail ? context.weights.gesture * 0.5 : -context.weights.gesture;
        }
    } else if (hint->kind == PatternKind::Jack) {
        const int laneDelta = std::abs(targetLane - previous->lane);
        if (context.style == ConversionStyle::Faithful) {
            score += laneDelta == 0 ? context.weights.gesture : -context.weights.gesture * 0.5;
        } else if (laneDelta == 1) {
            score += context.weights.gesture * 1.25;
        } else if (laneDelta == 0) {
            score -= context.weights.gesture * 0.25;
        } else {
            score -= context.weights.gesture * 1.5;
        }
    }

    return score;
}

std::vector<int> noCreatedJackCandidates(const LaneCandidateSet& baseSet,
                                         const Note& note,
                                         const AssignmentContext& context) {
    const int sourceLane = note.sourceLane.value_or(note.lane);
    const int window = std::max(0, context.jackWindowMs);
    if (window <= 0) {
        return baseSet.candidates;
    }

    std::vector<int> safe;
    for (const int lane : baseSet.candidates) {
        if (!createsSourceDifferentRepeat(context.placed, sourceLane, note.time, lane, window)) {
            addUnique(safe, lane);
        } else if (context.preventedJacksByAssignment != nullptr) {
            ++*context.preventedJacksByAssignment;
        }
    }
    if (!safe.empty()) {
        return safe;
    }

    for (int distance = 0; distance < context.targetKeyCount; ++distance) {
        const int left = baseSet.baseLane - distance;
        const int right = baseSet.baseLane + distance;
        if (left >= 0 && left < context.targetKeyCount &&
            candidateInPreferredZone(baseSet, left) &&
            !createsSourceDifferentRepeat(context.placed, sourceLane, note.time, left, window)) {
            addUnique(safe, left);
        }
        if (right >= 0 && right < context.targetKeyCount && right != left &&
            candidateInPreferredZone(baseSet, right) &&
            !createsSourceDifferentRepeat(context.placed, sourceLane, note.time, right, window)) {
            addUnique(safe, right);
        }
        if (!safe.empty()) {
            return safe;
        }
    }

    for (int distance = 0; distance < context.targetKeyCount; ++distance) {
        const int left = baseSet.baseLane - distance;
        const int right = baseSet.baseLane + distance;
        if (left >= 0 && left < context.targetKeyCount &&
            !createsSourceDifferentRepeat(context.placed, sourceLane, note.time, left, window)) {
            addUnique(safe, left);
        }
        if (right >= 0 && right < context.targetKeyCount && right != left &&
            !createsSourceDifferentRepeat(context.placed, sourceLane, note.time, right, window)) {
            addUnique(safe, right);
        }
        if (!safe.empty()) {
            return safe;
        }
    }

    return baseSet.candidates;
}

}  // namespace

PpgWeights weightsForStyle(ConversionStyle style) {
    PpgWeights weights;
    switch (style) {
        case ConversionStyle::Direct:
            weights.position = 3.0;
            weights.order = 1.0;
            weights.shape = 1.0;
            weights.jack = 0.0;
            weights.movement = 1.0;
            weights.density = 0.0;
            return weights;
        case ConversionStyle::Faithful:
            weights.position = 2.0;
            weights.order = 3.0;
            weights.shape = 2.5;
            weights.collision = 100.0;
            weights.lnConflict = 80.0;
            weights.jack = 0.3;
            weights.movement = 2.0;
            weights.density = 1.0;
            weights.handBalance = 3.0;
            return weights;
        case ConversionStyle::Training:
            weights.position = 0.7;
            weights.order = 1.5;
            weights.shape = 1.0;
            weights.collision = 120.0;
            weights.lnConflict = 100.0;
            weights.jack = 10.0;
            weights.movement = 4.0;
            weights.density = 8.0;
            weights.handBalance = 8.0;
            return weights;
        case ConversionStyle::DP:
            weights.position = 0.8;
            weights.order = 1.5;
            weights.shape = 1.3;
            weights.collision = 120.0;
            weights.lnConflict = 100.0;
            weights.jack = 5.0;
            weights.movement = 6.0;
            weights.density = 6.0;
            weights.handBalance = 15.0;
            return weights;
        case ConversionStyle::Expand:
        case ConversionStyle::Compress:
        case ConversionStyle::Playable:
            return weights;
    }
    return weights;
}

LaneCandidateSet generateCandidateLanes(int sourceLane, int sourceK, int targetK, ConversionStyle style) {
    LaneCandidateSet set;
    set.sourceLane = sourceLane;
    set.baseLane = useDualFiveSplit(sourceK, targetK)
                       ? dualFiveBaseLaneForSource(sourceLane, targetK)
                       : mapLaneDirect(sourceLane, sourceK, targetK);
    if (useDualFiveSplit(sourceK, targetK)) {
        const auto [zoneStart, zoneEnd] = dualFiveZoneForSource(sourceLane);
        set.hasPreferredZone = true;
        set.preferredZoneStart = zoneStart;
        set.preferredZoneEnd = zoneEnd;
    }
    set.radius = 1;
    if (style == ConversionStyle::Direct) {
        set.radius = 0;
    } else if (style == ConversionStyle::Playable) {
        set.radius = sourceK == targetK ? 1 : 2;
    } else if (style == ConversionStyle::Faithful) {
        set.radius = 1;
    } else if (style == ConversionStyle::Training) {
        set.radius = 3;
    } else if (style == ConversionStyle::DP) {
        set.radius = 2;
    }

    addUnique(set.candidates, clampInt(set.baseLane, 0, targetK - 1));
    for (int offset = 1; offset <= set.radius; ++offset) {
        addUnique(set.candidates, clampInt(set.baseLane - offset, 0, targetK - 1));
        addUnique(set.candidates, clampInt(set.baseLane + offset, 0, targetK - 1));
    }
    keepPreferredZoneCandidates(set);
    return set;
}

std::vector<SliceAssignment> generateSliceAssignments(const TimeSlice& slice,
                                                      const std::vector<Note>& sourceNotes,
                                                      const AssignmentContext& context) {
    std::vector<std::vector<int>> candidateLists;
    candidateLists.reserve(slice.noteIndices.size());

    for (const auto noteIndex : slice.noteIndices) {
        const auto& note = sourceNotes[noteIndex];
        const int sourceLane = note.sourceLane.value_or(note.lane);
        auto baseSet =
            generateCandidateLanes(sourceLane, context.sourceKeyCount, context.targetKeyCount, context.style);
        const auto* hint = findGestureHint(context.gestureRail, note.id);
        addGestureCandidates(baseSet, hint, context.targetKeyCount);
        auto candidates = noCreatedJackCandidates(baseSet, note, context);
        auto sourceAnchor =
            recentSourceLaneAnchor(context.placed, sourceLane, note.time, context.targetKeyCount);
        if (slice.noteIndices.size() == 1 && hint != nullptr && hint->kind == PatternKind::Jack) {
            sourceAnchor.reset();
        }
        const bool expansionMode = context.targetKeyCount > context.sourceKeyCount;
        if (!expansionMode && slice.noteIndices.size() == 1 && sourceAnchor.has_value() &&
            std::find(candidates.begin(), candidates.end(), *sourceAnchor) != candidates.end() &&
            !hasSameTimeNote(context.placed, note.time, *sourceAnchor)) {
            Note anchored = note;
            anchored.lane = *sourceAnchor;
            if (!hasLongNoteConflict(context.placed, anchored, *sourceAnchor)) {
                candidates = {*sourceAnchor};
            }
        }
        candidateLists.push_back(std::move(candidates));
    }

    std::vector<SliceAssignment> assignments;
    SliceAssignment current;
    current.targetLanes.resize(candidateLists.size(), 0);

    auto build = [&](auto&& self, std::size_t depth) -> void {
        if (assignments.size() >= 512) {
            return;
        }
        if (depth == candidateLists.size()) {
            current.score = scoreAssignment(slice, sourceNotes, current, context);
            assignments.push_back(current);
            return;
        }
        for (const int lane : candidateLists[depth]) {
            current.targetLanes[depth] = lane;
            self(self, depth + 1);
        }
    };

    build(build, 0);
    return assignments;
}

double scoreAssignment(const TimeSlice& slice,
                       const std::vector<Note>& sourceNotes,
                       const SliceAssignment& assignment,
                       const AssignmentContext& context) {
    double score = 0.0;
    double collisionPenalty = 0.0;
    double lnPenalty = 0.0;
    double jackPenalty = 0.0;
    double movementPenalty = 0.0;
    double densityPenalty = 0.0;
    double handPenalty = 0.0;

    std::set<int> usedInSlice;
    int left = 0;
    int right = 0;

    for (std::size_t i = 0; i < slice.noteIndices.size(); ++i) {
        const auto& source = sourceNotes[slice.noteIndices[i]];
        const int sourceLane = source.sourceLane.value_or(source.lane);
        const int targetLane = assignment.targetLanes[i];
        const auto* hint = findGestureHint(context.gestureRail, source.id);
        const int jackWindowMsForBalance = std::max(0, context.jackWindowMs);
        const bool sourceJackLike = hint != nullptr && hint->kind == PatternKind::Jack;
        const bool sourceRepeatNearby =
            recentSameSourceLane(context.placed, sourceLane, source.time, jackWindowMsForBalance).has_value();
        const auto sourceAnchor =
            recentSourceLaneAnchor(context.placed, sourceLane, source.time, context.targetKeyCount);

        score += context.weights.position *
                 (1.0 - std::abs(normalizedLane(sourceLane, context.sourceKeyCount) -
                                  normalizedLane(targetLane, context.targetKeyCount)));
        score += dualFivePanelScore(sourceLane, targetLane, hint, context);
        if (!sourceJackLike && !sourceRepeatNearby && !sourceAnchor.has_value()) {
            score += context.weights.density *
                     expandedLaneBalanceScore(context.laneUse,
                                              sourceLane,
                                              context.sourceKeyCount,
                                              targetLane,
                                              context.targetKeyCount) *
                     1.5;
            if (context.targetKeyCount > context.sourceKeyCount) {
                score += context.weights.handBalance *
                         handBalanceScore(context.laneUse, targetLane, context.targetKeyCount) *
                         0.75;
            }
        }
        score += gestureScore(source, targetLane, hint, context);
        if (!sourceJackLike && sourceAnchor.has_value()) {
            const double anchorWeight = context.targetKeyCount > context.sourceKeyCount ? 0.85 : 2.25;
            score += context.weights.shape *
                     sourceLaneAnchorScore(targetLane, *sourceAnchor, context.targetKeyCount) *
                     anchorWeight;
        }

        if (!usedInSlice.insert(targetLane).second) {
            collisionPenalty += 1.0;
        }
        if (hasSameTimeNote(context.placed, source.time, targetLane)) {
            collisionPenalty += 1.0;
        }

        Note candidate = source;
        candidate.lane = targetLane;
        if (hasLongNoteConflict(context.placed, candidate, targetLane)) {
            lnPenalty += 1.0;
        }

        const int jackWindowMs = std::max(0, context.jackWindowMs);
        jackPenalty += recentJackPenalty(context.placed, source.time, targetLane, jackWindowMs);
        score -= unwantedCreatedJackPenalty(context.placed, sourceLane, source.time, targetLane, jackWindowMs) * 80.0;
        if (const auto sourceRepeat = recentSameSourceLane(context.placed, sourceLane, source.time, jackWindowMs);
            sourceRepeat.has_value()) {
            if (context.style == ConversionStyle::Faithful) {
                score += context.weights.shape * (targetLane == sourceRepeat->lane ? 3.0 : -3.0);
            } else if (context.style == ConversionStyle::Playable || context.style == ConversionStyle::Training) {
                const auto recentRepeatLanes =
                    recentSameSourceTargetLanes(context.placed, sourceLane, source.time, jackWindowMs * 2);
                if (recentRepeatLanes.size() >= 2 && recentRepeatLanes.count(targetLane) == 0) {
                    score -= context.weights.shape * 3.0;
                }
                const int laneDelta = std::abs(targetLane - sourceRepeat->lane);
                if (laneDelta == 0) {
                    score += context.weights.shape * 0.75;
                } else if (laneDelta == 1) {
                    score += context.weights.shape * 0.50;
                } else {
                    score -= context.weights.shape;
                }
            }
        }
        if (targetLane >= 0 && targetLane < static_cast<int>(context.laneUse.size())) {
            densityPenalty += static_cast<double>(context.laneUse[static_cast<std::size_t>(targetLane)]) /
                              static_cast<double>(std::max(1, static_cast<int>(context.placed.size())));
        }

        if (const auto previous = lastPlacedBefore(context.placed, source.time); previous.has_value()) {
            movementPenalty +=
                std::abs(targetLane - previous->lane) / static_cast<double>(std::max(1, context.targetKeyCount - 1));

            const int sourceSign = signOf(sourceLane - previous->sourceLane.value_or(previous->lane));
            const int targetSign = signOf(targetLane - previous->lane);
            if (sourceSign != 0 && targetSign != 0) {
                score += context.weights.order * (sourceSign == targetSign ? 1.0 : -1.0);
            }
        }

        if (context.placed.size() >= 2) {
            const auto& prev = context.placed[context.placed.size() - 1];
            const auto& prev2 = context.placed[context.placed.size() - 2];
            if (sourceLane == prev2.sourceLane.value_or(prev2.lane) &&
                sourceLane != prev.sourceLane.value_or(prev.lane)) {
                score += context.weights.shape * (targetLane == prev2.lane && targetLane != prev.lane ? 1.0 : -1.0);
            }
        }

        if (targetLane < context.targetKeyCount / 2) {
            ++left;
        } else {
            ++right;
        }
    }

    if (slice.noteIndices.size() >= 2) {
        int targetMin = std::numeric_limits<int>::max();
        int targetMax = std::numeric_limits<int>::min();
        for (const auto lane : assignment.targetLanes) {
            targetMin = std::min(targetMin, lane);
            targetMax = std::max(targetMax, lane);
        }

        const double sourceSpan = static_cast<double>(slice.span) / static_cast<double>(std::max(1, context.sourceKeyCount - 1));
        const double targetSpan = static_cast<double>(targetMax - targetMin) /
                                  static_cast<double>(std::max(1, context.targetKeyCount - 1));
        score += context.weights.shape * (1.0 - std::abs(sourceSpan - targetSpan));

        for (std::size_t a = 0; a < slice.noteIndices.size(); ++a) {
            for (std::size_t b = a + 1; b < slice.noteIndices.size(); ++b) {
                const int sourceA = sourceNotes[slice.noteIndices[a]].sourceLane.value_or(sourceNotes[slice.noteIndices[a]].lane);
                const int sourceB = sourceNotes[slice.noteIndices[b]].sourceLane.value_or(sourceNotes[slice.noteIndices[b]].lane);
                const int sourceSign = signOf(sourceB - sourceA);
                const int targetSign = signOf(assignment.targetLanes[b] - assignment.targetLanes[a]);
                if (sourceSign != 0 && targetSign != 0) {
                    score += context.weights.order * (sourceSign == targetSign ? 1.0 : -1.0);
                }
            }
        }
    }

    handPenalty = std::abs(left - right) / static_cast<double>(std::max(1, left + right));

    score -= context.weights.collision * collisionPenalty;
    score -= context.weights.lnConflict * lnPenalty;
    score -= context.weights.jack * jackPenalty;
    score -= context.weights.movement * movementPenalty;
    score -= context.weights.density * densityPenalty;
    score -= context.weights.handBalance * handPenalty;
    return score;
}

}  // namespace keyconv
