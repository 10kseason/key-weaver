#pragma once

#include <keyconv/chart.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace keyconv::diagnostics {

inline constexpr std::int64_t kHotspotWindowMs = 8000;
inline constexpr std::uint64_t kHotspotMinNotes = 20;

struct HotspotWindow {
    std::int64_t index = 0;
    std::uint64_t noteCount = 0;
    std::vector<std::uint64_t> laneCounts;
    // null means insufficient data, not a negative finding.
    std::optional<bool> hotspot;
    std::vector<int> flaggedLanes;
};

struct EmptyWindowRange {
    std::int64_t beginIndex = 0;
    std::int64_t endIndexExclusive = 0;
};

struct PatternHotspotAudit {
    std::string status = "invalid_input";
    std::string reasonCode;
    int laneCount = 0;
    std::uint64_t inputNotes = 0;
    std::uint64_t totalNotes = 0;
    std::vector<std::uint64_t> globalLaneCounts;
    std::vector<HotspotWindow> windows;
    std::vector<EmptyWindowRange> emptyWindowRanges;
};

// Reads final note heads only; never sorts, repairs, or converts the input.
PatternHotspotAudit analyzePatternHotspots(const Chart& chart, int laneCount);
std::string patternHotspotsToJson(const PatternHotspotAudit& audit);

namespace detail {
// Exact >= 15 percentage-point comparison. nullopt denotes invalid counts or
// overflow; the caller must not turn that into a negative diagnostic result.
std::optional<bool> hasHotspotDeviation(std::uint64_t laneNotes,
                                      std::uint64_t windowNotes,
                                      std::uint64_t globalLaneNotes,
                                      std::uint64_t totalNotes);
}  // namespace detail

}  // namespace keyconv::diagnostics
