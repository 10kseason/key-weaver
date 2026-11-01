#include "diagnostics/pattern_hotspot.hpp"
#include "nk2/nk2_convert.hpp"
#include "nk2/nk2_report.hpp"
#include "nk2/safety_audit.hpp"
#include <keyconv/format/osu_exporter.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using keyconv::Chart;
using keyconv::Note;
using keyconv::NoteType;
using namespace keyconv::diagnostics;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Note tap(int time, int lane, const std::string& id = {}) {
    Note note;
    note.time = time;
    note.lane = lane;
    note.id = id;
    return note;
}

Note hold(int time, int lane, int end, const std::string& id = {}) {
    auto note = tap(time, lane, id);
    note.type = NoteType::Hold;
    note.endTime = end;
    return note;
}

void requireSameChart(const Chart& a, const Chart& b) {
    const auto meta = [](const auto& m) {
        return std::tie(m.title, m.artist, m.creator, m.version, m.sourceKeyCount,
                        m.targetKeyCount, m.format, m.mode);
    };
    require(meta(a.meta) == meta(b.meta) && a.warnings == b.warnings &&
                a.raw.sections == b.raw.sections && a.raw.sectionOrder == b.raw.sectionOrder &&
                a.raw.preamble == b.raw.preamble && a.raw.originalLines == b.raw.originalLines,
            "analysis changed chart metadata, raw input, or warnings");
    require(a.notes.size() == b.notes.size() && a.timingPoints.size() == b.timingPoints.size(),
            "analysis changed event or timing point count");
    for (std::size_t i = 0; i < a.notes.size(); ++i) {
        const auto fields = [](const auto& n) {
            return std::tie(n.id, n.time, n.lane, n.type, n.endTime, n.raw, n.sourceLane);
        };
        require(fields(a.notes[i]) == fields(b.notes[i]), "analysis changed or reordered a note");
    }
    for (std::size_t i = 0; i < a.timingPoints.size(); ++i) {
        const auto fields = [](const auto& t) {
            return std::tie(t.time, t.beatLength, t.meter, t.sampleSet, t.sampleIndex,
                            t.volume, t.uninherited, t.effects, t.rawLine);
        };
        require(fields(a.timingPoints[i]) == fields(b.timingPoints[i]), "analysis changed timing");
    }
}

void requireConservation(const PatternHotspotAudit& audit, std::uint64_t count) {
    require(audit.inputNotes == count && audit.totalNotes == count, "event count was lost");
    std::uint64_t sum = 0;
    std::vector<std::uint64_t> byLane(audit.globalLaneCounts.size());
    for (const auto& window : audit.windows) {
        require(std::accumulate(window.laneCounts.begin(), window.laneCounts.end(), std::uint64_t{0}) ==
                    window.noteCount, "window lane counts do not sum to its event count");
        sum += window.noteCount;
        for (std::size_t lane = 0; lane < byLane.size(); ++lane) byLane[lane] += window.laneCounts[lane];
    }
    require(sum == count && byLane == audit.globalLaneCounts, "window partition does not conserve events");
    require(std::accumulate(byLane.begin(), byLane.end(), std::uint64_t{0}) == count,
            "global lane totals do not conserve events");
}

Chart distributionFixture(bool concentrated) {
    const std::array<std::array<int, 10>, 4> biased{{
        {{8,8,4,0,0,0,0,0,0,0}}, {{0,0,4,8,8,0,0,0,0,0}},
        {{0,0,0,0,0,8,8,4,0,0}}, {{0,0,0,0,0,0,0,4,8,8}}
    }};
    Chart chart;
    chart.meta.sourceKeyCount = 10;
    for (int window = 0; window < 4; ++window) {
        int ordinal = 0;
        for (int lane = 0; lane < 10; ++lane) {
            const int count = concentrated ? biased[window][lane] : 2;
            for (int i = 0; i < count; ++i, ++ordinal) {
                chart.notes.push_back(tap(window * 8000 + 100 + ordinal * 350, lane,
                                          "n" + std::to_string(chart.notes.size())));
            }
        }
    }
    return chart;
}

void sameGlobalDifferentLocalUse() {
    const auto balanced = analyzePatternHotspots(distributionFixture(false), 10);
    const auto concentrated = analyzePatternHotspots(distributionFixture(true), 10);
    require(balanced.status == "ok" && concentrated.status == "ok", "fixtures must be eligible");
    require(balanced.globalLaneCounts == std::vector<std::uint64_t>(10, 8) &&
                balanced.globalLaneCounts == concentrated.globalLaneCounts,
            "A/B fixtures must have identical ten-percent global ratios");
    require(balanced.windows.size() == 4 && concentrated.windows.size() == 4, "expected four windows");
    const std::array<std::vector<int>, 4> expected{{{0,1}, {3,4}, {5,6}, {8,9}}};
    for (std::size_t i = 0; i < 4; ++i) {
        require(balanced.windows[i].hotspot == false && balanced.windows[i].flaggedLanes.empty(),
                "balanced windows must not be flagged");
        require(concentrated.windows[i].hotspot == true && concentrated.windows[i].flaggedLanes == expected[i],
                "only the forty-percent lanes should exceed the global ratio by thirty points");
    }
    requireConservation(balanced, 80);
    requireConservation(concentrated, 80);
}

void exactThresholdsAndOverflow() {
    using detail::hasHotspotDeviation;
    require(hasHotspotDeviation(5,20,40,400) == true, "exact +15pp must be included");
    require(hasHotspotDeviation(5,20,160,400) == true, "exact -15pp must be included");
    require(hasHotspotDeviation(5,20,41,400) == false, "14.75pp must be excluded");
    require(hasHotspotDeviation(5,20,39,400) == true, "15.25pp must be included");
    require(hasHotspotDeviation(0,20,40,400) == false, "zero local use differs from ten percent by only 10pp");
    require(!hasHotspotDeviation(0,0,0,0).has_value(), "undefined ratios must be rejected");
    require(!hasHotspotDeviation(21,20,40,400).has_value(), "inconsistent counts must be rejected");
    require(!hasHotspotDeviation(1,20,1,std::numeric_limits<std::uint64_t>::max()).has_value(),
            "overflow must not silently become a negative or floating-point finding");
}

struct CommaNumbers : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};

void deterministicAndReadOnly() {
    auto chart = distributionFixture(true);
    chart.notes.front().raw = "preserve raw note";
    chart.notes.front().sourceLane = 6;
    const auto before = chart;
    const auto json = patternHotspotsToJson(analyzePatternHotspots(chart, 10));
    requireSameChart(before, chart);
    require(patternHotspotsToJson(analyzePatternHotspots(chart, 10)) == json, "repeat changed diagnostic JSON");
    std::reverse(chart.notes.begin(), chart.notes.end());
    require(patternHotspotsToJson(analyzePatternHotspots(chart, 10)) == json, "input order changed results");
    std::rotate(chart.notes.begin(), chart.notes.begin() + 17, chart.notes.end());
    require(patternHotspotsToJson(analyzePatternHotspots(chart, 10)) == json, "permutation changed results");
    struct RestoreLocale {
        std::locale old = std::locale();
        ~RestoreLocale() { std::locale::global(old); }
    } restore;
    std::locale::global(std::locale(std::locale::classic(), new CommaNumbers));
    require(patternHotspotsToJson(analyzePatternHotspots(chart, 10)) == json,
            "global locale must not affect JSON decimals or integer formatting");
}

void insufficientWindowsStillCount() {
    Chart chart;
    for (int i = 0; i < 20; ++i) chart.notes.push_back(tap(i * 100, 0));
    for (int i = 0; i < 19; ++i) chart.notes.push_back(tap(8000 + i * 100, 1));
    const auto audit = analyzePatternHotspots(chart, 10);
    requireConservation(audit, 39);
    require(audit.status == "ok" && audit.windows[0].hotspot == true &&
                !audit.windows[1].hotspot.has_value(), "20 is eligible but 19 must remain unanalysed");
    require(audit.globalLaneCounts[0] == 20 && audit.globalLaneCounts[1] == 19,
            "insufficient windows must remain in global denominators");
}

void boundariesAndLongNoteHeads() {
    Chart chart;
    const int times[] = {std::numeric_limits<int>::min(), -8001, -8000, -1, 0, 7999,
                         8000, 8000, 15999, 16000, std::numeric_limits<int>::max()};
    for (int i = 0; i < 11; ++i) chart.notes.push_back(tap(times[i], i % 10));
    chart.notes.push_back(hold(7999, 9, 16001));
    const auto audit = analyzePatternHotspots(chart, 10);
    requireConservation(audit, 12);
    const std::map<std::int64_t,std::uint64_t> expected{
        {-268436,1}, {-2,1}, {-1,2}, {0,3}, {1,3}, {2,1}, {268435,1}};
    require(audit.windows.size() == expected.size(), "LN tails must not create additional occupied windows");
    for (const auto& window : audit.windows) {
        const auto found = expected.find(window.index);
        require(found != expected.end() && found->second == window.noteCount,
                "boundary event was duplicated, rounded, or assigned with truncating division");
    }
    require(audit.emptyWindowRanges.size() < 10, "long silence must use compressed empty ranges");
}

void noDataAndEmptyRanges() {
    const auto empty = analyzePatternHotspots({}, 10);
    require(empty.status == "no_data" && empty.totalNotes == 0 && empty.windows.empty() &&
                empty.emptyWindowRanges.empty() && empty.globalLaneCounts == std::vector<std::uint64_t>(10,0),
            "empty chart must explicitly report no data");
    Chart sparse;
    sparse.notes = {tap(24000, 0), tap(40000, 9)};
    const auto audit = analyzePatternHotspots(sparse, 10);
    requireConservation(audit, 2);
    require(audit.status == "no_eligible_windows" && audit.emptyWindowRanges.size() == 2,
            "sparse input is not an eligible chart with zero findings");
    require(audit.emptyWindowRanges[0].beginIndex == 0 && audit.emptyWindowRanges[0].endIndexExclusive == 3 &&
                audit.emptyWindowRanges[1].beginIndex == 4 && audit.emptyWindowRanges[1].endIndexExclusive == 5,
            "leading and intermediate empty ranges must be half open with no invented trailing duration");
    require(!patternHotspotsToJson(empty).empty() && !patternHotspotsToJson(audit).empty(),
            "empty and insufficient results must be serializable");
}

void invalidInputHasNoPartialResults() {
    const auto reject = [](const Chart& chart, int keys) {
        const auto result = analyzePatternHotspots(chart, keys);
        require(result.status == "invalid_input" && !result.reasonCode.empty() && result.totalNotes == 0 &&
                    result.globalLaneCounts.empty() && result.windows.empty() && result.emptyWindowRanges.empty(),
                "invalid input must not expose a partially normal count result");
    };
    reject({}, 0);
    reject({}, 19);
    auto chart = distributionFixture(false);
    for (int lane : {-1, 10}) { auto bad = chart; bad.notes.back().lane = lane; reject(bad, 10); }
    auto badType = chart; badType.notes.back().type = static_cast<NoteType>(99); reject(badType, 10);
    for (const auto end : {std::optional<int>{}, std::optional<int>{0}, std::optional<int>{100}}) {
        auto bad = chart;
        bad.notes.back() = hold(100, 0, 200);
        bad.notes.back().endTime = end;
        reject(bad, 10);
    }
    chart.warnings.push_back("parser skipped an invalid event");
    reject(chart, 10);
    Chart wide; wide.notes = {tap(100, 17)};
    const auto valid = analyzePatternHotspots(wide, 18);
    requireConservation(valid, 1);
    require(valid.globalLaneCounts.size() == 18 && valid.globalLaneCounts[17] == 1,
            "explicit key count must retain unused lanes");
    wide.notes.front().lane = 0;
    requireConservation(analyzePatternHotspots(wide, 1), 1);
}

void presetEOutputAndSafetyRemainUnchanged() {
    Chart source;
    source.meta.sourceKeyCount = 7;
    source.meta.version = "Native hotspot fixture";
    keyconv::TimingPoint timing; timing.beatLength = 500.0; timing.uninherited = true;
    source.timingPoints.push_back(timing);
    const int durations[] = {800,1250,2000};
    for (int i = 0; i < 64; ++i) {
        const int time = 1000 + i * 500;
        const auto id = "source-ln-" + std::to_string(i);
        source.notes.push_back(i % 8 == 0 ? hold(time, i % 7, time + durations[(i / 8) % 3], id)
                                         : tap(time, i % 7, id));
    }
    const auto sourceBefore = source;
    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7; options.targetKeyCount = 10; options.mode = keyconv::nk2::Mode::Faithful;
    options.phraseSearch = options.phraseSupport = options.phraseDenseLn = options.phraseSourceLn = true;
    const auto converted = keyconv::nk2::convertChart(source, options);
    require(converted.report.addedHoldNotes > 0 && converted.report.independentSafety.has_value() &&
                converted.report.independentSafety->passed() &&
                converted.report.independentSafety->sourceLnCoverageRequired &&
                converted.report.independentSafety->generatedHoldsOutsideSourceLn == 0,
            "E fixture must exercise generated LNs with strict coverage and safety");
    const auto chartBefore = converted.chart;
    const auto reportBefore = keyconv::nk2::reportToJson(converted.report);
    const auto exportBefore = keyconv::exportOsu(converted.chart, 10);
    const auto audit = analyzePatternHotspots(converted.chart, 10);
    require(audit.status == "ok" || audit.status == "no_eligible_windows", "E result must be analyzable");
    requireConservation(audit, converted.chart.notes.size());
    for (int lane = 0; lane < 10; ++lane) {
        require(audit.globalLaneCounts[lane] == static_cast<std::uint64_t>(converted.report.laneDistribution[lane]),
                "hotspot totals must match E's existing lane-distribution report");
    }
    requireSameChart(chartBefore, converted.chart);
    requireSameChart(sourceBefore, source);
    require(keyconv::nk2::reportToJson(converted.report) == reportBefore &&
                keyconv::exportOsu(converted.chart,10) == exportBefore,
            "analysis must leave E report and exported chart bytes unchanged");
    require(keyconv::nk2::auditSafety(source, converted.chart, 10, 500, true).passed(),
            "an independent audit must still accept E after hotspot analysis");
    const auto repeated = keyconv::nk2::convertChart(source, options);
    require(keyconv::nk2::reportToJson(repeated.report) == reportBefore &&
                keyconv::exportOsu(repeated.chart,10) == exportBefore &&
                patternHotspotsToJson(analyzePatternHotspots(repeated.chart,10)) == patternHotspotsToJson(audit),
            "E conversion and analysis must remain deterministic together");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string,void(*)()>> tests{
        {"same global, different local use", sameGlobalDifferentLocalUse},
        {"exact thresholds and overflow", exactThresholdsAndOverflow},
        {"deterministic read-only analysis", deterministicAndReadOnly},
        {"insufficient windows retain counts", insufficientWindowsStillCount},
        {"boundaries and LN heads", boundariesAndLongNoteHeads},
        {"empty and sparse inputs", noDataAndEmptyRanges},
        {"invalid inputs and explicit key counts", invalidInputHasNoPartialResults},
        {"E output and safety preservation", presetEOutputAndSafetyRemainUnchanged},
    };
    try {
        for (const auto& [name,test] : tests) { test(); std::cout << "[pass] " << name << '\n'; }
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    std::cout << tests.size() << " hotspot tests passed\n";
    return 0;
}
