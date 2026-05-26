#include "core/distance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace keyconv {

namespace {

constexpr int kFallbackTimingPointMs = 0;
constexpr double kFallbackBeatLengthMs = 500.0;
constexpr int kSnapDivisors[] = {4, 6, 8, 12, 16, 24, 32, 48};

struct TimingGrid {
    int time = kFallbackTimingPointMs;
    double beatLength = kFallbackBeatLengthMs;
};

std::vector<TimingGrid> timingGrids(std::vector<TimingPoint> timingPoints) {
    std::stable_sort(timingPoints.begin(), timingPoints.end(), [](const TimingPoint& a, const TimingPoint& b) {
        return a.time < b.time;
    });

    std::vector<TimingGrid> grids;
    for (const auto& point : timingPoints) {
        if (point.beatLength <= 0.0) {
            continue;
        }
        if (point.uninherited.has_value() && !*point.uninherited) {
            continue;
        }
        grids.push_back(TimingGrid{point.time, point.beatLength});
    }

    if (grids.empty()) {
        grids.push_back(TimingGrid{});
    }
    return grids;
}

TimingGrid gridAt(int time, const std::vector<TimingPoint>& timingPoints) {
    const auto grids = timingGrids(timingPoints);
    TimingGrid selected = grids.front();
    for (const auto& grid : grids) {
        if (grid.time <= time) {
            selected = grid;
        } else {
            break;
        }
    }
    return selected;
}

int roundedGridTime(const TimingGrid& grid, int divisor, int index) {
    const double step = grid.beatLength / static_cast<double>(divisor);
    return static_cast<int>(std::lround(static_cast<double>(grid.time) + static_cast<double>(index) * step));
}

std::vector<int> noteStartAndOptionalEndTimes(const Note& note, bool includeEnd) {
    std::vector<int> times{note.time};
    if (includeEnd && note.type == NoteType::Hold && note.endTime.has_value()) {
        times.push_back(*note.endTime);
    }
    return times;
}

}  // namespace

bool distancePolicyRejects(DistancePolicy policy) {
    return policy == DistancePolicy::AimodSafe || policy == DistancePolicy::Strict;
}

bool isSnapAligned(int time,
                   const std::vector<TimingPoint>& timingPoints,
                   int toleranceMs) {
    const auto grid = gridAt(time, timingPoints);
    int bestDelta = std::numeric_limits<int>::max();

    for (const int divisor : kSnapDivisors) {
        const double step = grid.beatLength / static_cast<double>(divisor);
        if (step <= 0.0) {
            continue;
        }
        const auto nearestIndex = static_cast<int>(std::lround((static_cast<double>(time) - grid.time) / step));
        const int snapped = roundedGridTime(grid, divisor, nearestIndex);
        bestDelta = std::min(bestDelta, std::abs(snapped - time));
    }

    return bestDelta <= std::max(0, toleranceMs);
}

std::vector<int> generateSnapTimesNear(int originalTimeMs,
                                       const std::vector<TimingPoint>& timingPoints,
                                       int maxRollMs) {
    const auto grid = gridAt(originalTimeMs, timingPoints);
    const int maxDistance = std::max(0, maxRollMs);
    std::set<int> unique;

    for (const int divisor : kSnapDivisors) {
        const double step = grid.beatLength / static_cast<double>(divisor);
        if (step <= 0.0) {
            continue;
        }

        const int minIndex =
            static_cast<int>(std::floor((static_cast<double>(originalTimeMs - maxDistance) - grid.time) / step)) - 1;
        const int maxIndex =
            static_cast<int>(std::ceil((static_cast<double>(originalTimeMs + maxDistance) - grid.time) / step)) + 1;
        for (int index = minIndex; index <= maxIndex; ++index) {
            const int candidate = roundedGridTime(grid, divisor, index);
            if (candidate < 0 || candidate == originalTimeMs) {
                continue;
            }
            if (std::abs(candidate - originalTimeMs) <= maxDistance) {
                unique.insert(candidate);
            }
        }
    }

    std::vector<int> candidates(unique.begin(), unique.end());
    std::stable_sort(candidates.begin(), candidates.end(), [originalTimeMs](int a, int b) {
        const int da = std::abs(a - originalTimeMs);
        const int db = std::abs(b - originalTimeMs);
        if (da != db) {
            return da < db;
        }
        return a < b;
    });
    return candidates;
}

bool hasDistanceConflict(const std::vector<Note>& placed,
                         const Note& candidate,
                         const ConvertOptions& options,
                         bool includeHoldEdges) {
    if (!distancePolicyRejects(options.distancePolicy)) {
        return false;
    }

    const int minObjectGap = std::max(0, options.minObjectGapMs);
    const int sameLaneGap = std::max(0, options.sameLaneMinGapMs);
    const auto candidateTimes = noteStartAndOptionalEndTimes(candidate, includeHoldEdges);

    for (const auto& existing : placed) {
        const auto existingTimes = noteStartAndOptionalEndTimes(existing, includeHoldEdges);
        for (const int candidateTime : candidateTimes) {
            for (const int existingTime : existingTimes) {
                const int delta = std::abs(candidateTime - existingTime);
                if (delta == 0) {
                    continue;
                }
                if (minObjectGap > 0 && delta < minObjectGap) {
                    return true;
                }
                if (candidate.lane == existing.lane && sameLaneGap > 0 && delta < sameLaneGap) {
                    return true;
                }
            }
        }
    }

    return false;
}

DistanceValidationResult validateDistance(const std::vector<Note>& notes,
                                          const ConvertOptions& options,
                                          const std::vector<TimingPoint>& timingPoints,
                                          const std::set<std::string>& rolledNoteIds) {
    DistanceValidationResult result;
    std::vector<Note> sorted = notes;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Note& a, const Note& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        if (a.lane != b.lane) {
            return a.lane < b.lane;
        }
        return a.id < b.id;
    });

    const int minObjectGap = std::max(0, options.minObjectGapMs);
    const int sameLaneGap = std::max(0, options.sameLaneMinGapMs);
    const int scanWindow = std::max(minObjectGap, sameLaneGap);

    for (std::size_t i = 0; i < sorted.size(); ++i) {
        if (!isSnapAligned(sorted[i].time, timingPoints, options.snapToleranceMs)) {
            ++result.unsnappedNotes;
            if (!sorted[i].id.empty() && rolledNoteIds.count(sorted[i].id) > 0) {
                ++result.unsnappedRolledNotes;
            }
        }

        for (std::size_t j = i + 1; j < sorted.size(); ++j) {
            const int delta = sorted[j].time - sorted[i].time;
            if (delta == 0) {
                continue;
            }
            if (result.minPositiveDeltaMs == 0 || delta < result.minPositiveDeltaMs) {
                result.minPositiveDeltaMs = delta;
            }
            if (scanWindow > 0 && delta >= scanWindow) {
                break;
            }
            if (minObjectGap > 0 && delta < minObjectGap) {
                ++result.nearTimeConflicts;
            }
            if (sorted[i].lane == sorted[j].lane && sameLaneGap > 0 && delta < sameLaneGap) {
                ++result.sameLaneNearConflicts;
            }
        }
    }

    return result;
}

}  // namespace keyconv
