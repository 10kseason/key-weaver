#pragma once

#include "core/slice.hpp"

#include <optional>
#include <string>
#include <vector>

namespace keyconv {

enum class PatternKind {
    Single,
    Chord,
    Jack,
    Trill,
    StairUp,
    StairDown,
    Stream,
    Burst,
    AnchorLn,
    ReleaseLn,
};

struct PatternToken {
    PatternKind kind = PatternKind::Single;
    int startSlice = 0;
    int endSlice = 0;
    std::vector<int> lanes;
    double confidence = 1.0;
    int direction = 0;
    std::optional<int> periodicity;
    std::optional<double> density;
};

std::string toString(PatternKind kind);
std::vector<PatternToken> detectPatternTokens(const std::vector<TimeSlice>& slices);

}  // namespace keyconv

