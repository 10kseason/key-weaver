#include "core/convert.hpp"

#include "core/compress.hpp"
#include "core/collision.hpp"
#include "core/expansion.hpp"
#include "core/distance.hpp"
#include "core/gesture.hpp"
#include "core/mapping.hpp"
#include "core/optimizer.hpp"
#include "core/quality.hpp"
#include "core/repeat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace keyconv {

namespace {

double baseLane(int sourceLane, int sourceK, int targetK) {
    if (sourceK <= 1 || targetK <= 1) {
        return 0.0;
    }
    return static_cast<double>(sourceLane) * static_cast<double>(targetK - 1) /
           static_cast<double>(sourceK - 1);
}

std::vector<int> lanesForStyle(const Note& note, const ConvertOptions& options) {
    const int sourceLane = note.sourceLane.value_or(note.lane);

    switch (options.style) {
        case ConversionStyle::Direct:
            return {mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount)};
        case ConversionStyle::Expand:
            if (options.sourceKeyCount < options.targetKeyCount) {
                return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 1);
            }
            return {mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount)};
        case ConversionStyle::Compress:
            if (options.sourceKeyCount > options.targetKeyCount) {
                return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 1);
            }
            return {mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount)};
        case ConversionStyle::Playable:
            if (options.sourceKeyCount != options.targetKeyCount) {
                return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 1);
            }
            return {mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount)};
        case ConversionStyle::Faithful:
            return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 1);
        case ConversionStyle::Training:
            return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 2);
        case ConversionStyle::DP:
            return getCandidateLanes(sourceLane, options.sourceKeyCount, options.targetKeyCount, 1);
    }

    return {mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount)};
}

bool createsSourceDifferentRepeat(const std::vector<Note>& placed,
                                  const Note& note,
                                  int lane,
                                  int jackWindowMs);

double sourceDifferentRepeatPenalty(const std::vector<Note>& placed,
                                    const Note& note,
                                    int lane,
                                    int jackWindowMs);

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

std::vector<int> noCreatedJackCandidates(const Note& note,
                                         const std::vector<int>& candidates,
                                         const std::vector<Note>& placed,
                                         const ConvertOptions& options,
                                         int* preventedJacksByAssignment) {
    const int window = std::max(0, options.jackWindowMs);
    if (window <= 0) {
        return candidates;
    }

    std::vector<int> safe;
    auto addSafe = [&](int lane) {
        if (std::find(safe.begin(), safe.end(), lane) == safe.end()) {
            safe.push_back(lane);
        }
    };

    for (const int lane : candidates) {
        if (!createsSourceDifferentRepeat(placed, note, lane, window)) {
            addSafe(lane);
        } else if (preventedJacksByAssignment != nullptr) {
            ++*preventedJacksByAssignment;
        }
    }
    if (!safe.empty()) {
        return safe;
    }

    const int sourceLane = note.sourceLane.value_or(note.lane);
    const int base = mapLaneDirect(sourceLane, options.sourceKeyCount, options.targetKeyCount);
    for (const int lane : orderedLaneCandidates(base, options.targetKeyCount)) {
        if (!createsSourceDifferentRepeat(placed, note, lane, window)) {
            addSafe(lane);
        }
    }
    return safe.empty() ? candidates : safe;
}

int chooseBestLane(const Note& note,
                   const std::vector<int>& candidates,
                   const std::vector<Note>& placed,
                   const std::vector<int>& laneUse,
                   const ConvertOptions& options,
                   int* preventedJacksByAssignment) {
    if (candidates.empty()) {
        return 0;
    }

    const auto safeCandidates =
        noCreatedJackCandidates(note, candidates, placed, options, preventedJacksByAssignment);
    const int sourceLane = note.sourceLane.value_or(note.lane);
    const double desired = baseLane(sourceLane, options.sourceKeyCount, options.targetKeyCount);
    int bestLane = safeCandidates.front();
    double bestScore = -std::numeric_limits<double>::infinity();

    for (const int lane : safeCandidates) {
        double score = 0.0;
        score -= std::abs(static_cast<double>(lane) - desired) * 10.0;
        score -= static_cast<double>(laneUse[static_cast<std::size_t>(lane)]) * 0.25;

        if (hasSameTimeNote(placed, note.time, lane)) {
            score -= 1000.0;
        }
        if (hasLongNoteConflict(placed, note, lane)) {
            score -= 500.0;
        }

        score -= sourceDifferentRepeatPenalty(placed, note, lane, options.jackWindowMs) * 80.0;

        for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
            if (note.time - it->time > 240) {
                break;
            }
            if (it->lane == lane && note.time > it->time) {
                score -= 3.0;
            }
        }

        if (score > bestScore) {
            bestScore = score;
            bestLane = lane;
        }
    }

    return bestLane;
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

double sourceDifferentRepeatPenalty(const std::vector<Note>& placed,
                                    const Note& note,
                                    int lane,
                                    int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0) {
        return 0.0;
    }

    const int sourceLane = note.sourceLane.value_or(note.lane);
    double penalty = 0.0;
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = note.time - it->time;
        if (dt > window) {
            break;
        }
        if (dt > 0 && it->lane == lane && it->sourceLane.value_or(it->lane) != sourceLane) {
            penalty += 1.0 - static_cast<double>(dt) / static_cast<double>(window);
        }
    }
    return penalty;
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

std::vector<Note> sortedNotes(std::vector<Note> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.lane < b.lane;
    });
    return notes;
}

void applyCollisionPolicy(std::vector<Note>& notes,
                          int targetKeyCount,
                          CollisionPolicy policy,
                          int jackWindowMs,
                          ConversionReport& report,
                          int* preventedJacksByRepair) {
    if (policy == CollisionPolicy::Keep) {
        return;
    }

    std::vector<Note> placed;
    for (auto note : sortedNotes(notes)) {
        const bool collides = hasSameTimeNote(placed, note.time, note.lane) ||
                              hasLongNoteConflict(placed, note, note.lane);

        if (!collides) {
            placed.push_back(note);
            continue;
        }

        if (policy == CollisionPolicy::ShiftNearest) {
            const auto shiftedLane =
                nearestFreeLane(placed, note, targetKeyCount, jackWindowMs, preventedJacksByRepair);
            if (shiftedLane.has_value()) {
                note.lane = *shiftedLane;
                ++report.shiftedNotes;
            } else {
                std::ostringstream warning;
                warning << "Could not shift collision at time " << note.time << " lane " << note.lane;
                report.warnings.push_back(warning.str());
            }
            placed.push_back(note);
        } else if (policy == CollisionPolicy::Merge && note.type == NoteType::Tap &&
                   hasSameTimeNote(placed, note.time, note.lane)) {
            ++report.mergedNotes;
        } else if (policy == CollisionPolicy::Drop) {
            ++report.droppedNotes;
        } else {
            placed.push_back(note);
        }
    }

    notes = std::move(placed);
}

struct JackSanitizerStats {
    int sanitizedCreatedJacks = 0;
    int unsolvedCreatedJacks = 0;
    int droppedNotes = 0;
};

struct DistanceSanitizerStats {
    int collapsedNearTimePairs = 0;
};

std::vector<Note> notesExcept(const std::vector<Note>& notes, int ignoredIndex) {
    std::vector<Note> others;
    others.reserve(notes.size() > 0 ? notes.size() - 1 : 0);
    for (std::size_t i = 0; i < notes.size(); ++i) {
        if (static_cast<int>(i) != ignoredIndex) {
            others.push_back(notes[i]);
        }
    }
    return others;
}

bool relaneCandidateIsSafe(const Chart& original,
                           const std::vector<Note>& notes,
                           int noteIndex,
                           int candidateLane,
                           const ConvertOptions& options) {
    if (candidateLane < 0 || candidateLane >= options.targetKeyCount ||
        noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
        return false;
    }

    Note moved = notes[static_cast<std::size_t>(noteIndex)];
    if (candidateLane == moved.lane) {
        return false;
    }
    moved.lane = candidateLane;

    const auto others = notesExcept(notes, noteIndex);
    if (hasSameTimeNote(others, moved.time, moved.lane) ||
        hasLongNoteConflict(others, moved, moved.lane) ||
        hasDistanceConflict(others, moved, options, false)) {
        return false;
    }
    return !wouldCreateCreatedJackOnLane(original,
                                        notes,
                                        moved,
                                        candidateLane,
                                        options.jackWindowMs,
                                        noteIndex);
}

std::vector<int> relaneOrder(const std::vector<Note>& notes, const CreatedJackPair& pair) {
    std::vector<int> order{pair.secondIndex, pair.firstIndex};
    std::stable_sort(order.begin(), order.end(), [&](int lhs, int rhs) {
        const auto& lhsNote = notes[static_cast<std::size_t>(lhs)];
        const auto& rhsNote = notes[static_cast<std::size_t>(rhs)];
        if (isGeneratedNoteId(lhsNote.id) != isGeneratedNoteId(rhsNote.id)) {
            return isGeneratedNoteId(lhsNote.id);
        }
        if (lhsNote.type != rhsNote.type) {
            return lhsNote.type == NoteType::Tap;
        }
        return lhsNote.time > rhsNote.time;
    });
    return order;
}

bool tryRelaneCreatedJack(const Chart& original,
                          std::vector<Note>& notes,
                          const CreatedJackPair& pair,
                          const ConvertOptions& options) {
    for (const int noteIndex : relaneOrder(notes, pair)) {
        const int currentLane = notes[static_cast<std::size_t>(noteIndex)].lane;
        for (const int lane : orderedLaneCandidates(currentLane, options.targetKeyCount)) {
            if (relaneCandidateIsSafe(original, notes, noteIndex, lane, options)) {
                notes[static_cast<std::size_t>(noteIndex)].lane = lane;
                return true;
            }
        }
    }
    return false;
}

std::optional<int> generatedIndexForPair(const std::vector<Note>& notes, const CreatedJackPair& pair) {
    const auto& first = notes[static_cast<std::size_t>(pair.firstIndex)];
    const auto& second = notes[static_cast<std::size_t>(pair.secondIndex)];
    if (isGeneratedNoteId(second.id)) {
        return pair.secondIndex;
    }
    if (isGeneratedNoteId(first.id)) {
        return pair.firstIndex;
    }
    return std::nullopt;
}

std::optional<int> compressionDropIndexForPair(const std::vector<Note>& notes,
                                               const CreatedJackPair& pair,
                                               const ConvertOptions& options) {
    if (options.targetKeyCount >= options.sourceKeyCount ||
        resolveCompressPolicy(options) == CompressPolicy::PreserveStrict) {
        return std::nullopt;
    }

    for (const int noteIndex : relaneOrder(notes, pair)) {
        if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
            continue;
        }
        const auto& note = notes[static_cast<std::size_t>(noteIndex)];
        if (note.type == NoteType::Tap) {
            return noteIndex;
        }
    }
    return std::nullopt;
}

JackSanitizerStats sanitizeCreatedJacks(const Chart& original,
                                        std::vector<Note>& notes,
                                        const ConvertOptions& options) {
    JackSanitizerStats stats;
    int guard = 0;
    while (guard++ < 512) {
        const auto pairs = detectCreatedJackPairs(original, notes, options.jackWindowMs);
        if (pairs.empty()) {
            return stats;
        }

        const auto& pair = pairs.front();
        if (tryRelaneCreatedJack(original, notes, pair, options)) {
            ++stats.sanitizedCreatedJacks;
            continue;
        }

        if (const auto generatedIndex = generatedIndexForPair(notes, pair); generatedIndex.has_value()) {
            notes.erase(notes.begin() + *generatedIndex);
            ++stats.sanitizedCreatedJacks;
            ++stats.droppedNotes;
            continue;
        }

        if (const auto compressionDropIndex = compressionDropIndexForPair(notes, pair, options);
            compressionDropIndex.has_value()) {
            notes.erase(notes.begin() + *compressionDropIndex);
            ++stats.sanitizedCreatedJacks;
            ++stats.droppedNotes;
            continue;
        }

        stats.unsolvedCreatedJacks = static_cast<int>(pairs.size());
        return stats;
    }

    stats.unsolvedCreatedJacks =
        static_cast<int>(detectCreatedJackPairs(original, notes, options.jackWindowMs).size());
    return stats;
}

bool retimeCandidateIsSafe(const Chart& original,
                           const std::vector<Note>& notes,
                           int noteIndex,
                           int newTime,
                           const ConvertOptions& options) {
    if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size()) || newTime < 0) {
        return false;
    }

    Note moved = notes[static_cast<std::size_t>(noteIndex)];
    if (newTime == moved.time) {
        return false;
    }
    const int oldTime = moved.time;
    moved.time = newTime;
    if (moved.type == NoteType::Hold && moved.endTime.has_value()) {
        const int duration = *moved.endTime - oldTime;
        if (duration <= 0) {
            return false;
        }
        moved.endTime = newTime + duration;
    }

    const auto others = notesExcept(notes, noteIndex);
    if (hasSameTimeNote(others, moved.time, moved.lane) ||
        hasLongNoteConflict(others, moved, moved.lane) ||
        hasDistanceConflict(others, moved, options, false)) {
        return false;
    }
    return !wouldCreateCreatedJackOnLane(original,
                                        notes,
                                        moved,
                                        moved.lane,
                                        options.jackWindowMs,
                                        noteIndex);
}

void retimeNotePreserveDuration(Note& note, int newTime) {
    const int oldTime = note.time;
    note.time = newTime;
    if (note.type == NoteType::Hold && note.endTime.has_value()) {
        const int duration = *note.endTime - oldTime;
        note.endTime = note.time + duration;
    }
}

struct StreamTransformStats {
    int transformedNotes = 0;
    int jitteredNotes = 0;
};

std::uint32_t mixHash(std::uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

std::uint32_t stableStringHash(const std::string& value) {
    std::uint32_t hash = 2166136261U;
    for (const char ch : value) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 16777619U;
    }
    return hash;
}

std::uint32_t superRandomNoteSeed(const Note& note,
                                  int noteIndex,
                                  int rankInGroup,
                                  const ConvertOptions& options) {
    std::uint32_t hash = options.seed == 0 ? 0x6d2b79f5U : options.seed;
    hash ^= static_cast<std::uint32_t>(note.time) * 0x9e3779b9U;
    hash ^= static_cast<std::uint32_t>(note.lane + 1) * 0x85ebca6bU;
    hash ^= static_cast<std::uint32_t>(noteIndex + 17) * 0xc2b2ae35U;
    hash ^= static_cast<std::uint32_t>(rankInGroup + 31) * 0x27d4eb2fU;
    hash ^= stableStringHash(note.id);
    return mixHash(hash);
}

std::vector<int> randomLaneOrder(std::uint32_t seed, int keyCount, int originalLane) {
    std::vector<int> lanes;
    if (keyCount <= 0) {
        return lanes;
    }

    lanes.reserve(static_cast<std::size_t>(keyCount));
    for (int lane = 0; lane < keyCount; ++lane) {
        lanes.push_back(lane);
    }
    std::stable_sort(lanes.begin(), lanes.end(), [&](int lhs, int rhs) {
        const std::uint32_t left = mixHash(seed ^ (static_cast<std::uint32_t>(lhs + 1) * 0x9e3779b9U));
        const std::uint32_t right = mixHash(seed ^ (static_cast<std::uint32_t>(rhs + 1) * 0x9e3779b9U));
        if (left != right) {
            return left < right;
        }
        return lhs < rhs;
    });
    if (lanes.size() > 1) {
        const auto original = std::find(lanes.begin(), lanes.end(), originalLane);
        if (original != lanes.end() && original == lanes.begin()) {
            std::iter_swap(lanes.begin(), std::next(lanes.begin()));
        }
    }
    return lanes;
}

std::vector<Note> notesExceptGroup(const std::vector<Note>& notes, const std::vector<int>& group) {
    std::vector<Note> others;
    others.reserve(notes.size() > group.size() ? notes.size() - group.size() : 0);
    for (int index = 0; index < static_cast<int>(notes.size()); ++index) {
        if (std::find(group.begin(), group.end(), index) == group.end()) {
            others.push_back(notes[static_cast<std::size_t>(index)]);
        }
    }
    return others;
}

std::optional<int> safeSuperRandomLaneForNote(const std::vector<Note>& notes,
                                              const std::vector<Note>& notesOutsideGroup,
                                              int noteIndex,
                                              const std::vector<int>& laneOrder,
                                              const std::set<int>& usedLanes) {
    if (noteIndex < 0 || noteIndex >= static_cast<int>(notes.size())) {
        return std::nullopt;
    }

    for (const int lane : laneOrder) {
        if (usedLanes.count(lane) > 0) {
            continue;
        }
        Note moved = notes[static_cast<std::size_t>(noteIndex)];
        moved.lane = lane;
        if (!hasLongNoteConflict(notesOutsideGroup, moved, lane)) {
            return lane;
        }
    }
    return std::nullopt;
}

int applyStreamSuperRandom(std::vector<Note>& notes, const ConvertOptions& options) {
    if (options.targetKeyCount <= 1 || notes.empty()) {
        return 0;
    }

    std::map<int, std::vector<int>> groups;
    for (int index = 0; index < static_cast<int>(notes.size()); ++index) {
        groups[notes[static_cast<std::size_t>(index)].time].push_back(index);
    }

    int transformed = 0;
    for (auto& [time, indices] : groups) {
        (void)time;
        std::stable_sort(indices.begin(), indices.end(), [&](int lhs, int rhs) {
            const auto& left = notes[static_cast<std::size_t>(lhs)];
            const auto& right = notes[static_cast<std::size_t>(rhs)];
            if (left.lane != right.lane) {
                return left.lane < right.lane;
            }
            return lhs < rhs;
        });

        const auto outsideGroup = notesExceptGroup(notes, indices);
        std::set<int> usedLanes;
        for (std::size_t rank = 0; rank < indices.size(); ++rank) {
            const int noteIndex = indices[rank];
            const auto& note = notes[static_cast<std::size_t>(noteIndex)];
            const auto lanes = randomLaneOrder(superRandomNoteSeed(note,
                                                                   noteIndex,
                                                                   static_cast<int>(rank),
                                                                   options),
                                               options.targetKeyCount,
                                               note.lane);
            const auto lane = safeSuperRandomLaneForNote(notes,
                                                         outsideGroup,
                                                         noteIndex,
                                                         lanes,
                                                         usedLanes);
            if (lane.has_value()) {
                usedLanes.insert(*lane);
                notes[static_cast<std::size_t>(noteIndex)].lane = *lane;
                ++transformed;
            } else {
                usedLanes.insert(note.lane);
            }
        }
    }

    return transformed;
}

std::vector<int> jitterOffsetPool(int time) {
    std::vector<int> offsets;
    offsets.reserve(30);
    const int direction = ((std::max(0, time) / 500) % 2 == 0) ? 1 : -1;
    for (int magnitude = 1; magnitude <= 15; ++magnitude) {
        offsets.push_back(direction * magnitude);
        offsets.push_back(-direction * magnitude);
    }
    return offsets;
}

int applyFullJitter(std::vector<Note>& notes) {
    std::map<int, std::vector<int>> groups;
    for (int index = 0; index < static_cast<int>(notes.size()); ++index) {
        groups[notes[static_cast<std::size_t>(index)].time].push_back(index);
    }

    int jittered = 0;
    for (auto& [time, indices] : groups) {
        std::stable_sort(indices.begin(), indices.end(), [&](int lhs, int rhs) {
            const auto& left = notes[static_cast<std::size_t>(lhs)];
            const auto& right = notes[static_cast<std::size_t>(rhs)];
            if (left.lane != right.lane) {
                return left.lane < right.lane;
            }
            return lhs < rhs;
        });

        const auto offsets = jitterOffsetPool(time);
        std::uint32_t hash = static_cast<std::uint32_t>(time);
        hash ^= static_cast<std::uint32_t>(indices.size() + 1) * 0x9e3779b9U;
        const int rotation = static_cast<int>(mixHash(hash) % static_cast<std::uint32_t>(offsets.size()));
        for (std::size_t rank = 0; rank < indices.size(); ++rank) {
            int offset = offsets[(rank + static_cast<std::size_t>(rotation)) % offsets.size()];
            if (time + offset < 0) {
                offset = std::abs(offset);
            }
            const int noteIndex = indices[rank];
            const int newTime = time + offset;
            if (newTime != notes[static_cast<std::size_t>(noteIndex)].time) {
                retimeNotePreserveDuration(notes[static_cast<std::size_t>(noteIndex)], newTime);
                ++jittered;
            }
        }
    }

    return jittered;
}

StreamTransformStats applyStreamTransform(std::vector<Note>& notes,
                                           const ConvertOptions& options,
                                           bool allowJitter) {
    StreamTransformStats stats;
    if (options.streamTransformPolicy == StreamTransformPolicy::SuperRandom && !allowJitter) {
        stats.transformedNotes = applyStreamSuperRandom(notes, options);
        if (stats.transformedNotes > 0) {
            notes = sortedNotes(std::move(notes));
        }
    } else if (options.streamTransformPolicy == StreamTransformPolicy::FullJitter && allowJitter) {
        stats.jitteredNotes = applyFullJitter(notes);
        if (stats.jitteredNotes > 0) {
            notes = sortedNotes(std::move(notes));
        }
    }
    return stats;
}

std::string trimString(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string stripExistingKeyWeaverMarker(std::string value) {
    const auto markerPos = value.find("KeyWeaver");
    if (markerPos == std::string::npos) {
        return trimString(value);
    }
    return trimString(value.substr(0, markerPos));
}

std::string difficultyExpansionTag(const ConvertOptions& options) {
    if (options.targetKeyCount <= options.sourceKeyCount) {
        return {};
    }
    switch (options.expansionPolicy) {
        case ExpansionPolicy::PreserveTapPlusMore:
            return "more";
        case ExpansionPolicy::PreserveTapPlus:
            return "normal";
        case ExpansionPolicy::PreserveTapPlusLow:
            return "low";
        default:
            return {};
    }
}

std::string difficultyStreamTag(StreamTransformPolicy policy) {
    switch (policy) {
        case StreamTransformPolicy::SuperRandom:
            return "sRan";
        case StreamTransformPolicy::FullJitter:
            return "jitter";
        case StreamTransformPolicy::Off:
            return {};
    }
    return {};
}

std::string conversionDifficultyMarker(const ConvertOptions& options) {
    std::string marker = "KeyWeaver" + std::to_string(options.targetKeyCount) + "K";
    const auto streamTag = difficultyStreamTag(options.streamTransformPolicy);
    if (!streamTag.empty()) {
        marker += "-";
        marker += streamTag;
    }
    const auto expansionTag = difficultyExpansionTag(options);
    if (!expansionTag.empty()) {
        marker += " (";
        marker += expansionTag;
        marker += ")";
    }
    return marker;
}

std::string convertedDifficultyName(const std::optional<std::string>& existing,
                                    const ConvertOptions& options) {
    const auto marker = conversionDifficultyMarker(options);
    if (!existing.has_value() || existing->empty()) {
        return marker;
    }
    const auto baseName = stripExistingKeyWeaverMarker(*existing);
    if (baseName.empty()) {
        return marker;
    }
    return baseName + " " + marker;
}

std::optional<std::pair<int, int>> firstNearTimePair(const std::vector<Note>& notes, const ConvertOptions& options) {
    if (!distancePolicyRejects(options.distancePolicy)) {
        return std::nullopt;
    }

    const int minObjectGap = std::max(0, options.minObjectGapMs);
    if (minObjectGap <= 0) {
        return std::nullopt;
    }

    std::vector<int> order(notes.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int lhs, int rhs) {
        const auto& lhsNote = notes[static_cast<std::size_t>(lhs)];
        const auto& rhsNote = notes[static_cast<std::size_t>(rhs)];
        if (lhsNote.time != rhsNote.time) {
            return lhsNote.time < rhsNote.time;
        }
        if (lhsNote.lane != rhsNote.lane) {
            return lhsNote.lane < rhsNote.lane;
        }
        return lhs < rhs;
    });

    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& first = notes[static_cast<std::size_t>(order[i])];
        for (std::size_t j = i + 1; j < order.size(); ++j) {
            const auto& second = notes[static_cast<std::size_t>(order[j])];
            const int delta = second.time - first.time;
            if (delta == 0) {
                continue;
            }
            if (delta >= minObjectGap) {
                break;
            }
            return std::make_pair(order[i], order[j]);
        }
    }
    return std::nullopt;
}

DistanceSanitizerStats sanitizeNearTimeOverlaps(const Chart& original,
                                                std::vector<Note>& notes,
                                                const ConvertOptions& options) {
    DistanceSanitizerStats stats;
    if (options.targetKeyCount <= options.sourceKeyCount || !distancePolicyRejects(options.distancePolicy)) {
        return stats;
    }

    int guard = 0;
    while (guard++ < 512) {
        const auto pair = firstNearTimePair(notes, options);
        if (!pair.has_value()) {
            return stats;
        }

        const int firstIndex = pair->first;
        const int secondIndex = pair->second;
        const int firstTime = notes[static_cast<std::size_t>(firstIndex)].time;
        const int secondTime = notes[static_cast<std::size_t>(secondIndex)].time;

        if (retimeCandidateIsSafe(original, notes, secondIndex, firstTime, options)) {
            retimeNotePreserveDuration(notes[static_cast<std::size_t>(secondIndex)], firstTime);
            ++stats.collapsedNearTimePairs;
            continue;
        }
        if (retimeCandidateIsSafe(original, notes, firstIndex, secondTime, options)) {
            retimeNotePreserveDuration(notes[static_cast<std::size_t>(firstIndex)], secondTime);
            ++stats.collapsedNearTimePairs;
            continue;
        }

        return stats;
    }
    return stats;
}

void removeResolvedDistanceWarnings(ConversionReport& report, const DistanceValidationResult& distance) {
    if (distance.nearTimeConflicts > 0 || distance.sameLaneNearConflicts > 0) {
        return;
    }

    report.warnings.erase(std::remove_if(report.warnings.begin(),
                                         report.warnings.end(),
                                         [](const std::string& warning) {
                                             return warning.find("Distance guard validation failed:") !=
                                                    std::string::npos;
                                         }),
                          report.warnings.end());
}

void fillReportCounts(ConversionReport& report, const std::vector<Note>& notes, int targetKeyCount) {
    report.totalNotes = static_cast<int>(notes.size());
    report.tapNotes = 0;
    report.holdNotes = 0;
    for (const auto& note : notes) {
        if (note.type == NoteType::Hold) {
            ++report.holdNotes;
        } else {
            ++report.tapNotes;
        }
    }

    const auto scan = detectCollisions(notes);
    report.sameTimeCollisions = scan.sameTimeCollisions;
    report.longNoteConflicts = scan.longNoteConflicts;
    report.laneDistribution = calculateLaneDistribution(notes, targetKeyCount);
}

}  // namespace

ConvertResult convertChart(const Chart& chart, const ConvertOptions& options) {
    ConvertResult result;
    result.chart = chart;
    result.chart.meta.targetKeyCount = options.targetKeyCount;
    CompressionPlanStats compressionStats;
    StreamTransformStats streamTransformStats;
    int preventedJacksByAssignment = 0;
    int preventedJacksByRepair = 0;
    int createdJacksFromBaseMapping = 0;
    int createdJacksFromAddedNotes = 0;

    result.report.sourceKeyCount = options.sourceKeyCount;
    result.report.targetKeyCount = options.targetKeyCount;
    result.report.warnings = chart.warnings;

    std::vector<Note> placed;
    if (options.style == ConversionStyle::Direct) {
        std::vector<int> laneUse(static_cast<std::size_t>(options.targetKeyCount), 0);

        int nextId = 0;
        for (auto note : sortedNotes(chart.notes)) {
            note.sourceLane = note.sourceLane.value_or(note.lane);
            const auto candidates = lanesForStyle(note, options);
            const int mappedLane =
                chooseBestLane(note, candidates, placed, laneUse, options, &preventedJacksByAssignment);
            note.lane = clampInt(mappedLane, 0, options.targetKeyCount - 1);
            if (note.id.empty()) {
                note.id = "n" + std::to_string(nextId);
            }
            ++nextId;
            ++laneUse[static_cast<std::size_t>(note.lane)];
            placed.push_back(note);
        }
    } else {
        auto optimized = greedyOptimizeSlices(chart, options);
        result.report.shiftedNotes += optimized.localRepairShifted;
        preventedJacksByAssignment += optimized.preventedJacksByAssignment;
        preventedJacksByRepair += optimized.preventedJacksByRepair;
        placed = std::move(optimized.notes);
    }

    createdJacksFromBaseMapping =
        static_cast<int>(detectCreatedJackPairs(chart, placed, options.jackWindowMs).size());

    result.chart.notes = placed;
    auto expansionStats = applyExpansionPlanner(result.chart, chart, options);
    placed = std::move(result.chart.notes);
    result.report.warnings.insert(result.report.warnings.end(),
                                  expansionStats.warnings.begin(),
                                  expansionStats.warnings.end());

    compressionStats = applyCompressPlanner(placed, options, chart.timingPoints, &chart);
    result.report.droppedNotes += compressionStats.droppedByCompression;
    result.report.warnings.insert(result.report.warnings.end(),
                                  compressionStats.warnings.begin(),
                                  compressionStats.warnings.end());

    {
        const auto stats = applyStreamTransform(placed, options, false);
        streamTransformStats.transformedNotes += stats.transformedNotes;
        streamTransformStats.jitteredNotes += stats.jitteredNotes;
    }

    applyCollisionPolicy(placed,
                         options.targetKeyCount,
                         options.collisionPolicy,
                         options.jackWindowMs,
                         result.report,
                         &preventedJacksByRepair);

    const auto createdPairsAfterRepair = detectCreatedJackPairs(chart, placed, options.jackWindowMs);
    for (const auto& pair : createdPairsAfterRepair) {
        if (pair.involvesGenerated) {
            ++createdJacksFromAddedNotes;
        }
    }
    JackSanitizerStats sanitizerStats;
    const bool canRelaneCreatedJacks = true;
    if (canRelaneCreatedJacks) {
        sanitizerStats = sanitizeCreatedJacks(chart, placed, options);
    } else {
        sanitizerStats.unsolvedCreatedJacks = static_cast<int>(createdPairsAfterRepair.size());
    }
    result.report.droppedNotes += sanitizerStats.droppedNotes;
    const auto distanceSanitizerStats = sanitizeNearTimeOverlaps(chart, placed, options);
    if (distanceSanitizerStats.collapsedNearTimePairs > 0) {
        placed = sortedNotes(std::move(placed));
    }
    {
        const auto stats = applyStreamTransform(placed, options, true);
        streamTransformStats.transformedNotes += stats.transformedNotes;
        streamTransformStats.jitteredNotes += stats.jitteredNotes;
    }

    result.chart.notes = std::move(placed);
    result.chart.meta.version = convertedDifficultyName(chart.meta.version, options);
    fillReportCounts(result.report, result.chart.notes, options.targetKeyCount);
    result.report.quality = computeQualityReport(chart, result.chart, options.sourceKeyCount, options.targetKeyCount);
    const auto finalNoOverlap = validateNoOverlap(result.chart.notes, options.targetKeyCount);
    const auto finalDistance =
        validateDistance(result.chart.notes, options, chart.timingPoints, compressionStats.rolledNoteIds);
    removeResolvedDistanceWarnings(result.report, finalDistance);
    const int maxJackSplitLanes =
        options.allowPlayableJackSplit && options.jackPreservePolicy != JackPreservePolicy::PreserveStrict
            ? options.maxJackSplitLanes
            : 1;
    const auto jackValidation = validateJackPreservation(chart, result.chart, options.jackWindowMs, maxJackSplitLanes);
    result.report.quality.jackPreservePolicy = toString(options.jackPreservePolicy);
    result.report.quality.sourceJackGroups = jackValidation.sourceJackGroups;
    result.report.quality.preservedJackGroups = jackValidation.preservedJackGroups;
    result.report.quality.splitJackGroups = jackValidation.splitJackGroups;
    result.report.quality.createdJacks = jackValidation.createdJacks;
    result.report.quality.createdJacksFromBaseMapping = createdJacksFromBaseMapping;
    result.report.quality.createdJacksFromRepair = 0;
    result.report.quality.createdJacksFromAddedNotes = createdJacksFromAddedNotes;
    result.report.quality.preventedJacksByAssignment = preventedJacksByAssignment;
    result.report.quality.preventedJacksByRepair = preventedJacksByRepair;
    result.report.quality.preventedJacksByExpansion = expansionStats.preventedJacks;
    result.report.quality.sanitizedCreatedJacks = sanitizerStats.sanitizedCreatedJacks;
    result.report.quality.unsolvedCreatedJacks = sanitizerStats.unsolvedCreatedJacks;
    result.report.quality.smoothedJacks = jackValidation.smoothedJacks;
    result.report.quality.jackPreserveScore = jackValidation.jackPreserveScore;
    result.report.quality.createdJackRate = jackValidation.createdJackRate;
    const auto gestureReport = evaluateGesturePreservation(chart,
                                                           result.chart,
                                                           options.sourceKeyCount,
                                                           options.targetKeyCount,
                                                           options.sameTimeEpsilonMs,
                                                           options.gestureRailEnabled);
    result.report.quality.detectedStairs = gestureReport.detectedStairs;
    result.report.quality.preservedStairs = gestureReport.preservedStairs;
    result.report.quality.brokenStairs = gestureReport.brokenStairs;
    result.report.quality.detectedTrills = gestureReport.detectedTrills;
    result.report.quality.preservedTrills = gestureReport.preservedTrills;
    result.report.quality.brokenTrills = gestureReport.brokenTrills;
    result.report.quality.detectedJacks = gestureReport.detectedJacks;
    result.report.quality.preservedJacks = gestureReport.preservedJacks;
    result.report.quality.brokenJacks = gestureReport.brokenJacks;
    result.report.quality.handZoneBreaks = gestureReport.handZoneBreaks;
    result.report.quality.motifDirectionFlips = gestureReport.motifDirectionFlips;
    result.report.quality.motifLaneScatterCount = gestureReport.motifLaneScatterCount;
    result.report.quality.gesturePreservationScore = gestureReport.gesturePreservationScore;
    result.report.quality.gestureRailEnabled = gestureReport.gestureRailEnabled;
    result.report.quality.impossibleSlices =
        std::max(result.report.quality.impossibleSlices,
                 std::max(compressionStats.impossibleSlices, finalNoOverlap.impossibleSlices));
    result.report.quality.droppedByCompression = compressionStats.droppedByCompression;
    result.report.quality.rolledByCompression = compressionStats.rolledByCompression;
    result.report.quality.shortenedHolds = compressionStats.shortenedHolds;
    result.report.quality.tapifiedHolds = compressionStats.tapifiedHolds;
    result.report.quality.noOverlapGuaranteed = finalNoOverlap.noOverlapGuaranteed;
    result.report.quality.nearTimeConflicts = finalDistance.nearTimeConflicts;
    result.report.quality.sameLaneNearConflicts = finalDistance.sameLaneNearConflicts;
    result.report.quality.unsnappedNotes = finalDistance.unsnappedNotes;
    result.report.quality.unsnappedRolledNotes = finalDistance.unsnappedRolledNotes;
    result.report.quality.minPositiveDeltaMs = finalDistance.minPositiveDeltaMs;
    result.report.quality.droppedByDistanceGuard = compressionStats.droppedByDistanceGuard;
    result.report.quality.rerolledByDistanceGuard =
        compressionStats.rerolledByDistanceGuard + distanceSanitizerStats.collapsedNearTimePairs;
    result.report.quality.deterministic = expansionStats.deterministic;
    result.report.quality.algorithmVersion = expansionStats.algorithmVersion;
    result.report.quality.expansionPolicy = toString(expansionStats.policy);
    result.report.quality.streamEchoProfile = toString(expansionStats.streamEchoProfile);
    result.report.quality.streamTransformPolicy = toString(options.streamTransformPolicy);
    result.report.quality.streamTransformedNotes = streamTransformStats.transformedNotes;
    result.report.quality.streamJitteredNotes = streamTransformStats.jitteredNotes;
    result.report.quality.addedNotes = expansionStats.addedNotes;
    result.report.quality.addedByTapPlus = expansionStats.addedByTapPlus;
    result.report.quality.addedByChordFill = expansionStats.addedByChordFill;
    result.report.quality.addedByEcho = expansionStats.addedByEcho;
    result.report.quality.addedByStairEcho = expansionStats.addedByStairEcho;
    result.report.quality.addedByTrillEcho = expansionStats.addedByTrillEcho;
    result.report.quality.addedByStreamEcho = expansionStats.addedByStreamEcho;
    result.report.quality.addedByTrainingScaffold = expansionStats.addedByTrainingScaffold;
    result.report.quality.rejectedExpansionCandidates = expansionStats.rejectedExpansionCandidates;
    result.report.quality.rejectedByCollision = expansionStats.rejectedByCollision;
    result.report.quality.rejectedByDistance = expansionStats.rejectedByDistance;
    result.report.quality.rejectedBySnap = expansionStats.rejectedBySnap;
    result.report.quality.rejectedByBudget = expansionStats.rejectedByBudget;
    result.report.quality.unsnappedAddedNotes = expansionStats.unsnappedAddedNotes;
    result.report.quality.addedNoteRatio = expansionStats.addedNoteRatio;
    result.report.quality.preventedJacks =
        preventedJacksByAssignment + preventedJacksByRepair + expansionStats.preventedJacks;
    result.report.quality.expansionComposerProfile = expansionStats.expansionComposerProfile;
    result.report.quality.targetAddedNoteRatio = expansionStats.targetAddedNoteRatio;
    result.report.quality.budgetUsedRatio = expansionStats.budgetUsedRatio;
    result.report.quality.adaptiveGrowthBudgetEnabled = expansionStats.adaptiveGrowthBudgetEnabled;
    result.report.quality.adaptiveBudgetWindowMs = expansionStats.adaptiveBudgetWindowMs;
    result.report.quality.adaptiveBudgetWindows = expansionStats.adaptiveBudgetWindows;
    result.report.quality.adaptiveBudgetAverageRatio = expansionStats.adaptiveBudgetAverageRatio;
    result.report.quality.adaptiveBudgetMinRatio = expansionStats.adaptiveBudgetMinRatio;
    result.report.quality.adaptiveBudgetMaxRatio = expansionStats.adaptiveBudgetMaxRatio;
    result.report.quality.acceptedByComposer = expansionStats.acceptedByComposer;
    result.report.quality.rejectedByComposerBudget = expansionStats.rejectedByComposerBudget;
    result.report.quality.rejectedByComposerSafety = expansionStats.rejectedByComposerSafety;
    result.report.quality.rejectedByAdaptiveBudget = expansionStats.rejectedByAdaptiveBudget;
    result.report.quality.rejectedEchoCandidates = expansionStats.rejectedEchoCandidates;
    result.report.quality.rejectedEchoByDensity = expansionStats.rejectedEchoByDensity;
    result.report.quality.rejectedEchoByDistance = expansionStats.rejectedEchoByDistance;
    result.report.quality.rejectedEchoBySnap = expansionStats.rejectedEchoBySnap;
    result.report.quality.rejectedEchoByBudget = expansionStats.rejectedEchoByBudget;
    result.report.quality.echoAddedRatio = expansionStats.echoAddedRatio;
    result.report.quality.rejectedStreamEchoByBurst = expansionStats.rejectedStreamEchoByBurst;
    result.report.quality.rejectedStreamEchoByJack = expansionStats.rejectedStreamEchoByJack;
    result.report.quality.rejectedStreamEchoByLNHeavy = expansionStats.rejectedStreamEchoByLNHeavy;
    result.report.quality.rejectedStreamEchoByLocalNps = expansionStats.rejectedStreamEchoByLocalNps;
    result.report.quality.streamEchoCandidates = expansionStats.streamEchoCandidates;
    result.report.quality.streamRawPatternCandidates = expansionStats.streamRawPatternCandidates;
    result.report.quality.streamEligiblePatternCandidates = expansionStats.streamEligiblePatternCandidates;
    result.report.quality.streamRawLaneCandidates = expansionStats.streamRawLaneCandidates;
    result.report.quality.streamSafeLaneCandidates = expansionStats.streamSafeLaneCandidates;
    result.report.quality.streamAcceptedCandidates = expansionStats.streamAcceptedCandidates;
    result.report.quality.rejectedStreamEchoByNoUnderusedLane =
        expansionStats.rejectedStreamEchoByNoUnderusedLane;
    result.report.quality.rejectedStreamEchoByPatternConfidence =
        expansionStats.rejectedStreamEchoByPatternConfidence;
    result.report.quality.rejectedStreamEchoByPatternLength =
        expansionStats.rejectedStreamEchoByPatternLength;
    result.report.quality.rejectedStreamEchoBySliceChordFull =
        expansionStats.rejectedStreamEchoBySliceChordFull;
    result.report.quality.rejectedStreamEchoByLaneRole = expansionStats.rejectedStreamEchoByLaneRole;
    result.report.quality.rejectedStreamPrimaryByPatternConfidence =
        expansionStats.rejectedStreamPrimaryByPatternConfidence;
    result.report.quality.rejectedStreamPrimaryByPatternLength =
        expansionStats.rejectedStreamPrimaryByPatternLength;
    result.report.quality.rejectedStreamPrimaryByBurst = expansionStats.rejectedStreamPrimaryByBurst;
    result.report.quality.rejectedStreamPrimaryByJack = expansionStats.rejectedStreamPrimaryByJack;
    result.report.quality.rejectedStreamPrimaryByLNHeavy = expansionStats.rejectedStreamPrimaryByLNHeavy;
    result.report.quality.rejectedStreamPrimaryByLocalNps = expansionStats.rejectedStreamPrimaryByLocalNps;
    result.report.quality.rejectedStreamPrimaryByNoUnderusedLane =
        expansionStats.rejectedStreamPrimaryByNoUnderusedLane;
    result.report.quality.rejectedStreamPrimaryBySliceChordFull =
        expansionStats.rejectedStreamPrimaryBySliceChordFull;
    result.report.quality.rejectedStreamPrimaryByLaneRole = expansionStats.rejectedStreamPrimaryByLaneRole;
    result.report.quality.rejectedStreamPrimaryByCollision = expansionStats.rejectedStreamPrimaryByCollision;
    result.report.quality.rejectedStreamPrimaryByDistance = expansionStats.rejectedStreamPrimaryByDistance;
    result.report.quality.rejectedStreamPrimaryBySnap = expansionStats.rejectedStreamPrimaryBySnap;
    result.report.quality.rejectedStreamPrimaryByBudget = expansionStats.rejectedStreamPrimaryByBudget;
    result.report.quality.streamEchoAddedRatio = expansionStats.streamEchoAddedRatio;
    result.report.quality.maxObservedLocalNpsAfterEcho = expansionStats.maxObservedLocalNpsAfterEcho;
    finalizeTargetKLikenessReport(result.report.quality,
                                  chart,
                                  result.chart,
                                  options.sourceKeyCount,
                                  options.targetKeyCount,
                                  options.targetKProfile.has_value() ? &*options.targetKProfile : nullptr);
    return result;
}

}  // namespace keyconv
