#include "core/slice.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace keyconv {

std::vector<TimeSlice> buildTimeSlices(const Chart& chart, int sourceKeyCount, int epsilonMs) {
    std::vector<std::size_t> order(chart.notes.size());
    std::iota(order.begin(), order.end(), static_cast<std::size_t>(0));
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const auto& lhs = chart.notes[a];
        const auto& rhs = chart.notes[b];
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        return lhs.lane < rhs.lane;
    });

    std::vector<TimeSlice> slices;
    std::size_t cursor = 0;
    while (cursor < order.size()) {
        TimeSlice slice;
        slice.index = static_cast<int>(slices.size());
        slice.time = chart.notes[order[cursor]].time;
        slice.sourceMask.assign(static_cast<std::size_t>(std::max(0, sourceKeyCount)), false);
        slice.minLane = std::numeric_limits<int>::max();
        slice.maxLane = std::numeric_limits<int>::min();

        while (cursor < order.size() && chart.notes[order[cursor]].time - slice.time <= epsilonMs) {
            const auto noteIndex = order[cursor];
            const auto& note = chart.notes[noteIndex];
            const int sourceLane = note.sourceLane.value_or(note.lane);
            slice.noteIndices.push_back(noteIndex);
            if (sourceLane >= 0 && sourceLane < sourceKeyCount) {
                slice.sourceMask[static_cast<std::size_t>(sourceLane)] = true;
            }
            slice.minLane = std::min(slice.minLane, sourceLane);
            slice.maxLane = std::max(slice.maxLane, sourceLane);
            ++cursor;
        }

        for (const auto& note : chart.notes) {
            if (note.type != NoteType::Hold || !note.endTime.has_value()) {
                continue;
            }
            const int sourceLane = note.sourceLane.value_or(note.lane);
            if (note.time < slice.time && *note.endTime >= slice.time) {
                slice.activeHolds.insert(sourceLane);
            }
        }

        slice.chordSize = static_cast<int>(slice.noteIndices.size());
        if (slice.noteIndices.empty()) {
            slice.minLane = 0;
            slice.maxLane = 0;
        }
        slice.span = slice.maxLane - slice.minLane;
        slices.push_back(std::move(slice));
    }

    return slices;
}

}  // namespace keyconv

