#include "nk2/nk2_convert.hpp"

#include "nk2/intent_graph.hpp"
#include "nk2/layout_model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace keyconv::nk2 {

namespace {

struct SliceInfo {
    int time = 0;
    std::vector<std::size_t> noteIndices;
};

struct PlacedNote {
    int time = 0;
    int lane = 0;
    int sourceLane = 0;
    NoteType type = NoteType::Tap;
    std::optional<int> endTime;
};

struct Candidate {
    int lane = 0;
    double score = 0.0;
};

constexpr double kFourToFiveFillAddedRatio = 0.12;
constexpr double kFourToFiveFillPhraseRatio = 0.30;
constexpr int kFourToFiveFillMinimumBudget = 8;
constexpr int kFourToFiveFillMinimumPhraseBudget = 3;
constexpr int kFourToFiveFillMaximumPhraseBudget = 12;
constexpr int kDefaultSupportJackWindowMs = 500;
constexpr int kFourToFiveSupportJackWindowMs = 240;
constexpr int kDefaultSupportSameSourceGapMs = 120;
constexpr int kFourToFiveSupportSameSourceGapMs = 80;

enum class MotifKind {
    Neutral,
    Jack,
    Trill,
    Stair,
    Stream,
    Chord,
    LnAnchor,
};

struct RecentSingleNote {
    int time = 0;
    int sourceLane = 0;
    int targetLane = 0;
};

enum class SupportKind {
    Ln,
    StrongBeat,
    Mirror,
};

struct SupportEvent {
    SupportKind kind = SupportKind::Ln;
    MotifKind anchorMotif = MotifKind::Neutral;
    int time = 0;
    int sourceLane = 0;
    int anchorLane = 0;
    std::string anchorId;
};

struct PlacementStats {
    int sameTimeCollisions = 0;
    int longNoteConflicts = 0;
    int createdJacks = 0;
    int preservedSourceJacks = 0;
    int sourceAnchorMatches = 0;
    int sourceAnchorTotal = 0;
    std::vector<int> laneDistribution;
};

int sourceLaneOf(const Note& note) {
    return note.sourceLane.value_or(note.lane);
}

int clampInt(int value, int lower, int upper) {
    return std::max(lower, std::min(upper, value));
}

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

bool isFourToFiveFillOptions(const NK2Options& options) {
    return options.sourceKeyCount == 4 && options.targetKeyCount == 5 && options.mode != Mode::Report;
}

int supportJackWindowMsFor(const NK2Options& options) {
    return isFourToFiveFillOptions(options) ? kFourToFiveSupportJackWindowMs
                                           : kDefaultSupportJackWindowMs;
}

int directLane(int sourceLane, int sourceKeyCount, int targetKeyCount) {
    if (sourceKeyCount <= 1 || targetKeyCount <= 1) {
        return 0;
    }
    const double scaled = static_cast<double>(sourceLane) * static_cast<double>(targetKeyCount - 1) /
                          static_cast<double>(sourceKeyCount - 1);
    return clampInt(static_cast<int>(std::round(scaled)), 0, targetKeyCount - 1);
}

bool isNk2GeneratedNote(const Note& note) {
    return note.id.rfind("nk2-", 0) == 0;
}

std::string convertedDifficultyName(const std::optional<std::string>& existing, int targetKeyCount) {
    const std::string marker = "KeyWeaverNK2-" + std::to_string(targetKeyCount) + "K";
    if (!existing.has_value() || existing->empty()) {
        return marker;
    }
    if (existing->find("KeyWeaver") != std::string::npos) {
        return *existing;
    }
    return *existing + " " + marker;
}

std::vector<std::size_t> sortedNoteOrder(const Chart& chart) {
    std::vector<std::size_t> order(chart.notes.size());
    for (std::size_t index = 0; index < chart.notes.size(); ++index) {
        order[index] = index;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& lhsNote = chart.notes[lhs];
        const auto& rhsNote = chart.notes[rhs];
        if (lhsNote.time != rhsNote.time) {
            return lhsNote.time < rhsNote.time;
        }
        const int lhsLane = sourceLaneOf(lhsNote);
        const int rhsLane = sourceLaneOf(rhsNote);
        if (lhsLane != rhsLane) {
            return lhsLane < rhsLane;
        }
        return lhsNote.id < rhsNote.id;
    });
    return order;
}

std::vector<SliceInfo> buildSlices(const Chart& chart, int sameTimeEpsilonMs) {
    std::vector<SliceInfo> slices;
    for (const auto index : sortedNoteOrder(chart)) {
        const auto& note = chart.notes[index];
        if (slices.empty() || std::abs(note.time - slices.back().time) > sameTimeEpsilonMs) {
            SliceInfo slice;
            slice.time = note.time;
            slice.noteIndices.push_back(index);
            slices.push_back(std::move(slice));
        } else {
            slices.back().noteIndices.push_back(index);
        }
    }
    return slices;
}

void addUnique(std::vector<int>& lanes, int lane, int targetKeyCount) {
    if (lane < 0 || lane >= targetKeyCount) {
        return;
    }
    if (std::find(lanes.begin(), lanes.end(), lane) == lanes.end()) {
        lanes.push_back(lane);
    }
}

std::vector<int> candidatePoolForSevenToTen(int sourceLane) {
    std::vector<int> lanes;
    const int direct = directLane(sourceLane, 7, 10);
    addUnique(lanes, direct, 10);

    switch (sourceLane) {
        case 0:
            addUnique(lanes, 1, 10);
            addUnique(lanes, 2, 10);
            break;
        case 1:
            addUnique(lanes, 1, 10);
            addUnique(lanes, 3, 10);
            addUnique(lanes, 0, 10);
            break;
        case 2:
            addUnique(lanes, 4, 10);
            addUnique(lanes, 2, 10);
            break;
        case 3:
            addUnique(lanes, 4, 10);
            addUnique(lanes, 5, 10);
            addUnique(lanes, 3, 10);
            addUnique(lanes, 6, 10);
            break;
        case 4:
            addUnique(lanes, 5, 10);
            addUnique(lanes, 7, 10);
            break;
        case 5:
            addUnique(lanes, 7, 10);
            addUnique(lanes, 6, 10);
            addUnique(lanes, 9, 10);
            break;
        case 6:
            addUnique(lanes, 8, 10);
            addUnique(lanes, 7, 10);
            break;
        default:
            break;
    }

    addUnique(lanes, 9 - direct, 10);
    for (int lane = 0; lane < 10; ++lane) {
        addUnique(lanes, lane, 10);
    }
    return lanes;
}

std::vector<int> candidatePoolFor(int sourceLane, int sourceKeyCount, int targetKeyCount) {
    if (sourceKeyCount == 7 && targetKeyCount == 10) {
        return candidatePoolForSevenToTen(sourceLane);
    }

    std::vector<int> lanes;
    const int direct = directLane(sourceLane, sourceKeyCount, targetKeyCount);
    addUnique(lanes, direct, targetKeyCount);
    addUnique(lanes, direct - 1, targetKeyCount);
    addUnique(lanes, direct + 1, targetKeyCount);
    addUnique(lanes, targetKeyCount - 1 - direct, targetKeyCount);
    for (int lane = 0; lane < targetKeyCount; ++lane) {
        addUnique(lanes, lane, targetKeyCount);
    }
    return lanes;
}

bool laneInSourcePanel(int sourceLane, int lane) {
    if (sourceLane <= 2) {
        return lane >= 0 && lane <= 4;
    }
    if (sourceLane >= 4) {
        return lane >= 5 && lane <= 9;
    }
    return lane >= 3 && lane <= 6;
}

bool laneInBridge(int lane) {
    return lane >= 3 && lane <= 6;
}

bool sameDirection(int lhs, int rhs) {
    return lhs != 0 && rhs != 0 && (lhs > 0) == (rhs > 0);
}

std::pair<int, int> bridgeRangeForTarget(int targetKeyCount) {
    if (targetKeyCount == 10) {
        return {3, 6};
    }
    if (targetKeyCount >= 7) {
        return {targetKeyCount / 2 - 1, targetKeyCount / 2 + 1};
    }
    return {0, targetKeyCount - 1};
}

double desiredLaneShare(int lane, int targetKeyCount, const LayoutWeights& weights) {
    if (targetKeyCount <= 0 || lane < 0 || lane >= targetKeyCount) {
        return 0.0;
    }

    const double totalWeight = static_cast<double>(std::max(1, weights.panel + weights.bridge + weights.fullField));
    double weightedShare = 0.0;
    weightedShare += static_cast<double>(weights.fullField) / static_cast<double>(targetKeyCount);

    if (targetKeyCount >= 8 && targetKeyCount % 2 == 0) {
        weightedShare += static_cast<double>(weights.panel) / static_cast<double>(targetKeyCount);
    } else {
        weightedShare += static_cast<double>(weights.panel) / static_cast<double>(targetKeyCount);
    }

    const auto [bridgeStart, bridgeEnd] = bridgeRangeForTarget(targetKeyCount);
    if (lane >= bridgeStart && lane <= bridgeEnd) {
        const int bridgeWidth = std::max(1, bridgeEnd - bridgeStart + 1);
        weightedShare += static_cast<double>(weights.bridge) / static_cast<double>(bridgeWidth);
    } else if (targetKeyCount < 7) {
        weightedShare += static_cast<double>(weights.bridge) / static_cast<double>(targetKeyCount);
    }

    const double weighted = weightedShare / totalWeight;
    if (targetKeyCount == 8) {
        constexpr double kWholeFieldBlend = 0.35;
        const double uniform = 1.0 / static_cast<double>(targetKeyCount);
        return weighted * (1.0 - kWholeFieldBlend) + uniform * kWholeFieldBlend;
    }
    return weighted;
}

double laneCoverageNeedScore(int lane,
                             const std::vector<int>& laneUse,
                             int targetKeyCount,
                             const LayoutWeights& weights) {
    if (targetKeyCount <= 0 || lane < 0 || lane >= targetKeyCount ||
        lane >= static_cast<int>(laneUse.size())) {
        return 0.0;
    }

    const int total = std::accumulate(laneUse.begin(), laneUse.end(), 0);
    const double desired = desiredLaneShare(lane, targetKeyCount, weights);
    if (desired <= 0.0) {
        return 0.0;
    }

    const double virtualTotal = std::max(4.0, static_cast<double>(targetKeyCount) * 0.5);
    const double actual = static_cast<double>(laneUse[static_cast<std::size_t>(lane)]) +
                          desired * virtualTotal;
    const double expected = desired * (static_cast<double>(total) + virtualTotal + 1.0);
    const double ratio = expected <= 0.0 ? 1.0 : actual / expected;
    if (ratio <= 1.0) {
        return clamp01((1.0 - ratio) * 1.8);
    }
    return -clamp01((ratio - 1.0) * 0.9);
}

double genericTargetFreedomMultiplier(int targetKeyCount) {
    if (targetKeyCount == 8) {
        return 1.35;
    }
    if (targetKeyCount == 5) {
        return 1.55;
    }
    return 1.0;
}

double genericTargetAnchorLockMultiplier(int targetKeyCount) {
    if (targetKeyCount == 8) {
        return 0.62;
    }
    return 1.0;
}

int holdDurationMs(const Note& note) {
    if (note.type != NoteType::Hold || !note.endTime.has_value()) {
        return 0;
    }
    return std::max(0, *note.endTime - note.time);
}

bool isLongHoldForAdjacentCopy(const Note& note) {
    return holdDurationMs(note) >= 1000;
}

std::vector<int> adjacentCopyLaneCandidates(const Note& note,
                                            const NK2Options& options,
                                            const std::map<int, PlacedNote>& lastBySource) {
    std::vector<int> lanes;
    const int sourceLane = sourceLaneOf(note);
    for (const int sourceDelta : {-1, 1}) {
        const int adjacentSource = sourceLane + sourceDelta;
        if (adjacentSource < 0 || adjacentSource >= options.sourceKeyCount) {
            continue;
        }

        const auto adjacentPlaced = lastBySource.find(adjacentSource);
        if (adjacentPlaced != lastBySource.end()) {
            addUnique(lanes, adjacentPlaced->second.lane - sourceDelta, options.targetKeyCount);
            addUnique(lanes, adjacentPlaced->second.lane, options.targetKeyCount);
        }

        const int adjacentDirect = directLane(
            adjacentSource, options.sourceKeyCount, options.targetKeyCount);
        addUnique(lanes, adjacentDirect - sourceDelta, options.targetKeyCount);
    }
    return lanes;
}

double adjacentCopyPreferenceScore(int lane, const std::vector<int>& adjacentCopyLanes) {
    double score = 0.0;
    for (std::size_t index = 0; index < adjacentCopyLanes.size(); ++index) {
        const int distance = std::abs(lane - adjacentCopyLanes[index]);
        const double rankDecay = static_cast<double>(index) * 0.08;
        if (distance == 0) {
            score = std::max(score, 1.35 - rankDecay);
        } else if (distance == 1) {
            score = std::max(score, 0.45 - rankDecay * 0.5);
        }
    }
    return score;
}

double wholeFieldNeedScore(int lane, const std::vector<int>& laneUse, int targetKeyCount) {
    if (targetKeyCount <= 0 || lane < 0 || lane >= targetKeyCount ||
        lane >= static_cast<int>(laneUse.size())) {
        return 0.0;
    }

    const int total = std::accumulate(laneUse.begin(), laneUse.end(), 0);
    const double virtualTotal = static_cast<double>(targetKeyCount);
    const double actual = static_cast<double>(laneUse[static_cast<std::size_t>(lane)]) + 1.0;
    const double expected = (static_cast<double>(total) + virtualTotal + 1.0) /
                            static_cast<double>(targetKeyCount);
    const double ratio = expected <= 0.0 ? 1.0 : actual / expected;
    if (ratio <= 1.0) {
        return clamp01((1.0 - ratio) * 2.2);
    }
    return -clamp01((ratio - 1.0) * 0.8);
}

std::optional<std::pair<int, int>> panelRangeForLane(int lane, int targetKeyCount) {
    if (targetKeyCount < 8 || targetKeyCount % 2 != 0 || lane < 0 || lane >= targetKeyCount) {
        return std::nullopt;
    }
    const int split = targetKeyCount / 2;
    if (lane < split) {
        return std::make_pair(0, split - 1);
    }
    return std::make_pair(split, targetKeyCount - 1);
}

double panelLaneNeedScore(int lane, const std::vector<int>& laneUse, int targetKeyCount) {
    if (lane < 0 || lane >= targetKeyCount || lane >= static_cast<int>(laneUse.size())) {
        return 0.0;
    }

    const auto range = panelRangeForLane(lane, targetKeyCount);
    if (!range.has_value()) {
        return 0.0;
    }

    const auto [start, end] = *range;
    int panelTotal = 0;
    for (int panelLane = start; panelLane <= end; ++panelLane) {
        panelTotal += laneUse[static_cast<std::size_t>(panelLane)];
    }

    const int width = std::max(1, end - start + 1);
    const double virtualTotal = static_cast<double>(width);
    const double actual = static_cast<double>(laneUse[static_cast<std::size_t>(lane)]) +
                          virtualTotal / static_cast<double>(width);
    const double expected = (static_cast<double>(panelTotal) + virtualTotal + 1.0) /
                            static_cast<double>(width);
    const double ratio = expected <= 0.0 ? 1.0 : actual / expected;
    if (ratio <= 1.0) {
        return clamp01((1.0 - ratio) * 1.7);
    }
    return -clamp01((ratio - 1.0) * 0.7);
}

double laneCoveragePreference(int lane,
                              const std::vector<int>& laneUse,
                              int targetKeyCount,
                              const LayoutWeights& weights) {
    const double globalNeed = laneCoverageNeedScore(lane, laneUse, targetKeyCount, weights);
    const double panelNeed = panelLaneNeedScore(lane, laneUse, targetKeyCount);
    const double freedomBoost = genericTargetFreedomMultiplier(targetKeyCount) - 1.0;
    const double wholeFieldNeed = wholeFieldNeedScore(lane, laneUse, targetKeyCount);
    const double combinedNeed = std::max(
        -1.0, std::min(1.0, globalNeed + panelNeed * 0.45 + freedomBoost * wholeFieldNeed));
    return 0.5 + 0.5 * combinedNeed;
}

bool isStrongBeat(int time, const std::vector<TimingPoint>& timingPoints) {
    double beatLength = 500.0;
    int timingBase = 0;
    for (const auto& point : timingPoints) {
        if (point.time > time) {
            break;
        }
        if (point.beatLength > 0.0 && (!point.uninherited.has_value() || *point.uninherited)) {
            beatLength = point.beatLength;
            timingBase = point.time;
        }
    }
    if (beatLength <= 0.0) {
        return false;
    }
    const double beatPosition = static_cast<double>(time - timingBase) / beatLength;
    const double nearest = std::round(beatPosition);
    return std::abs(beatPosition - nearest) <= 0.04;
}

bool hasSameTimeCollision(const std::set<int>& occupiedLanes, int lane) {
    return occupiedLanes.count(lane) > 0;
}

bool hasLongNoteConflict(const std::vector<PlacedNote>& placed, const Note& note, int lane) {
    for (const auto& existing : placed) {
        if (existing.lane != lane) {
            continue;
        }
        if (existing.type == NoteType::Hold && existing.endTime.has_value() &&
            note.time > existing.time && note.time <= *existing.endTime) {
            return true;
        }
        if (note.type == NoteType::Hold && note.endTime.has_value()) {
            const bool existingStartsInside = existing.time > note.time && existing.time <= *note.endTime;
            const bool holdOverlap = existing.type == NoteType::Hold && existing.endTime.has_value() &&
                                     note.time <= *existing.endTime && *note.endTime >= existing.time;
            if (existingStartsInside || holdOverlap) {
                return true;
            }
        }
    }
    return false;
}

std::optional<PlacedNote> lastPlacedForSource(const std::map<int, PlacedNote>& lastBySource,
                                              int sourceLane) {
    const auto found = lastBySource.find(sourceLane);
    if (found == lastBySource.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool isImmediateSourceJackContinuation(const std::vector<PlacedNote>& placed,
                                       int sourceLane,
                                       int time,
                                       int jackWindowMs) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt <= 0) {
            continue;
        }
        if (dt > jackWindowMs) {
            return false;
        }
        return it->sourceLane == sourceLane;
    }
    return false;
}

bool wouldCreateNewJack(const std::vector<PlacedNote>& placed,
                        int sourceLane,
                        int time,
                        int lane,
                        int jackWindowMs) {
    for (auto it = placed.rbegin(); it != placed.rend(); ++it) {
        const int dt = time - it->time;
        if (dt <= 0) {
            continue;
        }
        if (dt > jackWindowMs) {
            break;
        }
        if (it->lane == lane && it->sourceLane != sourceLane) {
            return true;
        }
    }
    return false;
}

MotifKind classifyMotif(const Note& note,
                        bool singleNoteSlice,
                        bool fastSingleContinuation,
                        bool sourceJackContinuation,
                        const std::vector<RecentSingleNote>& recentSingles) {
    if (sourceJackContinuation) {
        return MotifKind::Jack;
    }
    if (note.type == NoteType::Hold) {
        return MotifKind::LnAnchor;
    }
    if (!singleNoteSlice) {
        return MotifKind::Chord;
    }
    if (!fastSingleContinuation || recentSingles.empty()) {
        return MotifKind::Neutral;
    }

    const int sourceLane = sourceLaneOf(note);
    if (recentSingles.size() >= 3) {
        const auto& a = recentSingles[recentSingles.size() - 3];
        const auto& b = recentSingles[recentSingles.size() - 2];
        const auto& c = recentSingles[recentSingles.size() - 1];
        if (a.sourceLane == c.sourceLane && b.sourceLane == sourceLane &&
            a.sourceLane != b.sourceLane && note.time - a.time <= 900) {
            return MotifKind::Trill;
        }
    }

    if (recentSingles.size() >= 2) {
        const auto& previous = recentSingles.back();
        const auto& beforePrevious = recentSingles[recentSingles.size() - 2];
        const int previousDelta = previous.sourceLane - beforePrevious.sourceLane;
        const int currentDelta = sourceLane - previous.sourceLane;
        if (sameDirection(previousDelta, currentDelta) && note.time - beforePrevious.time <= 900) {
            return MotifKind::Stair;
        }
    }

    if (recentSingles.size() >= 3 && note.time - recentSingles[recentSingles.size() - 3].time <= 900) {
        return MotifKind::Stream;
    }
    return MotifKind::Stream;
}

void countMotifPlacement(NK2Report& report, MotifKind motif) {
    switch (motif) {
        case MotifKind::Jack:
            ++report.motifJackPlacements;
            break;
        case MotifKind::Trill:
            ++report.motifTrillPlacements;
            break;
        case MotifKind::Stair:
            ++report.motifStairPlacements;
            break;
        case MotifKind::Stream:
            ++report.motifStreamPlacements;
            break;
        case MotifKind::Chord:
            ++report.motifChordPlacements;
            break;
        case MotifKind::LnAnchor:
            ++report.motifLnPlacements;
            break;
        case MotifKind::Neutral:
            ++report.motifNeutralPlacements;
            break;
    }
}

std::string motifSlug(MotifKind motif) {
    switch (motif) {
        case MotifKind::Jack:
            return "jack";
        case MotifKind::Trill:
            return "trill";
        case MotifKind::Stair:
            return "stair";
        case MotifKind::Stream:
            return "stream";
        case MotifKind::Chord:
            return "chord";
        case MotifKind::LnAnchor:
            return "ln";
        case MotifKind::Neutral:
            return "neutral";
    }
    return "neutral";
}

void countGeneratedProvenance(NK2Report& report, MotifKind motif) {
    switch (motif) {
        case MotifKind::Jack:
            ++report.generatedFromJackMotif;
            break;
        case MotifKind::Trill:
            ++report.generatedFromTrillMotif;
            break;
        case MotifKind::Stair:
            ++report.generatedFromStairMotif;
            break;
        case MotifKind::Stream:
            ++report.generatedFromStreamMotif;
            break;
        case MotifKind::Chord:
            ++report.generatedFromChordMotif;
            break;
        case MotifKind::LnAnchor:
            ++report.generatedFromLnMotif;
            break;
        case MotifKind::Neutral:
            ++report.generatedFromNeutralMotif;
            break;
    }
}

double remixScoreForLane(int sourceLane,
                         int lane,
                         const std::vector<int>& laneUse,
                         int targetKeyCount,
                         const LayoutWeights& weights) {
    const double coverageScore = laneCoveragePreference(lane, laneUse, targetKeyCount, weights);
    const double panelScore = laneInSourcePanel(sourceLane, lane) ? 1.0 : 0.0;
    const double bridgeScore = laneInBridge(lane) ? 1.0 : 0.0;
    const double totalWeight = std::max(1, weights.panel + weights.bridge + weights.fullField);
    return (static_cast<double>(weights.panel) * panelScore +
            static_cast<double>(weights.bridge) * bridgeScore +
            static_cast<double>(weights.fullField) * coverageScore) /
           totalWeight;
}

double motifScoreAdjustment(MotifKind motif,
                            const Note& note,
                            int lane,
                            int direct,
                            int sourceLane,
                            const NK2Options& options,
                            const std::vector<int>& laneUse,
                            const std::optional<int>& previousSingleSourceLane,
                            const std::optional<int>& previousSingleTargetLane) {
    double score = 0.0;
    const bool hasPrevious = previousSingleSourceLane.has_value() && previousSingleTargetLane.has_value();
    const int sourceDelta = hasPrevious ? sourceLane - *previousSingleSourceLane : 0;
    const int targetDelta = hasPrevious ? lane - *previousSingleTargetLane : 0;
    const double longHoldSpreadScale = isLongHoldForAdjacentCopy(note) ? 0.25 : 1.0;

    switch (motif) {
        case MotifKind::Jack:
            score += lane == direct ? 0.25 : -0.20;
            break;
        case MotifKind::Trill:
            if (hasPrevious) {
                if (targetDelta == 0) {
                    score -= 1.30;
                } else {
                    score += 0.65;
                    if (std::abs(targetDelta) <= 3) {
                        score += 0.25;
                    } else {
                        score -= 0.20;
                    }
                }
            }
            if (laneInSourcePanel(sourceLane, lane)) {
                score += 0.10;
            }
            break;
        case MotifKind::Stair:
            if (hasPrevious && sourceDelta != 0) {
                if (targetDelta != 0 && sameDirection(sourceDelta, targetDelta)) {
                    score += 0.75;
                    if (std::abs(targetDelta) >= std::abs(sourceDelta)) {
                        score += 0.30;
                    }
                } else {
                    score -= 0.90;
                }
            }
            break;
        case MotifKind::Stream:
            if (hasPrevious) {
                if (targetDelta == 0) {
                    score -= 0.80;
                } else {
                    score += 0.25;
                    if (std::abs(targetDelta) <= 4) {
                        score += 0.15;
                    } else {
                        score -= 0.10;
                    }
                }
                if (sourceDelta != 0 && targetDelta != 0 && sameDirection(sourceDelta, targetDelta)) {
                    score += 0.20;
                }
            }
            score += 0.35 * laneCoverageNeedScore(lane, laneUse, options.targetKeyCount, options.layoutWeights);
            break;
        case MotifKind::Chord:
            score += laneInSourcePanel(sourceLane, lane) ? 0.35 : -0.35;
            if (laneInBridge(lane) && sourceLane == options.sourceKeyCount / 2) {
                score += 0.20;
            }
            break;
        case MotifKind::LnAnchor:
            score += lane == direct ? 0.05 * genericTargetAnchorLockMultiplier(options.targetKeyCount) : 0.0;
            score += 0.45 * longHoldSpreadScale * laneCoverageNeedScore(
                              lane, laneUse, options.targetKeyCount, options.layoutWeights);
            score += 0.20 * longHoldSpreadScale * panelLaneNeedScore(
                              lane, laneUse, options.targetKeyCount);
            if (!laneInSourcePanel(sourceLane, lane)) {
                score += 0.10 * longHoldSpreadScale * laneCoverageNeedScore(
                                  lane, laneUse, options.targetKeyCount, options.layoutWeights);
            }
            if (laneInBridge(lane)) {
                score += 0.05;
            }
            break;
        case MotifKind::Neutral:
            score += lane == direct ? 0.02 * genericTargetAnchorLockMultiplier(options.targetKeyCount) : 0.0;
            score += 0.45 * laneCoverageNeedScore(
                              lane, laneUse, options.targetKeyCount, options.layoutWeights);
            if (!laneInSourcePanel(sourceLane, lane)) {
                score += 0.10 * laneCoverageNeedScore(
                                  lane, laneUse, options.targetKeyCount, options.layoutWeights);
            }
            break;
    }

    if (note.type == NoteType::Hold && motif != MotifKind::LnAnchor) {
        score += lane == direct ? 0.10 * genericTargetAnchorLockMultiplier(options.targetKeyCount) : 0.0;
    }
    return score;
}

std::vector<Candidate> rankedCandidates(const Note& note,
                                        const NK2Options& options,
                                        const std::vector<int>& laneUse,
                                        const std::map<int, PlacedNote>& lastBySource,
                                        const std::vector<PlacedNote>& placed,
                                        const std::optional<int>& previousSingleSourceLane,
                                        const std::optional<int>& previousSingleTargetLane,
                                        bool sourceJackContinuation,
                                        MotifKind motif) {
    const int sourceLane = sourceLaneOf(note);
    std::vector<Candidate> ranked;
    const auto pool = candidatePoolFor(sourceLane, options.sourceKeyCount, options.targetKeyCount);
    const int direct = directLane(sourceLane, options.sourceKeyCount, options.targetKeyCount);
    const double totalBlend = std::max(0.001, options.nativeWeight + options.remixWeight);
    const double freedomMultiplier = genericTargetFreedomMultiplier(options.targetKeyCount);
    const double freedomBoost = freedomMultiplier - 1.0;
    const double anchorLockScale =
        genericTargetAnchorLockMultiplier(options.targetKeyCount) / freedomMultiplier;
    const bool longHoldCopy = isLongHoldForAdjacentCopy(note);
    const double longHoldSpreadScale = longHoldCopy ? 0.25 : 1.0;
    const auto adjacentCopyLanes =
        longHoldCopy ? adjacentCopyLaneCandidates(note, options, lastBySource) : std::vector<int>{};

    std::optional<int> sourceJackLane;
    if (sourceJackContinuation) {
        const auto last = lastPlacedForSource(lastBySource, sourceLane);
        if (last.has_value()) {
            sourceJackLane = last->lane;
        }
    }

    for (const int lane : pool) {
        const bool freeOriginalTap =
            note.type == NoteType::Tap && (motif == MotifKind::Neutral || motif == MotifKind::Stream);
        const double distance = std::abs(lane - direct);
        const double nativeDistance = (freeOriginalTap ? 7.0 : 4.0) * freedomMultiplier;
        const double rawNativeScore = std::max(0.0, 1.0 - distance / nativeDistance);
        const double nativeAnchorContrast = options.targetKeyCount == 8 ? 0.55 : 1.0;
        const double nativeScore = 0.5 + (rawNativeScore - 0.5) * nativeAnchorContrast;
        const double remixScore =
            remixScoreForLane(sourceLane, lane, laneUse, options.targetKeyCount, options.layoutWeights);
        const double coverageNeed =
            laneCoverageNeedScore(lane, laneUse, options.targetKeyCount, options.layoutWeights);
        const double panelNeed = panelLaneNeedScore(lane, laneUse, options.targetKeyCount);
        const double wholeFieldNeed = wholeFieldNeedScore(lane, laneUse, options.targetKeyCount);
        double score = (options.nativeWeight * nativeScore + options.remixWeight * remixScore) / totalBlend;
        score += (options.remixWeight / totalBlend) * (freeOriginalTap ? 1.35 : 1.0) * coverageNeed;
        score += (options.remixWeight / totalBlend) * freedomBoost *
                 (freeOriginalTap ? 2.35 : 1.55) * wholeFieldNeed * longHoldSpreadScale;
        if (longHoldCopy) {
            score += adjacentCopyPreferenceScore(lane, adjacentCopyLanes);
        }
        if (laneInSourcePanel(sourceLane, lane)) {
            score += (options.nativeWeight / totalBlend) * (freeOriginalTap ? 0.10 : 0.35) *
                     panelNeed * anchorLockScale;
            score += (options.remixWeight / totalBlend) * (freeOriginalTap ? 0.35 : 0.70) *
                     panelNeed;
        }

        const bool freeLnAnchor = motif == MotifKind::LnAnchor;
        const bool freerAnchor = freeLnAnchor || freeOriginalTap;
        if (lane == direct) {
            const double anchorPreference = std::min(
                laneCoveragePreference(lane, laneUse, options.targetKeyCount, options.layoutWeights),
                0.5 + 0.5 * panelLaneNeedScore(lane, laneUse, options.targetKeyCount));
            const double anchorScale = clamp01(0.10 + 0.90 * anchorPreference);
            score += (freerAnchor ? 0.10 : 0.45) * anchorScale * anchorLockScale;
            if (options.targetKeyCount == 8 && !sourceJackContinuation && motif != MotifKind::Jack) {
                score -= freerAnchor ? 0.16 : 0.09;
            }
        }
        if (laneInSourcePanel(sourceLane, lane)) {
            score += (freerAnchor ? 0.03 : 0.15) * anchorLockScale;
        }
        if (note.type == NoteType::Hold && laneInBridge(lane)) {
            score += freeLnAnchor ? 0.03 : 0.08;
        }
        if (sourceJackLane.has_value()) {
            score += lane == *sourceJackLane ? 2.0 : -1.0;
        }
        if (previousSingleSourceLane.has_value() && previousSingleTargetLane.has_value()) {
            const int sourceDelta = sourceLane - *previousSingleSourceLane;
            const int targetDelta = lane - *previousSingleTargetLane;
            if (sourceDelta != 0 && targetDelta != 0 && (sourceDelta > 0) == (targetDelta > 0)) {
                score += 0.35;
                if (std::abs(targetDelta) >= std::abs(sourceDelta)) {
                    score += 0.12;
                }
            } else if (sourceDelta != 0) {
                score -= 0.50;
            }
        }
        if (wouldCreateNewJack(placed, sourceLane, note.time, lane, 500)) {
            score -= 1.5;
        }
        score += motifScoreAdjustment(motif,
                                      note,
                                      lane,
                                      direct,
                                      sourceLane,
                                      options,
                                      laneUse,
                                      previousSingleSourceLane,
                                      previousSingleTargetLane);
        ranked.push_back({lane, score});
    }

    std::stable_sort(ranked.begin(), ranked.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.lane < rhs.lane;
    });
    return ranked;
}

int chooseLane(const Note& note,
               const NK2Options& options,
               const std::set<int>& occupiedLanes,
               const std::vector<int>& laneUse,
               const std::map<int, PlacedNote>& lastBySource,
               const std::vector<PlacedNote>& placed,
               const std::optional<int>& previousSingleSourceLane,
               const std::optional<int>& previousSingleTargetLane,
               MotifKind motif) {
    const int sourceLane = sourceLaneOf(note);
    const bool sourceJackContinuation =
        isImmediateSourceJackContinuation(placed, sourceLane, note.time, 500);
    const auto ranked = rankedCandidates(note,
                                         options,
                                         laneUse,
                                         lastBySource,
                                         placed,
                                         previousSingleSourceLane,
                                         previousSingleTargetLane,
                                         sourceJackContinuation,
                                         sourceJackContinuation ? MotifKind::Jack : motif);

    for (const auto& candidate : ranked) {
        if (hasSameTimeCollision(occupiedLanes, candidate.lane)) {
            continue;
        }
        if (hasLongNoteConflict(placed, note, candidate.lane)) {
            continue;
        }
        if (!sourceJackContinuation &&
            wouldCreateNewJack(placed, sourceLane, note.time, candidate.lane, 500)) {
            continue;
        }
        return candidate.lane;
    }

    for (const auto& candidate : ranked) {
        if (!hasSameTimeCollision(occupiedLanes, candidate.lane) &&
            !hasLongNoteConflict(placed, note, candidate.lane)) {
            return candidate.lane;
        }
    }

    return directLane(sourceLane, options.sourceKeyCount, options.targetKeyCount);
}

std::optional<int> chooseLaneStrict(const Note& note,
                                    const NK2Options& options,
                                    const std::set<int>& occupiedLanes,
                                    const std::vector<int>& laneUse,
                                    const std::map<int, PlacedNote>& lastBySource,
                                    const std::vector<PlacedNote>& placed,
                                    const std::optional<int>& previousSingleSourceLane,
                                    const std::optional<int>& previousSingleTargetLane,
                                    MotifKind motif) {
    const int sourceLane = sourceLaneOf(note);
    const bool sourceJackContinuation =
        isImmediateSourceJackContinuation(placed, sourceLane, note.time, 500);
    const auto ranked = rankedCandidates(note,
                                         options,
                                         laneUse,
                                         lastBySource,
                                         placed,
                                         previousSingleSourceLane,
                                         previousSingleTargetLane,
                                         sourceJackContinuation,
                                         sourceJackContinuation ? MotifKind::Jack : motif);
    for (const auto& candidate : ranked) {
        if (hasSameTimeCollision(occupiedLanes, candidate.lane)) {
            continue;
        }
        if (hasLongNoteConflict(placed, note, candidate.lane)) {
            continue;
        }
        if (!sourceJackContinuation &&
            wouldCreateNewJack(placed, sourceLane, note.time, candidate.lane, 500)) {
            continue;
        }
        return candidate.lane;
    }
    return std::nullopt;
}

PlacementStats collectPlacementStats(const Chart& converted,
                                     int sourceKeyCount,
                                     int targetKeyCount,
                                     int createdJackWindowMs) {
    PlacementStats stats;
    stats.laneDistribution.assign(static_cast<std::size_t>(std::max(0, targetKeyCount)), 0);
    std::vector<PlacedNote> placed;
    placed.reserve(converted.notes.size());
    for (const auto& note : converted.notes) {
        if (note.lane >= 0 && note.lane < targetKeyCount) {
            ++stats.laneDistribution[static_cast<std::size_t>(note.lane)];
        }
        const int sourceLane = sourceLaneOf(note);
        if (!isNk2GeneratedNote(note) && sourceLane >= 0 && sourceLane < sourceKeyCount) {
            ++stats.sourceAnchorTotal;
            if (note.lane == directLane(sourceLane, sourceKeyCount, targetKeyCount)) {
                ++stats.sourceAnchorMatches;
            }
        }
        for (const auto& existing : placed) {
            if (existing.time == note.time && existing.lane == note.lane) {
                ++stats.sameTimeCollisions;
            }
            if (hasLongNoteConflict({existing}, note, note.lane)) {
                ++stats.longNoteConflicts;
            }
        }
        placed.push_back({note.time, note.lane, sourceLaneOf(note), note.type, note.endTime});
    }

    std::vector<const Note*> convertedSorted;
    convertedSorted.reserve(converted.notes.size());
    for (const auto& note : converted.notes) {
        convertedSorted.push_back(&note);
    }
    std::stable_sort(convertedSorted.begin(), convertedSorted.end(), [](const Note* lhs, const Note* rhs) {
        if (lhs->time != rhs->time) {
            return lhs->time < rhs->time;
        }
        if (lhs->lane != rhs->lane) {
            return lhs->lane < rhs->lane;
        }
        return lhs->id < rhs->id;
    });

    for (std::size_t i = 1; i < convertedSorted.size(); ++i) {
        const auto* previous = convertedSorted[i - 1];
        const auto* current = convertedSorted[i];
        const int dt = current->time - previous->time;
        if (dt <= 0 || dt > createdJackWindowMs || current->lane != previous->lane) {
            continue;
        }
        const int previousSource = sourceLaneOf(*previous);
        const int currentSource = sourceLaneOf(*current);
        if (previousSource == currentSource) {
            ++stats.preservedSourceJacks;
        } else {
            ++stats.createdJacks;
        }
    }
    return stats;
}

double laneEntropyScore(const std::vector<int>& distribution) {
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

double handPanelBalanceScore(const std::vector<int>& distribution) {
    const int total = std::accumulate(distribution.begin(), distribution.end(), 0);
    if (total <= 0 || distribution.empty()) {
        return 0.0;
    }
    const int split = static_cast<int>(distribution.size()) / 2;
    int left = 0;
    int right = 0;
    for (int lane = 0; lane < static_cast<int>(distribution.size()); ++lane) {
        if (lane < split) {
            left += distribution[static_cast<std::size_t>(lane)];
        } else {
            right += distribution[static_cast<std::size_t>(lane)];
        }
    }
    return clamp01(1.0 - static_cast<double>(std::abs(left - right)) / static_cast<double>(total));
}

double panelSpreadFitScore(const std::vector<int>& distribution, int start, int end) {
    if (distribution.empty() || start < 0 || end < start ||
        end >= static_cast<int>(distribution.size())) {
        return 0.0;
    }

    int panelTotal = 0;
    for (int lane = start; lane <= end; ++lane) {
        panelTotal += distribution[static_cast<std::size_t>(lane)];
    }
    if (panelTotal <= 0) {
        return 0.0;
    }

    const int width = std::max(1, end - start + 1);
    double distance = 0.0;
    for (int lane = start; lane <= end; ++lane) {
        const double actual = static_cast<double>(distribution[static_cast<std::size_t>(lane)]) /
                              static_cast<double>(panelTotal);
        const double desired = 1.0 / static_cast<double>(width);
        distance += std::abs(actual - desired);
    }
    return clamp01(1.0 - 0.5 * distance);
}

double bridgeFitScore(const std::vector<int>& distribution, const LayoutWeights& weights) {
    const int targetKeyCount = static_cast<int>(distribution.size());
    const int total = std::accumulate(distribution.begin(), distribution.end(), 0);
    if (targetKeyCount <= 0 || total <= 0) {
        return 0.0;
    }

    const auto [bridgeStart, bridgeEnd] = bridgeRangeForTarget(targetKeyCount);
    double desiredBridge = 0.0;
    int actualBridge = 0;
    for (int lane = 0; lane < targetKeyCount; ++lane) {
        if (lane >= bridgeStart && lane <= bridgeEnd) {
            desiredBridge += desiredLaneShare(lane, targetKeyCount, weights);
            actualBridge += distribution[static_cast<std::size_t>(lane)];
        }
    }
    if (desiredBridge <= 0.0) {
        return 0.0;
    }

    const double actual = static_cast<double>(actualBridge) / static_cast<double>(total);
    return clamp01(1.0 - std::abs(actual - desiredBridge) / desiredBridge);
}

double layoutCoverageFitScore(const std::vector<int>& distribution, const LayoutWeights& weights) {
    const int targetKeyCount = static_cast<int>(distribution.size());
    const int total = std::accumulate(distribution.begin(), distribution.end(), 0);
    if (targetKeyCount <= 0 || total <= 0) {
        return 0.0;
    }

    double distance = 0.0;
    for (int lane = 0; lane < targetKeyCount; ++lane) {
        const double actual = static_cast<double>(distribution[static_cast<std::size_t>(lane)]) /
                              static_cast<double>(total);
        const double desired = desiredLaneShare(lane, targetKeyCount, weights);
        distance += std::abs(actual - desired);
    }
    return clamp01(1.0 - 0.5 * distance);
}

void fillLayoutScores(NK2Report& report) {
    report.panelScore = handPanelBalanceScore(report.laneDistribution);
    if (report.laneDistribution.size() >= 2 && report.laneDistribution.size() % 2 == 0) {
        const int split = static_cast<int>(report.laneDistribution.size()) / 2;
        report.leftPanelScore = panelSpreadFitScore(report.laneDistribution, 0, split - 1);
        report.rightPanelScore = panelSpreadFitScore(
            report.laneDistribution, split, static_cast<int>(report.laneDistribution.size()) - 1);
    }
    report.bridgeScore = bridgeFitScore(report.laneDistribution, report.options.layoutWeights);
    report.fullFieldScore = laneEntropyScore(report.laneDistribution);
    report.layoutCoverageScore = layoutCoverageFitScore(report.laneDistribution, report.options.layoutWeights);
}

int supportPhraseWindowForTime(int time);
int phraseSupportBudgetFor(const NK2Options& options, int sourceNoteCount);

void fillPhraseProfileScores(NK2Report& report, const Chart& converted) {
    std::map<int, int> sourceNotesByPhrase;
    std::map<int, int> supportNotesByPhrase;
    for (const auto& note : converted.notes) {
        const int window = supportPhraseWindowForTime(note.time);
        if (isNk2GeneratedNote(note)) {
            ++supportNotesByPhrase[window];
        } else {
            ++sourceNotesByPhrase[window];
        }
    }

    std::set<int> windows;
    for (const auto& [window, count] : sourceNotesByPhrase) {
        (void)count;
        windows.insert(window);
    }
    for (const auto& [window, count] : supportNotesByPhrase) {
        (void)count;
        windows.insert(window);
    }

    report.phraseProfileWindows = static_cast<int>(windows.size());
    report.phraseProfileOverBudgetWindows = 0;
    if (windows.empty()) {
        report.phraseProfileScore = 0.0;
        return;
    }

    double scoreSum = 0.0;
    for (const int window : windows) {
        const auto sourceCount = sourceNotesByPhrase.find(window);
        const auto supportCount = supportNotesByPhrase.find(window);
        const int sourceNotes = sourceCount == sourceNotesByPhrase.end() ? 0 : sourceCount->second;
        const int supportNotes = supportCount == supportNotesByPhrase.end() ? 0 : supportCount->second;
        const int budget = phraseSupportBudgetFor(report.options, sourceNotes);
        if (supportNotes > budget) {
            ++report.phraseProfileOverBudgetWindows;
            const int overflow = supportNotes - budget;
            scoreSum += clamp01(1.0 - static_cast<double>(overflow) /
                                          static_cast<double>(std::max(1, supportNotes)));
        } else {
            scoreSum += 1.0;
        }
    }
    report.phraseProfileScore = clamp01(scoreSum / static_cast<double>(windows.size()));
}

void fillDistributionAndSafety(NK2Report& report, const Chart& converted) {
    report.outputNotes = static_cast<int>(converted.notes.size());
    report.addedNotes = std::max(0, report.outputNotes - report.intent.totalNotes);
    report.sameTimeCollisions = 0;
    report.longNoteConflicts = 0;
    report.createdJacks = 0;
    report.preservedSourceJacks = 0;
    report.sourceAnchorMatches = 0;
    report.sourceAnchorTotal = 0;
    report.sourceAnchorScore = 0.0;

    const auto stats = collectPlacementStats(converted,
                                             report.options.sourceKeyCount,
                                             report.options.targetKeyCount,
                                             supportJackWindowMsFor(report.options));
    report.sameTimeCollisions = stats.sameTimeCollisions;
    report.longNoteConflicts = stats.longNoteConflicts;
    report.createdJacks = stats.createdJacks;
    report.preservedSourceJacks = stats.preservedSourceJacks;
    report.sourceAnchorMatches = stats.sourceAnchorMatches;
    report.sourceAnchorTotal = stats.sourceAnchorTotal;
    if (report.sourceAnchorTotal > 0) {
        report.sourceAnchorScore = static_cast<double>(report.sourceAnchorMatches) /
                                   static_cast<double>(report.sourceAnchorTotal);
    }
    report.laneDistribution = stats.laneDistribution;
    fillLayoutScores(report);
    fillPhraseProfileScores(report, converted);
}

int supportBudgetFor(const NK2Options& options, int sourceNoteCount) {
    const bool fourToFiveFill = isFourToFiveFillOptions(options);
    if ((!fourToFiveFill && options.mode == Mode::Faithful) || options.mode == Mode::Report) {
        return 0;
    }
    if (sourceNoteCount <= 0) {
        return 0;
    }
    if (fourToFiveFill) {
        return std::max(kFourToFiveFillMinimumBudget,
                        static_cast<int>(std::ceil(static_cast<double>(sourceNoteCount) *
                                                   kFourToFiveFillAddedRatio)));
    }
    const double ratio = options.mode == Mode::Harder ? 0.18 : 0.12;
    const int minimum = options.mode == Mode::Harder ? 2 : 1;
    return std::max(minimum, static_cast<int>(std::ceil(static_cast<double>(sourceNoteCount) * ratio)));
}

int supportPhraseWindowMs() {
    return 2000;
}

int supportPhraseWindowForTime(int time) {
    if (time < 0) {
        return 0;
    }
    return time / supportPhraseWindowMs();
}

int phraseSupportBudgetFor(const NK2Options& options, int sourceNoteCount) {
    const bool fourToFiveFill = isFourToFiveFillOptions(options);
    if ((!fourToFiveFill && options.mode == Mode::Faithful) || options.mode == Mode::Report) {
        return 0;
    }
    const int safeSourceNoteCount = std::max(1, sourceNoteCount);
    if (fourToFiveFill) {
        const int budget = std::max(
            kFourToFiveFillMinimumPhraseBudget,
            static_cast<int>(std::ceil(static_cast<double>(safeSourceNoteCount) *
                                       kFourToFiveFillPhraseRatio)));
        return clampInt(
            budget, kFourToFiveFillMinimumPhraseBudget, kFourToFiveFillMaximumPhraseBudget);
    }
    const double ratio = options.mode == Mode::Harder ? 0.18 : 0.12;
    const int minimum = options.mode == Mode::Harder ? 2 : 1;
    const int maximum = options.mode == Mode::Harder ? 6 : 4;
    const int budget = std::max(
        minimum, static_cast<int>(std::ceil(static_cast<double>(safeSourceNoteCount) * ratio)));
    return clampInt(budget, minimum, maximum);
}

std::map<int, int> sourceNoteCountsBySupportPhrase(const Chart& chart) {
    std::map<int, int> counts;
    for (const auto& note : chart.notes) {
        if (!isNk2GeneratedNote(note)) {
            ++counts[supportPhraseWindowForTime(note.time)];
        }
    }
    return counts;
}

bool oppositePanelSupport(int sourceLane, int lane) {
    if (sourceLane <= 2) {
        return lane >= 5 && lane <= 9;
    }
    if (sourceLane >= 4) {
        return lane >= 0 && lane <= 4;
    }
    return lane >= 3 && lane <= 6;
}

std::vector<Candidate> rankedSupportLanes(const SupportEvent& event,
                                          const std::vector<int>& laneUse,
                                          const NK2Options& options) {
    std::vector<Candidate> ranked;
    const int mirrorLane = options.targetKeyCount - 1 - event.anchorLane;
    auto pool = candidatePoolFor(event.sourceLane, options.sourceKeyCount, options.targetKeyCount);
    if (event.kind == SupportKind::Mirror) {
        addUnique(pool, mirrorLane, options.targetKeyCount);
        addUnique(pool, mirrorLane - 1, options.targetKeyCount);
        addUnique(pool, mirrorLane + 1, options.targetKeyCount);
    }
    for (const int lane : pool) {
        if (lane == event.anchorLane) {
            continue;
        }
        double score = remixScoreForLane(event.sourceLane, lane, laneUse, options.targetKeyCount, options.layoutWeights);
        score += 0.60 * laneCoverageNeedScore(lane, laneUse, options.targetKeyCount, options.layoutWeights);
        score += 0.45 * panelLaneNeedScore(lane, laneUse, options.targetKeyCount);
        if (lane == mirrorLane) {
            score += 0.55;
        }
        if (oppositePanelSupport(event.sourceLane, lane)) {
            score += 0.30;
        }
        if (event.kind == SupportKind::Mirror) {
            score += lane == mirrorLane ? 1.25 : 0.0;
            score += oppositePanelSupport(event.sourceLane, lane) ? 0.60 : -0.20;
            score += laneInSourcePanel(event.sourceLane, lane) ? -0.30 : 0.0;
        }
        if (event.kind == SupportKind::Ln && laneInBridge(lane)) {
            score += 0.20;
        }
        if (event.kind == SupportKind::StrongBeat && laneInSourcePanel(event.sourceLane, lane)) {
            score += 0.10;
        }
        ranked.push_back({lane, score});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.lane < rhs.lane;
    });
    return ranked;
}

std::map<std::string, MotifKind> buildSupportAnchorMotifs(const Chart& placed,
                                                          int sameTimeEpsilonMs) {
    std::map<std::string, MotifKind> motifs;
    const auto slices = buildSlices(placed, sameTimeEpsilonMs);
    std::vector<PlacedNote> placedNotes;
    std::vector<RecentSingleNote> recentSingles;
    std::optional<int> previousSingleTime;

    for (const auto& slice : slices) {
        const bool singleNoteSlice = slice.noteIndices.size() == 1;
        for (const auto noteIndex : slice.noteIndices) {
            const auto& note = placed.notes[noteIndex];
            const int sourceLane = sourceLaneOf(note);
            const bool fastSingleContinuation =
                singleNoteSlice && previousSingleTime.has_value() &&
                note.time > *previousSingleTime && note.time - *previousSingleTime <= 280;
            if (singleNoteSlice && !fastSingleContinuation) {
                recentSingles.clear();
            }

            const bool sourceJackContinuation =
                isImmediateSourceJackContinuation(placedNotes, sourceLane, note.time, 500);
            const auto motif = classifyMotif(note,
                                             singleNoteSlice,
                                             fastSingleContinuation,
                                             sourceJackContinuation,
                                             recentSingles);
            motifs[note.id] = motif;

            placedNotes.push_back({note.time, note.lane, sourceLane, note.type, note.endTime});
            if (singleNoteSlice) {
                previousSingleTime = note.time;
                recentSingles.push_back({note.time, sourceLane, note.lane});
                if (recentSingles.size() > 6) {
                    recentSingles.erase(recentSingles.begin());
                }
            }
        }

        if (!singleNoteSlice) {
            previousSingleTime.reset();
            recentSingles.clear();
        }
    }

    return motifs;
}

bool shouldEmitMirrorSupportEvent(const Note& note,
                                  MotifKind motif,
                                  int sourceKeyCount,
                                  int targetKeyCount) {
    if (targetKeyCount <= sourceKeyCount || note.type == NoteType::Hold) {
        return false;
    }
    if (motif == MotifKind::Jack || motif == MotifKind::Trill || motif == MotifKind::LnAnchor) {
        return false;
    }
    const int sourceLane = sourceLaneOf(note);
    return sourceLane >= 0 && sourceLane < sourceKeyCount && sourceLane != sourceKeyCount / 2;
}

std::vector<SupportEvent> buildSupportEvents(const Chart& placed, const NK2Options& options) {
    std::vector<SupportEvent> events;
    std::set<std::pair<int, std::string>> emitted;
    const auto anchorMotifs = buildSupportAnchorMotifs(placed, 2);
    for (const auto& note : placed.notes) {
        const int sourceLane = sourceLaneOf(note);
        if (note.type == NoteType::Hold && note.endTime.has_value() && *note.endTime > note.time + 120) {
            SupportEvent event;
            event.kind = SupportKind::Ln;
            event.anchorMotif = MotifKind::LnAnchor;
            event.time = *note.endTime;
            event.sourceLane = sourceLane;
            event.anchorLane = note.lane;
            event.anchorId = note.id;
            if (emitted.insert({event.time, "ln:" + event.anchorId}).second) {
                events.push_back(std::move(event));
            }
        }
    }
    for (const auto& note : placed.notes) {
        if (!isStrongBeat(note.time, placed.timingPoints)) {
            continue;
        }
        SupportEvent event;
        const auto motif = anchorMotifs.find(note.id);
        const auto anchorMotif = motif == anchorMotifs.end() ? MotifKind::Neutral : motif->second;
        if (shouldEmitMirrorSupportEvent(note,
                                         anchorMotif,
                                         options.sourceKeyCount,
                                         options.targetKeyCount)) {
            event.kind = SupportKind::Mirror;
            event.anchorMotif = anchorMotif;
            event.time = note.time;
            event.sourceLane = sourceLaneOf(note);
            event.anchorLane = note.lane;
            event.anchorId = note.id;
            if (emitted.insert({event.time, "mirror:" + event.anchorId}).second) {
                events.push_back(event);
            }
        }
        event.kind = SupportKind::StrongBeat;
        event.anchorMotif = anchorMotif;
        event.time = note.time;
        event.sourceLane = sourceLaneOf(note);
        event.anchorLane = note.lane;
        event.anchorId = note.id;
        if (emitted.insert({event.time, "beat:" + event.anchorId}).second) {
            events.push_back(std::move(event));
        }
    }
    return events;
}

std::vector<int> laneDistributionFor(const Chart& chart, int targetKeyCount) {
    std::vector<int> distribution(static_cast<std::size_t>(std::max(0, targetKeyCount)), 0);
    for (const auto& note : chart.notes) {
        if (note.lane >= 0 && note.lane < targetKeyCount) {
            ++distribution[static_cast<std::size_t>(note.lane)];
        }
    }
    return distribution;
}

bool supportCandidateIsSafe(const Chart& chart,
                            const Note& candidate,
                            int jackWindowMs,
                            int sameSourceGapMs) {
    for (const auto& note : chart.notes) {
        if (note.lane != candidate.lane) {
            continue;
        }
        if (note.time == candidate.time) {
            return false;
        }
        if (note.type == NoteType::Hold && note.endTime.has_value() &&
            candidate.time > note.time && candidate.time <= *note.endTime) {
            return false;
        }
        const int dt = std::abs(note.time - candidate.time);
        const int existingSourceLane = sourceLaneOf(note);
        const int guardedGap = existingSourceLane == candidate.sourceLane ? sameSourceGapMs : jackWindowMs;
        if (dt > 0 && dt <= guardedGap) {
            return false;
        }
    }
    return true;
}

std::string supportIdPrefix(SupportKind kind, MotifKind motif) {
    const std::string generator = kind == SupportKind::Ln
                                      ? "nk2-ln-"
                                      : (kind == SupportKind::Mirror ? "nk2-mirror-" : "nk2-beat-");
    return generator + motifSlug(motif) + "-";
}

void countSupportCandidate(NK2Report& report, SupportKind kind) {
    if (kind == SupportKind::Ln) {
        ++report.lnSupportCandidates;
    } else if (kind == SupportKind::Mirror) {
        ++report.mirrorSupportCandidates;
    } else {
        ++report.strongBeatSupportCandidates;
    }
}

void countSupportAccepted(NK2Report& report, SupportKind kind) {
    if (kind == SupportKind::Ln) {
        ++report.lnSupportAccepted;
    } else if (kind == SupportKind::Mirror) {
        ++report.mirrorSupportAccepted;
    } else {
        ++report.strongBeatSupportAccepted;
    }
}

void countSupportRejected(NK2Report& report, SupportKind kind) {
    if (kind == SupportKind::Ln) {
        ++report.lnSupportRejected;
    } else if (kind == SupportKind::Mirror) {
        ++report.mirrorSupportRejected;
    } else {
        ++report.strongBeatSupportRejected;
    }
}

bool tryAcceptSupportEvent(Chart& chart,
                           NK2Report& report,
                           const NK2Options& options,
                           const SupportEvent& event,
                           std::vector<int>& laneUse,
                           std::map<int, int>& acceptedByPhrase,
                           const std::map<int, int>& sourceNotesByPhrase,
                           int acceptedSoFar,
                           int budget) {
    countSupportCandidate(report, event.kind);

    if (acceptedSoFar >= budget) {
        ++report.supportRejectedByBudget;
        countSupportRejected(report, event.kind);
        return false;
    }

    const int phraseWindow = supportPhraseWindowForTime(event.time);
    const auto sourceCount = sourceNotesByPhrase.find(phraseWindow);
    const int phraseSourceNotes = sourceCount == sourceNotesByPhrase.end() ? 0 : sourceCount->second;
    const int phraseBudget = phraseSupportBudgetFor(options, phraseSourceNotes);
    if (acceptedByPhrase[phraseWindow] >= phraseBudget) {
        ++report.supportRejectedByBudget;
        ++report.supportRejectedByPhraseBudget;
        countSupportRejected(report, event.kind);
        return false;
    }

    const auto ranked = rankedSupportLanes(event, laneUse, options);
    for (const auto& candidateLane : ranked) {
        Note candidate;
        candidate.id = supportIdPrefix(event.kind, event.anchorMotif) + event.anchorId + "-" +
                       std::to_string(event.time);
        candidate.time = event.time;
        candidate.lane = candidateLane.lane;
        candidate.sourceLane = event.sourceLane;
        candidate.type = NoteType::Tap;

        const bool fourToFiveFill = isFourToFiveFillOptions(options);
        const int supportJackWindowMs = supportJackWindowMsFor(options);
        const int sameSourceGapMs =
            fourToFiveFill ? kFourToFiveSupportSameSourceGapMs : kDefaultSupportSameSourceGapMs;
        if (!supportCandidateIsSafe(chart, candidate, supportJackWindowMs, sameSourceGapMs)) {
            continue;
        }

        chart.notes.push_back(candidate);
        if (candidate.lane >= 0 && candidate.lane < options.targetKeyCount) {
            ++laneUse[static_cast<std::size_t>(candidate.lane)];
        }
        countSupportAccepted(report, event.kind);
        ++acceptedByPhrase[phraseWindow];
        countGeneratedProvenance(report, event.anchorMotif);
        return true;
    }

    ++report.supportRejectedBySafety;
    countSupportRejected(report, event.kind);
    return false;
}

void applySupportNotes(Chart& chart, NK2Report& report, const NK2Options& options) {
    const int budget = supportBudgetFor(options, report.intent.totalNotes);
    const auto events = buildSupportEvents(chart, options);
    const auto sourceNotesByPhrase = sourceNoteCountsBySupportPhrase(chart);
    report.supportPhraseWindows = static_cast<int>(sourceNotesByPhrase.size());
    auto laneUse = laneDistributionFor(chart, options.targetKeyCount);
    std::map<int, int> acceptedByPhrase;
    int accepted = 0;
    for (const auto& event : events) {
        if (tryAcceptSupportEvent(chart,
                                  report,
                                  options,
                                  event,
                                  laneUse,
                                  acceptedByPhrase,
                                  sourceNotesByPhrase,
                                  accepted,
                                  budget)) {
            ++accepted;
        }
    }
    std::stable_sort(chart.notes.begin(), chart.notes.end(), [](const Note& lhs, const Note& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        if (lhs.lane != rhs.lane) {
            return lhs.lane < rhs.lane;
        }
        return lhs.id < rhs.id;
    });
}

NK2Report buildBaseReport(const Chart& chart, const NK2Options& options) {
    NK2Report report;
    report.options = options;
    report.chartMutated = false;
    report.layout = buildTargetLayoutSummary(options.targetKeyCount, options.layoutWeights);
    report.intent = buildIntentGraphSummary(chart,
                                            options.sourceKeyCount,
                                            options.targetKeyCount,
                                            options.sameTimeEpsilonMs);
    report.outputNotes = report.intent.totalNotes;

    if (options.sourceKeyCount == options.targetKeyCount && options.mode != Mode::Transform) {
        report.noOp = true;
        report.noOpReason = "same key count without an explicit transform mode";
    }
    if (options.nativeWeight != options.remixWeight) {
        report.warnings.push_back("NK2 default design target is a 50/50 native/remix blend.");
    }
    return report;
}

bool supportsSevenToTenPrototype(const NK2Options& options) {
    return options.sourceKeyCount == 7 && options.targetKeyCount == 10 &&
           options.mode != Mode::Report;
}

bool supportsGenericPrototype(const NK2Options& options) {
    return options.sourceKeyCount > 0 && options.sourceKeyCount <= 10 &&
           options.targetKeyCount > 0 && options.targetKeyCount <= 10 &&
           options.sourceKeyCount != options.targetKeyCount &&
           options.mode != Mode::Report;
}

bool supportsFourToFiveFill(const NK2Options& options) {
    return isFourToFiveFillOptions(options);
}

}  // namespace

NK2Report analyzeReportOnly(const Chart& chart, const NK2Options& options) {
    auto report = buildBaseReport(chart, options);
    if (options.mode != Mode::Report) {
        report.warnings.push_back(
            "NK2 analysis-only path was used; no chart output was mutated.");
    }
    return report;
}

NK2ConversionResult convertChart(const Chart& chart, const NK2Options& options) {
    NK2ConversionResult result;
    result.chart = chart;
    result.report = buildBaseReport(chart, options);

    if (options.mode == Mode::Report) {
        return result;
    }

    if (result.report.noOp) {
        fillDistributionAndSafety(result.report, result.chart);
        return result;
    }

    const bool sevenToTenPrototype = supportsSevenToTenPrototype(options);
    const bool genericPrototype = supportsGenericPrototype(options);
    if (!sevenToTenPrototype && !genericPrototype) {
        result.report.noOp = true;
        result.report.noOpReason = "NK2 conversion currently supports 1K..10K experimental prototypes";
        result.report.warnings.push_back(
            "Use --nk2-mode report for analysis-only on unsupported key-count pairs.");
        fillDistributionAndSafety(result.report, result.chart);
        return result;
    }

    result.chart.meta.targetKeyCount = options.targetKeyCount;
    result.chart.meta.version = convertedDifficultyName(chart.meta.version, options.targetKeyCount);
    result.chart.notes.clear();
    result.chart.notes.reserve(chart.notes.size());

    const auto slices = buildSlices(chart, options.sameTimeEpsilonMs);
    std::vector<int> laneUse(static_cast<std::size_t>(options.targetKeyCount), 0);
    std::vector<PlacedNote> placed;
    std::map<int, PlacedNote> lastBySource;
    placed.reserve(chart.notes.size());

    std::optional<int> previousSingleSourceLane;
    std::optional<int> previousSingleTargetLane;
    std::optional<int> previousSingleTime;
    std::vector<RecentSingleNote> recentSingles;

    for (const auto& slice : slices) {
        std::set<int> occupiedLanes;
        const bool singleNoteSlice = slice.noteIndices.size() == 1;

        for (const auto noteIndex : slice.noteIndices) {
            auto note = chart.notes[noteIndex];
            const int sourceLane = sourceLaneOf(note);
            const bool fastSingleContinuation =
                singleNoteSlice && previousSingleTime.has_value() &&
                note.time > *previousSingleTime && note.time - *previousSingleTime <= 280;
            if (singleNoteSlice && !fastSingleContinuation) {
                recentSingles.clear();
            }
            const bool sourceJackContinuation =
                isImmediateSourceJackContinuation(placed, sourceLane, note.time, 500);
            const auto motif = classifyMotif(note,
                                             singleNoteSlice,
                                             fastSingleContinuation,
                                             sourceJackContinuation,
                                             recentSingles);

            std::optional<int> chosenLane;
            if (sevenToTenPrototype) {
                chosenLane = chooseLane(note,
                                        options,
                                        occupiedLanes,
                                        laneUse,
                                        lastBySource,
                                        placed,
                                        fastSingleContinuation ? previousSingleSourceLane : std::nullopt,
                                        fastSingleContinuation ? previousSingleTargetLane : std::nullopt,
                                        motif);
            } else {
                chosenLane = chooseLaneStrict(note,
                                              options,
                                              occupiedLanes,
                                              laneUse,
                                              lastBySource,
                                              placed,
                                              fastSingleContinuation ? previousSingleSourceLane : std::nullopt,
                                              fastSingleContinuation ? previousSingleTargetLane : std::nullopt,
                                              motif);
            }
            if (!chosenLane.has_value()) {
                ++result.report.droppedNotes;
                continue;
            }
            note.sourceLane = sourceLane;
            note.lane = *chosenLane;
            occupiedLanes.insert(*chosenLane);
            if (*chosenLane >= 0 && *chosenLane < options.targetKeyCount) {
                ++laneUse[static_cast<std::size_t>(*chosenLane)];
            }

            PlacedNote placedNote{note.time, note.lane, sourceLane, note.type, note.endTime};
            placed.push_back(placedNote);
            lastBySource[sourceLane] = placedNote;
            countMotifPlacement(result.report, motif);
            result.chart.notes.push_back(note);

            if (singleNoteSlice) {
                previousSingleSourceLane = sourceLane;
                previousSingleTargetLane = *chosenLane;
                previousSingleTime = note.time;
                recentSingles.push_back({note.time, sourceLane, *chosenLane});
                if (recentSingles.size() > 6) {
                    recentSingles.erase(recentSingles.begin());
                }
            }
        }

        if (!singleNoteSlice) {
            previousSingleSourceLane.reset();
            previousSingleTargetLane.reset();
            previousSingleTime.reset();
            recentSingles.clear();
        }
    }

    std::stable_sort(result.chart.notes.begin(), result.chart.notes.end(), [](const Note& lhs, const Note& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        if (lhs.lane != rhs.lane) {
            return lhs.lane < rhs.lane;
        }
        return lhs.id < rhs.id;
    });

    result.report.chartMutated = true;
    result.report.prototypeName = sevenToTenPrototype ? "nk2-7k10k-panel-bridge-fullfield"
                                                      : "nk2-generic-nk-relane-compress";
    const bool fourToFiveFill = supportsFourToFiveFill(options);
    if (sevenToTenPrototype || fourToFiveFill) {
        applySupportNotes(result.chart, result.report, options);
    } else if (options.mode != Mode::Faithful && options.targetKeyCount > options.sourceKeyCount) {
        result.report.warnings.push_back(
            "NK2 generic prototype currently relanes only; support-note generation is limited to 4K to 5K and 7K to 10K.");
    }
    fillDistributionAndSafety(result.report, result.chart);
    if (result.report.addedNotes == 0) {
        if (options.mode == Mode::Faithful && !fourToFiveFill) {
            result.report.warnings.push_back("NK2 faithful mode keeps source note count; support notes are disabled.");
        } else if (sevenToTenPrototype) {
            result.report.warnings.push_back(
                "NK2 support-note generation found no accepted safe candidates.");
        }
    }
    if (result.report.createdJacks > 0) {
        result.report.warnings.push_back("NK2 prototype created target same-lane repeats from different source lanes.");
    }
    if (result.report.sameTimeCollisions > 0 || result.report.longNoteConflicts > 0) {
        result.report.warnings.push_back("NK2 prototype produced placement conflicts that need repair.");
    }
    return result;
}

}  // namespace keyconv::nk2
