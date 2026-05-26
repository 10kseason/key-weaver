#pragma once

#include "core/chart.hpp"

#include <cstddef>
#include <set>
#include <vector>

namespace keyconv {

struct TimeSlice {
    int index = 0;
    int time = 0;
    std::vector<std::size_t> noteIndices;
    std::vector<bool> sourceMask;
    int chordSize = 0;
    int minLane = 0;
    int maxLane = 0;
    int span = 0;
    std::set<int> activeHolds;
};

std::vector<TimeSlice> buildTimeSlices(const Chart& chart, int sourceKeyCount, int epsilonMs = 2);

}  // namespace keyconv

