#include "nk2/nk2_convert.hpp"
#include "nk2/nk2_report.hpp"
#include "nk2/safety_audit.hpp"
#include "exporter/osu.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using keyconv::Chart;
using keyconv::Note;
using keyconv::NoteType;
using keyconv::nk2::Mode;
using keyconv::nk2::NK2ConversionResult;
using keyconv::nk2::NK2Options;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Note tap(const std::string& id, int time, int lane) {
    Note note;
    note.id = id;
    note.time = time;
    note.lane = lane;
    return note;
}

Note hold(const std::string& id, int time, int lane, int end) {
    auto note = tap(id, time, lane);
    note.type = NoteType::Hold;
    note.endTime = end;
    return note;
}

Chart chart(std::initializer_list<Note> notes = {}) {
    Chart result;
    result.meta.sourceKeyCount = 7;
    result.meta.version = "Native test";
    result.notes = notes;
    keyconv::TimingPoint timing;
    timing.time = 0;
    timing.beatLength = 500.0;
    timing.uninherited = true;
    result.timingPoints.push_back(timing);
    return result;
}

NK2Options faithfulOptions(int target) {
    NK2Options options;
    options.sourceKeyCount = 7;
    options.targetKeyCount = target;
    options.mode = Mode::Faithful;
    return options;
}

Chart mixedFixture() {
    return chart({hold("n0", 1000, 0, 2600), tap("n1", 1000, 2),
                  tap("n2", 1125, 1), tap("n3", 1250, 3),
                  hold("n4", 1500, 4, 2150), tap("n5", 1625, 2),
                  tap("n6", 1800, 6), tap("n7", 2200, 5),
                  tap("n8", 2700, 0), tap("n9", 2825, 0)});
}

void checkSearchContract(const Chart& source, const NK2ConversionResult& result, int target) {
    require(result.report.independentSafety.has_value(),
            "phrase search must automatically include its independent safety audit");
    require(result.report.independentSafety->passed(),
            "phrase output must pass independent lane-by-lane safety inspection");
    require(result.report.outputNotes == static_cast<int>(result.chart.notes.size()) &&
                result.report.outputNotes + result.report.droppedNotes == static_cast<int>(source.notes.size()),
            "output plus explicit source drops must account for every source object");
    require(result.report.addedNotes == 0 && result.report.lowerKeyRolledNotes == 0 &&
                result.report.independentSafety->addedNotes == 0 &&
                result.report.independentSafety->retimedSourceNotes == 0 &&
                result.report.independentSafety->changedHoldDurations == 0,
            "this search mode must not generate notes, retime source objects or alter LN duration");
    require(result.report.independentSafety->droppedSourceNotes == result.report.droppedNotes,
            "independent source accounting must agree with the search report");
    require(result.report.phraseSearchPeakNotes <= 48,
            "search work must remain bounded to at most 48 source objects per window");
    require(result.report.prototypeName == "nk2-phrase-search-v1",
            "the selected search path must be identifiable in its report");
    require(result.chart.meta.version->find("-phrase") != std::string::npos,
            "search output difficulty must be distinguishable from the baseline");

    std::map<std::string, const Note*> originals;
    for (const auto& note : source.notes) {
        originals.emplace(note.id, &note);
    }
    std::set<std::string> retained;
    for (const auto& note : result.chart.notes) {
        const auto original = originals.find(note.id);
        require(original != originals.end() && retained.insert(note.id).second,
                "every search object must identify one unique source object");
        require(note.time == original->second->time && note.type == original->second->type &&
                    note.endTime == original->second->endTime,
                "source timing, type and LN endpoints must survive exactly");
        require(note.lane >= 0 && note.lane < target, "search lane must stay inside the target layout");
    }
}

void defaultAndExplicitOffRemainIdentical() {
    const auto source = mixedFixture();
    const auto sourceBefore = keyconv::exportOsu(source, 7);
    for (const int target : {4, 5, 8, 10}) {
        const auto defaults = faithfulOptions(target);
        auto explicitOff = defaults;
        explicitOff.phraseSearch = false;
        explicitOff.phraseSupport = false;
        explicitOff.phraseDenseLn = false;
        explicitOff.phraseSourceLn = false;
        explicitOff.phraseSourceLnBoost = false;
        explicitOff.independentSafetyAudit = false;
        const auto baseline = keyconv::nk2::convertChart(source, defaults);
        const auto disabled = keyconv::nk2::convertChart(source, explicitOff);
        require(keyconv::exportOsu(baseline.chart, target) == keyconv::exportOsu(disabled.chart, target),
                "explicitly disabling optional search must preserve default chart bytes");
        require(keyconv::nk2::reportToJson(baseline.report) == keyconv::nk2::reportToJson(disabled.report),
                "default and explicit-off reports must agree");
        require(!baseline.report.independentSafety.has_value() && baseline.report.phraseSearchWindows == 0,
                "default conversion must not silently enable audit or search");

        auto auditOnly = defaults;
        auditOnly.independentSafetyAudit = true;
        const auto audited = keyconv::nk2::convertChart(source, auditOnly);
        require(audited.report.independentSafety.has_value() && audited.report.phraseSearchWindows == 0,
                "audit-only mode must inspect the baseline without enabling search");
        require(keyconv::exportOsu(baseline.chart, target) == keyconv::exportOsu(audited.chart, target),
                "adding an independent audit must not mutate baseline output bytes");
    }
    require(keyconv::exportOsu(source, 7) == sourceBefore,
            "neither optional inspection nor conversion may modify the input chart");
}

void futureHoldAndPatternFixturesStaySafe() {
    auto source = mixedFixture();
    // Source container order should not define the chronological search path.
    std::reverse(source.notes.begin(), source.notes.end());
    for (const int target : {4, 5, 8, 10}) {
        auto options = faithfulOptions(target);
        options.phraseSearch = true;
        const auto result = keyconv::nk2::convertChart(source, options);
        checkSearchContract(source, result, target);
        require(result.report.phraseSearchWindows >= 2 && result.report.phraseSearchCandidates > 0,
                "mixed patterns must exercise multiple committed search windows");
        require(!result.chart.notes.empty(), "a valid mixed phrase must retain playable source material");
        if (target == 10) {
            require(result.report.droppedNotes == 0, "the spacious mixed 10K fixture should retain every source note");
        }
    }
}

void searchIsDeterministic() {
    const auto source = mixedFixture();
    for (const int target : {4, 5, 8, 10}) {
        auto options = faithfulOptions(target);
        options.phraseSearch = true;
        const auto first = keyconv::nk2::convertChart(source, options);
        const auto second = keyconv::nk2::convertChart(source, options);
        require(keyconv::exportOsu(first.chart, target) == keyconv::exportOsu(second.chart, target),
                "repeated phrase search must produce identical exported chart bytes");
        require(keyconv::nk2::reportToJson(first.report) == keyconv::nk2::reportToJson(second.report),
                "deterministic search must also preserve its counters and report");
    }
}

void shortLookaheadRetainsNoteLostByBaseline() {
    // Four source lanes fit four targets, but a locally chosen second placement
    // of source lane 3 can consume the target needed by the final source lane 6.
    const auto source = chart({tap("n11", 2824, 5), tap("n12", 2952, 3),
                               tap("n13", 3032, 1), tap("n14", 3064, 3),
                               tap("n15", 3096, 6)});
    auto options = faithfulOptions(4);
    const auto baseline = keyconv::nk2::convertChart(source, options);
    require(keyconv::nk2::auditSafety(source, baseline.chart, 4).passed(),
            "retention comparison requires a baseline that is independently safe");
    options.phraseSearch = true;
    const auto searched = keyconv::nk2::convertChart(source, options);
    checkSearchContract(source, searched, 4);
    require(baseline.report.droppedNotes == 1 && searched.report.droppedNotes == 0 &&
                searched.chart.notes.size() == source.notes.size(),
            "bounded lookahead must retain all five notes in this reproduced four-lane fixture");
    require(searched.report.phraseSearchWindows == 1,
            "the retained final note must be decided within the same short search window");
}

void unsupportedConfigurationsFailExplicitly() {
    const auto source = mixedFixture();
    std::vector<NK2Options> unsupported;
    auto options = faithfulOptions(4);
    options.phraseSearch = true;
    options.sourceKeyCount = 6;
    unsupported.push_back(options);
    for (const int target : {3, 6, 7, 9, 12}) {
        options = faithfulOptions(target);
        options.phraseSearch = true;
        unsupported.push_back(options);
    }
    for (const auto mode : {Mode::Native, Mode::Harder, Mode::Transform, Mode::Report}) {
        options = faithfulOptions(10);
        options.phraseSearch = true;
        options.mode = mode;
        unsupported.push_back(options);
    }
    options = faithfulOptions(10);
    options.phraseSearch = true;
    options.superSymmetry = true;
    unsupported.push_back(options);
    for (const auto& invalid : unsupported) {
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, invalid);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("phrase search requires") != std::string::npos;
        }
        require(rejected, "an unsupported search combination must fail rather than quietly change engines");
    }
}

void denseWindowsRemainBoundedAcrossChordBoundaries() {
    auto source = chart();
    for (int slice = 0; slice < 12; ++slice) {
        for (int lane = 0; lane < 7; ++lane) {
            source.notes.push_back(tap("dense-" + std::to_string(slice * 7 + lane), 1000 + slice * 16, lane));
        }
    }
    for (const int target : {4, 5, 8, 10}) {
        auto options = faithfulOptions(target);
        options.phraseSearch = true;
        const auto result = keyconv::nk2::convertChart(source, options);
        checkSearchContract(source, result, target);
        require(result.report.phraseSearchWindows >= 2 && result.report.phraseSearchPeakNotes <= 48,
                "dense chords must split bounded windows without exceeding the object budget");
        if (target == 10) {
            require(result.report.droppedNotes == 0, "a repeated 7K chord must fit safely inside 10K");
        }
    }
}

void sparseWindowsAdvanceAndPreserveSources() {
    auto source = chart();
    for (int index = 0; index < 10; ++index) {
        source.notes.push_back(tap("sparse-" + std::to_string(index), 1000 + index * 2100, index % 7));
    }
    for (const int target : {4, 5, 8, 10}) {
        auto options = faithfulOptions(target);
        options.phraseSearch = true;
        const auto result = keyconv::nk2::convertChart(source, options);
        checkSearchContract(source, result, target);
        require(result.report.phraseSearchWindows == 10 && result.report.phraseSearchPeakNotes == 1 &&
                    result.report.droppedNotes == 0,
                "sparse material must advance through separate windows without dropping available notes");
    }
}

void emptyInputHasNoSearchWork() {
    const auto source = chart();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    const auto result = keyconv::nk2::convertChart(source, options);
    checkSearchContract(source, result, 10);
    require(result.chart.notes.empty() && result.report.phraseSearchWindows == 0 &&
                result.report.phraseSearchCandidates == 0,
            "empty input must not fabricate candidates or windows");
}

void oversizedSliceFailsWithoutUnboundedSearch() {
    auto source = chart();
    for (int index = 0; index < 49; ++index) {
        source.notes.push_back(tap("oversized-" + std::to_string(index), 1000, index % 7));
    }
    auto options = faithfulOptions(4);
    options.phraseSearch = true;
    bool rejected = false;
    try {
        (void)keyconv::nk2::convertChart(source, options);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("48 notes") != std::string::npos;
    }
    require(rejected, "an oversized malformed simultaneous slice must fail within the search bound");
}

Chart phraseSupportFixture() {
    auto source = chart();
    for (int index = 0; index < 32; ++index) {
        const int time = 1000 + index * 500;
        const auto id = "support-source-" + std::to_string(index);
        if (index == 0 || index == 14) {
            source.notes.push_back(hold(id, time, index % 7, time + 1250));
        } else {
            source.notes.push_back(tap(id, time, index % 7));
        }
    }
    return source;
}

void checkPhraseSupportContract(const Chart& source,
                                const NK2ConversionResult& baseline,
                                const NK2ConversionResult& augmented) {
    require(augmented.report.independentSafety.has_value() &&
                augmented.report.independentSafety->passed(),
            "optional phrase support must automatically pass independent safety inspection");
    require(augmented.report.prototypeName == "nk2-phrase-notes-v1",
            "support-enabled phrase output must identify its separate prototype");
    require(augmented.report.independentSafety->retimedSourceNotes == 0 &&
                augmented.report.independentSafety->changedHoldDurations == 0 &&
                augmented.report.lowerKeyRolledNotes == 0,
            "adding support must never retime originals, alter holds or invoke roll rescue");
    require(augmented.report.droppedNotes == baseline.report.droppedNotes &&
                augmented.report.independentSafety->retainedSourceNotes ==
                    baseline.report.independentSafety->retainedSourceNotes,
            "support generation must retain exactly the same source objects as phrase search alone");

    std::map<std::string, const Note*> baselineNotes;
    for (const auto& note : baseline.chart.notes) {
        baselineNotes.emplace(note.id, &note);
    }
    std::set<std::string> retained;
    std::map<int, int> additionsByWindow;
    std::map<int, int> additionsByTimestamp;
    int added = 0;
    for (const auto& note : augmented.chart.notes) {
        const auto original = baselineNotes.find(note.id);
        if (original != baselineNotes.end()) {
            require(retained.insert(note.id).second,
                    "support generation must not duplicate a baseline note");
            const auto& before = *original->second;
            require(note.time == before.time && note.lane == before.lane &&
                        note.type == before.type && note.endTime == before.endTime &&
                        note.sourceLane == before.sourceLane,
                    "every baseline object must keep its time, lane, type, endpoint and provenance");
            continue;
        }
        ++added;
        require(note.type == NoteType::Tap && !note.endTime.has_value(),
                "all optional support additions must be taps");
        require(note.id.starts_with("nk2-ln-") || note.id.starts_with("nk2-mirror-") ||
                    note.id.starts_with("nk2-beat-"),
                "support additions must have explicit generated-note provenance");
        require(note.lane >= 0 && note.lane < augmented.report.options.targetKeyCount,
                "support tap must stay inside the selected field");
        ++additionsByTimestamp[note.time];
        ++additionsByWindow[note.time < 0 ? 0 : note.time / 2000];

        // Inspect all timestamps on both sides, including later original notes
        // and other additions. Sharing a source lane grants no exemption.
        for (const auto& other : augmented.chart.notes) {
            if (&note == &other || note.lane != other.lane) {
                continue;
            }
            const auto delta = static_cast<long long>(note.time) - other.time;
            const auto gap = delta < 0 ? -delta : delta;
            require(gap > 500,
                    "support must stay outside the inclusive 500ms guard in both time directions");
            require(other.type != NoteType::Hold || !other.endTime.has_value() ||
                        note.time < other.time || note.time > *other.endTime,
                    "support must not fall inside an original LN or on its tail");
        }
    }
    require(retained.size() == baselineNotes.size(),
            "the entire baseline placement must remain a subset of the augmented output");
    require(added == augmented.report.addedNotes &&
                added == augmented.report.independentSafety->addedNotes &&
                augmented.report.outputNotes == static_cast<int>(augmented.chart.notes.size()) &&
                augmented.chart.notes.size() == baseline.chart.notes.size() + added,
            "support counts must agree across output, conversion report and independent audit");
    require(added == augmented.report.lnSupportAccepted + augmented.report.mirrorSupportAccepted +
                        augmented.report.strongBeatSupportAccepted,
            "accepted support-kind counters must account for each generated tap exactly once");
    require(added == augmented.report.generatedFromJackMotif + augmented.report.generatedFromTrillMotif +
                        augmented.report.generatedFromStairMotif + augmented.report.generatedFromStreamMotif +
                        augmented.report.generatedFromChordMotif + augmented.report.generatedFromLnMotif +
                        augmented.report.generatedFromNeutralMotif,
            "generated motif provenance counters must account for every addition");
    const auto totalBudget = (source.notes.size() * 12 + 99) / 100;
    require(static_cast<std::size_t>(added) <= totalBudget,
            "support must not exceed the ceil(12 percent) total source-note budget");
    for (const auto& [window, count] : additionsByWindow) {
        (void)window;
        require(count <= 4, "a two-second window must not receive more than four support taps");
    }
    for (const auto& [time, count] : additionsByTimestamp) {
        (void)time;
        require(count <= 1, "a single timestamp must not receive multiple added taps");
    }
}

void phraseSupportOffPreservesBExactly() {
    const auto source = phraseSupportFixture();
    auto defaults = faithfulOptions(10);
    defaults.phraseSearch = true;
    require(!defaults.phraseSupport, "phrase support must remain disabled by default");
    const auto baseline = keyconv::nk2::convertChart(source, defaults);
    auto explicitOff = defaults;
    explicitOff.phraseSupport = false;
    const auto disabled = keyconv::nk2::convertChart(source, explicitOff);
    require(keyconv::exportOsu(baseline.chart, 10) == keyconv::exportOsu(disabled.chart, 10) &&
                keyconv::nk2::reportToJson(baseline.report) == keyconv::nk2::reportToJson(disabled.report),
            "explicit support-off must preserve the original B phrase output and report exactly");
    require(baseline.report.addedNotes == 0, "B phrase search alone must remain addition-free");
}

void phraseSupportAddsSafeTapsWithoutChangingB() {
    const auto source = phraseSupportFixture();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    const auto baseline = keyconv::nk2::convertChart(source, options);
    options.phraseSupport = true;
    const auto augmented = keyconv::nk2::convertChart(source, options);
    checkPhraseSupportContract(source, baseline, augmented);
    require(augmented.report.addedNotes > 0,
            "support-enabled sparse LN and beat material must exercise actual note generation");
}

void phraseSupportRespectsLocalCapUnderCandidatePressure() {
    auto source = chart({hold("budget-hold", 0, 6, 200)});
    for (int index = 0; index < 72; ++index) {
        source.notes.push_back(tap("budget-tap-" + std::to_string(index), index * 25, 0));
    }
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    const auto baseline = keyconv::nk2::convertChart(source, options);
    options.phraseSupport = true;
    const auto augmented = keyconv::nk2::convertChart(source, options);
    checkPhraseSupportContract(source, baseline, augmented);
    require(augmented.report.supportRejectedByPhraseBudget > 0,
            "the concentrated fixture must exercise a local budget rejection");
    require(augmented.report.addedNotes == 4,
            "available support candidates in one dense window must stop at four additions");
}

void phraseSupportIsDeterministic() {
    const auto source = phraseSupportFixture();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto first = keyconv::nk2::convertChart(source, options);
    const auto second = keyconv::nk2::convertChart(source, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10) &&
                keyconv::nk2::reportToJson(first.report) == keyconv::nk2::reportToJson(second.report),
            "support-enabled conversion must remain byte- and report-deterministic");
}

void unsupportedPhraseSupportConfigurationsFail() {
    const auto source = phraseSupportFixture();
    std::vector<NK2Options> unsupported;
    auto valid = faithfulOptions(10);
    valid.phraseSearch = true;
    valid.phraseSupport = true;
    auto invalid = valid;
    invalid.phraseSearch = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.sourceKeyCount = 6;
    unsupported.push_back(invalid);
    for (const int target : {4, 5, 6, 7, 9, 12}) {
        invalid = valid;
        invalid.targetKeyCount = target;
        unsupported.push_back(invalid);
    }
    for (const auto mode : {Mode::Native, Mode::Harder, Mode::Transform, Mode::Report}) {
        invalid = valid;
        invalid.mode = mode;
        unsupported.push_back(invalid);
    }
    invalid = valid;
    invalid.superSymmetry = true;
    unsupported.push_back(invalid);
    for (const auto& options : unsupported) {
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, options);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "support must reject configurations outside faithful phrase 7K to 8K/10K");
    }
}

void emptyPhraseSupportInputDoesNotFabricateNotes() {
    const auto source = chart();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    const auto baseline = keyconv::nk2::convertChart(source, options);
    options.phraseSupport = true;
    const auto augmented = keyconv::nk2::convertChart(source, options);
    checkPhraseSupportContract(source, baseline, augmented);
    require(augmented.chart.notes.empty() && augmented.report.addedNotes == 0,
            "empty source must not receive fabricated support material");
}

Chart phraseDenseLnFixture() {
    auto source = chart();
    for (int index = 0; index < 64; ++index) {
        const int time = 1000 + index * 500;
        const auto id = "dense-ln-source-" + std::to_string(index);
        source.notes.push_back(index % 8 == 0 ? hold(id, time, index % 7, time + 1250)
                                               : tap(id, time, index % 7));
    }
    return source;
}

void checkPhraseDenseLnContract(const Chart& source,
                                const NK2ConversionResult& conservative,
                                const NK2ConversionResult& dense,
                                bool boosted = false) {
    require(dense.report.independentSafety.has_value() &&
                dense.report.independentSafety->passed(),
            "dense LN support must automatically pass the independent safety audit");
    const auto& audit = *dense.report.independentSafety;
    require(audit.retimedSourceNotes == 0 && audit.changedHoldDurations == 0 &&
                dense.report.lowerKeyRolledNotes == 0,
            "dense support must preserve source timing and hold duration without roll rescue");
    require(dense.report.droppedNotes == conservative.report.droppedNotes &&
                audit.retainedSourceNotes == conservative.report.independentSafety->retainedSourceNotes,
            "dense support must retain the same original objects as C");

    std::map<std::string, const Note*> conservativeNotes;
    for (const auto& note : conservative.chart.notes) {
        require(conservativeNotes.emplace(note.id, &note).second, "C fixture IDs must be unique");
    }
    std::set<std::string> sourceIds;
    std::map<int, int> sourcesByWindow;
    for (const auto& note : source.notes) {
        sourceIds.insert(note.id);
        ++sourcesByWindow[note.time < 0 ? 0 : note.time / 2000];
    }

    std::set<std::string> denseIds;
    std::set<std::string> retainedConservative;
    std::map<int, int> additionsByWindow;
    std::map<int, int> holdsByWindow;
    std::map<int, int> additionsByTimestamp;
    int addedTaps = 0;
    int addedHolds = 0;
    int additionsBeyondC = 0;
    for (const auto& note : dense.chart.notes) {
        require(denseIds.insert(note.id).second, "dense output IDs must remain unique");
        const auto prior = conservativeNotes.find(note.id);
        if (prior != conservativeNotes.end()) {
            retainedConservative.insert(note.id);
            const auto& before = *prior->second;
            require(note.time == before.time && note.lane == before.lane &&
                        note.type == before.type && note.endTime == before.endTime &&
                        note.raw == before.raw && note.sourceLane == before.sourceLane,
                    "an overlay must retain every base note field, including generated notes and provenance");
        } else {
            ++additionsBeyondC;
            require(!sourceIds.contains(note.id), "an overlay may only append generated objects to its base");
        }
        if (sourceIds.contains(note.id)) {
            continue;
        }

        require(note.lane >= 0 && note.lane < dense.report.options.targetKeyCount,
                "dense additions must remain inside the selected field");
        const int window = note.time < 0 ? 0 : note.time / 2000;
        ++additionsByWindow[window];
        ++additionsByTimestamp[note.time];
        int noteEnd = note.time;
        if (note.type == NoteType::Hold) {
            ++addedHolds;
            ++holdsByWindow[window];
            require(note.endTime.has_value(), "every added LN must have an endpoint");
            noteEnd = *note.endTime;
            const auto duration = static_cast<long long>(noteEnd) - note.time;
            require(duration >= 200 && duration <= 2000,
                    "generated LN duration must remain in the 200ms to 2000ms range");
        } else {
            ++addedTaps;
            require(!note.endTime.has_value(), "generated taps must not carry hold endpoints");
        }

        // Full intervals detect both a candidate inside an earlier LN and a
        // future head inside the candidate LN, including either shared tail.
        for (const auto& other : dense.chart.notes) {
            if (&note == &other || note.lane != other.lane) {
                continue;
            }
            const auto delta = static_cast<long long>(note.time) - other.time;
            require((delta < 0 ? -delta : delta) > 500,
                    "every generated head must respect the 500ms guard before and after it");
            const int otherEnd = other.type == NoteType::Hold && other.endTime.has_value()
                                     ? *other.endTime : other.time;
            require(noteEnd < other.time || otherEnd < note.time,
                    "generated intervals must not touch another head, LN body or LN tail");
        }
    }
    require(retainedConservative.size() == conservativeNotes.size(),
            "the entire base chart must remain an unchanged subset of its overlay");
    require(dense.chart.notes.size() == conservative.chart.notes.size() + additionsBeyondC,
            "overlay size must equal preserved base objects plus new additions");
    const int added = addedTaps + addedHolds;
    require(addedTaps == dense.report.addedTapNotes && addedHolds == dense.report.addedHoldNotes &&
                added == dense.report.addedNotes && added == audit.addedNotes &&
                dense.report.outputNotes == static_cast<int>(dense.chart.notes.size()),
            "reported tap, hold and total additions must match actual generated object types");
    const int totalPercent = boosted ? 50 : 35;
    const int holdPercent = boosted ? 22 : 12;
    require(static_cast<std::size_t>(added) <= (source.notes.size() * totalPercent + 99) / 100,
            "total additions including the base must fit the selected source-note budget");
    require(static_cast<std::size_t>(addedHolds) <= (source.notes.size() * holdPercent + 99) / 100,
            "generated LN count must fit the selected source-note budget");
    if (boosted) {
        // F's local allowance uses retained originals; deleted source objects
        // must not grant additional room to the second pass.
        sourcesByWindow.clear();
        for (const auto& note : dense.chart.notes) {
            if (sourceIds.contains(note.id)) {
                ++sourcesByWindow[note.time < 0 ? 0 : note.time / 2000];
            }
        }
    }
    for (const auto& [window, count] : additionsByWindow) {
        const int localBudget = std::clamp((sourcesByWindow[window] * totalPercent + 99) / 100,
                                           1, boosted ? 14 : 10);
        require(count <= localBudget, "all additions including the base must fit the local 2s budget");
    }
    for (const auto& [window, count] : holdsByWindow) {
        (void)window;
        require(count <= (boosted ? 5 : 3), "generated LNs must fit the selected 2s hold budget");
    }
    for (const auto& [time, count] : additionsByTimestamp) {
        (void)time;
        require(count <= (boosted ? 3 : 2), "all additions must fit the selected per-timestamp budget");
    }
}

void phraseDenseLnOffPreservesBAndCExactly() {
    const auto source = phraseDenseLnFixture();
    for (const bool withSupport : {false, true}) {
        auto options = faithfulOptions(10);
        options.phraseSearch = true;
        options.phraseSupport = withSupport;
        require(!options.phraseDenseLn, "dense LN support must remain disabled by default");
        const auto defaults = keyconv::nk2::convertChart(source, options);
        options.phraseDenseLn = false;
        const auto explicitOff = keyconv::nk2::convertChart(source, options);
        require(keyconv::exportOsu(defaults.chart, 10) == keyconv::exportOsu(explicitOff.chart, 10) &&
                    keyconv::nk2::reportToJson(defaults.report) == keyconv::nk2::reportToJson(explicitOff.report),
                "explicit dense-LN-off must preserve B and C output and reports exactly");
        require(defaults.report.addedHoldNotes == 0 &&
                    defaults.report.addedTapNotes == defaults.report.addedNotes,
                "B and C must remain free of generated holds");
    }
}

void phraseDenseLnAddsBothTypesWithoutChangingC() {
    const auto source = phraseDenseLnFixture();
    const auto sourceBefore = keyconv::exportOsu(source, 7);
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    const auto dense = keyconv::nk2::convertChart(source, options);
    checkPhraseDenseLnContract(source, conservative, dense);
    require(dense.report.addedNotes > conservative.report.addedNotes &&
                dense.report.addedTapNotes > conservative.report.addedTapNotes &&
                dense.report.addedHoldNotes > 0,
            "sparse mixed material must exercise extra taps and LNs beyond the C chart");
    require(keyconv::exportOsu(source, 7) == sourceBefore, "dense conversion must not mutate its input");
}

void phraseDenseLnRespectsFutureHeadsAndTails() {
    auto source = chart();
    for (int group = 0; group < 5; ++group) {
        const int start = 1000 + group * 4000;
        const auto prefix = "boundary-" + std::to_string(group) + "-";
        source.notes.push_back(hold(prefix + "short", start, 0, start + 1000));
        source.notes.push_back(tap(prefix + "inside", start + 500, 1));
        source.notes.push_back(tap(prefix + "at-tail", start + 1000, 2));
        source.notes.push_back(hold(prefix + "long", start + 1500, 3, start + 3500));
        source.notes.push_back(tap(prefix + "after", start + 2000, 4));
        source.notes.push_back(tap(prefix + "late-inside", start + 3000, 5));
        source.notes.push_back(tap(prefix + "long-tail", start + 3500, 6));
    }
    std::reverse(source.notes.begin(), source.notes.end());
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    const auto dense = keyconv::nk2::convertChart(source, options);
    checkPhraseDenseLnContract(source, conservative, dense);
    require(dense.report.addedHoldNotes > 0,
            "the unsorted head/tail boundary fixture must exercise actual generated LNs");
}

void phraseDenseLnRespectsConcentratedBudgets() {
    auto source = chart({hold("dense-budget-hold", 0, 6, 1200)});
    for (int index = 0; index < 96; ++index) {
        source.notes.push_back(tap("dense-budget-tap-" + std::to_string(index), index * 20, 0));
    }
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    const auto dense = keyconv::nk2::convertChart(source, options);
    checkPhraseDenseLnContract(source, conservative, dense);
    require(dense.report.addedNotes > conservative.report.addedNotes,
            "concentrated source material must exercise the larger D budget");
}

void phraseDenseLnIsDeterministic() {
    const auto source = phraseDenseLnFixture();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    options.phraseDenseLn = true;
    const auto first = keyconv::nk2::convertChart(source, options);
    const auto second = keyconv::nk2::convertChart(source, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10) &&
                keyconv::nk2::reportToJson(first.report) == keyconv::nk2::reportToJson(second.report),
            "dense LN conversion must remain byte- and report-deterministic");
}

void unsupportedPhraseDenseLnConfigurationsFail() {
    const auto source = phraseDenseLnFixture();
    auto valid = faithfulOptions(10);
    valid.phraseSearch = true;
    valid.phraseSupport = true;
    valid.phraseDenseLn = true;
    std::vector<NK2Options> unsupported;
    auto invalid = valid;
    invalid.phraseSearch = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.phraseSupport = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.sourceKeyCount = 6;
    unsupported.push_back(invalid);
    for (const int target : {4, 5, 6, 7, 9, 12}) {
        invalid = valid;
        invalid.targetKeyCount = target;
        unsupported.push_back(invalid);
    }
    for (const auto mode : {Mode::Native, Mode::Harder, Mode::Transform, Mode::Report}) {
        invalid = valid;
        invalid.mode = mode;
        unsupported.push_back(invalid);
    }
    invalid = valid;
    invalid.superSymmetry = true;
    unsupported.push_back(invalid);
    for (const auto& options : unsupported) {
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, options);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "dense LN must reject configurations outside faithful 7K to 8K/10K phrase support");
    }
}

void emptyPhraseDenseLnInputDoesNotFabricateNotes() {
    const auto source = chart();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    const auto dense = keyconv::nk2::convertChart(source, options);
    checkPhraseDenseLnContract(source, conservative, dense);
    require(dense.chart.notes.empty() && dense.report.addedNotes == 0 &&
                dense.report.addedTapNotes == 0 && dense.report.addedHoldNotes == 0,
            "empty source must not receive fabricated dense taps or LNs");
}

Chart phraseSourceLnFixture() {
    auto source = chart();
    const int durations[] = {800, 1250, 2000};
    for (int index = 0; index < 64; ++index) {
        const int time = 1000 + index * 500;
        const auto id = "source-ln-" + std::to_string(index);
        source.notes.push_back(index % 8 == 0
                                   ? hold(id, time, index % 7, time + durations[(index / 8) % 3])
                                   : tap(id, time, index % 7));
    }
    return source;
}

void checkPhraseSourceLnContract(const Chart& source,
                                 const NK2ConversionResult& conservative,
                                 const NK2ConversionResult& covered,
                                 bool boosted = false) {
    checkPhraseDenseLnContract(source, conservative, covered, boosted);
    require(covered.report.independentSafety->sourceLnCoverageRequired &&
                covered.report.independentSafety->generatedHoldsOutsideSourceLn == 0,
            "E must require full source LN coverage in its automatic independent audit");
    require(keyconv::nk2::auditSafety(source, covered.chart,
                covered.report.options.targetKeyCount, 500, true).passed(),
            "a separate strict audit of the final E chart must agree with the conversion report");
    std::set<std::string> sourceIds;
    for (const auto& original : source.notes) {
        sourceIds.insert(original.id);
    }
    for (const auto& note : covered.chart.notes) {
        if (sourceIds.contains(note.id) || note.type != NoteType::Hold) {
            continue;
        }
        const bool insideOneOriginal = std::any_of(source.notes.begin(), source.notes.end(),
            [&note](const Note& original) {
                return original.type == NoteType::Hold && original.endTime.has_value() &&
                       *original.endTime > original.time && original.time <= note.time &&
                       note.endTime.has_value() && *note.endTime <= *original.endTime;
            });
        require(insideOneOriginal,
                "each generated LN must fit in one original LN rather than their combined coverage");
    }
}

void phraseSourceLnOffPreservesBThroughDExactly() {
    const auto source = phraseSourceLnFixture();
    for (const int version : {0, 1, 2}) {
        auto options = faithfulOptions(10);
        options.phraseSearch = true;
        options.phraseSupport = version >= 1;
        options.phraseDenseLn = version >= 2;
        require(!options.phraseSourceLn, "source LN coverage mode must remain disabled by default");
        const auto defaults = keyconv::nk2::convertChart(source, options);
        options.phraseSourceLn = false;
        const auto explicitOff = keyconv::nk2::convertChart(source, options);
        require(keyconv::exportOsu(defaults.chart, 10) == keyconv::exportOsu(explicitOff.chart, 10) &&
                    keyconv::nk2::reportToJson(defaults.report) == keyconv::nk2::reportToJson(explicitOff.report),
                "explicit source-LN-off must preserve B, C and D charts and reports exactly");
        require(!defaults.report.independentSafety->sourceLnCoverageRequired,
                "B, C and D must not acquire the optional E coverage pass condition");
    }
}

void phraseSourceLnAddsVariedCoveredHoldsWithoutChangingC() {
    const auto source = phraseSourceLnFixture();
    const auto sourceBefore = keyconv::exportOsu(source, 7);
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    options.phraseSourceLn = true;
    const auto covered = keyconv::nk2::convertChart(source, options);
    checkPhraseSourceLnContract(source, conservative, covered);
    require(covered.report.addedNotes > conservative.report.addedNotes && covered.report.addedHoldNotes > 0,
            "sparse LN material must exercise actual E additions beyond C");

    std::set<std::string> sourceIds;
    for (const auto& note : source.notes) {
        sourceIds.insert(note.id);
    }
    std::set<int> generatedDurations;
    std::set<int> generatedLanes;
    for (const auto& note : covered.chart.notes) {
        if (!sourceIds.contains(note.id) && note.type == NoteType::Hold && note.endTime.has_value()) {
            generatedDurations.insert(*note.endTime - note.time);
            generatedLanes.insert(note.lane);
        }
    }
    require(generatedDurations.size() >= 2 && generatedLanes.size() >= 2,
            "a suitable sparse source must produce at least two generated LN durations and lanes");
    require(keyconv::exportOsu(source, 7) == sourceBefore, "E conversion must not mutate the source");
}

void phraseSourceLnAddsNoHoldsToTapOnlySource() {
    auto source = phraseSourceLnFixture();
    for (auto& note : source.notes) {
        note.type = NoteType::Tap;
        note.endTime.reset();
    }
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    options.phraseSourceLn = true;
    const auto covered = keyconv::nk2::convertChart(source, options);
    checkPhraseSourceLnContract(source, conservative, covered);
    require(covered.report.addedHoldNotes == 0 &&
                std::none_of(covered.chart.notes.begin(), covered.chart.notes.end(),
                    [](const Note& note) { return note.type == NoteType::Hold; }),
            "a source without LNs must produce zero new LNs in E");
}

void phraseSourceLnIsDeterministic() {
    const auto source = phraseSourceLnFixture();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    options.phraseDenseLn = true;
    options.phraseSourceLn = true;
    const auto first = keyconv::nk2::convertChart(source, options);
    const auto second = keyconv::nk2::convertChart(source, options);
    require(keyconv::exportOsu(first.chart, 10) == keyconv::exportOsu(second.chart, 10) &&
                keyconv::nk2::reportToJson(first.report) == keyconv::nk2::reportToJson(second.report),
            "E chart and report must remain deterministic despite varying LN durations and lanes");
}

void unsupportedPhraseSourceLnConfigurationsFail() {
    const auto source = phraseSourceLnFixture();
    auto valid = faithfulOptions(10);
    valid.phraseSearch = true;
    valid.phraseSupport = true;
    valid.phraseDenseLn = true;
    valid.phraseSourceLn = true;
    std::vector<NK2Options> unsupported;
    auto invalid = valid;
    invalid.phraseSearch = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.phraseSupport = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.phraseDenseLn = false;
    unsupported.push_back(invalid);
    invalid = valid;
    invalid.sourceKeyCount = 6;
    unsupported.push_back(invalid);
    for (const int target : {4, 5, 6, 7, 9, 12}) {
        invalid = valid;
        invalid.targetKeyCount = target;
        unsupported.push_back(invalid);
    }
    for (const auto mode : {Mode::Native, Mode::Harder, Mode::Transform, Mode::Report}) {
        invalid = valid;
        invalid.mode = mode;
        unsupported.push_back(invalid);
    }
    invalid = valid;
    invalid.superSymmetry = true;
    unsupported.push_back(invalid);
    for (const auto& options : unsupported) {
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, options);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected,
                "E must reject configurations outside faithful 7K to 8K/10K dense phrase support");
    }
}

void emptyPhraseSourceLnInputDoesNotFabricateNotes() {
    const auto source = chart();
    auto options = faithfulOptions(10);
    options.phraseSearch = true;
    options.phraseSupport = true;
    const auto conservative = keyconv::nk2::convertChart(source, options);
    options.phraseDenseLn = true;
    options.phraseSourceLn = true;
    const auto covered = keyconv::nk2::convertChart(source, options);
    checkPhraseSourceLnContract(source, conservative, covered);
    require(covered.chart.notes.empty() && covered.report.addedNotes == 0 &&
                covered.report.addedTapNotes == 0 && covered.report.addedHoldNotes == 0,
            "an empty source must not receive fabricated E notes");
}

void fiveKeyPhraseDropsOnlyExcessSourceObjects() {
    auto source = chart();
    for (int slice = 0; slice < 2; ++slice) {
        for (int lane = 0; lane < 7; ++lane) {
            source.notes.push_back(tap("five-key-" + std::to_string(slice * 7 + lane),
                                       1000 + slice * 2000, lane));
        }
    }
    auto options = faithfulOptions(5);
    options.phraseSearch = true;
    const auto result = keyconv::nk2::convertChart(source, options);
    checkSearchContract(source, result, 5);
    require(result.chart.notes.size() == 10 && result.report.droppedNotes == 4 &&
                result.report.addedTapNotes == 0 && result.report.addedHoldNotes == 0,
            "two seven-note chords must retain five each in 5K with explicit drops and no additions");
}

void eightKeyPhraseAndOverlaysStaySafe() {
    const auto source = phraseSourceLnFixture();
    auto options = faithfulOptions(8);
    options.phraseSearch = true;
    const auto base = keyconv::nk2::convertChart(source, options);
    checkSearchContract(source, base, 8);
    require(base.report.droppedNotes == 0, "the sparse 8K fixture must preserve every original object");
    options.phraseSupport = true;
    const auto support = keyconv::nk2::convertChart(source, options);
    checkPhraseSupportContract(source, base, support);
    options.phraseDenseLn = true;
    const auto dense = keyconv::nk2::convertChart(source, options);
    checkPhraseDenseLnContract(source, support, dense);
    options.phraseSourceLn = true;
    const auto covered = keyconv::nk2::convertChart(source, options);
    checkPhraseSourceLnContract(source, support, covered);
    require(support.report.addedTapNotes > 0 && dense.report.addedHoldNotes > 0 &&
                covered.report.addedHoldNotes > 0,
            "the 8K fixture must exercise actual support taps and D/E holds");
}

NK2Options sourceLnOptions(int target) {
    auto options = faithfulOptions(target);
    options.phraseSearch = true;
    options.phraseSupport = true;
    options.phraseDenseLn = true;
    options.phraseSourceLn = true;
    return options;
}

void checkPhraseSourceLnBoostContract(const Chart& source,
                                      const NK2ConversionResult& covered,
                                      const NK2ConversionResult& boosted) {
    checkPhraseSourceLnContract(source, covered, boosted, true);
    std::set<std::string> coveredIds;
    for (const auto& note : covered.chart.notes) {
        coveredIds.insert(note.id);
    }
    std::set<std::string> sourceIds;
    std::set<int> sourceEndpoints;
    for (const auto& note : source.notes) {
        sourceIds.insert(note.id);
        sourceEndpoints.insert(note.time);
        if (note.type == NoteType::Hold && note.endTime && *note.endTime > note.time) {
            sourceEndpoints.insert(*note.endTime);
        }
    }
    int newHolds = 0;
    std::map<int, int> generatedHoldsAtTime;
    for (const auto& note : boosted.chart.notes) {
        if (!coveredIds.contains(note.id)) {
            ++newHolds;
            require(note.type == NoteType::Hold && note.endTime.has_value(),
                    "the F second pass may only append holds to the complete E chart");
        }
        if (!sourceIds.contains(note.id) && note.type == NoteType::Hold) {
            ++generatedHoldsAtTime[note.time];
            require(sourceEndpoints.contains(note.time) && sourceEndpoints.contains(*note.endTime),
                    "every generated F LN endpoint must be an original head or valid LN tail");
        }
    }
    for (const auto& [time, count] : generatedHoldsAtTime) {
        (void)time;
        require(count <= 2, "F must contain at most two generated LN heads at one timestamp including E");
    }
    require(boosted.report.addedTapNotes == covered.report.addedTapNotes,
            "F must not increase, replace or remove E's generated taps");
    require(newHolds == boosted.report.sourceLnBoostHoldAccepted &&
                newHolds == boosted.report.addedHoldNotes - covered.report.addedHoldNotes &&
                newHolds == boosted.report.addedNotes - covered.report.addedNotes,
            "the F boost counter must equal exactly the new holds appended after E");
    require(boosted.report.denseHoldAccepted == boosted.report.addedHoldNotes,
            "denseHoldAccepted must include every generated F hold, including the E base");
}

void phraseSourceLnBoostOffPreservesBThroughEExactly() {
    const auto source = phraseSourceLnFixture();
    for (const int target : {8, 10}) {
        for (const int version : {0, 1, 2, 3}) {
            auto options = faithfulOptions(target);
            options.phraseSearch = true;
            options.phraseSupport = version >= 1;
            options.phraseDenseLn = version >= 2;
            options.phraseSourceLn = version >= 3;
            require(!options.phraseSourceLnBoost, "F LN boost must remain disabled by default");
            const auto defaults = keyconv::nk2::convertChart(source, options);
            options.phraseSourceLnBoost = false;
            const auto disabled = keyconv::nk2::convertChart(source, options);
            require(keyconv::exportOsu(defaults.chart, target) == keyconv::exportOsu(disabled.chart, target) &&
                        keyconv::nk2::reportToJson(defaults.report) == keyconv::nk2::reportToJson(disabled.report),
                    "explicit boost-off must preserve B/C/D/E output and reports exactly");
            require(defaults.report.sourceLnBoostHoldAccepted == 0,
                    "the F second pass must not run when boost is disabled");
        }
    }
}

void phraseSourceLnBoostAddsOnlySafeHoldsToE() {
    const auto source = phraseSourceLnFixture();
    const auto sourceBefore = keyconv::exportOsu(source, 7);
    for (const int target : {8, 10}) {
        auto options = sourceLnOptions(target);
        const auto covered = keyconv::nk2::convertChart(source, options);
        options.phraseSourceLnBoost = true;
        const auto boosted = keyconv::nk2::convertChart(source, options);
        checkPhraseSourceLnBoostContract(source, covered, boosted);
        require(boosted.report.sourceLnBoostHoldAccepted > 0,
                "sparse source LN material must exercise a positive F LN increase over E");
    }
    require(keyconv::exportOsu(source, 7) == sourceBefore, "F conversion must not mutate the original chart");
}

void phraseSourceLnBoostRespectsConcentratedBudgets() {
    auto source = chart({hold("boost-budget-hold", 0, 6, 1800)});
    for (int index = 0; index < 96; ++index) {
        source.notes.push_back(tap("boost-budget-tap-" + std::to_string(index), index * 20, 0));
    }
    auto options = sourceLnOptions(10);
    const auto covered = keyconv::nk2::convertChart(source, options);
    options.phraseSourceLnBoost = true;
    const auto boosted = keyconv::nk2::convertChart(source, options);
    checkPhraseSourceLnBoostContract(source, covered, boosted);
    require(boosted.report.sourceLnBoostHoldAccepted > 0,
            "concentrated source LN material must exercise the expanded F local budget");
}

void phraseSourceLnBoostIsDeterministic() {
    const auto source = phraseSourceLnFixture();
    for (const int target : {8, 10}) {
        auto options = sourceLnOptions(target);
        options.phraseSourceLnBoost = true;
        const auto first = keyconv::nk2::convertChart(source, options);
        const auto second = keyconv::nk2::convertChart(source, options);
        require(keyconv::exportOsu(first.chart, target) == keyconv::exportOsu(second.chart, target) &&
                    keyconv::nk2::reportToJson(first.report) == keyconv::nk2::reportToJson(second.report),
                "F must remain byte- and report-deterministic for both 8K and 10K");
    }
}

void unsupportedPhraseSourceLnBoostConfigurationsFail() {
    const auto source = phraseSourceLnFixture();
    auto valid = sourceLnOptions(10);
    valid.phraseSourceLnBoost = true;
    std::vector<NK2Options> unsupported;
    for (const int missing : {0, 1, 2, 3}) {
        auto invalid = valid;
        if (missing == 0) invalid.phraseSearch = false;
        if (missing == 1) invalid.phraseSupport = false;
        if (missing == 2) invalid.phraseDenseLn = false;
        if (missing == 3) invalid.phraseSourceLn = false;
        unsupported.push_back(invalid);
    }
    auto invalid = valid;
    invalid.sourceKeyCount = 6;
    unsupported.push_back(invalid);
    for (const int target : {4, 5, 6, 7, 9, 12}) {
        invalid = valid;
        invalid.targetKeyCount = target;
        unsupported.push_back(invalid);
    }
    for (const auto mode : {Mode::Native, Mode::Harder, Mode::Transform, Mode::Report}) {
        invalid = valid;
        invalid.mode = mode;
        unsupported.push_back(invalid);
    }
    invalid = valid;
    invalid.superSymmetry = true;
    unsupported.push_back(invalid);
    for (const auto& options : unsupported) {
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, options);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "F must reject configurations outside faithful 7K to 8K/10K with complete E support");
    }
}

void phraseSourceLnBoostAddsNoHoldsWithoutOriginalLns() {
    auto source = phraseSourceLnFixture();
    for (auto& note : source.notes) {
        note.type = NoteType::Tap;
        note.endTime.reset();
    }
    for (const int target : {8, 10}) {
        auto options = sourceLnOptions(target);
        const auto covered = keyconv::nk2::convertChart(source, options);
        options.phraseSourceLnBoost = true;
        const auto boosted = keyconv::nk2::convertChart(source, options);
        checkPhraseSourceLnBoostContract(source, covered, boosted);
        require(boosted.report.sourceLnBoostHoldAccepted == 0 && boosted.report.addedHoldNotes == 0 &&
                    boosted.chart.notes.size() == covered.chart.notes.size(),
                "F must preserve E exactly when no source LN can cover a new hold");
    }
}

void emptyPhraseSourceLnBoostInputDoesNotFabricateNotes() {
    const auto source = chart();
    for (const int target : {8, 10}) {
        auto options = sourceLnOptions(target);
        const auto covered = keyconv::nk2::convertChart(source, options);
        options.phraseSourceLnBoost = true;
        const auto boosted = keyconv::nk2::convertChart(source, options);
        checkPhraseSourceLnBoostContract(source, covered, boosted);
        require(boosted.chart.notes.empty() && boosted.report.sourceLnBoostHoldAccepted == 0,
                "an empty source must not receive fabricated F notes");
    }
}

Chart fourKeyExpansionFixture() {
    auto source = phraseSourceLnFixture();
    source.meta.sourceKeyCount = 4;
    for (std::size_t index = 0; index < source.notes.size(); ++index) {
        source.notes[index].lane = static_cast<int>(index % 4);
    }
    return source;
}

void fourKeyExpansionPreservesBAndAddsCoveredE() {
    const auto source = fourKeyExpansionFixture();
    const auto before = keyconv::exportOsu(source, 4);
    for (const int target : {5, 7}) {
        auto options = faithfulOptions(target);
        options.sourceKeyCount = 4;
        options.phraseSearch = true;
        const auto placement = keyconv::nk2::convertChart(source, options);
        checkSearchContract(source, placement, target);
        options.phraseSupport = true;
        const auto conservative = keyconv::nk2::convertChart(source, options);
        options.phraseDenseLn = true;
        options.phraseSourceLn = true;
        const auto covered = keyconv::nk2::convertChart(source, options);
        checkPhraseSourceLnContract(source, conservative, covered);
        require(covered.report.addedHoldNotes > 0,
                "native 4K LN material must exercise real E hold additions for both targets");
        const auto repeated = keyconv::nk2::convertChart(source, options);
        require(keyconv::exportOsu(covered.chart, target) == keyconv::exportOsu(repeated.chart, target) &&
                    keyconv::nk2::reportToJson(covered.report) == keyconv::nk2::reportToJson(repeated.report),
                "4K experimental E must be deterministic");
    }
    require(keyconv::exportOsu(source, 4) == before, "4K experiments must preserve the source");
}

void fourKeyExpansionHonorsTapOnlyAndConservativeBudgets() {
    auto source = fourKeyExpansionFixture();
    for (auto& note : source.notes) {
        note.type = NoteType::Tap;
        note.endTime.reset();
    }
    for (const int target : {5, 7}) {
        auto options = sourceLnOptions(target);
        options.sourceKeyCount = 4;
        const auto covered = keyconv::nk2::convertChart(source, options);
        require(covered.report.independentSafety->passed() && covered.report.addedHoldNotes == 0,
                "4K E must not invent holds in a tap-only source");
        auto tiny = chart({tap("tiny-0", 1000, 0), tap("tiny-1", 2000, 1)});
        tiny.meta.sourceKeyCount = 4;
        options.phraseDenseLn = false;
        options.phraseSourceLn = false;
        const auto conservative = keyconv::nk2::convertChart(tiny, options);
        require(conservative.report.independentSafety->passed() && conservative.report.addedNotes <= 1,
                "experimental C must use its 12 percent budget instead of the legacy 4K-to-5K minimum");
    }
}

void fourKeyExpansionRejectsUnsupportedTargetsAndBoost() {
    const auto source = fourKeyExpansionFixture();
    for (const int target : {4, 5, 7, 8, 10}) {
        auto options = sourceLnOptions(target);
        options.sourceKeyCount = 4;
        options.phraseSourceLnBoost = target == 5 || target == 7;
        bool rejected = false;
        try {
            (void)keyconv::nk2::convertChart(source, options);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "4K experiments allow E only for 5K/7K and do not enable F");
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"default off and observational audit", defaultAndExplicitOffRemainIdentical},
        {"mixed future LN and pattern safety", futureHoldAndPatternFixturesStaySafe},
        {"deterministic phrase output", searchIsDeterministic},
        {"short lookahead retention benefit", shortLookaheadRetainsNoteLostByBaseline},
        {"unsupported configurations", unsupportedConfigurationsFailExplicitly},
        {"dense bounded windows", denseWindowsRemainBoundedAcrossChordBoundaries},
        {"sparse window progress", sparseWindowsAdvanceAndPreserveSources},
        {"empty search input", emptyInputHasNoSearchWork},
        {"oversized simultaneous slice", oversizedSliceFailsWithoutUnboundedSearch},
        {"phrase support off preserves B", phraseSupportOffPreservesBExactly},
        {"phrase support safe additive output", phraseSupportAddsSafeTapsWithoutChangingB},
        {"phrase support local cap", phraseSupportRespectsLocalCapUnderCandidatePressure},
        {"phrase support deterministic output", phraseSupportIsDeterministic},
        {"phrase support unsupported combinations", unsupportedPhraseSupportConfigurationsFail},
        {"phrase support empty input", emptyPhraseSupportInputDoesNotFabricateNotes},
        {"dense LN off preserves B and C", phraseDenseLnOffPreservesBAndCExactly},
        {"dense LN additive taps and holds", phraseDenseLnAddsBothTypesWithoutChangingC},
        {"dense LN future head and tail safety", phraseDenseLnRespectsFutureHeadsAndTails},
        {"dense LN concentrated budgets", phraseDenseLnRespectsConcentratedBudgets},
        {"dense LN deterministic output", phraseDenseLnIsDeterministic},
        {"dense LN unsupported combinations", unsupportedPhraseDenseLnConfigurationsFail},
        {"dense LN empty input", emptyPhraseDenseLnInputDoesNotFabricateNotes},
        {"source LN off preserves B through D", phraseSourceLnOffPreservesBThroughDExactly},
        {"source LN covered and varied additions", phraseSourceLnAddsVariedCoveredHoldsWithoutChangingC},
        {"source LN tap-only input", phraseSourceLnAddsNoHoldsToTapOnlySource},
        {"source LN deterministic output", phraseSourceLnIsDeterministic},
        {"source LN unsupported combinations", unsupportedPhraseSourceLnConfigurationsFail},
        {"source LN empty input", emptyPhraseSourceLnInputDoesNotFabricateNotes},
        {"five-key phrase explicit drops", fiveKeyPhraseDropsOnlyExcessSourceObjects},
        {"eight-key phrase and overlays", eightKeyPhraseAndOverlaysStaySafe},
        {"source LN boost off preserves B through E", phraseSourceLnBoostOffPreservesBThroughEExactly},
        {"source LN boost only appends safe holds", phraseSourceLnBoostAddsOnlySafeHoldsToE},
        {"source LN boost concentrated budgets", phraseSourceLnBoostRespectsConcentratedBudgets},
        {"source LN boost deterministic output", phraseSourceLnBoostIsDeterministic},
        {"source LN boost unsupported combinations", unsupportedPhraseSourceLnBoostConfigurationsFail},
        {"source LN boost tap-only input", phraseSourceLnBoostAddsNoHoldsWithoutOriginalLns},
        {"source LN boost empty input", emptyPhraseSourceLnBoostInputDoesNotFabricateNotes},
        {"native four-key B and covered E", fourKeyExpansionPreservesBAndAddsCoveredE},
        {"native four-key tap-only and budgets", fourKeyExpansionHonorsTapOnlyAndConservativeBudgets},
        {"native four-key unsupported targets and F", fourKeyExpansionRejectsUnsupportedTargetsAndBoost},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "[pass] " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    std::cout << tests.size() << " NK2 phrase tests passed\n";
    return 0;
}
