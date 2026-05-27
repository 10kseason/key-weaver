#include "core/compress.hpp"

#include "core/collision.hpp"
#include "core/distance.hpp"
#include "core/mapping.hpp"
#include "core/repeat.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace keyconv {

namespace {

constexpr std::size_t kMaxRollCandidateTimes = 64;
constexpr int kHoldRollSearchWindowMs = 512;
constexpr int kSmallChartHoldRollSearchWindowMs = 2048;

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

int holdDuration(const Note& note) {
    if (note.type != NoteType::Hold || !note.endTime.has_value()) {
        return 0;
    }
    return std::max(0, *note.endTime - note.time);
}

double noteImportance(const Note& note) {
    double score = 10.0;
    if (note.type == NoteType::Hold) {
        score += 80.0;
        score += std::min(40.0, static_cast<double>(holdDuration(note)) / 100.0);
    }
    const int sourceLane = note.sourceLane.value_or(note.lane);
    if (sourceLane == 0) {
        score += 8.0;
    }
    if (note.id.empty()) {
        score -= 1.0;
    }
    return score;
}

std::vector<int> nearestLanes(int baseLane, int keyCount) {
    std::vector<int> lanes;
    for (int distance = 0; distance < keyCount; ++distance) {
        const int left = baseLane - distance;
        const int right = baseLane + distance;
        if (left >= 0) {
            lanes.push_back(left);
        }
        if (right < keyCount && right != left) {
            lanes.push_back(right);
        }
    }
    return lanes;
}

bool laneAvailable(const std::vector<Note>& placed, const Note& note, int lane) {
    if (hasSameTimeNote(placed, note.time, lane)) {
        return false;
    }
    if (hasLongNoteConflict(placed, note, lane)) {
        return false;
    }
    return true;
}

bool createsSourceDifferentRepeat(const std::vector<Note>& placed,
                                  const Note& note,
                                  int lane,
                                  int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0) {
        return false;
    }

    const int sourceLane = note.sourceLane.value_or(note.lane);
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = note.time - it->time;
        if (dt > window) {
            break;
        }
        if (dt > 0 && it->lane == lane && it->sourceLane.value_or(it->lane) != sourceLane) {
            return true;
        }
    }
    return false;
}

bool createsCompressedRepeat(const std::vector<Note>& placed,
                             const Note& note,
                             int lane,
                             const ConvertOptions& options,
                             const Chart* original) {
    if (original != nullptr) {
        return wouldCreateCreatedJackOnLane(*original, placed, note, lane, options.jackWindowMs);
    }
    return createsSourceDifferentRepeat(placed, note, lane, options.jackWindowMs);
}

struct PlacementAttempt {
    std::optional<Note> note;
    bool distanceBlocked = false;
};

struct RollAttempt {
    std::optional<Note> note;
    bool distanceBlocked = false;
};

PlacementAttempt placeAtNearestLane(const std::vector<Note>& placed,
                                    const Note& note,
                                    int targetKeyCount,
                                    const ConvertOptions& options,
                                    bool checkDistance,
                                    bool includeHoldEdges,
                                    const Chart* original,
                                    bool allowCreatedJackFallback) {
    PlacementAttempt result;
    const auto candidates = nearestLanes(note.lane, targetKeyCount);
    for (int pass = 0; pass < (allowCreatedJackFallback ? 2 : 1); ++pass) {
        const bool avoidCompressedJack = pass == 0;
        for (const int lane : candidates) {
            Note candidate = note;
            candidate.lane = lane;
            if (!laneAvailable(placed, candidate, lane)) {
                continue;
            }
            if (avoidCompressedJack &&
                createsCompressedRepeat(placed, candidate, lane, options, original)) {
                continue;
            }
            if (checkDistance && hasDistanceConflict(placed, candidate, options, includeHoldEdges)) {
                result.distanceBlocked = true;
                continue;
            }
            result.note = candidate;
            return result;
        }
    }
    return result;
}

std::vector<int> rawRollTimesNear(int originalTimeMs, int maxRollMs) {
    std::vector<int> times;
    for (int offset = 8; offset <= std::max(0, maxRollMs); offset += 4) {
        times.push_back(originalTimeMs + offset);
        if (originalTimeMs - offset >= 0) {
            times.push_back(originalTimeMs - offset);
        }
    }
    return times;
}

void addSnapTimesAround(std::set<int>& times,
                        int anchorTimeMs,
                        const ConvertOptions& options,
                        const std::vector<TimingPoint>& timingPoints,
                        std::optional<int> minTime,
                        std::optional<int> maxTime) {
    if (anchorTimeMs < 0) {
        return;
    }
    const int snapSearchWindow = std::max(options.maxRollMs, kHoldRollSearchWindowMs);
    if (minTime.has_value() && anchorTimeMs + snapSearchWindow < *minTime) {
        return;
    }
    if (maxTime.has_value() && anchorTimeMs - snapSearchWindow > *maxTime) {
        return;
    }

    auto addIfAllowed = [&](int time) {
        if (time < 0) {
            return;
        }
        if (minTime.has_value() && time < *minTime) {
            return;
        }
        if (maxTime.has_value() && time > *maxTime) {
            return;
        }
        times.insert(time);
    };

    if (options.snapRolledNotes) {
        if (isSnapAligned(anchorTimeMs, timingPoints, options.snapToleranceMs)) {
            addIfAllowed(anchorTimeMs);
        }
        for (const int time : generateSnapTimesNear(anchorTimeMs, timingPoints, std::max(options.maxRollMs, 512))) {
            addIfAllowed(time);
        }
    } else {
        addIfAllowed(anchorTimeMs);
    }
}

std::vector<int> rollTimesForNote(const std::vector<Note>& placed,
                                  const Note& note,
                                  const ConvertOptions& options,
                                  const std::vector<TimingPoint>& timingPoints) {
    std::set<int> unique;
    const auto nearby = options.snapRolledNotes
                            ? generateSnapTimesNear(note.time, timingPoints, options.maxRollMs)
                            : rawRollTimesNear(note.time, options.maxRollMs);
    unique.insert(nearby.begin(), nearby.end());

    if (note.type == NoteType::Hold && note.endTime.has_value()) {
        const int duration = holdDuration(note);
        const int gap = std::max({16, options.minObjectGapMs, options.sameLaneMinGapMs});
        const int rollWindow = placed.size() <= 16 ? kSmallChartHoldRollSearchWindowMs : kHoldRollSearchWindowMs;
        const int searchWindow = std::max(options.maxRollMs, rollWindow);
        const int minRollTime = std::max(0, note.time - searchWindow);
        const int maxRollTime = note.time + searchWindow;
        int latestPlacedEdge = 0;
        for (const auto& placedNote : placed) {
            const int placedEnd = placedNote.type == NoteType::Hold && placedNote.endTime.has_value()
                                      ? *placedNote.endTime
                                      : placedNote.time;
            latestPlacedEdge = std::max(latestPlacedEdge, placedEnd);
            addSnapTimesAround(unique,
                               placedEnd + gap,
                               options,
                               timingPoints,
                               std::max(placedEnd + gap, minRollTime),
                               maxRollTime);
            addSnapTimesAround(unique,
                               placedNote.time - duration - gap,
                               options,
                               timingPoints,
                               minRollTime,
                               std::min(placedNote.time - duration - gap, maxRollTime));
        }
        addSnapTimesAround(unique,
                           latestPlacedEdge + gap,
                           options,
                           timingPoints,
                           std::max(latestPlacedEdge + gap, minRollTime),
                           maxRollTime);
    }

    std::vector<int> result(unique.begin(), unique.end());
    std::stable_sort(result.begin(), result.end(), [original = note.time](int lhs, int rhs) {
        const int lhsDelta = std::abs(lhs - original);
        const int rhsDelta = std::abs(rhs - original);
        if (lhsDelta != rhsDelta) {
            return lhsDelta < rhsDelta;
        }
        return lhs < rhs;
    });
    if (result.size() > kMaxRollCandidateTimes) {
        result.resize(kMaxRollCandidateTimes);
    }
    return result;
}

RollAttempt rollNote(const std::vector<Note>& placed,
                     const Note& note,
                     int targetKeyCount,
                     const ConvertOptions& options,
                     const std::vector<TimingPoint>& timingPoints,
                     const Chart* original,
                     bool allowCreatedJackFallback) {
    RollAttempt result;
    const auto candidateTimes = rollTimesForNote(placed, note, options, timingPoints);

    for (const int candidateTime : candidateTimes) {
        Note rolled = note;
        const int offset = candidateTime - note.time;
        rolled.time = candidateTime;
        if (rolled.time < 0) {
            continue;
        }
        if (rolled.type == NoteType::Hold && rolled.endTime.has_value()) {
            *rolled.endTime += offset;
            if (*rolled.endTime <= rolled.time) {
                continue;
            }
        }
        if (options.snapRolledNotes &&
            !isSnapAligned(rolled.time, timingPoints, options.snapToleranceMs)) {
            continue;
        }

        const auto placedRolled = placeAtNearestLane(placed,
                                                     rolled,
                                                     targetKeyCount,
                                                     options,
                                                     true,
                                                     true,
                                                     original,
                                                     allowCreatedJackFallback);
        if (placedRolled.distanceBlocked) {
            result.distanceBlocked = true;
        }
        if (placedRolled.note.has_value()) {
            result.note = *placedRolled.note;
            return result;
        }
    }
    return result;
}

bool policyAllowsRoll(CompressPolicy policy) {
    return policy == CompressPolicy::NoOverlapRoll;
}

bool policyAllowsHoldRoll(CompressPolicy policy) {
    return policy == CompressPolicy::NoOverlapRoll || policy == CompressPolicy::NoOverlapHybrid ||
           policy == CompressPolicy::TrainingSimplify;
}

bool policyAllowsDrop(CompressPolicy policy) {
    return policy == CompressPolicy::NoOverlapDrop || policy == CompressPolicy::NoOverlapHybrid ||
           policy == CompressPolicy::TrainingSimplify || policy == CompressPolicy::NoOverlapRoll;
}

bool policyAllowsTapify(CompressPolicy policy) {
    return policy == CompressPolicy::TrainingSimplify;
}

int rollBudgetForPolicy(CompressPolicy policy, int targetKeyCount) {
    if (policy == CompressPolicy::NoOverlapRoll) {
        return std::max(256, targetKeyCount * 128);
    }
    if (policy == CompressPolicy::NoOverlapHybrid || policy == CompressPolicy::TrainingSimplify) {
        return std::max(96, targetKeyCount * 48);
    }
    return 0;
}

int activeHoldCountAt(const std::vector<Note>& placed, int time) {
    std::set<int> active;
    for (const auto& note : placed) {
        if (note.type != NoteType::Hold || !note.endTime.has_value()) {
            continue;
        }
        if (note.time < time && *note.endTime > time) {
            active.insert(note.lane);
        }
    }
    return static_cast<int>(active.size());
}

void copyDistanceStats(CompressionPlanStats& stats, const DistanceValidationResult& distance) {
    stats.nearTimeConflicts = distance.nearTimeConflicts;
    stats.sameLaneNearConflicts = distance.sameLaneNearConflicts;
    stats.unsnappedNotes = distance.unsnappedNotes;
    stats.unsnappedRolledNotes = distance.unsnappedRolledNotes;
    stats.minPositiveDeltaMs = distance.minPositiveDeltaMs;
}

void appendDistanceWarnings(CompressionPlanStats& stats,
                            const ConvertOptions& options,
                            const DistanceValidationResult& distance) {
    if ((options.distancePolicy == DistancePolicy::AimodSafe || options.distancePolicy == DistancePolicy::Strict) &&
        (distance.nearTimeConflicts > 0 || distance.sameLaneNearConflicts > 0)) {
        std::ostringstream warning;
        warning << "Distance guard validation failed: near-time conflicts=" << distance.nearTimeConflicts
                << ", same-lane near conflicts=" << distance.sameLaneNearConflicts;
        stats.warnings.push_back(warning.str());
    } else if (options.distancePolicy == DistancePolicy::WarnOnly &&
               (distance.nearTimeConflicts > 0 || distance.sameLaneNearConflicts > 0)) {
        std::ostringstream warning;
        warning << "Distance guard warning: near-time conflicts=" << distance.nearTimeConflicts
                << ", same-lane near conflicts=" << distance.sameLaneNearConflicts;
        stats.warnings.push_back(warning.str());
    }
    if (options.snapRolledNotes && distance.unsnappedRolledNotes > 0) {
        std::ostringstream warning;
        warning << "Snap roll validation failed: unsnapped rolled notes=" << distance.unsnappedRolledNotes;
        stats.warnings.push_back(warning.str());
    } else if (!options.snapRolledNotes && distance.unsnappedRolledNotes > 0) {
        std::ostringstream warning;
        warning << "Snap roll disabled: unsnapped rolled notes=" << distance.unsnappedRolledNotes;
        stats.warnings.push_back(warning.str());
    }
}

}  // namespace

CompressPolicy resolveCompressPolicy(const ConvertOptions& options) {
    if (options.compressPolicy != CompressPolicy::Auto) {
        return options.compressPolicy;
    }
    if (options.sourceKeyCount <= options.targetKeyCount) {
        return CompressPolicy::PreserveStrict;
    }
    if (options.style == ConversionStyle::Faithful || options.style == ConversionStyle::Direct) {
        return CompressPolicy::PreserveStrict;
    }
    if (options.style == ConversionStyle::Training) {
        return CompressPolicy::TrainingSimplify;
    }
    if (options.sourceKeyCount > options.targetKeyCount) {
        return CompressPolicy::NoOverlapDrop;
    }
    return CompressPolicy::NoOverlapHybrid;
}

CompressionPlanStats applyCompressPlanner(std::vector<Note>& notes,
                                          const ConvertOptions& options,
                                          const std::vector<TimingPoint>& timingPoints,
                                          const Chart* original) {
    CompressionPlanStats stats;
    const auto policy = resolveCompressPolicy(options);
    if (policy == CompressPolicy::PreserveStrict) {
        const auto validation = validateNoOverlap(notes, options.targetKeyCount);
        stats.impossibleSlices = validation.impossibleSlices;
        stats.noOverlapGuaranteed = validation.noOverlapGuaranteed;
        const auto distance = validateDistance(notes, options, timingPoints, stats.rolledNoteIds);
        copyDistanceStats(stats, distance);
        appendDistanceWarnings(stats, options, distance);
        return stats;
    }

    std::map<int, std::vector<Note>> groups;
    for (auto note : sortedNotes(notes)) {
        groups[note.time].push_back(note);
    }

    std::vector<Note> placed;
    int sliceIndex = 0;
    const int rollBudget = rollBudgetForPolicy(policy, options.targetKeyCount);
    const bool allowCreatedJackFallback = policy != CompressPolicy::NoOverlapDrop;

    for (auto& entry : groups) {
        auto& group = entry.second;
        const int activeHolds = activeHoldCountAt(placed, entry.first);
        const int freeLanes = std::max(0, options.targetKeyCount - activeHolds);
        if (static_cast<int>(group.size()) > freeLanes) {
            ++stats.impossibleSlices;
        }

        std::stable_sort(group.begin(), group.end(), [](const Note& a, const Note& b) {
            const double lhs = noteImportance(a);
            const double rhs = noteImportance(b);
            if (lhs != rhs) {
                return lhs > rhs;
            }
            if (a.lane != b.lane) {
                return a.lane < b.lane;
            }
            return a.id < b.id;
        });

        for (auto note : group) {
            bool blockedByDistance = false;
            const auto direct = placeAtNearestLane(placed,
                                                   note,
                                                   options.targetKeyCount,
                                                   options,
                                                   true,
                                                   false,
                                                   original,
                                                   allowCreatedJackFallback);
            blockedByDistance = blockedByDistance || direct.distanceBlocked;
            if (direct.note.has_value()) {
                placed.push_back(*direct.note);
                continue;
            }

            if (policyAllowsHoldRoll(policy) && note.type == NoteType::Hold &&
                stats.rolledByCompression < rollBudget) {
                const auto rolled = rollNote(placed,
                                             note,
                                             options.targetKeyCount,
                                             options,
                                             timingPoints,
                                             original,
                                             allowCreatedJackFallback);
                blockedByDistance = blockedByDistance || rolled.distanceBlocked;
                if (rolled.note.has_value()) {
                    placed.push_back(*rolled.note);
                    ++stats.rolledByCompression;
                    if (!rolled.note->id.empty()) {
                        stats.rolledNoteIds.insert(rolled.note->id);
                    }
                    if (distancePolicyRejects(options.distancePolicy)) {
                        ++stats.rerolledByDistanceGuard;
                    }
                    continue;
                }
            }

            if (policyAllowsTapify(policy) && note.type == NoteType::Hold && holdDuration(note) <= 160) {
                Note tapified = note;
                tapified.type = NoteType::Tap;
                tapified.endTime = std::nullopt;
                const auto placedTap = placeAtNearestLane(placed,
                                                          tapified,
                                                          options.targetKeyCount,
                                                          options,
                                                          true,
                                                          false,
                                                          original,
                                                          allowCreatedJackFallback);
                blockedByDistance = blockedByDistance || placedTap.distanceBlocked;
                if (placedTap.note.has_value()) {
                    placed.push_back(*placedTap.note);
                    ++stats.tapifiedHolds;
                    continue;
                }
            }

            if (policyAllowsRoll(policy) && note.type == NoteType::Tap &&
                stats.rolledByCompression < rollBudget) {
                const auto rolled = rollNote(placed,
                                             note,
                                             options.targetKeyCount,
                                             options,
                                             timingPoints,
                                             original,
                                             allowCreatedJackFallback);
                blockedByDistance = blockedByDistance || rolled.distanceBlocked;
                if (rolled.note.has_value()) {
                    placed.push_back(*rolled.note);
                    ++stats.rolledByCompression;
                    if (!rolled.note->id.empty()) {
                        stats.rolledNoteIds.insert(rolled.note->id);
                    }
                    if (distancePolicyRejects(options.distancePolicy)) {
                        ++stats.rerolledByDistanceGuard;
                    }
                    continue;
                }
            }

            if (policyAllowsDrop(policy)) {
                ++stats.droppedByCompression;
                if (blockedByDistance) {
                    ++stats.droppedByDistanceGuard;
                }
                continue;
            }

            placed.push_back(note);
        }

        ++sliceIndex;
    }

    notes = sortedNotes(std::move(placed));
    const auto validation = validateNoOverlap(notes, options.targetKeyCount);
    stats.noOverlapGuaranteed = validation.noOverlapGuaranteed;
    if (!validation.noOverlapGuaranteed) {
        std::ostringstream warning;
        warning << "No-overlap compression validation failed: same-time collisions="
                << validation.sameTimeCollisions << ", LN conflicts=" << validation.longNoteConflicts;
        stats.warnings.push_back(warning.str());
    }
    const auto distance = validateDistance(notes, options, timingPoints, stats.rolledNoteIds);
    copyDistanceStats(stats, distance);
    appendDistanceWarnings(stats, options, distance);
    return stats;
}

OverlapValidationResult validateNoOverlap(const std::vector<Note>& notes, int targetKeyCount) {
    OverlapValidationResult result;
    const auto scan = detectCollisions(notes);
    result.sameTimeCollisions = scan.sameTimeCollisions;
    result.longNoteConflicts = scan.longNoteConflicts;

    std::vector<Note> placed;
    std::map<int, std::vector<Note>> groups;
    for (const auto& note : sortedNotes(notes)) {
        groups[note.time].push_back(note);
    }
    for (const auto& entry : groups) {
        const int activeHolds = activeHoldCountAt(placed, entry.first);
        const int freeLanes = std::max(0, targetKeyCount - activeHolds);
        if (static_cast<int>(entry.second.size()) > freeLanes) {
            ++result.impossibleSlices;
        }
        for (const auto& note : entry.second) {
            placed.push_back(note);
        }
    }

    result.noOverlapGuaranteed = result.sameTimeCollisions == 0 && result.longNoteConflicts == 0;
    return result;
}

}  // namespace keyconv
