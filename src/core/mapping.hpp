#pragma once

#include <vector>

namespace keyconv {

int clampInt(int value, int low, int high);
int xToLane(int x, int keyCount);
int laneToX(int lane, int keyCount);
int mapLaneDirect(int sourceLane, int sourceK, int targetK);
std::vector<int> getCandidateLanes(int sourceLane, int sourceK, int targetK, int radius = 1);

}  // namespace keyconv

