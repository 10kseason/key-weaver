#include "pattern_hotspot.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <sstream>

namespace keyconv::diagnostics {
namespace {

bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t& result) {
    if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b) return false;
    result = a * b;
    return true;
}

std::int64_t windowIndex(int time) {
    const auto wide = static_cast<std::int64_t>(time);
    // C++ truncates toward zero; negative timestamps need mathematical floor.
    return wide / kHotspotWindowMs - (wide % kHotspotWindowMs < 0 ? 1 : 0);
}

template <typename T>
void writeArray(std::ostream& out, const std::vector<T>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ", ";
        out << values[i];
    }
    out << ']';
}

void writeRatios(std::ostream& out, const std::vector<std::uint64_t>& counts,
                 std::uint64_t total) {
    if (total == 0) {
        out << "null";
        return;
    }
    out << '[';
    for (std::size_t i = 0; i < counts.size(); ++i) {
        if (i) out << ", ";
        out << static_cast<double>(counts[i]) / static_cast<double>(total);
    }
    out << ']';
}

}  // namespace

std::optional<bool> detail::hasHotspotDeviation(std::uint64_t laneNotes,
                                               std::uint64_t windowNotes,
                                               std::uint64_t globalLaneNotes,
                                               std::uint64_t totalNotes) {
    if (windowNotes == 0 || totalNotes == 0 || laneNotes > windowNotes ||
        globalLaneNotes > totalNotes || windowNotes > totalNotes ||
        laneNotes > globalLaneNotes || globalLaneNotes - laneNotes > totalNotes - windowNotes) {
        return std::nullopt;
    }
    std::uint64_t local = 0, global = 0, denominator = 0, left = 0, right = 0;
    if (!multiply(laneNotes, totalNotes, local) ||
        !multiply(globalLaneNotes, windowNotes, global) ||
        !multiply(windowNotes, totalNotes, denominator) ||
        !multiply(local >= global ? local - global : global - local, 20, left) ||
        !multiply(denominator, 3, right)) {
        return std::nullopt;
    }
    return left >= right;
}

PatternHotspotAudit analyzePatternHotspots(const Chart& chart, int laneCount) {
    PatternHotspotAudit result;
    result.laneCount = laneCount;
    result.inputNotes = static_cast<std::uint64_t>(chart.notes.size());
    auto invalid = [&](const char* reason) {
        PatternHotspotAudit failed;
        failed.laneCount = laneCount;
        failed.inputNotes = result.inputNotes;
        failed.reasonCode = reason;
        return failed;
    };
    if (laneCount < 1 || laneCount > 18) return invalid("invalid_lane_count");
    // A permissive parser may have omitted events. Do not certify a partial
    // population as a complete lane-frequency analysis.
    if (!chart.warnings.empty()) return invalid("parser_warnings");
    for (const auto& note : chart.notes) {
        if (note.lane < 0 || note.lane >= laneCount) return invalid("invalid_lane");
        if (note.type != NoteType::Tap && note.type != NoteType::Hold) return invalid("invalid_note_type");
        if (note.type == NoteType::Hold && (!note.endTime || *note.endTime <= note.time)) {
            return invalid("invalid_hold");
        }
    }

    result.globalLaneCounts.assign(static_cast<std::size_t>(laneCount), 0);
    result.totalNotes = result.inputNotes;
    if (chart.notes.empty()) {
        result.status = "no_data";
        return result;
    }

    std::map<std::int64_t, HotspotWindow> windows;
    for (const auto& note : chart.notes) {
        const auto index = windowIndex(note.time);
        auto [it, inserted] = windows.try_emplace(index);
        auto& window = it->second;
        if (inserted) {
            window.index = index;
            window.laneCounts.assign(static_cast<std::size_t>(laneCount), 0);
        }
        // Every Note contributes one head, including generated notes and holds.
        ++window.noteCount;
        ++window.laneCounts[static_cast<std::size_t>(note.lane)];
        ++result.globalLaneCounts[static_cast<std::size_t>(note.lane)];
    }

    result.status = "no_eligible_windows";
    auto nextIndex = std::min<std::int64_t>(0, windows.begin()->first);
    for (auto& [index, window] : windows) {
        if (nextIndex < index) result.emptyWindowRanges.push_back({nextIndex, index});
        nextIndex = index + 1;
        if (window.noteCount >= kHotspotMinNotes) {
            result.status = "ok";
            window.hotspot = false;
            for (int lane = 0; lane < laneCount; ++lane) {
                const auto i = static_cast<std::size_t>(lane);
                const auto flagged = detail::hasHotspotDeviation(window.laneCounts[i], window.noteCount,
                                                                 result.globalLaneCounts[i], result.totalNotes);
                if (!flagged.has_value()) return invalid("count_overflow");
                if (*flagged) {
                    window.hotspot = true;
                    window.flaggedLanes.push_back(lane);
                }
            }
        }
        result.windows.push_back(std::move(window));
    }
    return result;
}

std::string patternHotspotsToJson(const PatternHotspotAudit& audit) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(12);
    out << "{\n  \"schemaVersion\": 1,\n  \"windowMs\": 8000,\n  \"originMs\": 0,\n"
        << "  \"minNotes\": 20,\n  \"thresholdPp\": 15,\n  \"countingUnit\": \"note_head\",\n"
        << "  \"laneIndexBase\": 0,\n  \"status\": \"" << audit.status << "\",\n"
        << "  \"reasonCode\": \"" << audit.reasonCode << "\",\n"
        << "  \"laneCount\": " << audit.laneCount << ",\n"
        << "  \"inputNotes\": " << audit.inputNotes << ",\n"
        << "  \"totalNotes\": " << audit.totalNotes << ",\n  \"globalLaneCounts\": ";
    writeArray(out, audit.globalLaneCounts);
    out << ",\n  \"globalLaneRatios\": ";
    writeRatios(out, audit.globalLaneCounts, audit.totalNotes);
    out << ",\n  \"analysisRangeMs\": ";
    if (audit.windows.empty()) out << "null";
    else out << '[' << std::min<std::int64_t>(0, audit.windows.front().index) * kHotspotWindowMs
             << ", " << (audit.windows.back().index + 1) * kHotspotWindowMs << ']';
    out << ",\n  \"windows\": [";
    for (std::size_t i = 0; i < audit.windows.size(); ++i) {
        const auto& window = audit.windows[i];
        if (i) out << ',';
        out << "\n    {\"index\": " << window.index
            << ", \"startMs\": " << window.index * kHotspotWindowMs
            << ", \"endMsExclusive\": " << (window.index + 1) * kHotspotWindowMs
            << ", \"noteCount\": " << window.noteCount << ", \"laneCounts\": ";
        writeArray(out, window.laneCounts);
        out << ", \"status\": \"" << (window.hotspot.has_value() ? "analyzed" : "insufficient_notes")
            << "\", \"hotspot\": ";
        if (window.hotspot.has_value()) out << (*window.hotspot ? "true" : "false");
        else out << "null";
        out << ", \"laneRatios\": ";
        writeRatios(out, window.laneCounts, window.hotspot.has_value() ? window.noteCount : 0);
        out << ", \"deltaPp\": ";
        if (!window.hotspot.has_value()) out << "null";
        else {
            out << '[';
            for (std::size_t lane = 0; lane < window.laneCounts.size(); ++lane) {
                if (lane) out << ", ";
                out << 100.0 * (static_cast<double>(window.laneCounts[lane]) / static_cast<double>(window.noteCount)
                             - static_cast<double>(audit.globalLaneCounts[lane]) / static_cast<double>(audit.totalNotes));
            }
            out << ']';
        }
        out << ", \"flaggedLanes\": ";
        writeArray(out, window.flaggedLanes);
        out << '}';
    }
    out << "\n  ],\n  \"emptyWindowRanges\": [";
    for (std::size_t i = 0; i < audit.emptyWindowRanges.size(); ++i) {
        if (i) out << ", ";
        out << "{\"beginIndex\": " << audit.emptyWindowRanges[i].beginIndex
            << ", \"endIndexExclusive\": " << audit.emptyWindowRanges[i].endIndexExclusive << '}';
    }
    out << "]\n}\n";
    return out.str();
}

}  // namespace keyconv::diagnostics
