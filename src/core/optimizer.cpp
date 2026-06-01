#include "core/optimizer.hpp"

#include "core/assignment.hpp"
#include "core/collision.hpp"
#include "core/gesture.hpp"
#include "core/mapping.hpp"

#include <algorithm>
#include <optional>

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

std::optional<int> nearestFreeLane(const std::vector<Note>& placed,
                                   const Note& note,
                                   int keyCount,
                                   int jackWindowMs,
                                   int* preventedJacksByRepair) {
    std::optional<int> fallback;
    for (int distance = 0; distance < keyCount; ++distance) {
        const int left = note.lane - distance;
        const int right = note.lane + distance;
        if (left >= 0 && !hasSameTimeNote(placed, note.time, left) &&
            !hasLongNoteConflict(placed, note, left)) {
            if (!createsSourceDifferentRepeat(placed, note, left, jackWindowMs)) {
                return left;
            }
            if (preventedJacksByRepair != nullptr) {
                ++*preventedJacksByRepair;
            }
            if (!fallback.has_value()) {
                fallback = left;
            }
        }
        if (right < keyCount && right != left && !hasSameTimeNote(placed, note.time, right) &&
            !hasLongNoteConflict(placed, note, right)) {
            if (!createsSourceDifferentRepeat(placed, note, right, jackWindowMs)) {
                return right;
            }
            if (preventedJacksByRepair != nullptr) {
                ++*preventedJacksByRepair;
            }
            if (!fallback.has_value()) {
                fallback = right;
            }
        }
    }
    return fallback;
}

}  // namespace

int localRepairAssignments(std::vector<Note>& notes,
                           int targetKeyCount,
                           int jackWindowMs,
                           int* preventedJacksByRepair) {
    int shifted = 0;
    std::vector<Note> placed;

    for (auto note : sortedNotes(notes)) {
        const bool collides = hasSameTimeNote(placed, note.time, note.lane) ||
                              hasLongNoteConflict(placed, note, note.lane);
        if (collides) {
            const auto repairedLane =
                nearestFreeLane(placed, note, targetKeyCount, jackWindowMs, preventedJacksByRepair);
            if (repairedLane.has_value()) {
                note.lane = *repairedLane;
                ++shifted;
            }
        }
        placed.push_back(note);
    }

    notes = std::move(placed);
    return shifted;
}

OptimizationResult greedyOptimizeSlices(const Chart& chart, const ConvertOptions& options) {
    OptimizationResult result;
    const auto slices = buildTimeSlices(chart, options.sourceKeyCount, options.sameTimeEpsilonMs);
    result.patterns = detectPatternTokens(slices, options.jackWindowMs);
    const bool fullFieldRemix = options.targetKeyCount == 10 && options.tenKFullFieldRemix;
    const auto gestureRail = fullFieldRemix
                                  ? buildFullFieldRail(chart,
                                                       options.sourceKeyCount,
                                                       options.targetKeyCount,
                                                       options.sameTimeEpsilonMs,
                                                       options.jackWindowMs,
                                                       options.gestureRailEnabled)
                                  : buildGestureRail(chart,
                                                     options.sourceKeyCount,
                                                     options.targetKeyCount,
                                                     options.sameTimeEpsilonMs,
                                                     options.jackWindowMs,
                                                     options.gestureRailEnabled,
                                                     options.sourceKeyCount == 7 &&
                                                         options.targetKeyCount == 10 &&
                                                         options.tenKeyPlannerPolicy ==
                                                             TenKeyPlannerPolicy::StagedMirrorCompress);

    AssignmentContext context;
    context.sourceKeyCount = options.sourceKeyCount;
    context.targetKeyCount = options.targetKeyCount;
    context.jackWindowMs = options.jackWindowMs;
    context.style = options.style;
    context.tenKeyPlannerPolicy = options.tenKeyPlannerPolicy;
    context.weights = weightsForStyle(options.style);
    context.preserveLaneDrift = options.preserveLaneDrift;
    context.tenKFullFieldRemix = fullFieldRemix;
    context.preventedJacksByAssignment = &result.preventedJacksByAssignment;
    context.gestureRail = &gestureRail;
    context.laneUse.assign(static_cast<std::size_t>(options.targetKeyCount), 0);

    for (const auto& slice : slices) {
        const auto assignments = generateSliceAssignments(slice, chart.notes, context);
        if (assignments.empty()) {
            continue;
        }

        const auto best = std::max_element(assignments.begin(), assignments.end(), [](const auto& a, const auto& b) {
            return a.score < b.score;
        });

        for (std::size_t i = 0; i < slice.noteIndices.size(); ++i) {
            Note converted = chart.notes[slice.noteIndices[i]];
            converted.sourceLane = converted.sourceLane.value_or(converted.lane);
            converted.lane = clampInt(best->targetLanes[i], 0, options.targetKeyCount - 1);
            if (converted.lane >= 0 && converted.lane < options.targetKeyCount) {
                ++context.laneUse[static_cast<std::size_t>(converted.lane)];
            }
            context.placed.push_back(converted);
        }
    }

    if (options.collisionPolicy == CollisionPolicy::ShiftNearest) {
        result.localRepairShifted = localRepairAssignments(context.placed,
                                                           options.targetKeyCount,
                                                           options.jackWindowMs,
                                                           &result.preventedJacksByRepair);
    }

    result.notes = std::move(context.placed);
    return result;
}

}  // namespace keyconv
