#pragma once

#include "core/chart.hpp"

#include <string>
#include <vector>

namespace keyconv {

enum class RepeatLaneMode {
    SourceLane,
    TargetLane,
};

struct JackGroup {
    int id = -1;
    int lane = 0;
    int startTimeMs = 0;
    int endTimeMs = 0;
    int hitCount = 0;
    double avgGapMs = 0.0;
    int minGapMs = 0;
    std::vector<std::string> noteIds;
};

struct JackValidationResult {
    int sourceJackGroups = 0;
    int preservedJackGroups = 0;
    int splitJackGroups = 0;
    int createdJacks = 0;
    int smoothedJacks = 0;
    double jackPreserveScore = 1.0;
    double createdJackRate = 0.0;
};

struct CreatedJackPair {
    int firstIndex = -1;
    int secondIndex = -1;
    int lane = 0;
    int firstTimeMs = 0;
    int secondTimeMs = 0;
    std::string firstId;
    std::string secondId;
    bool involvesGenerated = false;
};

bool isGeneratedNoteId(const std::string& id);
std::vector<JackGroup> detectJackGroups(const std::vector<Note>& notes,
                                        RepeatLaneMode laneMode,
                                        int jackWindowMs);
bool wouldCreateJackOnLane(const std::vector<Note>& notes,
                           const Note& candidate,
                           int jackWindowMs);
bool isSourceJackIntent(const Chart& original,
                        const Note& first,
                        const Note& second,
                        int jackWindowMs);
std::vector<CreatedJackPair> detectCreatedJackPairs(const Chart& original,
                                                    const std::vector<Note>& converted,
                                                    int jackWindowMs);
bool wouldCreateCreatedJackOnLane(const Chart& original,
                                  const std::vector<Note>& converted,
                                  const Note& candidate,
                                  int candidateLane,
                                  int jackWindowMs,
                                  int ignoredIndex = -1);
JackValidationResult validateJackPreservation(const Chart& original,
                                              const Chart& converted,
                                              int jackWindowMs,
                                              int maxSplitLanes);

}  // namespace keyconv
