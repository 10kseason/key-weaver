#include "core/repeat.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace keyconv {

namespace {

int laneForMode(const Note& note, RepeatLaneMode laneMode) {
    return laneMode == RepeatLaneMode::SourceLane ? note.sourceLane.value_or(note.lane) : note.lane;
}

void maybeAddGroup(std::vector<JackGroup>& groups,
                   const std::vector<const Note*>& run,
                   int lane,
                   int& nextId) {
    if (run.size() < 3) {
        return;
    }

    int minGap = 0;
    int totalGap = 0;
    for (std::size_t i = 1; i < run.size(); ++i) {
        const int gap = run[i]->time - run[i - 1]->time;
        if (gap <= 0) {
            return;
        }
        minGap = minGap == 0 ? gap : std::min(minGap, gap);
        totalGap += gap;
    }

    JackGroup group;
    group.id = nextId++;
    group.lane = lane;
    group.startTimeMs = run.front()->time;
    group.endTimeMs = run.back()->time;
    group.hitCount = static_cast<int>(run.size());
    group.avgGapMs = static_cast<double>(totalGap) / static_cast<double>(std::max<std::size_t>(1, run.size() - 1));
    group.minGapMs = minGap;
    for (const auto* note : run) {
        group.noteIds.push_back(note->id);
    }
    groups.push_back(std::move(group));
}

bool idLooksGenerated(const std::string& id) {
    return id.rfind("gen:", 0) == 0;
}

std::map<std::string, const Note*> notesById(const Chart& chart) {
    std::map<std::string, const Note*> byId;
    for (const auto& note : chart.notes) {
        if (!note.id.empty()) {
            byId[note.id] = &note;
        }
    }
    return byId;
}

const Note* sourceNoteFor(const std::map<std::string, const Note*>& sourceById, const Note& note) {
    if (note.id.empty() || idLooksGenerated(note.id)) {
        return nullptr;
    }
    const auto found = sourceById.find(note.id);
    return found == sourceById.end() ? nullptr : found->second;
}

bool sourceJackIntentFromMap(const std::map<std::string, const Note*>& sourceById,
                             const Note& first,
                             const Note& second,
                             int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0 || idLooksGenerated(first.id) || idLooksGenerated(second.id)) {
        return false;
    }

    const Note* sourceFirst = sourceNoteFor(sourceById, first);
    const Note* sourceSecond = sourceNoteFor(sourceById, second);
    const int firstLane = sourceFirst != nullptr ? sourceFirst->sourceLane.value_or(sourceFirst->lane)
                                                 : first.sourceLane.value_or(first.lane);
    const int secondLane = sourceSecond != nullptr ? sourceSecond->sourceLane.value_or(sourceSecond->lane)
                                                   : second.sourceLane.value_or(second.lane);
    const int firstTime = sourceFirst != nullptr ? sourceFirst->time : first.time;
    const int secondTime = sourceSecond != nullptr ? sourceSecond->time : second.time;
    const int dt = std::abs(secondTime - firstTime);
    return firstLane == secondLane && dt > 0 && dt <= window;
}

}  // namespace

bool isGeneratedNoteId(const std::string& id) {
    return idLooksGenerated(id);
}

std::vector<JackGroup> detectJackGroups(const std::vector<Note>& notes,
                                        RepeatLaneMode laneMode,
                                        int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0 || notes.size() < 3) {
        return {};
    }

    std::map<int, std::vector<const Note*>> byLane;
    for (std::size_t i = 0; i < notes.size(); ++i) {
        byLane[laneForMode(notes[i], laneMode)].push_back(&notes[i]);
    }

    std::vector<JackGroup> groups;
    int nextId = 0;
    for (auto& entry : byLane) {
        auto& laneNotes = entry.second;
        std::stable_sort(laneNotes.begin(), laneNotes.end(), [](const Note* lhs, const Note* rhs) {
            if (lhs->time != rhs->time) {
                return lhs->time < rhs->time;
            }
            return lhs->id < rhs->id;
        });

        std::vector<const Note*> run;
        for (const auto* note : laneNotes) {
            if (run.empty()) {
                run.push_back(note);
                continue;
            }

            const int gap = note->time - run.back()->time;
            if (gap > 0 && gap <= window) {
                run.push_back(note);
            } else {
                maybeAddGroup(groups, run, entry.first, nextId);
                run.clear();
                run.push_back(note);
            }
        }
        maybeAddGroup(groups, run, entry.first, nextId);
    }

    return groups;
}

bool wouldCreateJackOnLane(const std::vector<Note>& notes,
                           const Note& candidate,
                           int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0) {
        return false;
    }

    std::vector<int> times;
    times.reserve(notes.size() + 1);
    for (const auto& note : notes) {
        if (note.lane == candidate.lane) {
            const int dt = std::abs(note.time - candidate.time);
            if (dt > 0 && dt <= window) {
                return true;
            }
            times.push_back(note.time);
        }
    }
    times.push_back(candidate.time);
    std::stable_sort(times.begin(), times.end());

    int runLength = 1;
    bool runIncludesCandidate = times.front() == candidate.time;
    for (std::size_t i = 1; i < times.size(); ++i) {
        const int gap = times[i] - times[i - 1];
        if (gap > 0 && gap <= window) {
            ++runLength;
            runIncludesCandidate = runIncludesCandidate || times[i] == candidate.time;
        } else {
            if (runIncludesCandidate && runLength >= 3) {
                return true;
            }
            runLength = 1;
            runIncludesCandidate = times[i] == candidate.time;
        }
    }

    return runIncludesCandidate && runLength >= 3;
}

bool isSourceJackIntent(const Chart& original,
                        const Note& first,
                        const Note& second,
                        int jackWindowMs) {
    return sourceJackIntentFromMap(notesById(original), first, second, jackWindowMs);
}

std::vector<CreatedJackPair> detectCreatedJackPairs(const Chart& original,
                                                    const std::vector<Note>& converted,
                                                    int jackWindowMs) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0 || converted.size() < 2) {
        return {};
    }

    const auto sourceById = notesById(original);
    std::map<int, std::vector<int>> byLane;
    for (std::size_t i = 0; i < converted.size(); ++i) {
        byLane[converted[i].lane].push_back(static_cast<int>(i));
    }

    std::vector<CreatedJackPair> pairs;
    for (auto& entry : byLane) {
        auto& indices = entry.second;
        std::stable_sort(indices.begin(), indices.end(), [&](int lhs, int rhs) {
            const auto& lhsNote = converted[static_cast<std::size_t>(lhs)];
            const auto& rhsNote = converted[static_cast<std::size_t>(rhs)];
            if (lhsNote.time != rhsNote.time) {
                return lhsNote.time < rhsNote.time;
            }
            return lhsNote.id < rhsNote.id;
        });

        for (std::size_t i = 1; i < indices.size(); ++i) {
            const auto& first = converted[static_cast<std::size_t>(indices[i - 1])];
            const auto& second = converted[static_cast<std::size_t>(indices[i])];
            const int dt = second.time - first.time;
            if (dt <= 0 || dt > window) {
                continue;
            }
            if (sourceJackIntentFromMap(sourceById, first, second, window)) {
                continue;
            }

            CreatedJackPair pair;
            pair.firstIndex = indices[i - 1];
            pair.secondIndex = indices[i];
            pair.lane = entry.first;
            pair.firstTimeMs = first.time;
            pair.secondTimeMs = second.time;
            pair.firstId = first.id;
            pair.secondId = second.id;
            pair.involvesGenerated = idLooksGenerated(first.id) || idLooksGenerated(second.id);
            pairs.push_back(std::move(pair));
        }
    }
    return pairs;
}

bool wouldCreateCreatedJackOnLane(const Chart& original,
                                  const std::vector<Note>& converted,
                                  const Note& candidate,
                                  int candidateLane,
                                  int jackWindowMs,
                                  int ignoredIndex) {
    const int window = std::max(0, jackWindowMs);
    if (window <= 0) {
        return false;
    }

    const auto sourceById = notesById(original);
    Note moved = candidate;
    moved.lane = candidateLane;
    for (std::size_t i = 0; i < converted.size(); ++i) {
        if (static_cast<int>(i) == ignoredIndex) {
            continue;
        }
        const auto& note = converted[i];
        if (note.lane != candidateLane) {
            continue;
        }
        const int dt = std::abs(note.time - moved.time);
        if (dt > 0 && dt <= window && !sourceJackIntentFromMap(sourceById, note, moved, window)) {
            return true;
        }
    }
    return false;
}

JackValidationResult validateJackPreservation(const Chart& original,
                                              const Chart& converted,
                                              int jackWindowMs,
                                              int maxSplitLanes) {
    JackValidationResult result;
    const auto sourceGroups = detectJackGroups(original.notes, RepeatLaneMode::SourceLane, jackWindowMs);
    result.sourceJackGroups = static_cast<int>(sourceGroups.size());

    std::map<std::string, const Note*> convertedById;
    for (const auto& note : converted.notes) {
        if (!note.id.empty() && !idLooksGenerated(note.id)) {
            convertedById[note.id] = &note;
        }
    }

    std::map<std::string, int> sourceGroupById;
    for (const auto& group : sourceGroups) {
        std::set<int> targetLanes;
        bool complete = true;
        for (const auto& id : group.noteIds) {
            sourceGroupById[id] = group.id;
            const auto found = convertedById.find(id);
            if (found == convertedById.end()) {
                complete = false;
                continue;
            }
            targetLanes.insert(found->second->lane);
        }

        if (!complete || targetLanes.empty()) {
            ++result.smoothedJacks;
        } else if (targetLanes.size() == 1) {
            ++result.preservedJackGroups;
        } else if (static_cast<int>(targetLanes.size()) <= std::max(1, maxSplitLanes)) {
            ++result.splitJackGroups;
        } else {
            ++result.smoothedJacks;
        }
    }

    result.createdJacks =
        static_cast<int>(detectCreatedJackPairs(original, converted.notes, jackWindowMs).size());

    const auto targetGroups = detectJackGroups(converted.notes, RepeatLaneMode::TargetLane, jackWindowMs);
    const int targetRepeatCount = std::max(static_cast<int>(targetGroups.size()), result.createdJacks);
    result.createdJackRate = targetRepeatCount == 0
                                 ? 0.0
                                 : static_cast<double>(result.createdJacks) / static_cast<double>(targetRepeatCount);
    if (result.sourceJackGroups == 0) {
        result.jackPreserveScore = result.createdJacks == 0 ? 1.0 : std::max(0.0, 1.0 - result.createdJackRate);
    } else {
        const double preserved =
            static_cast<double>(result.preservedJackGroups) + static_cast<double>(result.splitJackGroups) * 0.75;
        const double base = preserved / static_cast<double>(result.sourceJackGroups);
        result.jackPreserveScore = std::max(0.0, std::min(1.0, base - result.createdJackRate * 0.25));
    }
    return result;
}

}  // namespace keyconv
