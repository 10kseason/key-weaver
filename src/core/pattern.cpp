#include "core/pattern.hpp"

#include <algorithm>

namespace keyconv {

namespace {

std::optional<int> singleLane(const TimeSlice& slice) {
    if (slice.chordSize != 1) {
        return std::nullopt;
    }
    return slice.minLane;
}

bool closeEnough(const TimeSlice& a, const TimeSlice& b, int thresholdMs) {
    return b.time > a.time && b.time - a.time <= thresholdMs;
}

void addToken(std::vector<PatternToken>& tokens,
              PatternKind kind,
              int start,
              int end,
              std::vector<int> lanes,
              double confidence,
              int direction = 0) {
    PatternToken token;
    token.kind = kind;
    token.startSlice = start;
    token.endSlice = end;
    token.lanes = std::move(lanes);
    token.confidence = confidence;
    token.direction = direction;
    tokens.push_back(std::move(token));
}

}  // namespace

std::string toString(PatternKind kind) {
    switch (kind) {
        case PatternKind::Single:
            return "single";
        case PatternKind::Chord:
            return "chord";
        case PatternKind::Jack:
            return "jack";
        case PatternKind::Trill:
            return "trill";
        case PatternKind::StairUp:
            return "stair_up";
        case PatternKind::StairDown:
            return "stair_down";
        case PatternKind::Stream:
            return "stream";
        case PatternKind::Burst:
            return "burst";
        case PatternKind::AnchorLn:
            return "anchor_ln";
        case PatternKind::ReleaseLn:
            return "release_ln";
    }
    return "single";
}

std::vector<PatternToken> detectPatternTokens(const std::vector<TimeSlice>& slices,
                                              int jackWindowMs) {
    std::vector<PatternToken> tokens;
    const int jackWindow = std::max(1, jackWindowMs);

    for (const auto& slice : slices) {
        if (slice.chordSize >= 2) {
            addToken(tokens, PatternKind::Chord, slice.index, slice.index, {slice.minLane, slice.maxLane}, 1.0);
        } else if (slice.chordSize == 1) {
            addToken(tokens, PatternKind::Single, slice.index, slice.index, {slice.minLane}, 1.0);
        }

        if (!slice.activeHolds.empty() && slice.chordSize > 0) {
            std::vector<int> lanes(slice.activeHolds.begin(), slice.activeHolds.end());
            addToken(tokens, PatternKind::AnchorLn, slice.index, slice.index, std::move(lanes), 0.9);
        }
    }

    for (std::size_t i = 0; i < slices.size();) {
        const auto lane = singleLane(slices[i]);
        if (!lane.has_value()) {
            ++i;
            continue;
        }

        std::size_t j = i + 1;
        while (j < slices.size() && singleLane(slices[j]) == lane &&
               closeEnough(slices[j - 1], slices[j], jackWindow)) {
            ++j;
        }

        if (j - i >= 3) {
            addToken(tokens,
                     PatternKind::Jack,
                     slices[i].index,
                     slices[j - 1].index,
                     {*lane},
                     0.85);
            i = j;
        } else {
            ++i;
        }
    }

    for (std::size_t i = 0; i + 3 < slices.size(); ++i) {
        const auto a = singleLane(slices[i]);
        const auto b = singleLane(slices[i + 1]);
        const auto c = singleLane(slices[i + 2]);
        const auto d = singleLane(slices[i + 3]);
        if (!a.has_value() || !b.has_value() || !c.has_value() || !d.has_value()) {
            continue;
        }
        if (*a != *b && *a == *c && *b == *d && closeEnough(slices[i], slices[i + 1], 240) &&
            closeEnough(slices[i + 1], slices[i + 2], 240) && closeEnough(slices[i + 2], slices[i + 3], 240)) {
            std::size_t end = i + 3;
            while (end + 1 < slices.size()) {
                const auto next = singleLane(slices[end + 1]);
                if (!next.has_value() || *next != ((end + 1 - i) % 2 == 0 ? *a : *b) ||
                    !closeEnough(slices[end], slices[end + 1], 240)) {
                    break;
                }
                ++end;
            }
            addToken(tokens, PatternKind::Trill, slices[i].index, slices[end].index, {*a, *b}, 0.9);
            i = end;
        }
    }

    for (std::size_t i = 0; i + 2 < slices.size(); ++i) {
        const auto first = singleLane(slices[i]);
        const auto second = singleLane(slices[i + 1]);
        if (!first.has_value() || !second.has_value() || *first == *second ||
            !closeEnough(slices[i], slices[i + 1], 300)) {
            continue;
        }
        const int direction = *second > *first ? 1 : -1;
        std::size_t end = i + 1;
        while (end + 1 < slices.size()) {
            const auto prev = singleLane(slices[end]);
            const auto next = singleLane(slices[end + 1]);
            if (!prev.has_value() || !next.has_value() || *prev == *next ||
                (*next > *prev ? 1 : -1) != direction || !closeEnough(slices[end], slices[end + 1], 300)) {
                break;
            }
            ++end;
        }
        if (end - i + 1 >= 3) {
            std::vector<int> lanes;
            for (std::size_t k = i; k <= end; ++k) {
                lanes.push_back(*singleLane(slices[k]));
            }
            addToken(tokens,
                     direction > 0 ? PatternKind::StairUp : PatternKind::StairDown,
                     slices[i].index,
                     slices[end].index,
                     std::move(lanes),
                     0.9,
                     direction);
            i = end;
        }
    }

    for (std::size_t i = 0; i + 3 < slices.size();) {
        std::size_t end = i;
        while (end + 1 < slices.size() && closeEnough(slices[end], slices[end + 1], 180)) {
            ++end;
        }
        if (end - i + 1 >= 4) {
            const double duration = static_cast<double>(std::max(1, slices[end].time - slices[i].time));
            const double avgInterval = duration / static_cast<double>(end - i);
            PatternToken token;
            token.kind = avgInterval <= 90.0 ? PatternKind::Burst : PatternKind::Stream;
            token.startSlice = slices[i].index;
            token.endSlice = slices[end].index;
            token.confidence = 0.75;
            token.density = static_cast<double>(end - i + 1) * 1000.0 / duration;
            tokens.push_back(std::move(token));
            i = end + 1;
        } else {
            ++i;
        }
    }

    return tokens;
}

}  // namespace keyconv
