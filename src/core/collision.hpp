#pragma once

#include "core/chart.hpp"

#include <vector>

namespace keyconv {

struct CollisionScan {
    int sameTimeCollisions = 0;
    int longNoteConflicts = 0;
};

CollisionScan detectCollisions(const std::vector<Note>& notes);
std::vector<int> calculateLaneDistribution(const std::vector<Note>& notes, int keyCount);
bool hasSameTimeNote(const std::vector<Note>& notes, int time, int lane);
bool hasLongNoteConflict(const std::vector<Note>& notes, const Note& note, int lane);

}  // namespace keyconv

