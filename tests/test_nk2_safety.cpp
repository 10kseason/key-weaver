#include "nk2/safety_audit.hpp"

#include <algorithm>
#include <climits>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using keyconv::Chart;
using keyconv::Note;
using keyconv::NoteType;
using keyconv::nk2::auditSafety;
using keyconv::nk2::SafetyAudit;

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

Note hold(const std::string& id, int time, int lane, std::optional<int> end) {
    Note note = tap(id, time, lane);
    note.type = NoteType::Hold;
    note.endTime = end;
    return note;
}

Chart chart(std::initializer_list<Note> notes) {
    Chart result;
    result.meta.sourceKeyCount = 7;
    result.notes = notes;
    return result;
}

auto counters(const SafetyAudit& audit) {
    return std::tie(audit.invalidLanes, audit.invalidHolds, audit.sameTimeCollisions,
                    audit.longNoteConflicts, audit.createdLaneRepeats,
                    audit.preservedLaneRepeats, audit.retainedSourceNotes,
                    audit.droppedSourceNotes, audit.addedNotes, audit.retimedSourceNotes,
                    audit.changedHoldDurations, audit.unknownSourceNotes,
                    audit.sourceLnCoverageRequired, audit.generatedHoldsOutsideSourceLn);
}

void interleavedLanesDoNotHideCreatedRepeat() {
    const auto source = chart({tap("a", 1000, 0), tap("b", 1050, 1), tap("c", 1100, 2)});
    auto output = source;
    output.notes[2].lane = 0;
    // Incorrect output hints must not manufacture evidence of original intent.
    output.notes[2].sourceLane = 0;
    const auto audit = auditSafety(source, output, 4);
    require(audit.createdLaneRepeats == 1 && !audit.passed(),
            "another lane between a pair must not hide a source-different repeat");
    require(audit.retainedSourceNotes == 3 && audit.droppedSourceNotes == 0,
            "original object accounting must survive lane movement");
}

void unsortedInputsAndOutputsAreDeterministic() {
    auto source = chart({hold("a", 1000, 0, 1600), tap("b", 1500, 1), tap("c", 1700, 2)});
    auto output = source;
    output.notes[1].lane = 0;
    const auto ordered = auditSafety(source, output, 4);
    std::reverse(source.notes.begin(), source.notes.end());
    std::reverse(output.notes.begin(), output.notes.end());
    const auto shuffled = auditSafety(source, output, 4);
    require(counters(ordered) == counters(shuffled),
            "audit counters must be independent of container insertion order");
    require(output.notes.front().id == "c" && source.notes.front().id == "c",
            "audit must not sort either caller-owned chart in place");
}

void holdBodyTailAndNestedHoldConflicts() {
    const auto source = chart({hold("a", 1000, 0, 2000), hold("b", 1200, 1, 1800),
                               tap("c", 1800, 2), tap("d", 2000, 3), tap("e", 2001, 4)});
    auto output = source;
    for (auto& note : output.notes) {
        note.lane = 0;
    }
    const auto audit = auditSafety(source, output, 4);
    require(audit.longNoteConflicts == 4 && !audit.passed(),
            "body, nested hold and inclusive tail conflicts must each be counted");
    require(audit.changedHoldDurations == 0, "relaning must not change hold duration");
}

void simultaneousHeadsAreCollisions() {
    const auto source = chart({hold("a", 1000, 0, 1400), hold("b", 1000, 1, 1500),
                               tap("c", 1000, 2)});
    auto output = source;
    output.notes[1].lane = 0;
    output.notes[2].lane = 0;
    const auto audit = auditSafety(source, output, 4);
    require(audit.sameTimeCollisions == 2 && audit.longNoteConflicts == 0,
            "three simultaneous heads count two excess objects without body double-counting");
    require(audit.createdLaneRepeats == 0 && !audit.passed(),
            "zero-time collisions must not also count as positive-time repeats");
}

void validSourceRepeatIsPreserved() {
    const auto source = chart({tap("a", 1000, 5), tap("b", 1200, 5), tap("c", 1500, 5)});
    auto output = source;
    for (auto& note : output.notes) {
        note.lane = 1;
    }
    const auto audit = auditSafety(source, output, 4);
    require(audit.createdLaneRepeats == 0 && audit.preservedLaneRepeats == 2 && audit.passed(),
            "relaning an existing source repeat must preserve its intent");
}

void shortenedGapCannotInventSourceIntent() {
    const auto source = chart({tap("a", 1000, 0), tap("b", 2000, 0)});
    auto output = source;
    output.notes[1].time = 1200;
    const auto audit = auditSafety(source, output, 4);
    require(audit.createdLaneRepeats == 1 && audit.preservedLaneRepeats == 0 &&
                audit.retimedSourceNotes == 1 && !audit.passed(),
            "same source lane is insufficient when original timing was outside the repeat window");
}

void generatedNotesHaveExplicitProvenance() {
    const auto source = chart({tap("a", 1000, 0)});
    auto output = source;
    output.notes.push_back(tap("nk2-beat-neutral-a-1200-0", 1200, 0));
    output.notes.push_back(tap("nk2-mirror-neutral-a-1800-1", 1800, 1));
    output.notes.push_back(tap("nk2-ln-anchor-a-2400-2", 2400, 2));
    output.notes.push_back(tap("gen:one", 3000, 3));
    const auto audit = auditSafety(source, output, 4);
    require(audit.addedNotes == 4 && audit.unknownSourceNotes == 0 &&
                audit.createdLaneRepeats == 1 && !audit.passed(),
            "generated support is counted and never treated as an original repeat");
}

void invalidLaneAndHoldCannotPass() {
    const auto source = chart({tap("a", 1000, 0), tap("b", 2000, 1), tap("c", 3000, 2)});
    auto output = source;
    output.notes[0].lane = -1;
    output.notes[1].lane = 4;
    output.notes[2] = hold("c", 3000, 2, 3000);
    output.notes.push_back(hold("gen:no-end", 4000, 0, std::nullopt));
    const auto audit = auditSafety(source, output, 4);
    require(audit.invalidLanes == 2 && audit.invalidHolds == 2 && !audit.passed(),
            "out-of-field lanes and missing or nonpositive hold duration must fail");
    require(!auditSafety({}, {}, 0).passed(), "an invalid empty target layout must not pass");
}

void changedHoldTypeAndDurationFail() {
    const auto source = chart({hold("a", 1000, 0, 1500), hold("b", 2000, 1, 2600)});
    auto output = source;
    output.notes[0].endTime = 1600;
    output.notes[1].type = NoteType::Tap;
    output.notes[1].endTime.reset();
    const auto audit = auditSafety(source, output, 4);
    require(audit.changedHoldDurations == 2 && !audit.passed(),
            "retained source holds must not change duration or silently become taps");
}

void retimingAndDroppingAreVisibleWithoutAutomaticFailure() {
    const auto source = chart({hold("a", 1000, 0, 1500), tap("b", 2000, 1)});
    auto output = chart({hold("a", 1016, 0, 1516)});
    const auto audit = auditSafety(source, output, 4);
    require(audit.retainedSourceNotes == 1 && audit.droppedSourceNotes == 1 &&
                audit.retimedSourceNotes == 1 && audit.changedHoldDurations == 0 && audit.passed(),
            "a safe time shift and explicit source deletion are diagnostics rather than hidden failures");
}

void missingAndDuplicateIdsCannotPass() {
    const auto source = chart({tap("a", 1000, 0), tap("b", 2000, 1)});
    auto output = chart({tap("a", 1000, 0), tap("a", 2000, 1),
                         tap("unknown", 3000, 2), tap("", 4000, 3)});
    const auto audit = auditSafety(source, output, 4);
    require(audit.retainedSourceNotes == 1 && audit.droppedSourceNotes == 1 &&
                audit.unknownSourceNotes == 3 && !audit.passed(),
            "copying an original object or losing provenance must fail the audit");
    const auto ambiguous = chart({tap("a", 1000, 0), tap("a", 2000, 1)});
    require(auditSafety(ambiguous, ambiguous, 4).unknownSourceNotes == 2,
            "duplicate original IDs cannot establish unambiguous identity");
}

void extremeTimesDoNotOverflowRepeatDetection() {
    const auto source = chart({tap("a", INT_MIN, 0), tap("b", INT_MAX, 1)});
    auto output = source;
    output.notes[1].lane = 0;
    const auto audit = auditSafety(source, output, 4);
    require(audit.createdLaneRepeats == 0 && audit.passed(),
            "wide signed timestamp difference must not overflow into a short repeat");
}

void sourceLnCoverageIncludesExactBoundaries() {
    const auto source = chart({hold("source", 1000, 0, 2000)});
    auto output = source;
    output.notes.push_back(hold("gen:exact", 1000, 1, 2000));
    output.notes.push_back(hold("gen:inside", 1250, 2, 1750));
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.sourceLnCoverageRequired && audit.generatedHoldsOutsideSourceLn == 0 && audit.passed(),
            "exact source LN boundaries and strictly contained spans must both satisfy coverage");
}

void missingSourceLnCoverageIsDiagnosticUnlessRequired() {
    const auto source = chart({tap("source", 1000, 0)});
    auto output = source;
    output.notes.push_back(hold("gen:outside", 2000, 1, 2500));
    const auto diagnostic = auditSafety(source, output, 4);
    const auto strict = auditSafety(source, output, 4, 500, true);
    require(!diagnostic.sourceLnCoverageRequired && diagnostic.generatedHoldsOutsideSourceLn == 1 &&
                diagnostic.passed(), "D must measure missing source LN coverage without failing it");
    require(strict.sourceLnCoverageRequired && strict.generatedHoldsOutsideSourceLn == 1 && !strict.passed(),
            "E must fail a generated LN when the source contains no LN");
}

void sourceLaneHintsCannotGrantOrDenyCoverage() {
    const auto source = chart({hold("source", 1000, 6, 2000)});
    auto inside = hold("gen:inside", 1100, 1, 1900);
    inside.sourceLane = -100;
    auto output = chart({inside});
    require(auditSafety(source, output, 4, 500, true).passed(),
            "source LN coverage is temporal and must not require a matching lane hint");
    auto outside = hold("gen:outside", 2100, 2, 2500);
    outside.sourceLane = 6;
    output.notes.push_back(outside);
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.generatedHoldsOutsideSourceLn == 1 && !audit.passed(),
            "a matching source lane hint must not excuse a span outside every original LN");
}

void partialSourceLnOverlapIsNotContainment() {
    const auto source = chart({hold("source", 1000, 0, 2000)});
    const auto output = chart({hold("gen:left", 900, 1, 1200),
                               hold("gen:right", 1800, 2, 2200),
                               hold("gen:enclosing", 900, 3, 2200)});
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.generatedHoldsOutsideSourceLn == 3 && !audit.passed(),
            "head-only, tail-only and enclosing overlap must all fail full-span containment");
}

void separateSourceLnsCannotFormOneCoverageSpan() {
    const auto source = chart({hold("touch-a", 1000, 0, 1500),
                               hold("touch-b", 1500, 1, 2000),
                               hold("overlap-a", 3000, 0, 3600),
                               hold("overlap-b", 3400, 1, 4000)});
    const auto output = chart({hold("gen:touch-bridge", 1200, 2, 1800),
                               hold("gen:overlap-bridge", 3200, 2, 3800)});
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.generatedHoldsOutsideSourceLn == 2 && !audit.passed(),
            "the union of touching or overlapping source LNs must not cover a new bridging LN");
}

void invalidHoldsDoNotParticipateInCoverage() {
    const auto source = chart({hold("missing", 1000, 0, std::nullopt),
                               hold("zero", 1000, 1, 1000),
                               hold("backward", 2000, 2, 500)});
    const auto output = chart({hold("gen:valid", 1000, 0, 1500),
                               hold("gen:missing", 2000, 1, std::nullopt),
                               hold("gen:zero", 3000, 2, 3000),
                               hold("gen:backward", 4000, 3, 3500)});
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.generatedHoldsOutsideSourceLn == 1 && audit.invalidHolds == 3 && !audit.passed(),
            "only valid generated holds require coverage, and invalid source holds cannot provide it");
}

void retainedOriginalHoldsAreExcludedFromCoverage() {
    const auto source = chart({hold("original", 1000, 0, 1500)});
    const auto output = chart({hold("original", 4000, 1, 4500)});
    const auto audit = auditSafety(source, output, 4, 500, true);
    require(audit.generatedHoldsOutsideSourceLn == 0 && audit.retimedSourceNotes == 1 && audit.passed(),
            "recognized originals must use existing integrity checks rather than generated LN coverage");
}

void coverageIsOrderIndependentAndUsesOneEnclosingLn() {
    auto source = chart({hold("outer", 1000, 0, 4000), hold("inner", 1200, 1, 1500),
                          hold("later", 6000, 0, 7000)});
    auto output = chart({hold("gen:contained", 1300, 2, 3500),
                          hold("gen:outside", 5500, 3, 6500)});
    const auto first = auditSafety(source, output, 4, 500, true);
    std::reverse(source.notes.begin(), source.notes.end());
    std::reverse(output.notes.begin(), output.notes.end());
    const auto second = auditSafety(source, output, 4, 500, true);
    require(first.generatedHoldsOutsideSourceLn == 1 && counters(first) == counters(second),
            "a shorter nested LN must not hide an enclosing LN, regardless of input ordering");
    require(source.notes.front().id == "later" && output.notes.front().id == "gen:outside",
            "coverage inspection must not reorder either caller-owned chart");
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, void (*)()>> tests = {
        {"interleaved lane repeat", interleavedLanesDoNotHideCreatedRepeat},
        {"unsorted deterministic audit", unsortedInputsAndOutputsAreDeterministic},
        {"hold body tail and nesting", holdBodyTailAndNestedHoldConflicts},
        {"simultaneous heads", simultaneousHeadsAreCollisions},
        {"source repeat preservation", validSourceRepeatIsPreserved},
        {"retimed gap source intent", shortenedGapCannotInventSourceIntent},
        {"generated provenance", generatedNotesHaveExplicitProvenance},
        {"invalid lanes and holds", invalidLaneAndHoldCannotPass},
        {"hold type and duration", changedHoldTypeAndDurationFail},
        {"explicit shifts and drops", retimingAndDroppingAreVisibleWithoutAutomaticFailure},
        {"unknown and duplicate identity", missingAndDuplicateIdsCannotPass},
        {"wide timestamp arithmetic", extremeTimesDoNotOverflowRepeatDetection},
        {"source LN exact coverage boundaries", sourceLnCoverageIncludesExactBoundaries},
        {"source LN optional coverage gate", missingSourceLnCoverageIsDiagnosticUnlessRequired},
        {"source LN ignores lane hints", sourceLaneHintsCannotGrantOrDenyCoverage},
        {"source LN partial overlap", partialSourceLnOverlapIsNotContainment},
        {"source LN cannot join intervals", separateSourceLnsCannotFormOneCoverageSpan},
        {"source LN invalid hold handling", invalidHoldsDoNotParticipateInCoverage},
        {"source LN original exemption", retainedOriginalHoldsAreExcludedFromCoverage},
        {"source LN nested and unsorted intervals", coverageIsOrderIndependentAndUsesOneEnclosingLn},
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
    std::cout << tests.size() << " independent NK2 safety tests passed\n";
    return 0;
}
