#include "core/mapping.hpp"

#include <algorithm>
#include <cmath>

namespace keyconv {

int clampInt(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

int xToLane(int x, int keyCount) {
    if (keyCount <= 1) {
        return 0;
    }
    const int lane = static_cast<int>(std::floor(static_cast<double>(x) * keyCount / 512.0));
    return clampInt(lane, 0, keyCount - 1);
}

int laneToX(int lane, int keyCount) {
    if (keyCount <= 1) {
        return 256;
    }
    const int clampedLane = clampInt(lane, 0, keyCount - 1);
    const int x = static_cast<int>(std::floor((static_cast<double>(clampedLane) + 0.5) * 512.0 / keyCount));
    return clampInt(x, 0, 511);
}

int mapLaneDirect(int sourceLane, int sourceK, int targetK) {
    if (sourceK <= 1 || targetK <= 1) {
        return 0;
    }
    const double mapped = static_cast<double>(sourceLane) * static_cast<double>(targetK - 1) /
                          static_cast<double>(sourceK - 1);
    return clampInt(static_cast<int>(std::lround(mapped)), 0, targetK - 1);
}

std::vector<int> getCandidateLanes(int sourceLane, int sourceK, int targetK, int radius) {
    std::vector<int> result;
    if (targetK <= 0) {
        return result;
    }

    const double base = (sourceK <= 1 || targetK <= 1)
                            ? 0.0
                            : static_cast<double>(sourceLane) * static_cast<double>(targetK - 1) /
                                  static_cast<double>(sourceK - 1);
    const int center = clampInt(static_cast<int>(std::lround(base)), 0, targetK - 1);
    for (int lane = center - radius; lane <= center + radius; ++lane) {
        const int clamped = clampInt(lane, 0, targetK - 1);
        if (std::find(result.begin(), result.end(), clamped) == result.end()) {
            result.push_back(clamped);
        }
    }
    return result;
}

}  // namespace keyconv

