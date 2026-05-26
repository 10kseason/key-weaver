#include "core/collision.hpp"

#include <map>

namespace keyconv {

bool hasSameTimeNote(const std::vector<Note>& notes, int time, int lane) {
    for (const auto& note : notes) {
        if (note.time == time && note.lane == lane) {
            return true;
        }
    }
    return false;
}

bool hasLongNoteConflict(const std::vector<Note>& notes, const Note& note, int lane) {
    for (const auto& placed : notes) {
        if (placed.lane != lane) {
            continue;
        }

        if (placed.type == NoteType::Hold && placed.endTime.has_value()) {
            if (note.time > placed.time && note.time <= *placed.endTime) {
                return true;
            }
        }

        if (note.type == NoteType::Hold && note.endTime.has_value()) {
            if (placed.time > note.time && placed.time <= *note.endTime) {
                return true;
            }
        }
    }
    return false;
}

CollisionScan detectCollisions(const std::vector<Note>& notes) {
    CollisionScan scan;
    std::map<std::pair<int, int>, int> sameTimeLaneCounts;

    for (const auto& note : notes) {
        ++sameTimeLaneCounts[{note.time, note.lane}];
    }

    for (const auto& entry : sameTimeLaneCounts) {
        if (entry.second > 1) {
            scan.sameTimeCollisions += entry.second - 1;
        }
    }

    for (std::size_t i = 0; i < notes.size(); ++i) {
        const auto& hold = notes[i];
        if (hold.type != NoteType::Hold || !hold.endTime.has_value()) {
            continue;
        }
        for (std::size_t j = 0; j < notes.size(); ++j) {
            if (i == j) {
                continue;
            }
            const auto& other = notes[j];
            if (other.lane != hold.lane) {
                continue;
            }
            if (other.time > hold.time && other.time <= *hold.endTime) {
                ++scan.longNoteConflicts;
            }
        }
    }

    return scan;
}

std::vector<int> calculateLaneDistribution(const std::vector<Note>& notes, int keyCount) {
    std::vector<int> distribution(static_cast<std::size_t>(keyCount > 0 ? keyCount : 0), 0);
    for (const auto& note : notes) {
        if (note.lane >= 0 && note.lane < keyCount) {
            ++distribution[static_cast<std::size_t>(note.lane)];
        }
    }
    return distribution;
}

}  // namespace keyconv

