#include "core/collision.hpp"
#include "core/compress.hpp"
#include "core/converter.hpp"
#include "core/convert.hpp"
#include "core/expansion.hpp"
#include "core/assignment.hpp"
#include "core/gesture.hpp"
#include "core/mapping.hpp"
#include "core/optimizer.hpp"
#include "core/pattern.hpp"
#include "core/quality.hpp"
#include "core/repeat.hpp"
#include "core/slice.hpp"
#include "exporter/bms.hpp"
#include "exporter/osu.hpp"
#include "nk2/nk2_convert.hpp"
#include "nk2/nk2_report.hpp"
#include "parser/bms.hpp"
#include "parser/osu.hpp"
#include "keyconv/reconvert_guard.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {

const char* simple4k = R"(osu file format v14

[General]
Mode:3

[Metadata]
Title:Simple 4K
Artist:Test
Creator:keyconv
Version:4K

[Difficulty]
CircleSize:4

[TimingPoints]
0,500,4,2,1,70,1,0

[HitObjects]
64,192,1000,1,0,0:0:0:0:
192,192,1000,1,0,0:0:0:0:
320,192,1500,1,0,0:0:0:0:
448,192,2000,128,0,2500:0:0:0:0:
)";

const char* simple7kLn = R"(osu file format v14

[General]
Mode:3

[Metadata]
Title:Simple 7K LN
Artist:Test
Creator:keyconv
Version:7K

[Difficulty]
CircleSize:7

[HitObjects]
36,192,1000,128,0,1800:0:0:0:0:
182,192,1200,1,0,0:0:0:0:
256,192,1500,128,0,2200:0:0:0:0:
)";

const char* simpleBms = R"(*---------------------- HEADER FIELD

#PLAYER 1
#TITLE Simple BMS
#ARTIST Test
#BPM 120
#PLAYLEVEL 1
#LNTYPE 1
#WAV01 tap.wav
#WAV02 hold.wav

*---------------------- MAIN DATA FIELD

#00011:0100
#00012:0001
#00151:0202
)";

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string distributionText(const std::vector<int>& values) {
    std::ostringstream out;
    out << "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << values[i];
    }
    out << "]";
    return out.str();
}

int activeLaneCount(const std::vector<int>& values) {
    return static_cast<int>(std::count_if(values.begin(), values.end(), [](int value) {
        return value > 0;
    }));
}

int laneDistributionRange(const std::vector<int>& values) {
    if (values.empty()) {
        return 0;
    }
    const auto [minValue, maxValue] = std::minmax_element(values.begin(), values.end());
    return *maxValue - *minValue;
}

keyconv::Chart makeChart(int keyCount, const std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>>& notes) {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = keyCount;
    int index = 0;
    for (const auto& entry : notes) {
        keyconv::Note note;
        note.id = "n" + std::to_string(index++);
        note.time = std::get<0>(entry);
        note.lane = std::get<1>(entry);
        note.sourceLane = note.lane;
        note.type = std::get<2>(entry);
        note.endTime = std::get<3>(entry);
        chart.notes.push_back(note);
    }
    return chart;
}

void addTimingPoint(keyconv::Chart& chart, double beatLength = 500.0) {
    keyconv::TimingPoint timing;
    timing.time = 0;
    timing.beatLength = beatLength;
    timing.uninherited = true;
    chart.timingPoints.push_back(timing);
}

keyconv::Chart makeStreamChart(int keyCount, int count, int intervalMs) {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = keyCount;
    for (int i = 0; i < count; ++i) {
        keyconv::Note note;
        note.id = "s" + std::to_string(i);
        note.time = 1000 + i * intervalMs;
        const int lanePattern[] = {0, 2, 1, 3};
        note.lane = lanePattern[static_cast<std::size_t>(i % 4)] % keyCount;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }
    return chart;
}

int streamPatternPrimaryRejectSum(const keyconv::QualityReport& quality) {
    return quality.rejectedStreamPrimaryByPatternConfidence + quality.rejectedStreamPrimaryByPatternLength +
           quality.rejectedStreamPrimaryByBurst + quality.rejectedStreamPrimaryByJack +
           quality.rejectedStreamPrimaryByLNHeavy + quality.rejectedStreamPrimaryByLocalNps;
}

int streamLanePrimaryRejectSum(const keyconv::QualityReport& quality) {
    return quality.rejectedStreamPrimaryByLaneRole + quality.rejectedStreamPrimaryByCollision +
           quality.rejectedStreamPrimaryByDistance + quality.rejectedStreamPrimaryBySnap +
           quality.rejectedStreamPrimaryByBudget;
}

std::vector<int> lanesOf(const keyconv::Chart& chart) {
    std::vector<keyconv::Note> notes = chart.notes;
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.id < b.id;
    });

    std::vector<int> lanes;
    for (const auto& note : notes) {
        lanes.push_back(note.lane);
    }
    return lanes;
}

int sameLaneRepeatCount(const keyconv::Chart& chart, int thresholdMs = 180) {
    std::vector<keyconv::Note> notes = chart.notes;
    std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.id < b.id;
    });

    int count = 0;
    for (std::size_t i = 1; i < notes.size(); ++i) {
        const int dt = notes[i].time - notes[i - 1].time;
        if (dt > 0 && dt <= thresholdMs && notes[i].lane == notes[i - 1].lane) {
            ++count;
        }
    }
    return count;
}

void testParser() {
    const auto chart = keyconv::parseOsu(simple4k);
    require(chart.meta.sourceKeyCount == 4, "CircleSize should parse as source key count");
    require(chart.meta.mode == 3, "Mode should parse");
    require(chart.notes.size() == 4, "simple4k should have four notes");
    require(chart.notes[0].lane == 0, "x=64 should map to lane 0 in 4K");
    require(chart.notes[1].lane == 1, "x=192 should map to lane 1 in 4K");
    require(chart.notes[3].type == keyconv::NoteType::Hold, "last note should be hold");
    require(chart.notes[3].endTime == 2500, "hold endTime should parse");
}

void testInvalidHitObjectWarning() {
    const std::string invalid = R"(osu file format v14

[General]
Mode:3

[Difficulty]
CircleSize:4

[HitObjects]
bad
64,192,1000,1,0,0:0:0:0:
)";
    const auto chart = keyconv::parseOsu(invalid);
    require(chart.notes.size() == 1, "invalid hit object should be skipped");
    require(!chart.warnings.empty(), "invalid hit object should create warning");
}

void testMapping() {
    std::vector<int> mapped;
    for (int lane = 0; lane < 4; ++lane) {
        mapped.push_back(keyconv::mapLaneDirect(lane, 4, 7));
    }
    require((mapped == std::vector<int>{0, 2, 4, 6}), "4K to 7K direct mapping should preserve edges");

    const auto candidates = keyconv::getCandidateLanes(1, 4, 10, 1);
    require((candidates == std::vector<int>{2, 3, 4}), "candidate lanes should center around mapped lane");
    require(keyconv::xToLane(keyconv::laneToX(9, 10), 10) == 9, "lane x roundtrip should stay in lane");
}

void testExporterRoundtrip() {
    auto chart = keyconv::parseOsu(simple4k);
    chart.meta.targetKeyCount = 10;
    const auto text = keyconv::exportOsu(chart, 10);
    const auto reparsed = keyconv::parseOsu(text);
    require(reparsed.meta.sourceKeyCount == 10, "export should update CircleSize");
    require(reparsed.meta.version == "4K KeyWeaver10K", "export should append KeyWeaver target marker to difficulty name");
    require(reparsed.meta.creator == "KeyWeaver", "export should rename default keyconv creator to KeyWeaver");
    require(reparsed.notes.size() == chart.notes.size(), "roundtrip should keep note count");
    require(reparsed.notes[3].type == keyconv::NoteType::Hold, "roundtrip should keep hold note");
    require(reparsed.notes[3].endTime == 2500, "roundtrip should keep hold endTime");
}

void testBmsParserExporterRoundtrip() {
    const auto chart = keyconv::parseBms(simpleBms);
    require(chart.meta.format == "bms", "BMS parser should set chart format");
    require(chart.meta.sourceKeyCount == 2, "BMS parser should infer used key count");
    require(chart.notes.size() == 3, "BMS parser should read taps and LNTYPE 1 holds");
    require(chart.notes[2].type == keyconv::NoteType::Hold, "BMS parser should pair long-note channel objects");

    keyconv::ConvertOptions options;
    options.sourceKeyCount = chart.meta.sourceKeyCount;
    options.targetKeyCount = 4;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto text = keyconv::exportBms(result.chart, 4);
    require(text.find("KeyWeaver4K") != std::string::npos, "BMS exporter should mark converted key count");

    const auto reparsed = keyconv::parseBms(text);
    require(!reparsed.notes.empty(), "exported BMS should reparse playable notes");
    require(reparsed.meta.sourceKeyCount >= 1, "exported BMS should infer playable keys");
}

void testBmsExporterKeyModeHeaders() {
    const auto chart = makeChart(10,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 5, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 6, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 9, keyconv::NoteType::Tap, std::nullopt},
                                 });

    const auto fourKey = keyconv::exportBms(chart, 4);
    require(fourKey.find("#PLAYER 1\n") != std::string::npos, "4K BMS output should be SP");
    require(fourKey.find("#4K\n") != std::string::npos, "4K BMS output should write #4K extension command");

    const auto eightKey = keyconv::exportBms(chart, 8);
    require(eightKey.find("#PLAYER 1\n") != std::string::npos, "8K BMS output should be SP");
    require(eightKey.find("#8K\n") != std::string::npos, "8K BMS output should write #8K extension command");

    const auto nineKey = keyconv::exportBms(chart, 9);
    require(nineKey.find("#PLAYER 1\n") != std::string::npos, "9K PMS-style output should stay SP");
    require(nineKey.find("#9K\n") == std::string::npos, "9K PMS-style output should not write a #9K extension");
    require(nineKey.find("#00016:") != std::string::npos, "9K PMS-style output should use 1P channel 16");
    require(nineKey.find("#00017:") != std::string::npos, "9K PMS-style output should use 1P channel 17");
    require(nineKey.find("#00021:") == std::string::npos, "9K PMS-style output should not spill into 2P channels");

    const auto tenKey = keyconv::exportBms(chart, 10);
    require(tenKey.find("#PLAYER 3\n") != std::string::npos, "10K BMS output should use double-play player mode");
    require(tenKey.find("#10K\n") == std::string::npos, "10K BMS output should not write a #10K extension");
    require(tenKey.find("#00016:") == std::string::npos, "10K BMS output should not use 1P scratch");
    require(tenKey.find("#00026:") == std::string::npos, "10K BMS output should not use 2P scratch");
    require(tenKey.find("#00021:") != std::string::npos, "10K BMS output should use 2P key channels");
}

void testConvert() {
    const auto chart = keyconv::parseOsu(simple4k);
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    require(result.chart.meta.targetKeyCount == 10, "convert should set target key count");
    require(result.chart.notes.size() == chart.notes.size(), "convert should keep note count");
    require(result.report.totalNotes == 4, "report should include total notes");
    require(result.report.holdNotes == 1, "report should count hold notes");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 10, "converted lane should be inside target range");
    }
}

void testCompressionKeepsHolds() {
    const auto chart = keyconv::parseOsu(simple7kLn);
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    require(result.chart.notes.size() == chart.notes.size(), "7K to 4K should keep note count with shift policy");
    require(result.report.holdNotes == 2, "hold notes should remain holds");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 4, "compressed lane should be inside target range");
        if (note.type == keyconv::NoteType::Hold) {
            require(note.endTime.has_value() && *note.endTime > note.time, "hold endTime should remain valid");
        }
    }
}

void testCollisionPolicies() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 4;
    chart.notes = {
        keyconv::Note{"a", 1000, 0, keyconv::NoteType::Tap, std::nullopt, "", 0},
        keyconv::Note{"b", 1000, 0, keyconv::NoteType::Tap, std::nullopt, "", 0},
        keyconv::Note{"c", 1100, 0, keyconv::NoteType::Hold, 1400, "", 0},
        keyconv::Note{"d", 1200, 0, keyconv::NoteType::Tap, std::nullopt, "", 0},
    };

    const auto scan = keyconv::detectCollisions(chart.notes);
    require(scan.sameTimeCollisions == 1, "same-time same-lane collision should be detected");
    require(scan.longNoteConflicts == 1, "LN conflict should be detected");

    keyconv::ConvertOptions shiftOptions;
    shiftOptions.sourceKeyCount = 4;
    shiftOptions.targetKeyCount = 4;
    shiftOptions.style = keyconv::ConversionStyle::Direct;
    shiftOptions.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;
    const auto shifted = keyconv::convertChart(chart, shiftOptions);
    require(shifted.report.shiftedNotes > 0, "shift-nearest should shift colliding notes when possible");
    require(shifted.report.sameTimeCollisions == 0, "shift-nearest should resolve same-time collision here");

    keyconv::ConvertOptions mergeOptions = shiftOptions;
    mergeOptions.collisionPolicy = keyconv::CollisionPolicy::Merge;
    const auto merged = keyconv::convertChart(chart, mergeOptions);
    require(merged.report.mergedNotes == 1, "merge should merge duplicate tap");

    keyconv::ConvertOptions dropOptions = shiftOptions;
    dropOptions.collisionPolicy = keyconv::CollisionPolicy::Drop;
    const auto dropped = keyconv::convertChart(chart, dropOptions);
    require(dropped.report.droppedNotes > 0, "drop should remove colliding notes");
    require(dropped.chart.notes.size() < chart.notes.size(), "drop should reduce note count");
}

void testPpgPatternDetection() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });
    const auto slices = keyconv::buildTimeSlices(chart, 4, 2);
    const auto tokens = keyconv::detectPatternTokens(slices);
    bool sawStairUp = false;
    bool sawStream = false;
    for (const auto& token : tokens) {
        sawStairUp = sawStairUp || token.kind == keyconv::PatternKind::StairUp;
        sawStream = sawStream || token.kind == keyconv::PatternKind::Stream || token.kind == keyconv::PatternKind::Burst;
    }
    require(slices.size() == 4, "PPG should build one slice per stair note");
    require(sawStairUp, "PPG should detect stair_up");
    require(sawStream, "PPG should detect stream or burst context");
}

void testPpgStairPreserved() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 4, "converted stair should keep note count");
    require(lanes[0] < lanes[1] && lanes[1] < lanes[2] && lanes[2] < lanes[3],
            "4K stair should remain increasing after PPG conversion");
}

void testPpgTrillPreserved() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes[0] == lanes[2] && lanes[1] == lanes[3], "trill should remain an alternating two-lane pattern");
    require(lanes[0] != lanes[1], "trill lanes should not collapse");
}

void testGestureRailSevenKeyAscendingStairLeftZone() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 4, "7K stair should keep note count");
    const auto rail = keyconv::buildGestureRail(chart, 7, 10, 2, 500, true);
    for (const auto& note : chart.notes) {
        const auto* hint = keyconv::findGestureHint(&rail, note.id);
        require(hint != nullptr, "left-panel stair should receive gesture hints");
        require(hint->role == keyconv::PhraseRole::LeftHandVoice,
                "left-panel 7K stair should be classified as a left-hand voice");
    }
    require(lanes[0] <= lanes[1] && lanes[1] <= lanes[2] && lanes[2] <= lanes[3],
            "7K ascending stair should remain monotonic in 10K");
    require(lanes.front() >= 0 && lanes.back() <= 4, "left-hand 7K stair should stay in the 10K left zone");
    require(result.report.quality.detectedStairs >= 1, "gesture report should detect the source stair");
    require(result.report.quality.brokenStairs == 0, "gesture rail should preserve the source stair");
    require(result.report.quality.createdJacks == 0, "gesture rail stair should not create jacks");
    require(result.report.quality.collisionCount == 0, "gesture rail stair should not create collisions");
    require(result.report.quality.lnConflictCount == 0, "gesture rail stair should not create LN conflicts");
}

void testGestureRailSevenKeyDescendingStairLeftZone() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 4, "7K descending stair should keep note count");
    require(lanes[0] >= lanes[1] && lanes[1] >= lanes[2] && lanes[2] >= lanes[3],
            "7K descending stair should remain monotonic down in 10K");
    require(lanes.back() >= 0 && lanes.front() <= 4, "left-hand descending stair should stay in 10K left zone");
    require(result.report.quality.detectedStairs >= 1, "gesture report should detect descending stair");
    require(result.report.quality.brokenStairs == 0, "gesture rail should preserve descending stair");
}

void testGestureRailSevenKeyAscendingStairRightFiveKeyPanel() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 4, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 5, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 6, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 4, "right-panel 7K stair should keep note count");
    const auto rail = keyconv::buildGestureRail(chart, 7, 10, 2, 500, true);
    for (const auto& note : chart.notes) {
        const auto* hint = keyconv::findGestureHint(&rail, note.id);
        require(hint != nullptr, "right-panel stair should receive gesture hints");
        require(hint->role == keyconv::PhraseRole::RightHandVoice,
                "high-side 7K stair should be classified as a right-hand voice");
    }
    require(lanes[0] <= lanes[1] && lanes[1] <= lanes[2] && lanes[2] <= lanes[3],
            "right-panel 7K stair should remain monotonic in 10K");
    for (std::size_t i = 1; i < lanes.size(); ++i) {
        require(std::abs(lanes[i] - lanes[i - 1]) <= 2,
                "right-panel 7K stair should preserve compact voice-leading inside the 5K panel");
    }
    require(lanes.front() >= 5 && lanes.back() <= 9,
            "high-side 7K stair should be recomposed inside the right 5K panel");
    require(result.report.quality.detectedStairs >= 1, "gesture report should detect right-panel stair");
    require(result.report.quality.brokenStairs == 0, "right-panel stair should preserve gesture shape");
    require(result.report.quality.createdJacks == 0, "right-panel stair should not create jacks");
    require(result.report.quality.collisionCount == 0, "right-panel stair should not create collisions");
    require(result.report.quality.lnConflictCount == 0, "right-panel stair should not create LN conflicts");
}

void testGestureRailSevenKeyTrillShape() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1625, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    std::vector<int> unique = lanes;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    require(unique.size() == 2, "7K trill should remain centered on two target lanes");
    require(unique.back() - unique.front() <= 3, "7K trill target lanes should stay close");
    require(lanes[0] == lanes[2] && lanes[2] == lanes[4], "trill A side should stay stable");
    require(lanes[1] == lanes[3] && lanes[3] == lanes[5], "trill B side should stay stable");
    require(lanes[0] != lanes[1], "trill sides should not collapse");
    require(result.report.quality.detectedTrills >= 1, "gesture report should detect trill");
    require(result.report.quality.brokenTrills == 0, "gesture rail should preserve trill shape");
}

void testGestureRailSourceJackIdentity() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.detectedJacks >= 1, "gesture report should detect source jack");
    require(result.report.quality.brokenJacks == 0, "source jack identity should be preserved or adjacent-safe");
    require(result.report.quality.createdJacksFromBaseMapping == 0, "source jack handling should not create unrelated base jacks");
    require(result.report.quality.unsolvedCreatedJacks == 0, "source jack handling should leave no unsolved created jacks");
}

void testSevenToTenSourceAnchorsTakePriorityOverLeftEdgeBalance() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int pattern[] = {0, 2, 1, 3, 1, 2, 0, 3};
    for (int i = 0; i < 64; ++i) {
        keyconv::Note note;
        note.id = "lb" + std::to_string(i);
        note.time = 1000 + i * 400;
        note.lane = pattern[static_cast<std::size_t>(i % 8)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto& distribution = result.report.quality.laneDistribution;
    require(distribution.size() == 10, "7K to 10K balance test should report ten lanes");
    require(distribution[0] > 0, "7K to 10K should keep using the left edge lane");
    std::vector<int> sourceZeroTargets;
    for (const auto& note : result.chart.notes) {
        if (note.sourceLane.value_or(note.lane) == 0) {
            sourceZeroTargets.push_back(note.lane);
        }
    }
    require(!sourceZeroTargets.empty(), "left-edge source phrase should survive conversion");
    std::set<int> sourceZeroUnique(sourceZeroTargets.begin(), sourceZeroTargets.end());
    require(sourceZeroUnique.size() <= 3,
            "repeated left-edge source phrases should stay compact while allowing coverage pressure");
    for (const int lane : sourceZeroTargets) {
        require(lane >= 0 && lane <= 4,
                "repeated left-edge source phrases should stay in the left 5K panel");
    }
    require(result.report.quality.createdJacks == 0, "left balance should not create jacks");
    require(result.report.quality.collisionCount == 0, "left balance should not create collisions");
    require(result.report.quality.lnConflictCount == 0, "left balance should not create LN conflicts");
}

void testSevenToTenSparseSourceLaneAnchor() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 5; ++i) {
        keyconv::Note note;
        note.id = "anchor" + std::to_string(i);
        note.time = 1000 + i * 500;
        note.lane = 2;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == chart.notes.size(), "source-lane anchor test should keep note count");
    const auto [minLane, maxLane] = std::minmax_element(lanes.begin(), lanes.end());
    require(*maxLane - *minLane <= 2,
            "same sparse 7K source lane should stay compact while allowing nearby coverage pressure");
    for (const int lane : lanes) {
        require(lane >= 0 && lane <= 4,
                "same sparse 7K source lane should stay inside the left 5K panel");
    }
    require(result.report.quality.createdJacks == 0, "source-lane anchoring should not create unrelated jacks");
    require(result.report.quality.collisionCount == 0, "source-lane anchoring should not create collisions");
    require(result.report.quality.lnConflictCount == 0, "source-lane anchoring should not create LN conflicts");
}

void testTenKeyNonJackSourceLaneAnchorRelaxesInsidePanel() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 8; ++i) {
        keyconv::Note note;
        note.id = "relax" + std::to_string(i);
        note.time = 1000 + i * 700;
        note.lane = 1;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    std::set<int> unique(lanes.begin(), lanes.end());
    require(unique.size() >= 2,
            "10K non-jack source-lane anchors should loosen enough to use nearby lanes");
    for (const int lane : lanes) {
        require(lane >= 0 && lane <= 4,
                "10K relaxed non-jack anchors should stay inside the source hand panel");
    }
    require(result.report.quality.sourceJackGroups == 0,
            "10K non-jack relaxation fixture should stay outside the jack window");
    require(result.report.quality.createdJacks == 0,
            "10K non-jack relaxation should not create target jacks from different source lanes");
}

void testSevenToTenStagedPlannerSplitsCenterBridge() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 10; ++i) {
        keyconv::Note note;
        note.id = "bridge" + std::to_string(i);
        note.time = 1000 + i * 700;
        note.lane = 3;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    std::set<int> unique(lanes.begin(), lanes.end());
    require(result.report.quality.tenKeyPlanner == "staged-7-9-10",
            "7K to 10K playable conversion should use the staged 7->9->10 planner by default");
    require(unique.size() >= 2,
            "the staged 9K center bridge should loosen source lane 3 across the 10K center split");
    require(unique.count(4) > 0 || unique.count(3) > 0,
            "center bridge should keep a left-center anchor");
    require(unique.count(5) > 0 || unique.count(6) > 0,
            "center bridge should expand into the right-center 5K panel");
    for (const int lane : lanes) {
        require(lane >= 3 && lane <= 6,
                "center bridge expansion should stay near the middle lanes");
    }
    require(result.report.quality.createdJacks == 0,
            "staged center bridge should not create target jacks");
}

void testSevenToTenMirrorCompressPlannerUsesFourteenLanePairs() {
    const auto edgeSet =
        keyconv::generateCandidateLanes(0,
                                        7,
                                        10,
                                        keyconv::ConversionStyle::Playable,
                                        keyconv::TenKeyPlannerPolicy::StagedMirrorCompress);
    require(std::find(edgeSet.candidates.begin(), edgeSet.candidates.end(), 0) != edgeSet.candidates.end(),
            "7->14->10 mirror planner should expose the left compressed edge");
    require(std::find(edgeSet.candidates.begin(), edgeSet.candidates.end(), 9) != edgeSet.candidates.end(),
            "7->14->10 mirror planner should expose the right compressed edge");

    const auto centerSet =
        keyconv::generateCandidateLanes(6,
                                        7,
                                        10,
                                        keyconv::ConversionStyle::Playable,
                                        keyconv::TenKeyPlannerPolicy::StagedMirrorCompress);
    require(std::find(centerSet.candidates.begin(), centerSet.candidates.end(), 4) != centerSet.candidates.end(),
            "7->14->10 mirror planner should expose the left center-compressed lane");
    require(std::find(centerSet.candidates.begin(), centerSet.candidates.end(), 5) != centerSet.candidates.end(),
            "7->14->10 mirror planner should expose the right center-compressed lane");

    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 21; ++i) {
        keyconv::Note note;
        note.id = "mirror" + std::to_string(i);
        note.time = 1000 + i * 650;
        note.lane = phrase[static_cast<std::size_t>(i % 7)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.tenKeyPlannerPolicy = keyconv::TenKeyPlannerPolicy::StagedMirrorCompress;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.tenKeyPlanner == "staged-7-14-10",
            "explicit 7->14->10 mirror-compress planner should be reported when active");
    require(result.chart.notes.size() == chart.notes.size(),
            "7->14->10 mirror-compress planner should preserve note count without tap-plus");
    require(result.report.quality.createdJacks == 0,
            "7->14->10 mirror-compress planner should not create target jacks");
    require(result.report.quality.collisionCount == 0,
            "7->14->10 mirror-compress planner should not create collisions");
    require(result.report.quality.lnConflictCount == 0,
            "7->14->10 mirror-compress planner should not create LN conflicts");
}

void testSevenToTenMirrorCompressPlannerFillsPanelCenters() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int phrase[] = {0, 1, 2, 4, 5, 6};
    for (int i = 0; i < 120; ++i) {
        keyconv::Note note;
        note.id = "panel_center" + std::to_string(i);
        note.time = 1000 + i * 220;
        note.lane = phrase[static_cast<std::size_t>(i % 6)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.tenKeyPlannerPolicy = keyconv::TenKeyPlannerPolicy::StagedMirrorCompress;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto distribution = result.report.laneDistribution;
    require(distribution.size() == 10, "mirror-compress panel-center fixture should produce 10 lanes");
    const double leftAverage =
        static_cast<double>(distribution[0] + distribution[1] + distribution[2] + distribution[3] + distribution[4]) /
        5.0;
    const double rightAverage =
        static_cast<double>(distribution[5] + distribution[6] + distribution[7] + distribution[8] + distribution[9]) /
        5.0;
    require(distribution[2] >= static_cast<int>(leftAverage * 0.55),
            "mirror-compress should backfill left 5K panel center lane: " + distributionText(distribution));
    require(distribution[7] >= static_cast<int>(rightAverage * 0.55),
            "mirror-compress should backfill right 5K panel center lane: " + distributionText(distribution));
    require(result.report.quality.createdJacks == 0,
            "mirror-compress panel-center fill should not create target jacks");
    require(result.report.quality.collisionCount == 0,
            "mirror-compress panel-center fill should not create collisions");
    require(result.report.quality.lnConflictCount == 0,
            "mirror-compress panel-center fill should not create LN conflicts");
}

void testTenKeyFullFieldRemixRotatesEchoWithinOppositeZone() {
    require(keyconv::rotateWithinZone(9, 0, 5, 5, 2) == 9,
            "phase rotation should keep the mirror lane at slice zero");
    require(keyconv::rotateWithinZone(9, 1, 5, 5, 2) == 6,
            "phase step 2 should move through the 5-lane zone");
    require(keyconv::rotateWithinZone(9, 4, 5, 5, 2) == 7,
            "phase step 2 should visit the full 5-lane zone before repeating");
}

void testTenKeyFullFieldRailCoversAllNotesWithoutStructChanges() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 14; ++i) {
        keyconv::Note note;
        note.id = "rail" + std::to_string(i);
        note.time = 1000 + i * 250;
        note.lane = phrase[static_cast<std::size_t>(i % 7)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    const auto rail = keyconv::buildFullFieldRail(chart, 7, 10, 2, 500, true);
    require(rail.enabled, "full-field rail should stay enabled when requested");
    require(rail.hintsByNoteId.size() == chart.notes.size(),
            "full-field rail should provide a hint for each source note");
    const auto first = rail.hintsByNoteId.find("rail0");
    require(first != rail.hintsByNoteId.end(), "full-field rail should include the first note");
    require(first->second.role == keyconv::PhraseRole::LeftHandVoice,
            "full-field rail should start the primary phrase on the left hand");
    require(first->second.zoneStart == 0 && first->second.zoneEnd == 4,
            "left-hand full-field zone should be lanes 0..4");
}

void testTenKeyFullFieldStreamAlternatesHandsInsideLongToken() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    addTimingPoint(chart);
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6, 5};
    for (int i = 0; i < 24; ++i) {
        keyconv::Note note;
        note.id = "stream" + std::to_string(i);
        note.time = 1000 + i * 120;
        note.lane = phrase[static_cast<std::size_t>(i % 8)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    const auto rail = keyconv::buildFullFieldRail(chart, 7, 10, 2, 500, true);
    int left = 0;
    int right = 0;
    for (int i = 0; i < 24; ++i) {
        const auto found = rail.hintsByNoteId.find("stream" + std::to_string(i));
        require(found != rail.hintsByNoteId.end(), "full-field stream rail should cover each stream note");
        if (found->second.role == keyconv::PhraseRole::LeftHandVoice) {
            ++left;
        } else if (found->second.role == keyconv::PhraseRole::RightHandVoice) {
            ++right;
        }
    }

    require(left > 0 && right > 0, "long full-field stream should not collapse into one hand");
    require(std::abs(left - right) <= 1, "long full-field stream should alternate primary hands evenly");
}

void testTenKeyFullFieldLongStairAlternatesHandsInsideToken() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    addTimingPoint(chart);
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 14; ++i) {
        keyconv::Note note;
        note.id = "stair" + std::to_string(i);
        note.time = 1000 + i * 250;
        note.lane = phrase[static_cast<std::size_t>(i % 7)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    const auto rail = keyconv::buildFullFieldRail(chart, 7, 10, 2, 500, true);
    int left = 0;
    int right = 0;
    for (int i = 0; i < 7; ++i) {
        const auto found = rail.hintsByNoteId.find("stair" + std::to_string(i));
        require(found != rail.hintsByNoteId.end(), "full-field stair rail should cover each stair note");
        if (found->second.role == keyconv::PhraseRole::LeftHandVoice) {
            ++left;
        } else if (found->second.role == keyconv::PhraseRole::RightHandVoice) {
            ++right;
        }
    }

    require(left > 0 && right > 0, "long full-field stair should not collapse into one hand");
}

void testTenKeyFullFieldRemixDensityAndSafety() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    addTimingPoint(chart);
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 20; ++i) {
        keyconv::Note note;
        note.id = "fullfield" + std::to_string(i);
        note.time = 1000 + i * 250;
        note.lane = phrase[static_cast<std::size_t>(i % 7)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;
    options.tenKFullFieldRemix = true;
    options.tenKFullFieldRemixDensityCeiling = 1.6;
    options.tenKFullFieldRemixPhaseStep = 2;
    options.maxAddedPerMeasure = 64;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "full-field remix should be deterministic");
    require(keyconv::reportToJson(first.report) == keyconv::reportToJson(second.report),
            "full-field remix report should be deterministic");

    const auto& quality = first.report.quality;
    require(quality.expansionComposerProfile == "full-field-remix",
            "full-field remix should report the dedicated composer profile");
    require(std::abs(quality.targetAddedNoteRatio - 0.60) < 1e-9,
            "full-field remix should use the mode-local 60 percent added-note target");
    const double totalRatio =
        static_cast<double>(first.chart.notes.size()) / static_cast<double>(chart.notes.size());
    require(totalRatio >= 1.45 && totalRatio <= 1.65,
            "full-field remix should land near the 1.6x density target");
    require(quality.createdJacks == 0, "full-field remix should not create target jacks");
    require(quality.collisionCount == 0, "full-field remix should not create collisions");
    require(quality.lnConflictCount == 0, "full-field remix should not create LN conflicts");

    std::set<int> usedLanes;
    std::map<int, std::vector<int>> lanesByTime;
    for (const auto& note : first.chart.notes) {
        usedLanes.insert(note.lane);
        lanesByTime[note.time].push_back(note.lane);
    }
    require(usedLanes.size() == 10, "full-field remix should exercise all 10 lanes");
    for (auto& [time, lanes] : lanesByTime) {
        (void)time;
        std::vector<int> left;
        std::vector<int> right;
        for (const int lane : lanes) {
            (lane < 5 ? left : right).push_back(lane);
        }
        for (const auto& hand : {left, right}) {
            if (hand.empty()) {
                continue;
            }
            const auto [minIt, maxIt] = std::minmax_element(hand.begin(), hand.end());
            require(*maxIt - *minIt <= 4,
                    "full-field remix same-slice chord should stay within per-hand 5-lane reach");
        }
    }
}

void testEightKeyPlayableCandidateRadiusRemainsStable() {
    const auto eightKey = keyconv::generateCandidateLanes(1, 7, 8, keyconv::ConversionStyle::Playable);

    require(eightKey.radius == 2, "8K playable candidate radius should keep the existing expansion width");
    require(std::find(eightKey.candidates.begin(), eightKey.candidates.end(), 4) == eightKey.candidates.end(),
            "8K playable candidates should not inherit 10K-only relaxation behavior");
}

void testSevenToTenNonGestureChordsStayInSourcePanels() {
    const auto lowChord = makeChart(7,
                                    {
                                        {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                        {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                        {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                    });
    const auto highChord = makeChart(7,
                                     {
                                         {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                         {1000, 5, keyconv::NoteType::Tap, std::nullopt},
                                         {1000, 6, keyconv::NoteType::Tap, std::nullopt},
                                     });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.0;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto lowResult = keyconv::convertChart(lowChord, options);
    const auto lowLanes = lanesOf(lowResult.chart);
    require(lowLanes.size() == 3, "low 7K chord should keep note count");
    for (const int lane : lowLanes) {
        require(lane >= 0 && lane <= 4, "low 7K non-gesture chord should stay in the left 5K panel");
    }
    require(lowResult.report.quality.collisionCount == 0, "low 7K chord should avoid collisions");
    require(lowResult.report.quality.createdJacks == 0, "low 7K chord should not create jacks");

    const auto highResult = keyconv::convertChart(highChord, options);
    const auto highLanes = lanesOf(highResult.chart);
    require(highLanes.size() == 3, "high 7K chord should keep note count");
    for (const int lane : highLanes) {
        require(lane >= 5 && lane <= 9, "high 7K non-gesture chord should stay in the right 5K panel");
    }
    require(highResult.report.quality.collisionCount == 0, "high 7K chord should avoid collisions");
    require(highResult.report.quality.createdJacks == 0, "high 7K chord should not create jacks");
}

void testSevenToTenTapPlusHandZoneBalance() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int pattern[] = {0, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 70; ++i) {
        keyconv::Note note;
        note.id = "hb" + std::to_string(i);
        note.time = 1000 + i * 400;
        note.lane = pattern[static_cast<std::size_t>(i % 7)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const int totalHands = result.report.quality.leftHandNotes + result.report.quality.rightHandNotes;
    require(result.report.quality.addedByTapPlus > 0, "tap-plus hand balance test should add notes");
    require(totalHands == result.report.totalNotes, "hand-zone counts should cover every target note");
    require(result.report.quality.handBalanceRatio >= 0.85,
            "7K to 10K should keep reasonable hand balance while source-lane anchors preserve phrase intent");
    require(result.report.quality.createdJacks == 0, "hand-zone balance should not create jacks");
    require(result.report.quality.collisionCount == 0, "hand-zone balance should not create collisions");
    require(result.report.quality.lnConflictCount == 0, "hand-zone balance should not create LN conflicts");

    int generatedLeft = 0;
    int generatedRight = 0;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) != 0) {
            continue;
        }
        if (note.lane < 5) {
            ++generatedLeft;
        } else {
            ++generatedRight;
        }
    }
    require(generatedLeft + generatedRight == result.report.quality.addedByTapPlus,
            "tap-plus generated hand-zone accounting should match addedByTapPlus");
    require(generatedLeft > 0 || generatedRight > 0,
            "tap-plus should still add deterministic hand-zone helper notes");
}

void testTargetKLikenessReportSevenToTen() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    const int phrase[] = {0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};
    for (int i = 0; i < 72; ++i) {
        keyconv::Note note;
        note.id = "tk" + std::to_string(i);
        note.time = 1000 + i * 180;
        note.lane = phrase[static_cast<std::size_t>(i % 12)];
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto& quality = result.report.quality;
    require(quality.kLikenessScore > 0.0 && quality.kLikenessScore <= 100.0,
            "Target-K likeness report should expose a bounded score");
    require(quality.laneCoverageScore > 0.0, "Target-K likeness should score lane coverage");
    require(quality.laneEntropyScore > 0.0, "Target-K likeness should score lane entropy");
    require(quality.edgeUsageScore > 0.0, "Target-K likeness should score outer lane usage");
    require(quality.activeLaneWindowScore > 0.0, "Target-K likeness should score local active lanes");
    require(quality.anchorPreserveScore > 0.40,
            "Target-K likeness should keep anchor preservation visible while allowing 10K coverage pressure");
    require(quality.patternVocabularyScore > 0.0, "Target-K likeness should score preserved vocabulary");
    require(quality.addedRatioFitScore > 0.0, "Target-K likeness should score added-note fit without a fixed cap");
    require(quality.centerBridgeRate > 0.0, "Target-K likeness should measure 10K center bridge usage");
    require(quality.centerBridgeScore > 0.0, "Target-K likeness should score 10K center bridge usage");
    require(quality.centerSplitBalanceScore >= 0.0,
            "Target-K likeness should expose center split balance scoring");
    require(quality.splitChordScore >= 0.0,
            "Target-K likeness should expose split-chord scoring");
    require(quality.targetKSafetyScore == 1.0, "safe conversion should keep full Target-K safety score");

    const auto json = keyconv::reportToJson(result.report);
    require(json.find("\"kLikenessScore\"") != std::string::npos,
            "JSON report should include K-likeness score");
    require(json.find("\"adjacentExpansionScore\"") != std::string::npos,
            "JSON report should include adjacent expansion score");
    require(json.find("\"centerBridgeRate\"") != std::string::npos,
            "JSON report should include center bridge rate");
    require(json.find("\"splitChordScore\"") != std::string::npos,
            "JSON report should include split-chord score");
}

void testTargetKLikenessUsesReferenceProfile() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1180, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1360, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1540, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1720, 4, keyconv::NoteType::Tap, std::nullopt},
                                     {1900, 5, keyconv::NoteType::Tap, std::nullopt},
                                     {2080, 6, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::TargetKProfile profile;
    profile.targetKeys = 10;
    profile.sampleCount = 12;
    profile.windowMs = 1000;
    profile.profileName = "keyweaver_10k_style_test";
    profile.profileKind = "style";
    profile.sourceName = "u_e fixture";
    profile.authorToken = "u_e";
    profile.desiredLaneEntropy = 0.95;
    profile.desiredEdgeUsage = 0.35;
    profile.desiredActiveLaneRate = 0.85;
    profile.desiredChordSpan = 0.55;
    profile.desiredHandBalance = 0.90;
    profile.desiredAdjacentExpansion = 0.25;
    profile.desiredCenterBridgeRate = 0.45;
    profile.desiredCenterSplitBalance = 0.50;
    profile.desiredSplitChordRate = 0.20;

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.targetKProfile = profile;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.targetProfileChartCount == 12,
            "Target-K report should expose reference profile sample count");
    require(result.report.quality.targetProfileWindowMs == 1000,
            "Target-K report should expose reference profile window size");
    require(result.report.quality.targetProfileName == "keyweaver_10k_style_test",
            "Target-K report should expose reference profile name");
    require(result.report.quality.targetProfileKind == "style",
            "Target-K report should expose reference profile kind");
    require(result.report.quality.targetProfileSource == "u_e fixture",
            "Target-K report should expose reference profile source");
    require(result.report.quality.targetProfileAuthor == "u_e",
            "Target-K report should expose reference profile author");
    require(result.report.quality.kLikenessScore > 0.0,
            "Target-K report should still compute score with reference profile");
    require(result.report.quality.centerBridgeScore > 0.0,
            "Target-K report should use reference profile center bridge metrics");
}

void testAdaptiveGrowthBudgetReportsProfileWindows() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 60; ++i) {
        keyconv::Note note;
        note.id = "agb" + std::to_string(i);
        note.time = 1000 + i * 50;
        note.lane = i % 7;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::TargetKProfile profile;
    profile.targetKeys = 10;
    profile.sampleCount = 14;
    profile.windowMs = 1000;
    profile.profileName = "keyweaver_10k_style_test";
    profile.profileKind = "style";
    profile.sourceName = "u_e fixture";
    profile.authorToken = "u_e";
    profile.desiredLaneEntropy = 0.90;
    profile.desiredEdgeUsage = 0.35;
    profile.desiredActiveLaneRate = 0.80;
    profile.desiredChordSpan = 0.55;
    profile.desiredHandBalance = 0.88;
    profile.desiredAdjacentExpansion = 0.32;

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.targetKProfile = profile;
    options.maxAddedPerSlice = 1;

    const auto result = keyconv::convertChart(chart, options);
    const auto& quality = result.report.quality;
    require(quality.adaptiveGrowthBudgetEnabled, "profiled tap-plus conversion should enable adaptive growth budget");
    require(quality.adaptiveBudgetWindowMs == 1000, "adaptive budget should use the target profile window size");
    require(quality.adaptiveBudgetWindows >= 3, "adaptive budget should report local windows");
    require(quality.adaptiveBudgetAverageRatio > 0.0, "adaptive budget should report a positive average ratio");
    require(quality.adaptiveBudgetMaxRatio >= quality.adaptiveBudgetMinRatio,
            "adaptive budget should report a bounded min/max ratio");
    require(quality.rejectedByAdaptiveBudget > 0,
            "dense profiled windows should throttle tap-plus additions before the global cap");

    const auto json = keyconv::reportToJson(result.report);
    require(json.find("\"adaptiveGrowthBudgetEnabled\"") != std::string::npos,
            "JSON report should expose adaptive budget state");
    require(json.find("\"rejectedByAdaptiveBudget\"") != std::string::npos,
            "JSON report should expose adaptive budget rejects");
}

void testAdaptiveGrowthBudgetUsesDensityBuckets() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 60; ++i) {
        keyconv::Note note;
        note.id = "bucket" + std::to_string(i);
        note.time = 1000 + i * 50;
        note.lane = i % 7;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::TargetKProfile profile;
    profile.targetKeys = 10;
    profile.sampleCount = 14;
    profile.windowMs = 1000;
    profile.profileName = "keyweaver_10k_bucket_test";
    profile.profileKind = "style";
    profile.desiredActiveLaneRate = 0.10;
    profile.desiredAdjacentExpansion = 0.0;
    profile.densityBuckets.present = true;
    profile.densityBuckets.lowMaxNps = 7.0;
    profile.densityBuckets.midMaxNps = 15.0;
    profile.densityBuckets.high.present = true;
    profile.densityBuckets.high.windowCount = 10;
    profile.densityBuckets.high.activeLaneRate.present = true;
    profile.densityBuckets.high.activeLaneRate.p25 = 0.90;
    profile.densityBuckets.high.activeLaneRate.median = 1.00;
    profile.densityBuckets.high.activeLaneRate.p75 = 1.00;
    profile.densityBuckets.high.adjacentExpansion.present = true;
    profile.densityBuckets.high.adjacentExpansion.median = 0.32;
    profile.densityBuckets.high.densityNps.present = true;
    profile.densityBuckets.high.densityNps.median = 21.0;
    profile.densityBuckets.high.densityNps.p75 = 28.0;
    profile.densityBuckets.high.densityNps.p90 = 36.0;

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.targetKProfile = profile;
    options.maxAddedNoteRatio = 0.45;
    options.maxAddedPerSlice = 1;

    const auto result = keyconv::convertChart(chart, options);
    const auto& quality = result.report.quality;
    require(quality.adaptiveGrowthBudgetEnabled,
            "profiled tap-plus conversion should enable adaptive growth budget");
    require(quality.adaptiveBudgetAverageRatio > 0.10,
            "adaptive budget should use density bucket windows instead of root chart summary fields");
    require(quality.adaptiveBudgetMaxRatio > 0.14,
            "high-density 10K bucket should permit coverage fill within the 15 percent normal preset");
}

void testAdaptiveGrowthBudgetAllowsSparseProfiledFill() {
    const auto chart = makeChart(4,
                                 {
                                     {0, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3000, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::TargetKProfile profile;
    profile.targetKeys = 10;
    profile.sampleCount = 14;
    profile.windowMs = 1000;
    profile.profileName = "keyweaver_10k_style_test";
    profile.profileKind = "style";
    profile.sourceName = "u_e fixture";
    profile.authorToken = "u_e";
    profile.desiredLaneEntropy = 0.90;
    profile.desiredEdgeUsage = 0.35;
    profile.desiredActiveLaneRate = 0.80;
    profile.desiredChordSpan = 0.55;
    profile.desiredHandBalance = 0.88;
    profile.desiredAdjacentExpansion = 0.32;

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.targetKProfile = profile;
    options.maxAddedPerSlice = 1;

    const auto result = keyconv::convertChart(chart, options);
    const auto& quality = result.report.quality;
    require(quality.adaptiveGrowthBudgetEnabled, "sparse profiled tap-plus should enable adaptive growth budget");
    require(quality.addedNotes == 1,
            "sparse profiled windows should keep one local fill slot when the global budget has room");
    require(quality.createdJacks == 0, "sparse profiled fill should not create jacks");
    require(quality.nearTimeConflicts == 0, "sparse profiled fill should avoid near-time conflicts");
}

void testPreserveTapPlusCanUseSecondSliceFillSlot() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.50;
    options.maxAddedPerSlice = 2;

    const auto result = keyconv::convertChart(chart, options);
    const auto& quality = result.report.quality;
    require(quality.addedNotes == 2, "tap-plus should use the second per-slice fill slot when budget allows");
    require(quality.createdJacks == 0, "second fill slot should not create jacks");
    require(quality.collisionCount == 0, "second fill slot should avoid collisions");
}

void testPpgChordDoesNotCollapse() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    const auto scan = keyconv::detectCollisions(result.chart.notes);
    require(scan.sameTimeCollisions == 0, "same-time chord should not collapse into one target lane");
    require(result.report.quality.spanPreserveScore > 0.5, "chord span should be roughly preserved");
}

void testPpgLnAvoidsTap() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Hold, 2000},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.lnConflictCount == 0, "PPG playable should avoid placing tap into active LN lane");
    require(result.chart.notes.size() == 2, "LN avoidance should not drop notes");
}

void testPpgPlayableReducesJackAndFaithfulPreservesMore() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1100, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1200, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1300, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions directOptions;
    directOptions.sourceKeyCount = 4;
    directOptions.targetKeyCount = 10;
    directOptions.style = keyconv::ConversionStyle::Direct;
    directOptions.collisionPolicy = keyconv::CollisionPolicy::Keep;
    const auto direct = keyconv::convertChart(chart, directOptions);

    keyconv::ConvertOptions playableOptions = directOptions;
    playableOptions.style = keyconv::ConversionStyle::Playable;
    playableOptions.collisionPolicy = keyconv::CollisionPolicy::ShiftNearest;
    const auto playable = keyconv::convertChart(chart, playableOptions);

    keyconv::ConvertOptions faithfulOptions = playableOptions;
    faithfulOptions.style = keyconv::ConversionStyle::Faithful;
    const auto faithful = keyconv::convertChart(chart, faithfulOptions);

    require(playable.report.quality.jackRateAfter < direct.report.quality.jackRateAfter,
            "playable mode should reduce jack rate compared to direct");
    require(sameLaneRepeatCount(faithful.chart) >= sameLaneRepeatCount(playable.chart),
            "faithful mode should preserve more same-lane repeats than playable mode");
    require(playable.report.quality.laneEntropy > 0.0, "quality report should include lane entropy");
    for (const auto& note : playable.chart.notes) {
        require(note.lane >= 0 && note.lane < 10, "PPG converted note should stay inside target lane range");
    }
}

void testPpgAvoidsCreatedJackFromMovedPattern() {
    keyconv::AssignmentContext context;
    context.sourceKeyCount = 4;
    context.targetKeyCount = 10;
    context.style = keyconv::ConversionStyle::Playable;
    context.weights = keyconv::weightsForStyle(keyconv::ConversionStyle::Playable);
    context.laneUse.assign(10, 0);

    keyconv::Note first{"a", 1000, 5, keyconv::NoteType::Tap};
    first.sourceLane = 0;
    keyconv::Note second{"b", 1120, 5, keyconv::NoteType::Tap};
    second.sourceLane = 2;
    context.placed = {first, second};

    keyconv::Note source{"c", 1240, 1, keyconv::NoteType::Tap};
    source.sourceLane = 1;
    std::vector<keyconv::Note> sourceNotes{source};

    keyconv::TimeSlice slice;
    slice.index = 0;
    slice.time = 1240;
    slice.noteIndices = {0};
    slice.chordSize = 1;
    slice.minLane = 1;
    slice.maxLane = 1;

    keyconv::SliceAssignment createdJack;
    createdJack.targetLanes = {5};
    keyconv::SliceAssignment movedAway;
    movedAway.targetLanes = {6};

    require(keyconv::scoreAssignment(slice, sourceNotes, movedAway, context) >
                keyconv::scoreAssignment(slice, sourceNotes, createdJack, context),
            "source-different target repeat should be scored below a nearby non-jack lane");
}

void testDifferentSourceLanesDoNotFoldIntoJack() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1120, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 3;
    options.style = keyconv::ConversionStyle::Direct;
    options.collisionPolicy = keyconv::CollisionPolicy::Keep;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 2, "direct fold regression should keep both source notes");
    require(lanes[0] != lanes[1], "different source lanes should not fold into a target jack pair");
    require(result.report.quality.createdJacks == 0, "created jack invariant should leave no created jack");
    require(result.report.quality.preventedJacksByAssignment > 0,
            "assignment should report the hard-rejected folding candidate");
}

void testAssignmentAvoidsCreatedJackWithAdaptiveRadius() {
    keyconv::AssignmentContext context;
    context.sourceKeyCount = 4;
    context.targetKeyCount = 3;
    context.style = keyconv::ConversionStyle::Direct;
    context.weights = keyconv::weightsForStyle(keyconv::ConversionStyle::Direct);
    context.laneUse.assign(3, 0);
    int prevented = 0;
    context.preventedJacksByAssignment = &prevented;

    keyconv::Note placed{"a", 1000, 1, keyconv::NoteType::Tap};
    placed.sourceLane = 1;
    context.placed = {placed};

    keyconv::Note source{"b", 1120, 2, keyconv::NoteType::Tap};
    source.sourceLane = 2;
    std::vector<keyconv::Note> sourceNotes{source};

    keyconv::TimeSlice slice;
    slice.index = 0;
    slice.time = 1120;
    slice.noteIndices = {0};
    slice.chordSize = 1;
    slice.minLane = 2;
    slice.maxLane = 2;

    const auto assignments = keyconv::generateSliceAssignments(slice, sourceNotes, context);
    require(!assignments.empty(), "adaptive radius should still provide a legal assignment");
    require(assignments.front().targetLanes.front() != 1,
            "adaptive radius should escape the default folding lane");
    require(prevented > 0, "adaptive assignment should count the rejected folding lane");
}

void testRepairDoesNotCreateJack() {
    auto notes = std::vector<keyconv::Note>{
        keyconv::Note{"hold", 900, 0, keyconv::NoteType::Hold, 1300, "", 2},
        keyconv::Note{"a", 1000, 1, keyconv::NoteType::Tap, std::nullopt, "", 0},
        keyconv::Note{"b", 1120, 0, keyconv::NoteType::Tap, std::nullopt, "", 1},
    };
    int prevented = 0;
    const int shifted = keyconv::localRepairAssignments(notes, 3, 180, &prevented);

    require(shifted == 1, "repair should move the tap out of the LN lane");
    require(prevented > 0, "repair should reject the lane that would create a jack");
    const auto moved = std::find_if(notes.begin(), notes.end(), [](const auto& note) {
        return note.id == "b";
    });
    require(moved != notes.end(), "repaired note should still exist");
    require(moved->lane != 1, "repair should not move into a source-different target jack");
}

void testConverterFacadeReservedOptions() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::DP;
    options.dpMode = true;
    options.optimizer = keyconv::OptimizerKind::Beam;
    options.beamWidth = 8;
    options.sameTimeEpsilonMs = 4;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.chart.notes.size() == chart.notes.size(), "Converter facade should preserve note count");
    require(result.report.warnings.size() >= 2, "reserved DP and Beam options should produce fallback warnings");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 10, "Converter facade output should stay inside target lane range");
    }
}

void testTrainingStyleParsesAndConverts() {
    const auto parsed = keyconv::parseConversionStyle("training");
    require(parsed.has_value() && *parsed == keyconv::ConversionStyle::Training, "training style should parse");
    require(keyconv::parseOptimizerKind("beam") == keyconv::OptimizerKind::Beam, "beam optimizer option should parse");

    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1100, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1200, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });
    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Training;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 3, "training style should convert through Converter facade");
    require(result.report.quality.laneEntropy > 0.0, "training style should compute quality metrics");
}

void testSameTimeEpsilonGroupsSlices() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1003, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });
    const auto strict = keyconv::buildTimeSlices(chart, 4, 0);
    const auto loose = keyconv::buildTimeSlices(chart, 4, 4);
    require(strict.size() == 2, "epsilon 0 should keep near notes in separate slices");
    require(loose.size() == 1, "epsilon 4 should group near notes into one slice");
    require(loose.front().chordSize == 2, "epsilon grouped slice should count as chord");
}

void testCompressDropFiveNoteChord() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapDrop;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 4, "5-note chord to 4K should keep only 4 notes under drop policy");
    require(result.report.quality.droppedByCompression == 1, "5-note chord to 4K should drop one note");
    require(result.report.quality.collisionCount == 0, "drop policy should remove same-time collision");
    require(result.report.quality.noOverlapGuaranteed, "drop policy should guarantee no overlap");
}

void testCompressDropSixNoteChord() {
    const auto chart = makeChart(6,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 5, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 6;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapDrop;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 4, "6-note chord to 4K should keep only 4 notes under drop policy");
    require(result.report.quality.droppedByCompression == 2, "6-note chord to 4K should drop two notes");
    require(result.report.quality.collisionCount == 0, "6-note drop policy should remove same-time collision");
}

void testCompressHybridDropsTapOverflow() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapHybrid;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 4, "hybrid should delete tap overflow above target key count");
    require(result.report.quality.droppedByCompression == 1, "hybrid tap overflow should be counted as dropped");
    require(result.report.quality.rolledByCompression == 0, "hybrid should not roll tap overflow by default");
    require(result.report.quality.collisionCount == 0, "hybrid tap drop should avoid collisions");
    require(result.report.quality.noOverlapGuaranteed, "hybrid tap drop should guarantee no overlap");
}

void testCompressHybridRollsOverflowHold() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Hold, 2000},
                                     {1000, 1, keyconv::NoteType::Hold, 2000},
                                     {1000, 2, keyconv::NoteType::Hold, 2000},
                                     {1000, 3, keyconv::NoteType::Hold, 2000},
                                     {1000, 4, keyconv::NoteType::Hold, 2000},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapHybrid;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 5, "hybrid should preserve overflow holds by rolling them");
    require(result.report.holdNotes == 5, "hybrid should keep overflow holds as holds");
    require(result.report.quality.rolledByCompression >= 1, "hybrid should roll at least one overflow hold");
    require(result.report.quality.droppedByCompression == 0, "hybrid should not drop overflow holds when roll is possible");
    require(result.report.quality.collisionCount == 0, "rolled overflow hold should avoid collisions");
    require(result.report.quality.lnConflictCount == 0, "rolled overflow hold should avoid LN overlap");
    require(result.report.quality.noOverlapGuaranteed, "rolled overflow hold should guarantee no overlap");

    bool sawRolledHold = false;
    for (const auto& note : result.chart.notes) {
        if (note.type == keyconv::NoteType::Hold && note.time != 1000) {
            sawRolledHold = true;
            require(note.endTime.has_value() && *note.endTime - note.time == 1000,
                    "rolled overflow hold should preserve duration");
        }
    }
    require(sawRolledHold, "one overflow hold should move to a different ms");
}

void testCompressAutoDropsOverflowHoldForLowKeyRecreation() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Hold, 2000},
                                     {1000, 1, keyconv::NoteType::Hold, 2000},
                                     {1000, 2, keyconv::NoteType::Hold, 2000},
                                     {1000, 3, keyconv::NoteType::Hold, 2000},
                                     {1000, 4, keyconv::NoteType::Hold, 2000},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 4,
            "auto high-to-low compression should drop overflow holds instead of preserving every object");
    require(result.report.holdNotes == 4, "auto high-to-low compression should keep the surviving holds as holds");
    require(result.report.quality.droppedByCompression == 1,
            "auto high-to-low compression should count the omitted overflow hold");
    require(result.report.quality.rolledByCompression == 0,
            "auto high-to-low compression should not retime overflow holds by default");
    require(result.report.quality.collisionCount == 0, "auto high-to-low compression should avoid collisions");
    require(result.report.quality.lnConflictCount == 0, "auto high-to-low compression should avoid LN overlap");
    require(result.report.quality.noOverlapGuaranteed,
            "auto high-to-low compression should still guarantee no overlap");
}

void testCompressActiveLnDropsTap() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 0, keyconv::NoteType::Hold, 2000},
                                     {1000, 2, keyconv::NoteType::Hold, 2000},
                                     {1000, 4, keyconv::NoteType::Hold, 2000},
                                     {1000, 6, keyconv::NoteType::Hold, 2000},
                                     {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapHybrid;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.lnConflictCount == 0, "tap under full active LN occupancy should not remain as conflict");
    require(result.report.quality.droppedByCompression == 1,
            "tap under full active LN occupancy should be deleted");
    require(result.report.quality.rolledByCompression == 0,
            "tap under full active LN occupancy should not be rolled in hybrid policy");
    require(result.report.quality.noOverlapGuaranteed, "LN compression case should guarantee no overlap");
}

void testCompressPreserveStrictReportsImpossible() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Faithful;
    options.compressPolicy = keyconv::CompressPolicy::PreserveStrict;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.totalNotes == 5, "preserve strict should keep all notes");
    require(result.report.quality.impossibleSlices >= 1, "preserve strict should report impossible compression slice");
    require(!result.report.quality.noOverlapGuaranteed, "preserve strict should not claim no-overlap guarantee here");
}

void testCompressHybridNoOverlapSynthetic() {
    const auto chart = makeChart(6,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 5, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 6;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapHybrid;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.collisionCount == 0, "hybrid compression should remove collisions");
    require(result.report.quality.lnConflictCount == 0, "hybrid compression should remove LN conflicts");
    require(result.report.quality.nearTimeConflicts == 0, "hybrid compression should reject near-time rolls");
    require(result.report.quality.sameLaneNearConflicts == 0, "hybrid compression should reject same-lane near rolls");
    require(result.report.quality.noOverlapGuaranteed, "hybrid compression should guarantee no overlap");
}

void testCompressPlannerAvoidsJackifiedFold() {
    std::vector<keyconv::Note> notes;
    for (const auto& entry : std::vector<std::tuple<int, int, int>>{
             {1000, 1, 1},
             {1125, 1, 2},
             {1250, 1, 1},
             {1375, 1, 2},
         }) {
        keyconv::Note note;
        note.id = "fold" + std::to_string(notes.size());
        note.time = std::get<0>(entry);
        note.lane = std::get<1>(entry);
        note.sourceLane = std::get<2>(entry);
        note.type = keyconv::NoteType::Tap;
        notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 8;
    options.targetKeyCount = 5;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapHybrid;
    options.jackWindowMs = 180;

    const auto stats = keyconv::applyCompressPlanner(notes, options, {});
    require(stats.noOverlapGuaranteed, "compression de-jack placement should keep no-overlap guarantee");
    require(notes.size() == 4, "compression de-jack placement should keep non-overflow notes");
    for (std::size_t i = 1; i < notes.size(); ++i) {
        const int dt = notes[i].time - notes[i - 1].time;
        const int prevSource = notes[i - 1].sourceLane.value_or(notes[i - 1].lane);
        const int source = notes[i].sourceLane.value_or(notes[i].lane);
        require(!(dt > 0 && dt <= options.jackWindowMs && notes[i].lane == notes[i - 1].lane &&
                  source != prevSource),
                "8K to 5K compression should not fold alternating source lanes into a target jack");
    }
}

void testCompressAutoDropsWhenOnlyCreatedJackLaneFits() {
    auto original = makeChart(8,
                              {
                                  {1000, 0, keyconv::NoteType::Hold, 2000},
                                  {1000, 1, keyconv::NoteType::Hold, 2000},
                                  {1000, 2, keyconv::NoteType::Hold, 2000},
                                  {1000, 3, keyconv::NoteType::Hold, 2000},
                                  {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {1125, 5, keyconv::NoteType::Tap, std::nullopt},
                              });
    std::vector<keyconv::Note> notes = original.notes;
    notes[4].lane = 4;
    notes[5].lane = 4;

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 8;
    options.targetKeyCount = 5;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::Auto;
    options.jackWindowMs = 180;

    const auto stats = keyconv::applyCompressPlanner(notes, options, {}, &original);
    require(stats.droppedByCompression == 1,
            "auto low-key recreation should drop a note when only a created-jack lane fits");
    require(stats.rolledByCompression == 0,
            "auto low-key recreation should not retime the created-jack offender");
    require(keyconv::detectCreatedJackPairs(original, notes, options.jackWindowMs).empty(),
            "auto low-key recreation should leave no created jack pair when dropping is possible");
}

void testDistanceGuardRejectsEightMsRoll() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapRoll;
    options.snapRolledNotes = false;
    options.minObjectGapMs = 16;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.rolledByCompression == 1, "raw roll should still preserve one overflow note");
    require(result.report.quality.nearTimeConflicts == 0, "+8ms or +12ms raw rolls should be rejected by min gap");
    require(result.report.quality.minPositiveDeltaMs >= 16,
            "raw roll should land at the first distance-safe offset");
    for (const auto& note : result.chart.notes) {
        require(note.time != 1008 && note.time != 992 && note.time != 1012 && note.time != 988,
                "raw roll candidate under 16ms should not be used");
    }
}

void testExpandedOutputCollapsesSourceNearTimePair() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1009, 6, keyconv::NoteType::Hold, 1109},
                                     {1200, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1400, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.minObjectGapMs = 16;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    require(result.report.quality.nearTimeConflicts == 0,
            "higher-key conversion should collapse inherited sub-16ms near-time pairs");
    require(result.report.quality.minPositiveDeltaMs >= 16,
            "collapsed source near-time pair should no longer be the minimum positive gap");
    require(result.report.quality.rerolledByDistanceGuard >= 1,
            "distance sanitizer should report the collapsed near-time pair");
    require(result.report.sameTimeCollisions == 0, "collapsed near-time pair should not create same-lane collision");
    require(result.report.longNoteConflicts == 0, "collapsed near-time pair should not create LN conflict");

    auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id == "n1";
    });
    require(hold != result.chart.notes.end(), "near-time hold should remain present");
    require(hold->type == keyconv::NoteType::Hold, "near-time hold should remain a hold");
    require(hold->time == 1000, "later near-time hold should collapse to the earlier chord time");
    require(hold->endTime.has_value() && *hold->endTime == 1100,
            "collapsed near-time hold should preserve its duration");
}

void testSameLaneGapGuardRejectsTwelveMsRoll() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapRoll;
    options.snapRolledNotes = false;
    options.minObjectGapMs = 0;
    options.sameLaneMinGapMs = 20;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.rolledByCompression == 1, "raw roll should preserve one overflow note");
    require(result.report.quality.sameLaneNearConflicts == 0,
            "same-lane +8ms, +12ms, and +16ms rolls should be rejected");
    require(result.report.quality.minPositiveDeltaMs >= 20,
            "same-lane guard should force at least 20ms distance here");
}

void testSnapRollEnabledNoUnsnappedRolledNotes() {
    const auto chart = makeChart(5,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Playable;
    options.compressPolicy = keyconv::CompressPolicy::NoOverlapRoll;
    options.snapRolledNotes = true;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.rolledByCompression == 1, "snap roll should preserve one overflow note");
    require(result.report.quality.unsnappedRolledNotes == 0, "snap roll should produce no unsnapped rolled notes");
    require(result.report.quality.nearTimeConflicts == 0, "snap roll should also satisfy default min gap");
}

void testExpansionSameInputSameOutput() {
    const auto chart = keyconv::parseOsu(simple4k);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::TrainingScaffold;
    options.maxAddedNoteRatio = 1.0;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "same input/options should produce identical osu output");
    require(keyconv::reportToJson(first.report) == keyconv::reportToJson(second.report),
            "same input/options should produce identical json report");
}

void testChordFillDeterministicLane() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicChordFill;
    options.maxAddedNoteRatio = 0.5;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedNotes == 1, "chord fill should add one note within budget");
    require(result.report.quality.addedByChordFill == 1, "chord fill should report its generated note");
    require(result.report.quality.collisionCount == 0, "chord fill should not create collisions");
    require(result.report.quality.nearTimeConflicts == 0, "chord fill should not create near-time conflicts");

    std::vector<int> lanes;
    for (const auto& note : result.chart.notes) {
        lanes.push_back(note.lane);
    }
    std::sort(lanes.begin(), lanes.end());
    require(lanes == std::vector<int>({0, 5, 9}), "wide 4K edge chord should deterministically fill lane 5 in 10K");
}

void testTrainingScaffoldDeterministicLane() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::TrainingScaffold;
    options.maxAddedNoteRatio = 1.0;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedNotes == 1, "training scaffold should add one note within budget");
    require(result.report.quality.addedByTrainingScaffold == 1,
            "training scaffold should report its generated note");
    require(result.report.quality.collisionCount == 0, "training scaffold should not create collisions");
    require(result.report.quality.nearTimeConflicts == 0,
            "training scaffold should not create near-time conflicts");

    bool foundGenerated = false;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:training_scaffold:", 0) == 0) {
            foundGenerated = true;
            require(note.lane == 4, "training scaffold should choose the least-used central lane first");
        }
    }
    require(foundGenerated, "training scaffold should create a tracked generated note id");
}

void testPreserveTapPlusIncludesHoldsBudgetAndAddsOnlyTaps() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Hold, 1600},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1625, 2, keyconv::NoteType::Hold, 2200},
                                     {1750, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1875, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2250, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {2375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2625, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2750, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {2875, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    const auto& quality = first.report.quality;

    require(quality.expansionPolicy == "preserve-tap-plus", "tap plus policy should be reported");
    require(quality.expansionComposerProfile == "tap-plus", "tap plus composer profile should be reported");
    require(quality.targetAddedNoteRatio == 0.15,
            "tap plus should target a 15 percent added-note budget on high-key expansion");
    require(quality.addedNotes == 2, "tap plus should use the normal high-key source-note budget");
    require(quality.addedByTapPlus == 2, "tap plus should report generated tap-plus notes");
    require(first.report.holdNotes == 2, "tap plus should preserve original hold notes");
    require(quality.collisionCount == 0, "tap plus should avoid collisions");
    require(quality.lnConflictCount == 0, "tap plus should avoid LN conflicts");
    require(quality.nearTimeConflicts == 0, "tap plus should avoid near-time conflicts");
    require(quality.unsnappedAddedNotes == 0, "tap plus should keep added taps snapped");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "tap plus should be deterministic");

    int generatedTaps = 0;
    for (const auto& note : first.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) == 0) {
            ++generatedTaps;
            require(note.type == keyconv::NoteType::Tap, "tap plus should generate only tap notes");
        }
    }
    require(generatedTaps == 2, "tap plus should generate the expected tracked tap notes");
}

void testPreserveTapPlusLowCapsHighKeyGrowth() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Hold, 1600},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1625, 2, keyconv::NoteType::Hold, 2200},
                                     {1750, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1875, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2250, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {2375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2625, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2750, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {2875, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusLow;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto& quality = result.report.quality;

    require(quality.expansionPolicy == "preserve-tap-plus-low",
            "low tap-plus policy should be reported");
    require(quality.expansionComposerProfile == "tap-plus-low",
            "low tap-plus composer profile should be reported");
    require(quality.targetAddedNoteRatio == 0.10,
            "auto-low should cap high-key generated notes at 10 percent");
    require(quality.addedNotes <= 1,
            "auto-low should add no more than 10 percent of the 16 source objects");
    require(quality.createdJacks == 0, "auto-low should keep the no-created-jack invariant");
}

void testPreserveTapPlusAddsHoldsInLnHeavyWindow() {
    auto chart = makeChart(7,
                           {
                               {1000, 0, keyconv::NoteType::Hold, 1180},
                               {1250, 1, keyconv::NoteType::Hold, 1430},
                               {1500, 2, keyconv::NoteType::Hold, 1680},
                               {1750, 3, keyconv::NoteType::Hold, 1930},
                               {2000, 4, keyconv::NoteType::Hold, 2180},
                               {2250, 5, keyconv::NoteType::Hold, 2430},
                               {2500, 6, keyconv::NoteType::Hold, 2680},
                               {2750, 0, keyconv::NoteType::Hold, 2930},
                           });
    addTimingPoint(chart);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    int generatedHolds = 0;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) == 0) {
            ++generatedHolds;
            require(note.type == keyconv::NoteType::Hold, "LN-heavy tap-plus window should generate hold notes");
            require(note.endTime.has_value(), "generated LN-heavy tap-plus note should have an end time");
            require(*note.endTime - note.time == 180,
                    "generated LN-heavy tap-plus note should keep 8th-to-16th LN duration");

            int nearestOriginalHoldDistance = 1000;
            for (const auto& neighbor : result.chart.notes) {
                if (neighbor.id.rfind("gen:", 0) == 0 || neighbor.type != keyconv::NoteType::Hold ||
                    neighbor.time != note.time) {
                    continue;
                }
                nearestOriginalHoldDistance = std::min(nearestOriginalHoldDistance,
                                                       std::abs(neighbor.lane - note.lane));
            }
            require(nearestOriginalHoldDistance <= 2,
                    "generated LN-heavy tap-plus note should stay near a same-time source hold");
        }
    }

    require(generatedHolds > 0, "tap plus should add holds inside LN-heavy windows");
    require(result.report.quality.collisionCount == 0, "LN-heavy tap plus should avoid collisions");
    require(result.report.quality.lnConflictCount == 0, "LN-heavy tap plus should avoid LN conflicts");
    require(result.report.quality.unsolvedCreatedJacks == 0, "LN-heavy tap plus should not leave created jacks");
}

void testPreserveTapPlusDoesNotTurnTapOnlySliceIntoHold() {
    const auto chart = makeChart(7,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Hold, 1430},
                                     {1500, 2, keyconv::NoteType::Hold, 1680},
                                     {1750, 3, keyconv::NoteType::Hold, 1930},
                                     {2000, 4, keyconv::NoteType::Hold, 2180},
                                     {2250, 5, keyconv::NoteType::Hold, 2430},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    bool sawSourceTap = false;
    bool sawGeneratedTapAtTapOnlySlice = false;
    for (const auto& note : result.chart.notes) {
        if (note.id == "n0") {
            sawSourceTap = true;
            require(note.type == keyconv::NoteType::Tap, "source tap should not be converted to hold");
        }
        if (note.id.rfind("gen:tap_plus:", 0) == 0 && note.time == 1000) {
            sawGeneratedTapAtTapOnlySlice = true;
            require(note.type == keyconv::NoteType::Tap,
                    "tap-only slice inside LN-heavy window should still receive a tap addition");
        }
    }

    require(sawSourceTap, "converted chart should keep the original tap note id");
    require(sawGeneratedTapAtTapOnlySlice, "tap-plus should spend the first budget slot on the tap-only slice");
    require(result.report.quality.lnConflictCount == 0, "tap-only slice guard should avoid LN conflicts");
}

void testGeneratedShortHoldCloneIgnoresAdjacentLongHoldLength() {
    keyconv::Chart original = makeChart(10,
                                        {
                                            {1000, 4, keyconv::NoteType::Hold, 1800},
                                        });
    keyconv::Chart converted = makeChart(10,
                                         {
                                             {1000, 4, keyconv::NoteType::Hold, 1800},
                                             {1000, 5, keyconv::NoteType::Hold, 1180},
                                         });
    converted.notes[1].id = "gen:ln_clone:0";
    addTimingPoint(original);
    addTimingPoint(converted);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 10;
    options.targetKeyCount = 10;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;

    keyconv::applyExpansionPlanner(converted, original, options);
    const auto clone = std::find_if(converted.notes.begin(), converted.notes.end(), [](const auto& note) {
        return note.id == "gen:ln_clone:0";
    });
    require(clone != converted.notes.end(), "generated hold clone should remain present");
    require(clone->endTime.has_value(), "generated hold clone should keep an end time");
    require(*clone->endTime - clone->time == 180,
            "generated 8th-to-16th hold clone should not stretch to a long adjacent hold");
}

void testGeneratedLongHoldCloneIsTapified() {
    keyconv::Chart original = makeChart(10,
                                        {
                                            {1000, 4, keyconv::NoteType::Hold, 1800},
                                        });
    keyconv::Chart converted = makeChart(10,
                                         {
                                             {1000, 4, keyconv::NoteType::Hold, 1800},
                                             {1000, 5, keyconv::NoteType::Hold, 1800},
                                         });
    converted.notes[1].id = "gen:ln_clone:0";
    addTimingPoint(original);
    addTimingPoint(converted);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 10;
    options.targetKeyCount = 10;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;

    keyconv::applyExpansionPlanner(converted, original, options);
    const auto clone = std::find_if(converted.notes.begin(), converted.notes.end(), [](const auto& note) {
        return note.id == "gen:ln_clone:0";
    });
    require(clone != converted.notes.end(), "generated long hold clone should remain present");
    require(clone->type == keyconv::NoteType::Tap, "generated long hold clone should be converted to tap");
    require(!clone->endTime.has_value(), "tapified generated long hold should clear end time");
}

keyconv::Chart makeNoJackChordFillTrapChart() {
    return makeChart(5,
                     {
                         {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                         {1125, 0, keyconv::NoteType::Tap, std::nullopt},
                         {1125, 4, keyconv::NoteType::Tap, std::nullopt},
                         {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                     });
}

keyconv::Chart makeSourceJackChart() {
    return makeChart(4,
                     {
                         {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                         {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                         {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                         {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                     });
}

void testNoJackInputDoesNotCreateJack() {
    const auto chart = makeNoJackChordFillTrapChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicChordFill;
    options.maxAddedNoteRatio = 0.50;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.sourceJackGroups == 0, "trap source should have no jack group");
    require(result.report.quality.createdJacks == 0, "jack guard should prevent created target jacks");
}

void testSourceJackPreservedFaithful() {
    const auto chart = makeSourceJackChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreserveStrict;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.sourceJackGroups == 1, "source jack group should be detected");
    require(result.report.quality.preservedJackGroups == 1, "faithful conversion should preserve source jack lane");
    require(result.report.quality.createdJacks == 0, "source jack preservation should not count as created jack");
    require(result.report.quality.jackPreserveScore >= 0.99, "strict preserved jack should score high");
}

void testSourceJackPlayableSplit() {
    const auto chart = makeSourceJackChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.50;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.sourceJackGroups == 1, "source jack group should be detected in playable mode");
    require(result.report.quality.preservedJackGroups + result.report.quality.splitJackGroups >= 1,
            "playable conversion should preserve or split the source jack identity");
    require(result.report.quality.createdJacks == 0, "playable jack handling should not create unrelated jacks");
}

void testEightKeyLooseRepeatDoesNotBecomeJack() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 8;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(result.report.quality.sourceJackGroups == 0,
            "two-hit loose repeat fixture should not be classified as a source jack group");
    require(lanes.size() == 3, "8K loose-repeat fixture should preserve note count");
    require(lanes[0] != lanes[1],
            "8K conversion should not turn a loose two-hit repeat into a target-lane jack");
    require(result.report.quality.sanitizedCreatedJacks > 0,
            "8K loose-repeat guard should report that it moved an unintended target repeat");
}

void testEightKeySourceJackStillPreserved() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 8;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.sourceJackGroups == 1,
            "8K true source jack fixture should detect the source jack group");
    require(result.report.quality.preservedJackGroups + result.report.quality.splitJackGroups >= 1,
            "8K unintended-jack guard should not erase a real source jack group");
    require(result.report.quality.unsolvedCreatedJacks == 0,
            "8K true source jack fixture should not leave unsolved created jacks");
}

void testFiveHundredMsJackWindowDetectsSlowSourceJack() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    require(result.report.quality.sourceJackGroups == 1,
            "500 ms same-source repeats should be detected as one source jack group");
    require(result.report.quality.preservedJackGroups + result.report.quality.splitJackGroups >= 1,
            "500 ms source jack group should be preserved or adjacent-safe split");
    require(result.report.quality.brokenJacks == 0,
            "gesture evaluation should not break the 500 ms source jack phrase");
}

void testAddedNoteRejectedIfCreatesUnwantedJack() {
    const auto original = makeNoJackChordFillTrapChart();
    auto converted = makeChart(10,
                               {
                                   {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                   {1125, 0, keyconv::NoteType::Tap, std::nullopt},
                                   {1125, 6, keyconv::NoteType::Tap, std::nullopt},
                                   {1250, 3, keyconv::NoteType::Tap, std::nullopt},
                               });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 7;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicChordFill;
    options.maxAddedNoteRatio = 0.50;
    options.maxAddedPerSlice = 1;

    const auto stats = keyconv::applyExpansionPlanner(converted, original, options);
    require(stats.preventedJacks == 1,
            "the center chord-fill candidate should be rejected as an unwanted jack");
    require(stats.addedNotes == 1, "a safe alternative candidate should still be allowed");
    for (const auto& note : converted.notes) {
        require(!(note.time == 1125 && note.lane == 3),
                "rejected unwanted jack should not survive in expanded output");
    }
}

void testAddedNoteFromSourceJackStillRejectedIfCreatesTargetJack() {
    const auto original = makeSourceJackChart();
    auto converted = makeChart(10,
                               {
                                   {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                   {1125, 0, keyconv::NoteType::Tap, std::nullopt},
                                   {1125, 6, keyconv::NoteType::Tap, std::nullopt},
                                   {1250, 3, keyconv::NoteType::Tap, std::nullopt},
                               });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 7;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicChordFill;
    options.maxAddedNoteRatio = 0.50;
    options.maxAddedPerSlice = 1;

    const auto stats = keyconv::applyExpansionPlanner(converted, original, options);
    require(stats.preventedJacks == 1,
            "generated notes sourced from a jack slice should still reject a new target jack");
    require(stats.addedNotes == 1, "a safe non-jack candidate should still be allowed after rejection");
    for (const auto& note : converted.notes) {
        require(!(note.time == 1125 && note.lane == 3),
                "source-jack-sourced generated note should not create a new target jack");
    }
}

void testHarderDoesNotCreateRandomJack() {
    const auto chart = makeNoJackChordFillTrapChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 5;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.maxAddedNoteRatio = 0.50;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.createdJacks == 0, "harder remix should not create random new jacks");
    require(result.report.quality.jackPreserveScore >= 0.99, "harder remix no-jack output should score cleanly");
}

void testDeterministicJackTieBreak() {
    const auto chart = makeNoJackChordFillTrapChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.maxAddedNoteRatio = 0.50;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "jack guard tie-breaks should be deterministic");
    require(first.report.quality.preventedJacks == second.report.quality.preventedJacks,
            "prevented jack count should be deterministic");
}

void testJackReportStable() {
    const auto chart = makeSourceJackChart();

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;
    options.maxAddedNoteRatio = 0.50;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.sourceJackGroups == second.report.quality.sourceJackGroups,
            "source jack group count should be stable");
    require(first.report.quality.preservedJackGroups == second.report.quality.preservedJackGroups,
            "preserved jack group count should be stable");
    require(first.report.quality.splitJackGroups == second.report.quality.splitJackGroups,
            "split jack group count should be stable");
    require(first.report.quality.createdJacks == second.report.quality.createdJacks,
            "created jack count should be stable");
    require(first.report.quality.jackPreserveScore == second.report.quality.jackPreserveScore,
            "jack preserve score should be stable");
}

void testHarderRemixDeterministic() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1750, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.maxAddedNoteRatio = 0.75;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.addedNotes > 0, "harder remix should add deterministic notes");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "harder remix should produce identical output for identical inputs");
    require(first.report.quality.collisionCount == 0, "harder remix should avoid collisions");
    require(first.report.quality.lnConflictCount == 0, "harder remix should avoid LN conflicts");
    require(first.report.quality.nearTimeConflicts == 0, "harder remix should avoid near-time conflicts");
}

void testExpansionBudgetRespected() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicChordFill;
    options.maxAddedNoteRatio = 0.25;
    options.maxAddedPerSlice = 1;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedNotes == 1, "expansion should respect maxAddedNoteRatio");
    require(result.report.quality.addedNoteRatio == 0.25, "added note ratio should be reported");
}

void testExpansionComposerPreserveBudget() {
    const auto chart = makeStreamChart(4, 12, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedNotes == 0, "preserve composer should not add notes");
    require(result.report.quality.expansionComposerProfile == "preserve",
            "preserve composer profile should be reported");
    require(result.report.quality.targetAddedNoteRatio == 0.0, "preserve composer target ratio should be zero");
    require(result.report.quality.acceptedByComposer == 0, "preserve composer should accept no additions");
}

void testExpansionComposerProfileBudgetReport() {
    const auto chart = makeStreamChart(4, 80, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 64;
    options.maxEchoPerMeasure = 64;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.expansionComposerProfile == "balanced",
            "balanced composer profile should be reported");
    require(result.report.quality.targetAddedNoteRatio == 0.04,
            "balanced composer target ratio should be reported");
    require(result.report.quality.acceptedByComposer == result.report.quality.addedNotes,
            "composer accepted count should match added notes");
    require(result.report.quality.budgetUsedRatio >= 0.0, "budget usage should be non-negative");
    require(result.report.quality.collisionCount == 0, "composer budgeted output should avoid collisions");
    require(result.report.quality.nearTimeConflicts == 0, "composer budgeted output should avoid near conflicts");
}

void testHarderComposerProfileAndSafety() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1625, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
    options.streamEchoProfile = keyconv::StreamEchoProfile::Training;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 16;
    options.maxEchoPerMeasure = 16;
    options.maxAddedPerSlice = 2;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.expansionComposerProfile == "harder",
            "harder remix should report harder composer profile");
    require(first.report.quality.targetAddedNoteRatio == 0.12,
            "harder composer target ratio should be reported");
    require(first.report.quality.addedNotes > 0, "harder composer should add notes");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "harder composer should be deterministic");
    require(first.report.quality.collisionCount == 0, "harder composer should avoid collisions");
    require(first.report.quality.lnConflictCount == 0, "harder composer should avoid LN conflicts");
    require(first.report.quality.nearTimeConflicts == 0, "harder composer should avoid near-time conflicts");
    require(first.report.quality.unsnappedAddedNotes == 0, "harder composer should keep additions snapped");
}

void testStairEchoDeterministicAndDirectionPreserved() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StairOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 8;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "stair echo should be deterministic");
    require(first.report.quality.addedByStairEcho == 3, "stair echo should add one helper per stair gap");
    require(first.report.quality.collisionCount == 0, "stair echo should avoid collisions");
    require(first.report.quality.nearTimeConflicts == 0, "stair echo should avoid near-time conflicts");
    require(first.report.quality.unsnappedAddedNotes == 0, "stair echo should keep added notes snapped");

    std::vector<std::pair<int, int>> generatedLanes;
    for (const auto& note : first.chart.notes) {
        if (note.id.rfind("gen:echo:stair_up:", 0) == 0) {
            generatedLanes.push_back({note.time, note.lane});
        }
    }
    std::sort(generatedLanes.begin(), generatedLanes.end());
    require(generatedLanes.size() == 3, "stair_up echo should keep one helper per stair gap");
    require(generatedLanes[0].second < generatedLanes[1].second &&
                generatedLanes[1].second < generatedLanes[2].second,
            "stair_up echo helper lanes should preserve upward direction");
    for (const auto& [time, lane] : generatedLanes) {
        (void)time;
        require(lane > 0 && lane < 9, "stair_up echo should avoid spending helper notes on outermost lanes");
    }
}

void testStairDownEchoDirectionPreserved() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StairOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 8;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    std::vector<std::pair<int, int>> generatedLanes;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:echo:stair_down:", 0) == 0) {
            generatedLanes.push_back({note.time, note.lane});
        }
    }
    std::sort(generatedLanes.begin(), generatedLanes.end());
    require(generatedLanes.size() == 3, "stair_down echo should keep one helper per stair gap");
    require(generatedLanes[0].second > generatedLanes[1].second &&
                generatedLanes[1].second > generatedLanes[2].second,
            "stair_down echo helper lanes should preserve downward direction");
    for (const auto& [time, lane] : generatedLanes) {
        (void)time;
        require(lane > 0 && lane < 9, "stair_down echo should avoid spending helper notes on outermost lanes");
    }
}

void testTrillEchoPreservesAB() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 2, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::TrillOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 4;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByTrillEcho == 4, "trill echo should add one helper per trill slice");
    require(result.report.quality.collisionCount == 0, "trill echo should avoid collisions");
    require(result.report.quality.nearTimeConflicts == 0, "trill echo should avoid near-time conflicts");

    std::vector<int> aSide;
    std::vector<int> bSide;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:echo:trill:", 0) != 0) {
            continue;
        }
        if (note.time == 1000 || note.time == 1250) {
            aSide.push_back(note.lane);
        } else if (note.time == 1125 || note.time == 1375) {
            bSide.push_back(note.lane);
        }
    }
    require(aSide.size() == 2 && bSide.size() == 2, "trill echo should keep A/B slice assignment");
    std::sort(aSide.begin(), aSide.end());
    std::sort(bSide.begin(), bSide.end());
    require(aSide.back() - aSide.front() <= 2 && bSide.back() - bSide.front() <= 2,
            "trill echo helper lanes should stay near each A/B side after 500 ms jack guarding");
    require(result.report.quality.createdJacks == 0,
            "trill echo should not leave created helper jacks under the 500 ms guard");
}

void testEchoBudgetRespected() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StairOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 0.25;
    options.maxEchoPerPattern = 8;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByEcho == 1, "echo should respect maxEchoAddedRatio");
    require(result.report.quality.rejectedEchoByBudget > 0, "echo budget rejections should be reported");
}

void testHarderRemixEchoStable() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.echoPolicy = keyconv::EchoPolicy::StairTrill;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 8;
    options.maxAddedPerSlice = 2;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.addedByEcho > 0, "harder remix should include deterministic echo when enabled");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "harder remix echo should be deterministic");
    require(first.report.quality.collisionCount == 0, "harder remix echo should avoid collisions");
    require(first.report.quality.nearTimeConflicts == 0, "harder remix echo should avoid near-time conflicts");
}

void testStreamEchoLowDensityAddsAndDeterministic() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1150, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1300, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1450, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1600, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 3;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.addedByStreamEcho > 0, "low-density stream should add limited stream echo");
    require(first.report.quality.collisionCount == 0, "stream echo should avoid collisions");
    require(first.report.quality.lnConflictCount == 0, "stream echo should avoid LN conflicts");
    require(first.report.quality.nearTimeConflicts == 0, "stream echo should avoid near-time conflicts");
    require(first.report.quality.unsnappedAddedNotes == 0, "stream echo should keep added notes snapped");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "stream echo should be deterministic");
}

void testStreamEchoHighDensityRejects() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1095, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1190, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1285, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1380, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1475, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1570, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1665, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1760, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1855, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1950, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2045, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByStreamEcho == 0, "high-density stream should reject stream echo");
    require(result.report.quality.rejectedStreamEchoByLocalNps > 0,
            "high-density stream should report local NPS rejection");
}

void testStreamEchoBurstRejects() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1060, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1120, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1180, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1240, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByStreamEcho == 0, "burst-like pattern should not get stream echo");
    require(result.report.quality.rejectedStreamEchoByBurst > 0,
            "burst-like pattern should report burst rejection");
}

void testStreamEchoBudgetRespected() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1150, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1300, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1450, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1600, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 0.2;
    options.maxEchoPerPattern = 8;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByStreamEcho == 1, "stream echo should respect maxEchoAddedRatio");
    require(result.report.quality.rejectedEchoByBudget > 0, "stream echo budget rejections should be reported");
}

void testStreamEchoDoesNotRunByDefault() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1150, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1300, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1450, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1600, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    require(result.report.quality.addedByStreamEcho == 0, "default preserve expansion should not run stream echo");
    require(result.report.quality.addedNotes == 0, "default preserve expansion should keep note count");
}

void testStreamEchoDiagnosticsStable() {
    const auto chart = makeStreamChart(4, 24, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.echoDiagnostics = true;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 16;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.streamEchoCandidates > 0, "diagnostics should count stream echo candidates");
    require(first.report.quality.streamEchoCandidates == first.report.quality.streamRawPatternCandidates,
            "compat streamEchoCandidates should equal raw pattern candidates");
    require(first.report.quality.streamAcceptedCandidates == first.report.quality.addedByStreamEcho,
            "accepted stream candidates should match added stream echo");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "diagnostics profile should keep output deterministic");
    require(keyconv::reportToJson(first.report) == keyconv::reportToJson(second.report),
            "diagnostics metrics should be stable");

    options.echoDiagnostics = false;
    const auto withoutDiagnostics = converter.convert(chart, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(withoutDiagnostics.chart, 10),
            "echo diagnostics should not alter conversion output");
    require(keyconv::reportToJson(first.report) == keyconv::reportToJson(withoutDiagnostics.report),
            "echo diagnostics should not alter conversion report");
}

void testStreamPrimaryRejectSums() {
    const auto burst = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1060, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1120, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1180, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1240, 0, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;

    const keyconv::Converter converter;
    const auto burstResult = converter.convert(burst, options);
    const auto& burstQuality = burstResult.report.quality;
    require(burstQuality.streamRawPatternCandidates ==
                burstQuality.streamEligiblePatternCandidates + streamPatternPrimaryRejectSum(burstQuality),
            "pattern-level primary rejects should reconcile raw stream pattern candidates");
    require(burstQuality.rejectedStreamPrimaryByBurst == burstQuality.streamRawPatternCandidates,
            "burst sample should reject every raw pattern candidate as burst");

    const auto lowDensity = makeStreamChart(4, 8, 180);
    options.maxEchoAddedRatio = 0.125;
    options.maxEchoPerPattern = 64;
    options.maxEchoPerMeasure = 64;
    const auto laneResult = converter.convert(lowDensity, options);
    const auto& laneQuality = laneResult.report.quality;
    require(laneQuality.streamRawLaneCandidates ==
                laneQuality.streamSafeLaneCandidates + streamLanePrimaryRejectSum(laneQuality),
            "lane-level primary rejects should reconcile raw lane candidates");
    require(laneQuality.streamSafeLaneCandidates == laneQuality.streamAcceptedCandidates,
            "safe stream candidates are immediately accepted in the same-slice MVP");
}

void testStreamEchoBalancedAtLeastConservative() {
    const auto chart = makeStreamChart(4, 80, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 64;
    options.maxEchoPerMeasure = 64;

    const keyconv::Converter converter;
    options.streamEchoProfile = keyconv::StreamEchoProfile::Conservative;
    const auto conservative = converter.convert(chart, options);
    options.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
    const auto balanced = converter.convert(chart, options);

    require(balanced.report.quality.addedByStreamEcho >= conservative.report.quality.addedByStreamEcho,
            "balanced stream echo should add at least as many stream helpers as conservative");
    require(balanced.report.quality.streamEchoProfile == "balanced", "balanced profile should be reported");
    require(balanced.report.quality.collisionCount == 0, "balanced stream echo should avoid collisions");
    require(balanced.report.quality.lnConflictCount == 0, "balanced stream echo should avoid LN conflicts");
    require(balanced.report.quality.nearTimeConflicts == 0, "balanced stream echo should avoid near-time conflicts");
}

void testTrainingStreamEchoProfileDeterministic() {
    const auto chart = makeStreamChart(4, 80, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    options.echoPolicy = keyconv::EchoPolicy::StreamOnly;
    options.streamEchoProfile = keyconv::StreamEchoProfile::Training;
    options.maxAddedNoteRatio = 1.0;
    options.maxEchoAddedRatio = 1.0;
    options.maxEchoPerPattern = 64;
    options.maxEchoPerMeasure = 64;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    require(first.report.quality.addedByStreamEcho > 0, "training profile should add stream helpers on low-density stream");
    require(first.report.quality.streamEchoProfile == "training", "training profile should be reported");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "training stream echo should be deterministic");
    require(first.report.quality.collisionCount == 0, "training stream echo should avoid collisions");
    require(first.report.quality.lnConflictCount == 0, "training stream echo should avoid LN conflicts");
    require(first.report.quality.nearTimeConflicts == 0, "training stream echo should avoid near-time conflicts");
}

void testFeelReportValuesStable() {
    const auto chart = makeStreamChart(4, 48, 180);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    options.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
    options.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    const auto& quality = first.report.quality;

    require(quality.densityDelta >= 0.0, "feel report should expose non-negative density delta for expansion");
    require(quality.laneCoverageBefore > 0.0, "feel report should expose source lane coverage");
    require(quality.laneCoverageAfter > 0.0, "feel report should expose target lane coverage");
    require(quality.laneEntropyAfter == quality.laneEntropy, "laneEntropyAfter should mirror existing laneEntropy");
    require(quality.handSpreadAfter >= 0.0, "feel report should expose hand spread");
    require(!quality.feelTags.empty(), "expanded chart should receive deterministic feel tags");
    require(keyconv::reportToJson(first.report) == keyconv::reportToJson(second.report),
            "feel report values should be deterministic");
}

void testPolicyComparisonCoreMatrixStable() {
    const auto chart = makeStreamChart(4, 48, 180);
    keyconv::ConvertOptions base;
    base.sourceKeyCount = 4;
    base.targetKeyCount = 10;
    base.style = keyconv::ConversionStyle::Direct;

    std::vector<std::pair<std::string, keyconv::ConvertOptions>> policies;

    auto preserve = base;
    preserve.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    policies.push_back({"preserve", preserve});

    auto echo = base;
    echo.expansionPolicy = keyconv::ExpansionPolicy::DeterministicEcho;
    echo.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
    echo.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
    policies.push_back({"echo-balanced", echo});

    auto training = base;
    training.expansionPolicy = keyconv::ExpansionPolicy::TrainingScaffold;
    policies.push_back({"training-scaffold", training});

    auto harder = base;
    harder.expansionPolicy = keyconv::ExpansionPolicy::HarderRemix;
    harder.echoPolicy = keyconv::EchoPolicy::StairTrillStream;
    harder.streamEchoProfile = keyconv::StreamEchoProfile::Balanced;
    policies.push_back({"harder-balanced", harder});

    const keyconv::Converter converter;
    int preserveAdded = -1;
    int trainingAdded = -1;
    int harderAdded = -1;
    for (const auto& policy : policies) {
        const auto first = converter.convert(chart, policy.second);
        const auto second = converter.convert(chart, policy.second);
        const auto& quality = first.report.quality;
        require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
                policy.first + " comparison output should be deterministic");
        require(keyconv::reportToJson(first.report) == keyconv::reportToJson(second.report),
                policy.first + " comparison report should be deterministic");
        require(quality.collisionCount == 0, policy.first + " comparison should avoid collisions");
        require(quality.lnConflictCount == 0, policy.first + " comparison should avoid LN conflicts");
        require(quality.nearTimeConflicts == 0, policy.first + " comparison should avoid near-time conflicts");
        require(quality.unsnappedAddedNotes == 0, policy.first + " comparison should avoid unsnapped added notes");
        if (policy.first == "preserve") {
            preserveAdded = quality.addedNotes;
        } else if (policy.first == "training-scaffold") {
            trainingAdded = quality.addedNotes;
        } else if (policy.first == "harder-balanced") {
            harderAdded = quality.addedNotes;
        }
    }
    require(preserveAdded == 0, "preserve comparison should keep addedNotes at zero");
    require(trainingAdded > preserveAdded, "training comparison should add notes on stream sample");
    require(harderAdded >= trainingAdded, "harder comparison should be at least as dense as training sample");
}

void testPreserveConvertLaneDriftMovesStablePhrases() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {5000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {7000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {9000, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreserveStrict;
    options.allowPlayableJackSplit = false;
    options.maxAddedNoteRatio = 0.0;
    options.preserveLaneDrift = true;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    auto lanes = lanesOf(result.chart);
    std::sort(lanes.begin(), lanes.end());
    const auto uniqueEnd = std::unique(lanes.begin(), lanes.end());

    require(result.report.quality.addedNotes == 0, "preserve lane drift should not add notes");
    require(std::distance(lanes.begin(), uniqueEnd) > 1,
            "preserve lane drift should move stable phrases across adjacent lanes");
}

void testPreserveConvertLaneDriftKeepsSourceJackTogether() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1080, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1160, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Faithful;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreserveStrict;
    options.allowPlayableJackSplit = false;
    options.maxAddedNoteRatio = 0.0;
    options.preserveLaneDrift = true;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto lanes = lanesOf(result.chart);

    require(lanes.size() == 3, "source jack sample should keep all notes");
    require(lanes[0] == lanes[1] && lanes[1] == lanes[2],
            "preserve lane drift should not split strict source jacks");
}

void testAutoMoreExpansionReportsLargerBudget() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1625, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1750, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1875, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto& quality = result.report.quality;

    require(quality.expansionPolicy == "preserve-tap-plus-more",
            "auto-more should report the more tap-plus policy");
    require(quality.expansionComposerProfile == "tap-plus-more",
            "auto-more should report the more composer profile");
    require(quality.targetAddedNoteRatio == 0.20,
            "auto-more should use the 20 percent high-key generated-note budget");
}

void testHighKeyTapPlusPrefersEighthBeatAdditions() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 4;
    keyconv::TimingPoint timing;
    timing.time = 0;
    timing.beatLength = 500.0;
    timing.uninherited = true;
    chart.timingPoints.push_back(timing);

    const int times[] = {1100, 1200, 1250, 1300, 1500, 1600, 1750, 1800, 2000, 2100};
    for (int i = 0; i < 10; ++i) {
        keyconv::Note note;
        note.id = "eg" + std::to_string(i);
        note.time = times[i];
        note.lane = i % 4;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;

    const auto result = keyconv::convertChart(chart, options);
    int generated = 0;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) != 0) {
            continue;
        }
        ++generated;
        require(note.time % 250 == 0,
                "high-key tap-plus should prefer 8th-beat source slices for generated notes");
    }
    require(generated == 2, "20 percent high-key tap-plus should add two notes to ten source notes");
}

void testTenKeyTapPlusBoostsQuarterEighthDensityAboveEightKey() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 4;
    keyconv::TimingPoint timing;
    timing.time = 0;
    timing.beatLength = 500.0;
    timing.uninherited = true;
    chart.timingPoints.push_back(timing);

    const int times[] = {1110, 1190, 1250, 1330, 1410, 1500, 1610, 1690, 1810, 1910};
    for (int i = 0; i < 10; ++i) {
        keyconv::Note note;
        note.id = "qd" + std::to_string(i);
        note.time = times[i];
        note.lane = i % 4;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    auto generatedTapPlusTime = [&](int targetKeyCount) {
        keyconv::ConvertOptions options;
        options.sourceKeyCount = 4;
        options.targetKeyCount = targetKeyCount;
        options.style = keyconv::ConversionStyle::Direct;
        options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusLow;

        const auto result = keyconv::convertChart(chart, options);
        std::vector<int> generatedTimes;
        for (const auto& note : result.chart.notes) {
            if (note.id.rfind("gen:tap_plus:", 0) == 0) {
                generatedTimes.push_back(note.time);
            }
        }
        require(generatedTimes.size() == 1, "low tap-plus fixture should add exactly one note");
        return generatedTimes.front();
    };

    require(generatedTapPlusTime(8) == 1250,
            "8K tap-plus should keep the baseline first eighth-beat fill");
    require(generatedTapPlusTime(10) == 1500,
            "10K tap-plus should boost quarter-beat density above the 8K baseline");
}

void testTenKeyDenimTapPlusShiftsEachBeat() {
    auto chart = makeChart(4,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                               {3000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {3000, 1, keyconv::NoteType::Tap, std::nullopt},
                           });
    keyconv::TimingPoint timing;
    timing.time = 1000;
    timing.beatLength = 500.0;
    timing.uninherited = true;
    chart.timingPoints.push_back(timing);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedNoteRatio = 0.50;
    options.maxAddedPerSlice = 1;

    const auto result = keyconv::convertChart(chart, options);
    std::map<int, int> generatedLaneByTime;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) == 0) {
            generatedLaneByTime[note.time] = note.lane;
        }
    }

    require(generatedLaneByTime.size() >= 3,
            "10K denim beat-shift fixture should generate helper notes on multiple beats");
    require(generatedLaneByTime.count(1000) == 1 &&
                generatedLaneByTime.count(1500) == 1 &&
                generatedLaneByTime.count(2000) == 1,
            "10K denim beat-shift fixture should cover consecutive whole-beat anchors");

    for (const auto& [time, lane] : generatedLaneByTime) {
        (void)time;
        require(lane >= 0 && lane < 5,
                "10K left-hand denim helpers should stay inside the left 5K panel");
    }

    const std::vector<int> beatAnchors = {1000, 1500, 2000, 2500, 3000};
    for (std::size_t index = 1; index < beatAnchors.size(); ++index) {
        const auto previous = generatedLaneByTime.find(beatAnchors[index - 1]);
        const auto current = generatedLaneByTime.find(beatAnchors[index]);
        if (previous != generatedLaneByTime.end() && current != generatedLaneByTime.end()) {
            require(previous->second != current->second,
                    "10K denim helper should shift on consecutive whole-beat anchors");
        }
    }
    require(generatedLaneByTime.at(1000) != generatedLaneByTime.at(2000),
            "10K denim helper should also shift the 5K panel phase every two beats");
}

void testEightKeyTapPlusReducesOneHandDenimWebAdditions() {
    auto chart = makeChart(4,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1750, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1750, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2250, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2250, 1, keyconv::NoteType::Tap, std::nullopt},
                           });
    addTimingPoint(chart);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 8;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedPerSlice = 2;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.addedNotes == 0,
            "8K tap-plus should skip generated notes that turn a one-hand two-lane chord into a denim web");
    require(result.report.quality.rejectedExpansionCandidates > 0,
            "8K denim web guard should reject the unsafe generated-note candidates");
}

void testHighKeyExtremeTrillAvoidsBothOuterEdges() {
    keyconv::Chart chart;
    chart.meta.sourceKeyCount = 7;
    for (int i = 0; i < 12; ++i) {
        keyconv::Note note;
        note.id = "xt" + std::to_string(i);
        note.time = 1000 + i * 125;
        note.lane = (i % 2 == 0) ? 0 : 6;
        note.sourceLane = note.lane;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.detectedTrills >= 1, "extreme alternating source should be detected as a trill");
    for (std::size_t index = 1; index < result.chart.notes.size(); ++index) {
        const auto& previous = result.chart.notes[index - 1];
        const auto& current = result.chart.notes[index];
        if (std::abs(current.time - previous.time) > 180) {
            continue;
        }
        const bool bothOuter = (previous.lane == 0 && current.lane == 9) ||
                               (previous.lane == 9 && current.lane == 0);
        require(!bothOuter, "8K+ trill mapping should avoid both outer edges as the alternating pair");
    }
}

void testLongSourceJackStaysSingleLanePlayable() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3500, 1, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    std::optional<int> jackLane;
    int jackNotes = 0;
    for (const auto& note : result.chart.notes) {
        if (note.id < "n0" || note.id > "n5") {
            continue;
        }
        if (!jackLane.has_value()) {
            jackLane = note.lane;
        }
        require(note.lane == *jackLane, "long source jack should stay on one lane in playable conversion");
        ++jackNotes;
    }
    require(jackNotes == 6, "long jack test should inspect all converted jack notes");
    require(result.report.quality.sourceJackGroups == 1, "long same-source repeat should be one source jack group");
    require(result.report.quality.preservedJackGroups == 1, "long source jack should report as preserved");
    require(result.report.quality.brokenJacks == 0, "long source jack should not be reported broken");
}

void testChordEmbeddedLongSourceJackStaysSingleLanePlayable() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {2500, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {3000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {3500, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {3500, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.jackPreservePolicy = keyconv::JackPreservePolicy::PreservePlayable;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);

    const std::set<std::string> jackIds{"n0", "n2", "n4", "n6", "n8", "n10"};
    std::optional<int> jackLane;
    int jackNotes = 0;
    for (const auto& note : result.chart.notes) {
        if (jackIds.count(note.id) == 0) {
            continue;
        }
        if (!jackLane.has_value()) {
            jackLane = note.lane;
        }
        require(note.lane == *jackLane,
                "chord-embedded long source jack should stay on one lane in playable conversion");
        ++jackNotes;
    }
    require(jackNotes == 6, "chord-embedded long jack test should inspect all repeated source notes");
    require(result.report.quality.sourceJackGroups == 1,
            "chord-embedded repeated source lane should be one source jack group");
    require(result.report.quality.preservedJackGroups == 1,
            "chord-embedded long source jack should report as preserved");
    require(result.report.quality.detectedJacks >= 1,
            "gesture rail should detect source-lane jack groups even when each repeat is inside a chord");
    require(result.report.quality.brokenJacks == 0,
            "chord-embedded long source jack should not be reported broken");
}

void testEvenKeyFastThirtySecondStairSuppressesAdditions() {
    auto chart = makeChart(4,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1062, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1187, 3, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1312, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1375, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1437, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1562, 3, keyconv::NoteType::Tap, std::nullopt},
                               {1625, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1687, 1, keyconv::NoteType::Tap, std::nullopt},
                           });
    addTimingPoint(chart);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedPerSlice = 2;

    const auto result = keyconv::convertChart(chart, options);
    require(result.report.quality.addedNotes == 0,
            "32nd-or-faster even-key stair slices should suppress generated notes");
}

void testEightKeyFastSixLaneStairKeepsSymmetricVacancies() {
    auto chart = makeChart(6,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1062, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1125, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1187, 3, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 4, keyconv::NoteType::Tap, std::nullopt},
                               {1312, 5, keyconv::NoteType::Tap, std::nullopt},
                               {1375, 4, keyconv::NoteType::Tap, std::nullopt},
                               {1437, 3, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1562, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1625, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1687, 1, keyconv::NoteType::Tap, std::nullopt},
                           });
    addTimingPoint(chart);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 6;
    options.targetKeyCount = 8;
    options.style = keyconv::ConversionStyle::Playable;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.maxAddedPerSlice = 2;

    const auto result = keyconv::convertChart(chart, options);
    std::set<int> usedLanes;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:", 0) == 0) {
            continue;
        }
        usedLanes.insert(note.lane);
    }

    const std::set<int> expected{0, 2, 3, 4, 5, 7};
    std::string actualLanes;
    for (const int lane : usedLanes) {
        if (!actualLanes.empty()) {
            actualLanes += ",";
        }
        actualLanes += std::to_string(lane);
    }
    require(usedLanes == expected,
            "6K to 8K fast stair should leave lanes 2 and 7 empty in one-indexed 8K; actual=" +
                actualLanes);
    require(result.report.quality.addedNotes == 0,
            "8K fast stair symmetric-vacancy fixture should still suppress generated notes");
}

void testEvenKeyLeftOnlyAdditionsStayLeftHand() {
    auto chart = makeChart(4,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1750, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2250, 1, keyconv::NoteType::Tap, std::nullopt},
                               {2500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {2750, 1, keyconv::NoteType::Tap, std::nullopt},
                               {3000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {3250, 1, keyconv::NoteType::Tap, std::nullopt},
                               {3500, 0, keyconv::NoteType::Tap, std::nullopt},
                               {3750, 1, keyconv::NoteType::Tap, std::nullopt},
                           });
    addTimingPoint(chart);

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;

    const auto result = keyconv::convertChart(chart, options);
    int generated = 0;
    for (const auto& note : result.chart.notes) {
        if (note.id.rfind("gen:tap_plus:", 0) != 0) {
            continue;
        }
        ++generated;
        require(note.lane < 5, "left-hand-only even-key source slices should add notes in the left target hand");
    }
    require(generated > 0, "left-only even-key chart should still receive generated notes");
}

void testStreamSuperRandomRelanesEveryNote() {
    auto chart = makeChart(10, {});
    for (int i = 0; i < 10; ++i) {
        keyconv::Note note;
        note.id = "r" + std::to_string(i);
        note.time = 1000 + i * 120;
        note.lane = 0;
        note.sourceLane = 0;
        note.type = keyconv::NoteType::Tap;
        chart.notes.push_back(note);
    }

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 10;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.streamTransformPolicy = keyconv::StreamTransformPolicy::SuperRandom;

    const keyconv::Converter converter;
    const auto first = converter.convert(chart, options);
    const auto second = converter.convert(chart, options);
    auto seededOptions = options;
    seededOptions.seed = 12345;
    const auto seeded = converter.convert(chart, seededOptions);
    auto lanes = lanesOf(first.chart);
    std::sort(lanes.begin(), lanes.end());
    const auto uniqueEnd = std::unique(lanes.begin(), lanes.end());

    require(first.report.quality.streamTransformPolicy == "superrandom",
            "superrandom stream transform should be reported");
    require(first.report.quality.streamTransformedNotes == static_cast<int>(chart.notes.size()),
            "superrandom stream transform should touch every note");
    require(std::distance(lanes.begin(), uniqueEnd) > 1,
            "superrandom stream transform should spread notes across lanes");
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10),
            "superrandom stream transform should be deterministic");
    require(keyconv::exportOsu(first.chart, 10) != keyconv::exportOsu(seeded.chart, 10),
            "superrandom stream transform should change when the seed changes");
}

void testStreamSuperRandomKeepsChordDistinct() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1750, 1, keyconv::NoteType::Hold, 2250},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.streamTransformPolicy = keyconv::StreamTransformPolicy::SuperRandom;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    const auto validation = keyconv::validateNoOverlap(result.chart.notes, 4);

    require(result.report.quality.streamTransformedNotes == static_cast<int>(chart.notes.size()),
            "superrandom should touch chord and hold notes");
    require(validation.sameTimeCollisions == 0, "superrandom should keep same-time chord lanes distinct");
    require(validation.longNoteConflicts == 0, "superrandom should avoid LN conflicts");
}

void testFullJitterOffsetsSameTimeChords() {
    const auto chart = makeChart(4,
                                 {
                                     {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                     {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                     {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                 });

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 4;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveNoteCount;
    options.streamTransformPolicy = keyconv::StreamTransformPolicy::FullJitter;

    const keyconv::Converter converter;
    const auto result = converter.convert(chart, options);
    std::vector<int> chordTimes;
    int checkedNotes = 0;
    for (const auto& note : result.chart.notes) {
        const int originalIndex = std::stoi(note.id.substr(1));
        const int originalTime = originalIndex < 3 ? 1000 : 1500;
        const int offset = std::abs(note.time - originalTime);
        require(offset >= 1 && offset <= 15, "full-jitter should offset every note by 1 to 15 ms");
        ++checkedNotes;
        if (std::abs(note.time - 1000) <= 15) {
            chordTimes.push_back(note.time);
        }
    }
    std::sort(chordTimes.begin(), chordTimes.end());
    const auto uniqueEnd = std::unique(chordTimes.begin(), chordTimes.end());

    require(result.report.quality.streamTransformPolicy == "full-jitter",
            "full-jitter stream transform should be reported");
    require(result.report.quality.streamJitteredNotes == checkedNotes,
            "full-jitter should offset every note");
    require(chordTimes.size() == 3 && std::distance(chordTimes.begin(), uniqueEnd) > 1,
            "full-jitter should split a same-time chord into close ms offsets");
}

void testStreamSuperSymmetryPolicyParses() {
    const auto parsed = keyconv::parseStreamTransformPolicy("super-symmetry");
    require(parsed.has_value() && *parsed == keyconv::StreamTransformPolicy::SuperSymmetry,
            "super-symmetry should parse as a stream transform policy");
    require(keyconv::toString(keyconv::StreamTransformPolicy::SuperSymmetry) == "super-symmetry",
            "super-symmetry stream transform should stringify stably");
}

void testDifficultyNameMarksExpansionAndStreamTransform() {
    auto chart = makeChart(4,
                           {
                               {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                               {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                               {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                               {1750, 3, keyconv::NoteType::Tap, std::nullopt},
                           });
    chart.meta.version = "4K";

    keyconv::ConvertOptions options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 10;
    options.style = keyconv::ConversionStyle::Direct;
    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlus;

    const keyconv::Converter converter;
    auto result = converter.convert(chart, options);
    auto reparsed = keyconv::parseOsu(keyconv::exportOsu(result.chart, 10), {});
    require(reparsed.meta.version == "4K KeyWeaver10K (normal)",
            "normal expansion should be visible in osu difficulty name");

    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusMore;
    options.streamTransformPolicy = keyconv::StreamTransformPolicy::SuperRandom;
    result = converter.convert(chart, options);
    reparsed = keyconv::parseOsu(keyconv::exportOsu(result.chart, 10), {});
    require(reparsed.meta.version == "4K KeyWeaver10K-sRan (more)",
            "superrandom more conversion should be visible in osu difficulty name");

    options.expansionPolicy = keyconv::ExpansionPolicy::PreserveTapPlusLow;
    options.streamTransformPolicy = keyconv::StreamTransformPolicy::FullJitter;
    result = converter.convert(chart, options);
    reparsed = keyconv::parseOsu(keyconv::exportOsu(result.chart, 10), {});
    require(reparsed.meta.version == "4K KeyWeaver10K-jitter (low)",
            "jitter low conversion should be visible in osu difficulty name");
}

void testConvertedChartMarkerGuard() {
    using keyconv::ConvertedChartMarkerKind;

    require(keyconv::convertedChartMarkerKind(std::string_view("Song [A7K]")) ==
                ConvertedChartMarkerKind::AKey,
            "A7K marker should be detected");
    require(keyconv::convertedChartMarkerKind(std::string_view("Song (a10K)")) ==
                ConvertedChartMarkerKind::AKey,
            "a10K marker should be detected");
    require(keyconv::convertedChartMarkerKind(std::string_view("Song [4to7c]")) ==
                ConvertedChartMarkerKind::ToKeyC,
            "4to7c marker should be detected");
    require(keyconv::convertedChartMarkerKind(std::string_view("Song [7 to 10 c]")) ==
                ConvertedChartMarkerKind::ToKeyC,
            "spaced 7 to 10 c marker should be detected");
    require(keyconv::convertedChartMarkerKind(std::string_view("Song (4K10C)")) ==
                ConvertedChartMarkerKind::KeyC,
            "4K10C marker should be detected");
    require(keyconv::convertedChartMarkerKind(std::wstring_view(L"KeyWeaver10K")) ==
                ConvertedChartMarkerKind::KeyWeaver,
            "KeyWeaver marker should be detected for GUI wide strings");
    require(!keyconv::hasConvertedChartMarker(std::string_view("mania10k")),
            "plain mania10k text should not trip the aNK guard");
    require(!keyconv::hasConvertedChartMarker(std::string_view("Simple 10K")),
            "plain source key labels should not be treated as converted markers");
}

void testNk2ReportOnlyIntentGraph() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 4, keyconv::NoteType::Hold, 1900},
                                  {2000, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {2125, 5, keyconv::NoteType::Tap, std::nullopt},
                                  {2250, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {2375, 5, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Report;

    const auto report = keyconv::nk2::analyzeReportOnly(chart, options);
    require(!report.chartMutated, "NK2 milestone 1 must not mutate charts");
    require(!report.noOp, "7K to 10K should not be same-K no-op");
    require(report.intent.totalNotes == 8, "NK2 intent should count notes");
    require(report.intent.holdNotes == 1, "NK2 intent should count LN anchors");
    require(report.intent.stairMotifs > 0, "NK2 intent should detect stair motif");
    require(report.intent.trillMotifs > 0, "NK2 intent should detect trill motif");
    require(report.intent.mirrorSupportCandidates > 0, "NK2 should expose mirror support candidates");
    require(report.layout.targetKeyCount == 10, "NK2 target layout should track target K");
    require(report.layout.hasPanels, "10K layout should expose 5K panels");
    require(report.layout.hasBridge, "10K layout should expose bridge lanes");
    require(report.options.layoutWeights.panel == 3 &&
                report.options.layoutWeights.bridge == 2 &&
                report.options.layoutWeights.fullField == 6,
            "NK2 default 10K layout weights should be 3/2/6");

    const auto json = keyconv::nk2::reportToJson(report);
    require(json.find("\"engine\": \"nk2\"") != std::string::npos, "NK2 JSON should include engine");
    require(json.find("\"chartMutated\": false") != std::string::npos, "NK2 JSON should report no mutation");
}

void testNk2SameKeyNoOpUnlessTransform() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1100, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1200, 2, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 7;
    options.mode = keyconv::nk2::Mode::Native;
    const auto nativeReport = keyconv::nk2::analyzeReportOnly(chart, options);
    require(nativeReport.noOp, "same-K NK2 native should no-op");
    require(nativeReport.noOpReason.find("same key count") != std::string::npos,
            "same-K no-op should explain why");

    options.mode = keyconv::nk2::Mode::Transform;
    const auto transformReport = keyconv::nk2::analyzeReportOnly(chart, options);
    require(!transformReport.noOp, "same-K NK2 transform should not be treated as no-op");
    require(!transformReport.chartMutated, "NK2 analysis-only transform report should not mutate charts");
}

void testNk2FourToFiveGenericPrototypeConversion() {
    auto chart = makeChart(4, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 3, keyconv::NoteType::Hold, 1900},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 5;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.chartMutated, "NK2 generic 4K to 5K should mutate chart lanes");
    require(result.report.prototypeName == "nk2-generic-nk-relane-compress",
            "NK2 generic 4K to 5K should use the generic prototype");
    require(result.chart.meta.targetKeyCount == 5, "NK2 generic output should set target key count");
    require(result.chart.notes.size() > chart.notes.size(),
            "NK2 generic 4K to 5K should add fill notes");
    require(result.report.addedNotes > 0,
            "NK2 generic 4K to 5K should report added fill notes");
    require(result.report.strongBeatSupportAccepted + result.report.mirrorSupportAccepted +
                result.report.lnSupportAccepted == result.report.addedNotes,
            "NK2 generic 4K to 5K support counters should match added notes");
    require(result.report.droppedNotes == 0,
            "NK2 generic 4K to 5K should not drop notes");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic 4K to 5K should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic 4K to 5K should avoid LN conflicts");
    require(result.report.createdJacks == 0,
            "NK2 generic 4K to 5K should avoid created jacks");
    require(result.report.laneDistribution.size() == 5,
            "NK2 generic 4K to 5K should report 5 target lanes");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 5,
                "NK2 generic 4K to 5K lane should stay inside target field");
    }
    const auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.type == keyconv::NoteType::Hold;
    });
    require(hold != result.chart.notes.end() && hold->endTime == 1900,
            "NK2 generic 4K to 5K should preserve LN duration");
}

void testNk2FourToFiveGenericUsesWholeFieldSpread() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 40; ++i) {
        notes.push_back({1000 + i * 140, i % 4, keyconv::NoteType::Tap, std::nullopt});
    }
    notes.push_back({7000, 3, keyconv::NoteType::Hold, 7440});
    auto chart = makeChart(4, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 4;
    options.targetKeyCount = 5;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.chart.notes.size() > chart.notes.size(),
            "NK2 generic 4K to 5K whole-field spread should add fill notes");
    require(result.report.addedNotes > 0,
            "NK2 generic 4K to 5K whole-field spread should report fill notes");
    require(result.report.droppedNotes == 0,
            "NK2 generic 4K to 5K whole-field spread should not drop notes");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic 4K to 5K whole-field spread should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic 4K to 5K whole-field spread should avoid LN conflicts");
    require(result.report.createdJacks == 0,
            "NK2 generic 4K to 5K whole-field spread should avoid created jacks");
    require(activeLaneCount(result.report.laneDistribution) == 5,
            "NK2 generic 4K to 5K should use every target lane: " +
                distributionText(result.report.laneDistribution));
    require(laneDistributionRange(result.report.laneDistribution) <= 8,
            "NK2 generic 4K to 5K should not starve one lane: " +
                distributionText(result.report.laneDistribution));
}

void testNk2SevenToEightGenericPrototypeConversion() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {1625, 5, keyconv::NoteType::Tap, std::nullopt},
                                  {1750, 6, keyconv::NoteType::Hold, 2200},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.chartMutated, "NK2 generic 7K to 8K should mutate chart lanes");
    require(result.report.prototypeName == "nk2-generic-nk-relane-compress",
            "NK2 generic 7K to 8K should use the generic prototype");
    require(result.chart.meta.targetKeyCount == 8, "NK2 generic 7K to 8K output should set target key count");
    require(result.chart.notes.size() == chart.notes.size(),
            "NK2 generic 7K to 8K should preserve source note count");
    require(result.report.droppedNotes == 0,
            "NK2 generic 7K to 8K should not drop notes");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic 7K to 8K should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic 7K to 8K should avoid LN conflicts");
    require(result.report.createdJacks == 0,
            "NK2 generic 7K to 8K should avoid created jacks");
    require(result.report.laneDistribution.size() == 8,
            "NK2 generic 7K to 8K should report 8 target lanes");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 8,
                "NK2 generic 7K to 8K lane should stay inside target field");
    }
    const auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.type == keyconv::NoteType::Hold;
    });
    require(hold != result.chart.notes.end() && hold->endTime == 2200,
            "NK2 generic 7K to 8K should preserve LN duration");
}

void testNk2SevenToEightGenericUsesWholeFieldSpread() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 56; ++i) {
        notes.push_back({1000 + i * 120, i % 7, keyconv::NoteType::Tap, std::nullopt});
    }
    notes.push_back({8000, 6, keyconv::NoteType::Hold, 8440});
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.droppedNotes == 0,
            "NK2 generic 7K to 8K whole-field spread should not drop notes");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic 7K to 8K whole-field spread should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic 7K to 8K whole-field spread should avoid LN conflicts");
    require(result.report.createdJacks == 0,
            "NK2 generic 7K to 8K whole-field spread should avoid created jacks");
    require(activeLaneCount(result.report.laneDistribution) == 8,
            "NK2 generic 7K to 8K should use every target lane: " +
                distributionText(result.report.laneDistribution));
    require(laneDistributionRange(result.report.laneDistribution) <= 10,
            "NK2 generic 7K to 8K should not leave a lane under-filled: " +
                distributionText(result.report.laneDistribution));
    require(result.report.sourceAnchorScore <= 0.75,
            "NK2 generic 7K to 8K should soften fixed source anchors");
}

void testNk2SevenToEightLongLnCopiesAdjacentPlacement() {
    auto chart = makeChart(7, {
                                  {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 3, keyconv::NoteType::Hold, 3200},
                                  {3600, 5, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto anchor = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id == "n0";
    });
    const auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id == "n1";
    });

    require(anchor != result.chart.notes.end(), "NK2 adjacent-copy fixture should keep anchor tap");
    require(hold != result.chart.notes.end(), "NK2 adjacent-copy fixture should keep long LN");
    require(hold->type == keyconv::NoteType::Hold && hold->endTime == 3200,
            "NK2 adjacent-copy fixture should preserve long LN duration");
    require(hold->lane == anchor->lane + 1,
            "NK2 long LN should copy the adjacent source placement");
    require(hold->lane == 3,
            "NK2 long LN should prefer adjacent-copy lane over the direct 7K-to-8K lane");
    require(result.report.sameTimeCollisions == 0,
            "NK2 adjacent-copy long LN should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 adjacent-copy long LN should avoid LN conflicts");
    require(result.report.createdJacks == 0,
            "NK2 adjacent-copy long LN should avoid created jacks");
}

void testNk2LocalSolverHandlesSameTimeChord() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 6, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.localSolverWindows > 0,
            "NK2 local solver should run on same-time chord windows");
    require(result.report.localSolverCandidates > 0,
            "NK2 local solver should evaluate chord candidate combinations");
    require(result.report.localSolverFallbacks == 0,
            "NK2 local solver should solve a fitting same-time chord without fallback");
    require(result.chart.notes.size() == chart.notes.size(),
            "NK2 local solver should preserve a fitting same-time chord");
    require(result.report.sameTimeCollisions == 0,
            "NK2 local solver should avoid same-time chord collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 local solver should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 local solver should keep created-jack gate intact");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"localSolverWindows\"") != std::string::npos,
            "NK2 JSON should include local solver diagnostics");
    require(json.find("\"localSolverCandidates\"") != std::string::npos,
            "NK2 JSON should include local solver candidate count");
}

void testNk2SevenToFourGenericPrototypeCompression() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 5, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 6, keyconv::NoteType::Tap, std::nullopt},
                                  {1750, 1, keyconv::NoteType::Hold, 2200},
                                  {2200, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {2500, 5, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 4;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.chartMutated, "NK2 generic 7K to 4K should mutate chart lanes");
    require(result.report.prototypeName == "nk2-generic-nk-relane-compress",
            "NK2 generic 7K to 4K should use the generic prototype");
    require(result.chart.meta.targetKeyCount == 4, "NK2 generic down output should set target key count");
    require(result.report.lowerKeyRolledNotes > 0,
            "NK2 generic 7K to 4K should roll safe overflow taps before dropping");
    require(result.chart.notes.size() >= chart.notes.size() - 1,
            "NK2 generic 7K to 4K should preserve most overflow notes through safe rolls");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic 7K to 4K should avoid collisions");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic 7K to 4K should avoid LN conflicts");
    const auto collisionScan = keyconv::detectCollisions(result.chart.notes);
    require(collisionScan.longNoteConflicts == 0,
            "NK2 generic 7K to 4K should avoid LN-tail overlaps in the exported chart");
    require(result.report.createdJacks == 0,
            "NK2 generic 7K to 4K should avoid created jacks");
    require(result.report.laneDistribution.size() == 4,
            "NK2 generic 7K to 4K should report 4 target lanes");
    require(result.report.localSolverWindows > 0,
            "NK2 generic 7K to 4K should try local solver on oversized chord windows");
    require(result.report.localSolverFallbacks > 0,
            "NK2 generic 7K to 4K should fall back when a chord exceeds target lanes");
    for (const auto& note : result.chart.notes) {
        require(note.lane >= 0 && note.lane < 4,
                "NK2 generic 7K to 4K lane should stay inside target field");
    }
    const auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.type == keyconv::NoteType::Hold;
    });
    require(hold != result.chart.notes.end() && hold->endTime == 2200,
            "NK2 generic 7K to 4K should preserve surviving LN duration");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"lowerKeyRolledNotes\"") != std::string::npos,
            "NK2 JSON should include lower-key roll diagnostics");
}

void testNk2SevenToEightNativeSupportNotesAreGeneralized() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Native;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.addedNotes > 0,
            "NK2 generic 7K to 8K native should add generalized support notes");
    require(result.report.mirrorSupportCandidates > 0,
            "NK2 generic high-key support should emit mirror candidates");
    require(result.report.mirrorSupportAccepted > 0,
            "NK2 generic high-key support should accept safe mirror notes");
    require(result.report.sameTimeCollisions == 0,
            "NK2 generic high-key support should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 generic high-key support should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 generic high-key support should keep created-jack gate intact");
}

void testNk2LnAdjacentSupportAddsHeadAndTailTaps() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Hold, 2000},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 8;
    options.mode = keyconv::nk2::Mode::Harder;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.lnSupportCandidates >= 2,
            "NK2 LN-adjacent support should emit both head and tail candidates");
    require(result.report.lnSupportAccepted >= 2,
            "NK2 LN-adjacent support should accept safe head and tail taps");

    bool hasHeadTap = false;
    bool hasTailTap = false;
    for (const auto& note : result.chart.notes) {
        if (note.id.find("nk2-ln-ln-") != 0) {
            continue;
        }
        hasHeadTap = hasHeadTap || note.time == 1000;
        hasTailTap = hasTailTap || note.time == 2000;
        require(note.type == keyconv::NoteType::Tap,
                "NK2 LN-adjacent support should stay tap-only");
        require(!note.endTime.has_value(),
                "NK2 LN-adjacent support should not synthesize generated LNs");
    }
    require(hasHeadTap, "NK2 LN-adjacent support should add a head-side tap");
    require(hasTailTap, "NK2 LN-adjacent support should add a tail-side tap");
    require(result.report.sameTimeCollisions == 0,
            "NK2 LN-adjacent support should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 LN-adjacent support should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 LN-adjacent support should keep created-jack gate intact");
}

void testNk2SuperSymmetryPreservesSameTimeMirrors() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1000, 6, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 5, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;
    options.superSymmetry = true;

    const auto result = keyconv::nk2::convertChart(chart, options);
    std::map<std::pair<int, int>, int> laneByTimeAndSource;
    for (const auto& note : result.chart.notes) {
        laneByTimeAndSource[{note.time, note.sourceLane.value_or(note.lane)}] = note.lane;
    }

    require(result.report.options.superSymmetry,
            "NK2 report should keep the super-symmetry option");
    require(result.report.superSymmetryMirrorAnchors >= 2,
            "NK2 super-symmetry should count same-time mirror anchors");
    require(laneByTimeAndSource[{1000, 0}] + laneByTimeAndSource[{1000, 6}] == 9,
            "NK2 super-symmetry should mirror source 0/6 into target 10K");
    require(laneByTimeAndSource[{1250, 1}] + laneByTimeAndSource[{1250, 5}] == 9,
            "NK2 super-symmetry should mirror source 1/5 into target 10K");
    require(result.report.sameTimeCollisions == 0,
            "NK2 super-symmetry mirrored chords should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 super-symmetry mirrored chords should keep LN gate intact");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"superSymmetry\": true") != std::string::npos,
            "NK2 JSON should report super-symmetry mode");
    require(json.find("\"superSymmetryMirrorAnchors\"") != std::string::npos,
            "NK2 JSON should include super-symmetry mirror anchors");
}

void testNk2SuperSymmetryPreservesGaplessStair() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1120, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1240, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1360, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {1480, 4, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;
    options.superSymmetry = true;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.chart.notes.size() == chart.notes.size(),
            "NK2 super-symmetry stair fixture should preserve note count");
    require(result.report.superSymmetryGaplessStairs >= 4,
            "NK2 super-symmetry should count preserved gapless stair links");

    const auto sourceLane = [](const keyconv::Note& note) {
        return note.sourceLane.value_or(note.lane);
    };
    for (std::size_t i = 1; i < result.chart.notes.size(); ++i) {
        const int sourceDelta = sourceLane(result.chart.notes[i]) - sourceLane(result.chart.notes[i - 1]);
        const int targetDelta = result.chart.notes[i].lane - result.chart.notes[i - 1].lane;
        require(sourceDelta == 1 && targetDelta == 1,
                "NK2 super-symmetry should keep a gapless ascending stair gapless");
    }
    require(result.report.sameTimeCollisions == 0,
            "NK2 super-symmetry stair should keep collision gate intact");
    require(result.report.createdJacks == 0,
            "NK2 super-symmetry stair should not create target jacks");
}

void testNk2SevenToTenPrototypeConversion() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1250, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1375, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {1750, 4, keyconv::NoteType::Hold, 2350},
                                  {2250, 6, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.chartMutated, "NK2 7K to 10K prototype should mutate chart lanes");
    require(result.chart.meta.targetKeyCount == 10, "NK2 converted chart should set target key count");
    require(result.chart.notes.size() == chart.notes.size(), "NK2 prototype should preserve note count");
    require(result.report.addedNotes == 0, "NK2 faithful prototype should not add support notes");
    require(result.report.sameTimeCollisions == 0, "NK2 prototype should avoid same-time lane collisions");
    require(result.report.longNoteConflicts == 0, "NK2 prototype should avoid LN conflicts");
    require(result.report.createdJacks == 0, "NK2 prototype should avoid new target jacks");
    require(result.report.laneDistribution.size() == 10, "NK2 report should include 10K lane distribution");
    require(result.report.sourceAnchorTotal == static_cast<int>(chart.notes.size()),
            "NK2 source-anchor diagnostic should count original notes");
    require(result.report.sourceAnchorMatches > 0,
            "NK2 source-anchor diagnostic should see preserved direct anchors");
    require(result.report.sourceAnchorScore > 0.0 && result.report.sourceAnchorScore <= 1.0,
            "NK2 source-anchor score should be normalized");

    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == chart.notes.size(), "NK2 lane list should match note count");
    require(lanes[0] < lanes[1] && lanes[1] < lanes[2] && lanes[2] < lanes[3],
            "NK2 7K stair should keep ascending target direction");
    for (const int lane : lanes) {
        require(lane >= 0 && lane < 10, "NK2 converted lane should stay inside 10K field");
    }
    const auto hold = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.type == keyconv::NoteType::Hold;
    });
    require(hold != result.chart.notes.end(), "NK2 should preserve hold note");
    require(hold->endTime == 2350, "NK2 must preserve LN duration exactly");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"chartMutated\": true") != std::string::npos, "NK2 JSON should report mutation");
    require(json.find("\"prototypeName\": \"nk2-7k10k-panel-bridge-fullfield\"") != std::string::npos,
            "NK2 JSON should name the prototype");
    require(json.find("\"sourceAnchorScore\"") != std::string::npos,
            "NK2 JSON should include source-anchor score");
}

void testNk2SevenToTenPreservesSourceJack() {
    auto chart = makeChart(7, {
                                  {1000, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1120, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {1650, 3, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    require(lanes.size() == 4, "NK2 source jack fixture should keep note count");
    require(lanes[0] == lanes[1], "NK2 should preserve source jack as same target lane");
    require(result.report.preservedSourceJacks > 0, "NK2 report should count preserved source jacks");
    require(result.report.createdJacks == 0, "NK2 source jack preservation should not count as created jack");
}

void testNk2SevenToTenSupportNotesAreGated() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Hold, 1600},
                                  {1125, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {1500, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {2000, 5, keyconv::NoteType::Hold, 2600},
                                  {2125, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {2500, 6, keyconv::NoteType::Tap, std::nullopt},
                                  {3000, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {3500, 3, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Native;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.chartMutated, "NK2 support fixture should convert chart");
    require(result.chart.notes.size() > chart.notes.size(), "NK2 native should add limited support notes");
    require(result.report.addedNotes > 0, "NK2 report should count added support notes");
    require(result.report.lnSupportCandidates > 0, "NK2 should expose LN support candidates");
    require(result.report.mirrorSupportCandidates > 0, "NK2 should expose mirror support candidates");
    require(result.report.lnSupportAccepted + result.report.strongBeatSupportAccepted +
                result.report.mirrorSupportAccepted == result.report.addedNotes,
            "NK2 support accepted counters should match added notes");
    require(result.report.sourceAnchorTotal == static_cast<int>(chart.notes.size()),
            "NK2 source-anchor diagnostic should ignore generated support notes");
    const int generatedProvenance =
        result.report.generatedFromJackMotif + result.report.generatedFromTrillMotif +
        result.report.generatedFromStairMotif + result.report.generatedFromStreamMotif +
        result.report.generatedFromChordMotif + result.report.generatedFromLnMotif +
        result.report.generatedFromNeutralMotif;
    require(generatedProvenance == result.report.addedNotes,
            "NK2 generated provenance counters should match added notes");
    require(result.report.supportRejectedByBudget > 0,
            "NK2 support generation should expose budget rejections on dense candidate sets");
    require(result.report.supportPhraseWindows > 0,
            "NK2 support generation should report phrase-local budget windows");
    require(result.report.phraseProfileWindows > 0,
            "NK2 support generation should report phrase-profile windows");
    require(result.report.phraseProfileOverBudgetWindows == 0,
            "NK2 support generation should keep support inside phrase profile caps");
    require(result.report.phraseProfileScore > 0.0 && result.report.phraseProfileScore <= 1.0,
            "NK2 phrase-profile score should be normalized");
    require(result.report.sameTimeCollisions == 0, "NK2 support notes should pass collision gate");
    require(result.report.longNoteConflicts == 0, "NK2 support notes should pass LN conflict gate");
    require(result.report.createdJacks == 0, "NK2 support notes should pass created-jack gate");

    for (const auto& note : result.chart.notes) {
        if (note.id == "n0") {
            require(note.type == keyconv::NoteType::Hold && note.endTime == 1600,
                    "NK2 support generation must preserve first LN duration");
        }
        if (note.id == "n3") {
            require(note.type == keyconv::NoteType::Hold && note.endTime == 2600,
                    "NK2 support generation must preserve second LN duration");
        }
        if (note.id.find("nk2-") == 0) {
            require(note.type == keyconv::NoteType::Tap, "NK2 support notes should be taps in milestone 3");
            require(!note.endTime.has_value(), "NK2 support notes should not introduce new LN durations");
        }
    }
    const bool hasLnProvenanceId = std::any_of(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id.find("nk2-ln-ln-") == 0;
    });
    const bool hasBeatProvenanceId = std::any_of(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id.find("nk2-beat-") == 0 && note.id.find("nk2-beat--") != 0;
    });
    const bool hasMirrorProvenanceId = std::any_of(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id.find("nk2-mirror-") == 0 && note.id.find("nk2-mirror--") != 0;
    });
    require(result.report.lnSupportAccepted == 0 || hasLnProvenanceId,
            "NK2 LN support IDs should include LN provenance");
    require(result.report.strongBeatSupportAccepted == 0 || hasBeatProvenanceId,
            "NK2 beat support IDs should include motif provenance");
    require(result.report.mirrorSupportAccepted == 0 || hasMirrorProvenanceId,
            "NK2 mirror support IDs should include motif provenance");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"support\"") != std::string::npos, "NK2 JSON should include support counters");
    require(json.find("\"lnAccepted\"") != std::string::npos, "NK2 JSON should include LN accepted count");
    require(json.find("\"strongBeatAccepted\"") != std::string::npos,
            "NK2 JSON should include strong-beat accepted count");
    require(json.find("\"mirrorAccepted\"") != std::string::npos,
            "NK2 JSON should include mirror accepted count");
    require(json.find("\"generatedProvenance\"") != std::string::npos,
            "NK2 JSON should include generated provenance counters");
    require(json.find("\"phraseProfile\"") != std::string::npos,
            "NK2 JSON should include phrase-profile diagnostics");
}

void testNk2SevenToTenMirrorSupportUsesOppositePanel() {
    auto chart = makeChart(7, {
                                  {1000, 0, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Native;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.mirrorSupportCandidates > 0,
            "NK2 mirror support should emit independent candidates");
    require(result.report.mirrorSupportAccepted > 0,
            "NK2 mirror support should accept a safe opposite-hand note");
    require(result.report.addedNotes == result.report.mirrorSupportAccepted,
            "single-note mirror fixture should spend its support budget on mirror support");
    require(result.report.sameTimeCollisions == 0,
            "NK2 mirror support should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 mirror support should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 mirror support should keep created-jack gate intact");

    const auto mirror = std::find_if(result.chart.notes.begin(), result.chart.notes.end(), [](const auto& note) {
        return note.id.find("nk2-mirror-") == 0;
    });
    require(mirror != result.chart.notes.end(), "NK2 mirror support should write a mirror note ID");
    require(mirror->lane >= 5 && mirror->lane < 10,
            "left-source mirror support should land in the right 5K panel");
}

void testNk2SevenToTenPhraseBudgetLimitsClusteredSupport() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 14; ++i) {
        notes.push_back({1000 + (i % 2) * 500, i % 7, keyconv::NoteType::Tap, std::nullopt});
    }
    for (int i = 0; i < 36; ++i) {
        notes.push_back({2500 + i * 500, i % 7, keyconv::NoteType::Tap, std::nullopt});
    }
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Native;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.addedNotes > 0,
            "NK2 phrase-budget fixture should still accept some support notes");
    require(result.report.supportPhraseWindows > 1,
            "NK2 phrase-budget fixture should span multiple phrase windows");
    require(result.report.supportRejectedByPhraseBudget > 0,
            "NK2 phrase budget should reject clustered support candidates");
    require(result.report.phraseProfileOverBudgetWindows == 0,
            "NK2 phrase profile should confirm accepted support stayed under local caps");
    require(result.report.supportRejectedByBudget >= result.report.supportRejectedByPhraseBudget,
            "NK2 phrase-budget rejects should also be counted as budget rejects");
    require(result.report.sameTimeCollisions == 0,
            "NK2 phrase budget should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 phrase budget should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 phrase budget should keep created-jack gate intact");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"rejectedByPhraseBudget\"") != std::string::npos,
            "NK2 JSON should include phrase-budget rejections");
    require(json.find("\"phraseWindows\"") != std::string::npos,
            "NK2 JSON should include phrase-budget windows");
}

void testNk2SevenToTenLnAnchorsUseFreerField() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 20; ++i) {
        const int time = 1000 + i * 900;
        notes.push_back({time, 0, keyconv::NoteType::Hold, time + 360});
    }
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    const std::set<int> uniqueLanes(lanes.begin(), lanes.end());
    const bool usesOppositePanel = std::any_of(lanes.begin(), lanes.end(), [](int lane) {
        return lane >= 5;
    });

    require(result.report.addedNotes == 0,
            "NK2 faithful LN freedom fixture should not add support notes");
    require(uniqueLanes.size() >= 5,
            "NK2 LN anchors should use a freer target field: " + distributionText(result.report.laneDistribution));
    require(usesOppositePanel,
            "NK2 repeated side-lane LN anchors should be able to reach the opposite panel");
    require(result.report.sameTimeCollisions == 0,
            "NK2 freer LN placement should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 freer LN placement should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 freer LN placement should keep created-jack gate intact");
    for (const auto& note : result.chart.notes) {
        require(note.type == keyconv::NoteType::Hold && note.endTime.has_value(),
                "NK2 freer LN placement should keep source LN type");
        require(*note.endTime - note.time == 360,
                "NK2 freer LN placement must preserve LN duration exactly");
    }
}

void testNk2SevenToTenOriginalTapsUseFreerField() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 24; ++i) {
        notes.push_back({1000 + i * 900, 0, keyconv::NoteType::Tap, std::nullopt});
    }
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto lanes = lanesOf(result.chart);
    const std::set<int> uniqueLanes(lanes.begin(), lanes.end());
    const bool usesOppositePanel = std::any_of(lanes.begin(), lanes.end(), [](int lane) {
        return lane >= 5;
    });

    require(result.report.addedNotes == 0,
            "NK2 faithful tap freedom fixture should not add support notes");
    require(uniqueLanes.size() >= 6,
            "NK2 original taps should use a freer target field: " + distributionText(result.report.laneDistribution));
    require(usesOppositePanel,
            "NK2 repeated side-lane taps should be able to reach the opposite panel");
    require(result.report.sourceAnchorMatches < result.report.sourceAnchorTotal / 2,
            "NK2 original tap freedom should reduce direct source-anchor locking");
    require(result.report.sameTimeCollisions == 0,
            "NK2 freer tap placement should keep collision gate intact");
    require(result.report.longNoteConflicts == 0,
            "NK2 freer tap placement should keep LN gate intact");
    require(result.report.createdJacks == 0,
            "NK2 freer tap placement should keep created-jack gate intact");
}

void testNk2SevenToTenCoveragePressureFillsBridgeGaps() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 24; ++i) {
        notes.push_back({1000 + i * 600, 3, keyconv::NoteType::Tap, std::nullopt});
    }
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto& distribution = result.report.laneDistribution;
    require(distribution.size() == 10, "NK2 coverage fixture should report 10K distribution");
    require(distribution[4] > 0,
            "NK2 coverage pressure should use the low bridge target lane: " + distributionText(distribution));
    require(distribution[5] < static_cast<int>(chart.notes.size()),
            "NK2 coverage pressure should not pin every center source note to the direct lane");
    require(result.report.createdJacks == 0, "NK2 coverage pressure must not create target jacks");
    require(result.report.layoutCoverageScore > 0.0, "NK2 report should score layout coverage");
    require(result.report.candidateScoredNotes == static_cast<int>(chart.notes.size()),
            "NK2 single-slice candidate scoring should cover every source note");
    require(result.report.candidateScoredLanes >= result.report.candidateScoredNotes,
            "NK2 candidate scoring should count evaluated lane candidates");
    require(result.report.candidateScoreSamples == result.report.candidateScoredNotes,
            "NK2 accepted candidate score samples should match selected single-slice notes");
    require(result.report.candidateAcceptedFirstChoice + result.report.candidateAcceptedAfterHardGate +
                    result.report.candidateDirectFallbacks ==
                result.report.candidateScoredNotes,
            "NK2 candidate acceptance counters should account for scored notes");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"layoutScores\"") != std::string::npos, "NK2 JSON should include layout scores");
    require(json.find("\"coverage\"") != std::string::npos, "NK2 JSON should include coverage score");
    require(json.find("\"candidateRanking\"") != std::string::npos,
            "NK2 JSON should include candidate ranking diagnostics");
    require(json.find("\"acceptedScoreAverages\"") != std::string::npos,
            "NK2 JSON should include accepted score dimension averages");
}

void testNk2SevenToTenPanelCoverageFillsRightPanelGaps() {
    std::vector<std::tuple<int, int, keyconv::NoteType, std::optional<int>>> notes;
    for (int i = 0; i < 30; ++i) {
        notes.push_back({1000 + i * 600, 6, keyconv::NoteType::Tap, std::nullopt});
    }
    auto chart = makeChart(7, notes);
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    const auto& distribution = result.report.laneDistribution;
    require(distribution.size() == 10, "NK2 right-panel fixture should report 10K distribution");
    require(distribution[8] > 0,
            "NK2 panel pressure should use the under-filled right-panel target lane: " +
                distributionText(distribution));
    require(distribution[9] < static_cast<int>(chart.notes.size()),
            "NK2 panel pressure should not pin every far-right source note to the direct lane");
    require(result.report.createdJacks == 0, "NK2 right-panel coverage must not create target jacks");
    require(result.report.rightPanelScore > 0.0, "NK2 report should score right-panel spread");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"rightPanel\"") != std::string::npos, "NK2 JSON should include right-panel score");
}

void testNk2SevenToTenMotifPoliciesAreSeparated() {
    auto chart = makeChart(7, {
                                  {1000, 3, keyconv::NoteType::Hold, 1500},
                                  {1900, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {1900, 6, keyconv::NoteType::Tap, std::nullopt},
                                  {2500, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {2620, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {3200, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {3320, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {3440, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {3560, 4, keyconv::NoteType::Tap, std::nullopt},
                                  {4200, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {4320, 1, keyconv::NoteType::Tap, std::nullopt},
                                  {4440, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {4560, 3, keyconv::NoteType::Tap, std::nullopt},
                                  {5200, 0, keyconv::NoteType::Tap, std::nullopt},
                                  {5320, 2, keyconv::NoteType::Tap, std::nullopt},
                                  {5440, 5, keyconv::NoteType::Tap, std::nullopt},
                                  {5560, 3, keyconv::NoteType::Tap, std::nullopt},
                              });
    addTimingPoint(chart);

    keyconv::nk2::NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = 10;
    options.mode = keyconv::nk2::Mode::Faithful;

    const auto result = keyconv::nk2::convertChart(chart, options);
    require(result.report.motifLnPlacements > 0, "NK2 motif policy should count LN anchors");
    require(result.report.motifChordPlacements > 0, "NK2 motif policy should count chord placements");
    require(result.report.motifJackPlacements > 0, "NK2 motif policy should count source jacks");
    require(result.report.motifTrillPlacements > 0, "NK2 motif policy should count trills");
    require(result.report.motifStairPlacements > 0, "NK2 motif policy should count stairs");
    require(result.report.motifStreamPlacements > 0, "NK2 motif policy should count streams");
    require(result.report.sameTimeCollisions == 0, "NK2 motif policies should keep collision gate intact");
    require(result.report.longNoteConflicts == 0, "NK2 motif policies should keep LN gate intact");
    require(result.report.createdJacks == 0, "NK2 motif policies should keep created-jack gate intact");

    const auto lanes = lanesOf(result.chart);
    require(lanes[5] != lanes[6] && lanes[6] != lanes[7] && lanes[7] != lanes[8],
            "NK2 trill policy should avoid collapsing alternating notes to one lane");
    require(lanes[9] < lanes[10] && lanes[10] < lanes[11] && lanes[11] < lanes[12],
            "NK2 stair policy should preserve ascending direction");

    const auto json = keyconv::nk2::reportToJson(result.report);
    require(json.find("\"motifPlacements\"") != std::string::npos,
            "NK2 JSON should include motif placement counters");
    require(json.find("\"trill\"") != std::string::npos, "NK2 JSON should include trill counter");
    require(json.find("\"stair\"") != std::string::npos, "NK2 JSON should include stair counter");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"parser", testParser},
        {"invalid hit object warning", testInvalidHitObjectWarning},
        {"mapping", testMapping},
        {"exporter roundtrip", testExporterRoundtrip},
        {"BMS parser exporter roundtrip", testBmsParserExporterRoundtrip},
        {"BMS exporter key mode headers", testBmsExporterKeyModeHeaders},
        {"convert", testConvert},
        {"compression keeps holds", testCompressionKeepsHolds},
        {"collision policies", testCollisionPolicies},
        {"PPG pattern detection", testPpgPatternDetection},
        {"PPG stair preserved", testPpgStairPreserved},
        {"PPG trill preserved", testPpgTrillPreserved},
        {"gesture rail 7K ascending stair left zone", testGestureRailSevenKeyAscendingStairLeftZone},
        {"gesture rail 7K descending stair left zone", testGestureRailSevenKeyDescendingStairLeftZone},
        {"gesture rail 7K ascending stair right 5K panel",
         testGestureRailSevenKeyAscendingStairRightFiveKeyPanel},
        {"gesture rail 7K trill shape", testGestureRailSevenKeyTrillShape},
        {"gesture rail source jack identity", testGestureRailSourceJackIdentity},
        {"7K to 10K source anchors beat left-edge balance",
         testSevenToTenSourceAnchorsTakePriorityOverLeftEdgeBalance},
        {"7K to 10K sparse source-lane anchor", testSevenToTenSparseSourceLaneAnchor},
        {"10K non-jack source-lane anchor relaxes inside panel",
         testTenKeyNonJackSourceLaneAnchorRelaxesInsidePanel},
        {"7K to 10K staged planner splits center bridge",
         testSevenToTenStagedPlannerSplitsCenterBridge},
        {"7K to 10K mirror-compress planner uses 14-lane pairs",
         testSevenToTenMirrorCompressPlannerUsesFourteenLanePairs},
        {"7K to 10K mirror-compress planner fills panel centers",
         testSevenToTenMirrorCompressPlannerFillsPanelCenters},
        {"10K full-field remix rotates echo within opposite zone",
         testTenKeyFullFieldRemixRotatesEchoWithinOppositeZone},
        {"10K full-field rail covers all notes",
         testTenKeyFullFieldRailCoversAllNotesWithoutStructChanges},
        {"10K full-field stream alternates hands inside long token",
         testTenKeyFullFieldStreamAlternatesHandsInsideLongToken},
        {"10K full-field long stair alternates hands inside token",
         testTenKeyFullFieldLongStairAlternatesHandsInsideToken},
        {"10K full-field remix density and safety",
         testTenKeyFullFieldRemixDensityAndSafety},
        {"8K playable candidate radius remains stable", testEightKeyPlayableCandidateRadiusRemainsStable},
        {"7K to 10K non-gesture chords stay in source panels",
         testSevenToTenNonGestureChordsStayInSourcePanels},
        {"7K to 10K tap-plus hand-zone balance", testSevenToTenTapPlusHandZoneBalance},
        {"Target-K likeness report 7K to 10K", testTargetKLikenessReportSevenToTen},
        {"Target-K likeness uses reference profile", testTargetKLikenessUsesReferenceProfile},
        {"adaptive growth budget reports profile windows", testAdaptiveGrowthBudgetReportsProfileWindows},
        {"adaptive growth budget uses density buckets", testAdaptiveGrowthBudgetUsesDensityBuckets},
        {"adaptive growth budget allows sparse profiled fill", testAdaptiveGrowthBudgetAllowsSparseProfiledFill},
        {"preserve tap plus can use second slice fill slot", testPreserveTapPlusCanUseSecondSliceFillSlot},
        {"PPG chord does not collapse", testPpgChordDoesNotCollapse},
        {"PPG LN avoids tap", testPpgLnAvoidsTap},
        {"PPG playable reduces jack and faithful preserves more", testPpgPlayableReducesJackAndFaithfulPreservesMore},
        {"PPG avoids created jack from moved pattern", testPpgAvoidsCreatedJackFromMovedPattern},
        {"different source lanes do not fold into jack", testDifferentSourceLanesDoNotFoldIntoJack},
        {"assignment avoids created jack with adaptive radius", testAssignmentAvoidsCreatedJackWithAdaptiveRadius},
        {"repair does not create jack", testRepairDoesNotCreateJack},
        {"Converter facade reserved options", testConverterFacadeReservedOptions},
        {"training style parses and converts", testTrainingStyleParsesAndConverts},
        {"sameTimeEpsilonMs groups slices", testSameTimeEpsilonGroupsSlices},
        {"compress drop five-note chord", testCompressDropFiveNoteChord},
        {"compress drop six-note chord", testCompressDropSixNoteChord},
        {"compress hybrid drops tap overflow", testCompressHybridDropsTapOverflow},
        {"compress hybrid rolls overflow hold", testCompressHybridRollsOverflowHold},
        {"compress auto drops overflow hold for low-key recreation",
         testCompressAutoDropsOverflowHoldForLowKeyRecreation},
        {"compress active LN drops tap", testCompressActiveLnDropsTap},
        {"compress preserve strict reports impossible", testCompressPreserveStrictReportsImpossible},
        {"compress hybrid no-overlap synthetic", testCompressHybridNoOverlapSynthetic},
        {"compress planner avoids jackified fold", testCompressPlannerAvoidsJackifiedFold},
        {"compress auto drops when only created-jack lane fits",
         testCompressAutoDropsWhenOnlyCreatedJackLaneFits},
        {"distance guard rejects eight-ms roll", testDistanceGuardRejectsEightMsRoll},
        {"same-lane gap guard rejects twelve-ms roll", testSameLaneGapGuardRejectsTwelveMsRoll},
        {"snap roll enabled has no unsnapped rolled notes", testSnapRollEnabledNoUnsnappedRolledNotes},
        {"expansion same input same output", testExpansionSameInputSameOutput},
        {"chord fill deterministic lane", testChordFillDeterministicLane},
        {"training scaffold deterministic lane", testTrainingScaffoldDeterministicLane},
        {"expanded output collapses source near-time pair", testExpandedOutputCollapsesSourceNearTimePair},
        {"preserve tap plus includes holds budget and adds only taps", testPreserveTapPlusIncludesHoldsBudgetAndAddsOnlyTaps},
        {"preserve tap plus low caps high-key growth", testPreserveTapPlusLowCapsHighKeyGrowth},
        {"preserve tap plus adds holds in LN-heavy window", testPreserveTapPlusAddsHoldsInLnHeavyWindow},
        {"preserve tap plus does not turn tap-only slice into hold", testPreserveTapPlusDoesNotTurnTapOnlySliceIntoHold},
        {"generated short hold clone ignores adjacent long hold length",
         testGeneratedShortHoldCloneIgnoresAdjacentLongHoldLength},
        {"generated long hold clone is tapified", testGeneratedLongHoldCloneIsTapified},
        {"no-jack input does not create jack", testNoJackInputDoesNotCreateJack},
        {"source jack preserved faithful", testSourceJackPreservedFaithful},
        {"source jack playable split", testSourceJackPlayableSplit},
        {"8K loose repeat does not become jack", testEightKeyLooseRepeatDoesNotBecomeJack},
        {"8K source jack still preserved", testEightKeySourceJackStillPreserved},
        {"500ms jack window detects slow source jack", testFiveHundredMsJackWindowDetectsSlowSourceJack},
        {"added note rejected if creates unwanted jack", testAddedNoteRejectedIfCreatesUnwantedJack},
        {"added note from source jack still rejected if creates target jack",
         testAddedNoteFromSourceJackStillRejectedIfCreatesTargetJack},
        {"harder does not create random jack", testHarderDoesNotCreateRandomJack},
        {"deterministic jack tie-break", testDeterministicJackTieBreak},
        {"jack report stable", testJackReportStable},
        {"harder remix deterministic", testHarderRemixDeterministic},
        {"expansion budget respected", testExpansionBudgetRespected},
        {"expansion composer preserve budget", testExpansionComposerPreserveBudget},
        {"expansion composer profile budget report", testExpansionComposerProfileBudgetReport},
        {"harder composer profile and safety", testHarderComposerProfileAndSafety},
        {"stair echo deterministic and direction preserved", testStairEchoDeterministicAndDirectionPreserved},
        {"stair down echo direction preserved", testStairDownEchoDirectionPreserved},
        {"trill echo preserves AB", testTrillEchoPreservesAB},
        {"echo budget respected", testEchoBudgetRespected},
        {"harder remix echo stable", testHarderRemixEchoStable},
        {"stream echo low density adds and deterministic", testStreamEchoLowDensityAddsAndDeterministic},
        {"stream echo high density rejects", testStreamEchoHighDensityRejects},
        {"stream echo burst rejects", testStreamEchoBurstRejects},
        {"stream echo budget respected", testStreamEchoBudgetRespected},
        {"stream echo does not run by default", testStreamEchoDoesNotRunByDefault},
        {"stream echo diagnostics stable", testStreamEchoDiagnosticsStable},
        {"stream primary reject sums", testStreamPrimaryRejectSums},
        {"stream echo balanced at least conservative", testStreamEchoBalancedAtLeastConservative},
        {"training stream echo profile deterministic", testTrainingStreamEchoProfileDeterministic},
        {"feel report values stable", testFeelReportValuesStable},
        {"policy comparison core matrix stable", testPolicyComparisonCoreMatrixStable},
        {"preserve convert lane drift moves stable phrases", testPreserveConvertLaneDriftMovesStablePhrases},
        {"preserve convert lane drift keeps source jack together",
         testPreserveConvertLaneDriftKeepsSourceJackTogether},
        {"auto-more expansion reports larger budget", testAutoMoreExpansionReportsLargerBudget},
        {"high-key tap-plus prefers eighth-beat additions", testHighKeyTapPlusPrefersEighthBeatAdditions},
        {"10K tap-plus boosts quarter-eighth density above 8K",
         testTenKeyTapPlusBoostsQuarterEighthDensityAboveEightKey},
        {"10K denim tap-plus shifts each beat", testTenKeyDenimTapPlusShiftsEachBeat},
        {"8K tap-plus reduces one-hand denim web additions",
         testEightKeyTapPlusReducesOneHandDenimWebAdditions},
        {"high-key extreme trill avoids both outer edges", testHighKeyExtremeTrillAvoidsBothOuterEdges},
        {"long source jack stays single lane playable", testLongSourceJackStaysSingleLanePlayable},
        {"chord-embedded long source jack stays single lane playable",
         testChordEmbeddedLongSourceJackStaysSingleLanePlayable},
        {"even-key fast 32nd stair suppresses additions", testEvenKeyFastThirtySecondStairSuppressesAdditions},
        {"8K fast 6-lane stair keeps symmetric vacancies",
         testEightKeyFastSixLaneStairKeepsSymmetricVacancies},
        {"even-key left-only additions stay left hand", testEvenKeyLeftOnlyAdditionsStayLeftHand},
        {"stream superrandom relanes every note", testStreamSuperRandomRelanesEveryNote},
        {"stream superrandom keeps chord distinct", testStreamSuperRandomKeepsChordDistinct},
        {"full jitter offsets same-time chords", testFullJitterOffsetsSameTimeChords},
        {"stream super-symmetry policy parses", testStreamSuperSymmetryPolicyParses},
        {"difficulty name marks expansion and stream transform",
         testDifficultyNameMarksExpansionAndStreamTransform},
        {"converted chart marker guard", testConvertedChartMarkerGuard},
        {"NK2 report-only intent graph", testNk2ReportOnlyIntentGraph},
        {"NK2 same-key no-op unless transform", testNk2SameKeyNoOpUnlessTransform},
        {"NK2 4K to 5K generic prototype conversion",
         testNk2FourToFiveGenericPrototypeConversion},
        {"NK2 4K to 5K generic uses whole-field spread",
         testNk2FourToFiveGenericUsesWholeFieldSpread},
        {"NK2 7K to 8K generic prototype conversion",
         testNk2SevenToEightGenericPrototypeConversion},
        {"NK2 7K to 8K generic uses whole-field spread",
         testNk2SevenToEightGenericUsesWholeFieldSpread},
        {"NK2 7K to 8K long LN copies adjacent placement",
         testNk2SevenToEightLongLnCopiesAdjacentPlacement},
        {"NK2 local solver handles same-time chord", testNk2LocalSolverHandlesSameTimeChord},
        {"NK2 7K to 4K generic prototype compression",
         testNk2SevenToFourGenericPrototypeCompression},
        {"NK2 7K to 8K native support notes are generalized",
         testNk2SevenToEightNativeSupportNotesAreGeneralized},
        {"NK2 LN-adjacent support adds head and tail taps",
         testNk2LnAdjacentSupportAddsHeadAndTailTaps},
        {"NK2 super-symmetry preserves same-time mirrors",
         testNk2SuperSymmetryPreservesSameTimeMirrors},
        {"NK2 super-symmetry preserves gapless stair",
         testNk2SuperSymmetryPreservesGaplessStair},
        {"NK2 7K to 10K prototype conversion", testNk2SevenToTenPrototypeConversion},
        {"NK2 7K to 10K preserves source jack", testNk2SevenToTenPreservesSourceJack},
        {"NK2 7K to 10K support notes are gated", testNk2SevenToTenSupportNotesAreGated},
        {"NK2 7K to 10K mirror support uses opposite panel",
         testNk2SevenToTenMirrorSupportUsesOppositePanel},
        {"NK2 7K to 10K phrase budget limits clustered support",
         testNk2SevenToTenPhraseBudgetLimitsClusteredSupport},
        {"NK2 7K to 10K LN anchors use freer field",
         testNk2SevenToTenLnAnchorsUseFreerField},
        {"NK2 7K to 10K original taps use freer field",
         testNk2SevenToTenOriginalTapsUseFreerField},
        {"NK2 7K to 10K coverage pressure fills bridge gaps",
         testNk2SevenToTenCoveragePressureFillsBridgeGaps},
        {"NK2 7K to 10K panel coverage fills right-panel gaps",
         testNk2SevenToTenPanelCoverageFillsRightPanelGaps},
        {"NK2 7K to 10K motif policies are separated",
         testNk2SevenToTenMotifPoliciesAreSeparated},
    };

    try {
        for (const auto& test : tests) {
            test.second();
            std::cout << "[pass] " << test.first << "\n";
        }
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << tests.size() << " tests passed\n";
    return EXIT_SUCCESS;
}
