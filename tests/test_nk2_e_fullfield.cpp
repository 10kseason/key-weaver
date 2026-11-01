#include "nk2/e_fullfield.hpp"
#include "nk2/nk2_convert.hpp"
#include "nk2/nk2_report.hpp"
#include "nk2/safety_audit.hpp"
#include <keyconv/format/osu_exporter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
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
using keyconv::nk2::EFullFieldStats;
using keyconv::nk2::NK2Options;
using keyconv::nk2::spreadEFullField;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Note tap(const std::string& id, int time, int lane) {
    Note result;
    result.id = id; result.time = time; result.lane = lane;
    result.sourceLane = lane; result.raw = "fixture:" + id;
    return result;
}

Note hold(const std::string& id, int time, int lane, int end) {
    auto result = tap(id,time,lane);
    result.type = NoteType::Hold; result.endTime = end;
    return result;
}

Chart chart(int keys = 10) {
    Chart result;
    result.meta.sourceKeyCount = keys; result.meta.targetKeyCount = 10;
    result.meta.version = "Native full-field fixture";
    keyconv::TimingPoint timing;
    timing.beatLength = 500.0; timing.uninherited = true;
    result.timingPoints.push_back(timing);
    return result;
}

void requireFixedEvents(const Chart& before, const Chart& after, int targetKeys = 10) {
    require(before.notes.size() == after.notes.size(), "E+ added or removed a note");
    for (std::size_t i = 0; i < before.notes.size(); ++i) {
        const auto fields = [](const Note& n) {
            return std::tie(n.id,n.time,n.type,n.endTime,n.raw,n.sourceLane);
        };
        require(fields(before.notes[i]) == fields(after.notes[i]),
                "E+ changed head time, hold duration, identity, raw data, provenance, or order");
        require(after.notes[i].lane >= 0 && after.notes[i].lane < targetKeys, "E+ left the requested output field");
    }
}

std::pair<double,double> measures(const Chart& input) {
    if (input.notes.empty()) return {0.0,0.0};
    std::map<std::int64_t,std::array<std::uint64_t,10>> windows;
    for (const auto& note : input.notes) {
        const auto time = static_cast<std::int64_t>(note.time);
        const auto window = time / 2000 - (time % 2000 < 0 ? 1 : 0);
        ++windows[window][static_cast<std::size_t>(note.lane)];
    }
    double activeSum = 0.0, concentration = 0.0;
    for (const auto& [index,counts] : windows) {
        (void)index;
        std::uint64_t total = 0;
        for (const auto count : counts) total += count;
        for (const auto count : counts) {
            activeSum += count > 0;
            const auto ratio = static_cast<double>(count) / static_cast<double>(total);
            concentration += ratio * ratio;
        }
    }
    const auto windowCount = static_cast<double>(windows.size());
    return {activeSum/windowCount,concentration/windowCount};
}

void requireRepeatChains(const Chart& before, const Chart& after) {
    std::array<std::vector<std::size_t>,10> lanes;
    for (std::size_t i = 0; i < before.notes.size(); ++i) lanes[before.notes[i].lane].push_back(i);
    for (auto& lane : lanes) {
        std::stable_sort(lane.begin(),lane.end(),[&](auto a, auto b) {
            return before.notes[a].time < before.notes[b].time;
        });
        for (std::size_t begin = 0; begin < lane.size();) {
            auto end = begin + 1;
            while (end < lane.size() && static_cast<std::int64_t>(before.notes[lane[end]].time) -
                   before.notes[lane[end-1]].time <= 500) ++end;
            const bool shortChain = static_cast<std::int64_t>(before.notes[lane[end-1]].time) -
                                    before.notes[lane[begin]].time <= 2000;
            const auto bin = [](int t) { return t / 2000 - (t % 2000 < 0 ? 1 : 0); };
            for (auto i = begin + 1; i < end; ++i) {
                const auto a = lane[i-1], b = lane[i];
                if (shortChain || bin(before.notes[a].time) == bin(before.notes[b].time))
                    require(after.notes[a].lane == after.notes[b].lane,
                            "E+ split a short repeat chain or the interior of a 2s long-chain segment");
            }
            begin = end;
        }
    }
}

void requireValidTransform(const Chart& source, const Chart& before,
                           const Chart& after, const EFullFieldStats& stats, int targetKeys = 10) {
    requireFixedEvents(before,after,targetKeys);
    requireRepeatChains(before,after);
    require(keyconv::nk2::auditSafety(source,after,targetKeys,500,true).passed(),
            "E+ failed independent collision, LN, created-jack, or source-LN coverage inspection");
    const auto [activeBefore,concentrationBefore] = measures(before);
    const auto [activeAfter,concentrationAfter] = measures(after);
    require(std::isfinite(stats.activeLanesAfter) && std::isfinite(stats.concentrationAfter) &&
                std::abs(stats.activeLanesBefore-activeBefore) < 1e-12 &&
                std::abs(stats.activeLanesAfter-activeAfter) < 1e-12 &&
                std::abs(stats.concentrationBefore-concentrationBefore) < 1e-12 &&
                std::abs(stats.concentrationAfter-concentrationAfter) < 1e-12,
            "E+ statistics disagree with the actual disjoint 2s window lane counts");
    require(activeAfter + 1e-12 >= activeBefore, "E+ reduced mean active lanes");
    require(concentrationAfter <= concentrationBefore + 1e-12, "E+ increased mean 2s concentration");
    const auto sourceConcentration = [&](const Chart& output) {
        std::map<std::string,int> originals;
        for (const auto& note : source.notes) originals[note.id] = note.sourceLane.value_or(note.lane);
        std::map<int,std::array<int,10>> counts;
        for (const auto& note : output.notes) if (originals.count(note.id))
            ++counts[originals.at(note.id)][note.lane];
        double concentration = 0;
        for (const auto& [origin,row] : counts) {
            (void)origin;
            double total = 0; for (int n : row) total += n;
            for (int n : row) concentration += (n/total)*(n/total);
        }
        return counts.empty() ? 0 : concentration/counts.size();
    };
    require(std::abs(stats.sourceConcentrationBefore-sourceConcentration(before)) < 1e-12 &&
            std::abs(stats.sourceConcentrationAfter-sourceConcentration(after)) < 1e-12,
            "source concentration must count retained originals, excluding all generated notes");
    int moved = 0;
    for (std::size_t i = 0; i < before.notes.size(); ++i) moved += before.notes[i].lane != after.notes[i].lane;
    require(stats.movedNotes == moved && stats.movedGroups <= stats.movedNotes,
            "E+ move statistics must describe retained lane changes");
}

void movableNotesReachFullField() {
    auto source = chart();
    for (int i = 0; i < 20; ++i) source.notes.push_back(tap("n"+std::to_string(i),1000+i*600,0));
    auto output = source;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,source,output,stats);
    std::set<int> lanes;
    for (const auto& note : output.notes) lanes.insert(note.lane);
    require(!stats.reverted && stats.movedNotes > 0 && stats.acceptedMoves > 0 && lanes.size() == 10,
            "unconstrained separated heads should exercise all ten lanes");
    require(stats.concentrationAfter < stats.concentrationBefore && stats.activeLanesAfter > stats.activeLanesBefore,
            "spread-friendly fixture must demonstrate both distribution and active-lane improvement");
    auto repeat = source;
    const auto repeatedStats = spreadEFullField(source,repeat);
    require(keyconv::exportOsu(output,10) == keyconv::exportOsu(repeat,10) &&
                stats.safetyWork == repeatedStats.safetyWork && stats.acceptedMoves == repeatedStats.acceptedMoves,
            "full-field search order and work count must be deterministic");
}

void repeatChainsMoveTogether() {
    auto source = chart();
    for (int group = 0; group < 6; ++group) {
        for (int note = 0; note < 3; ++note) {
            source.notes.push_back(tap("chain"+std::to_string(group)+"-"+std::to_string(note),
                                       1000+group*1601+note*500,0));
        }
    }
    auto output = source;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,source,output,stats);
    require(!stats.reverted && stats.movedGroups > 0, "movable repeat groups should exercise real redistribution");
}

void headAndTailBlockersStaySafe() {
    for (const bool longNote : {false,true}) {
        auto source = chart();
        source.notes.push_back(longNote ? hold("anchor",0,0,1000) : tap("anchor",0,0));
        const int blockedTime = longNote ? 1000 : 500;
        for (int lane = 1; lane < 10; ++lane) {
            source.notes.push_back(tap("block"+std::to_string(lane),blockedTime,lane));
        }
        for (int i = 0; i < 20; ++i) {
            source.notes.push_back(tap("later"+std::to_string(i),blockedTime+601+i*601,0));
        }
        auto output = source;
        const auto stats = spreadEFullField(source,output);
        requireValidTransform(source,source,output,stats);
        require(!stats.reverted && output.notes.front().lane == 0,
                "inclusive 500ms head guard and exact LN tail conflict must reject all anchor destinations");
    }
}

void emptySparseAndNoSafeRoom() {
    auto empty = chart();
    auto output = empty;
    const auto emptyStats = spreadEFullField(empty,output);
    requireValidTransform(empty,empty,output,emptyStats);
    require(emptyStats.movedNotes == 0, "empty input must not fabricate movement");

    auto sparse = chart(); sparse.notes.push_back(tap("only",1234,3)); output = sparse;
    const auto sparseStats = spreadEFullField(sparse,output);
    requireValidTransform(sparse,sparse,output,sparseStats);
    require(sparseStats.movedNotes == 0, "a single note has no strictly better distribution");

    auto full = chart();
    for (int time : {1000,1200,1400}) for (int lane = 0; lane < 10; ++lane) {
        full.notes.push_back(tap(std::to_string(time)+"-"+std::to_string(lane),time,lane));
    }
    output = full;
    const auto fullStats = spreadEFullField(full,output);
    requireValidTransform(full,full,output,fullStats);
    require(fullStats.movedNotes == 0 && fullStats.acceptedMoves == 0 &&
                keyconv::exportOsu(full,10) == keyconv::exportOsu(output,10),
            "a fully occupied chord field must remain unchanged without safe destinations");
}

void allSourceKeyCountsReleaseLongAnchors() {
    for (int keys = 1; keys <= 18; ++keys) {
        auto source = chart(keys);
        for (int lane = 0; lane < keys; ++lane) for (int i = 0; i < 32; ++i)
            source.notes.push_back(tap("source"+std::to_string(lane)+"-"+std::to_string(i),
                                       lane*12000 + i*250, lane));
        auto baseline = source;
        for (auto& note : baseline.notes) note.lane %= 10;
        auto result = baseline;
        const auto stats = spreadEFullField(source,result);
        requireValidTransform(source,baseline,result,stats);
        require(stats.splitLongChains == keys && stats.sourceConcentrationAfter < 0.6 && !stats.reverted,
                "every source key count must permit long chains to use multiple output lanes");
        std::map<int,std::set<int>> destinations;
        for (const auto& note : result.notes) destinations[*note.sourceLane].insert(note.lane);
        for (const auto& [origin,lanes] : destinations) {
            (void)origin;
            require(lanes.size() >= 3, "source lane stayed fixed despite safe phrase destinations");
        }
        auto repeated = baseline;
        (void)spreadEFullField(source,repeated);
        require(keyconv::exportOsu(result,10) == keyconv::exportOsu(repeated,10),
                "source-unpinning must be deterministic for all source key counts");

        // Also exercise each key count through completed-E generation, rather
        // than only the isolated post-pass and hand-authored target lanes.
        auto native = chart(keys);
        for (int i = 0; i < 64; ++i)
            native.notes.push_back(tap("native-"+std::to_string(i),i*250,i%keys));
        NK2Options options;
        options.sourceKeyCount = keys; options.targetKeyCount = 10;
        options.mode = keyconv::nk2::Mode::Faithful;
        options.phraseSearch = options.phraseSupport = options.phraseDenseLn = options.phraseSourceLn = true;
        const auto e = keyconv::nk2::convertChart(native,options);
        options.eFullField = true;
        const auto plus = keyconv::nk2::convertChart(native,options);
        require(plus.report.fullFieldTransform && plus.report.independentSafety &&
                plus.report.independentSafety->passed(), "all source key counts must pass E+ integration safety");
        requireValidTransform(native,e.chart,plus.chart,*plus.report.fullFieldTransform);
    }
}

void balancedGeneratedFieldDoesNotHideSourcePinning() {
    auto source = chart(4);
    auto baseline = source;
    for (int i = 0; i < 12; ++i) {
        auto original = tap("retained-"+std::to_string(i),i*2000+900,0);
        source.notes.push_back(original);
        original.lane = 3;
        baseline.notes.push_back(original);
        for (int lane = 0; lane < 10; ++lane) if (lane != 3)
            baseline.notes.push_back(tap("gen:"+std::to_string(i)+"-"+std::to_string(lane),
                                         original.time,lane));
    }
    auto output = baseline;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,baseline,output,stats);
    require(stats.phraseSwaps > 0 && stats.sourceConcentrationBefore == 1 &&
            stats.sourceConcentrationAfter < 0.4 && std::abs(stats.concentrationAfter-0.1) < 1e-12 &&
            stats.activeLanesAfter == 10 && !stats.reverted,
            "balanced whole-field use must not mask an avoidably pinned original source");
}

void denseBoundaryCannotBypassCreatedJackGuard() {
    auto source = chart(10);
    for (int i = 0; i < 24; ++i) for (int lane = 0; lane < 10; ++lane)
        source.notes.push_back(tap(std::to_string(i)+"-"+std::to_string(lane),i*250,lane));
    auto output = source;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,source,output,stats);
    require(stats.splitLongChains == 10 && stats.movedNotes == 0 && stats.phraseSwaps == 0,
            "unpinning must not rotate dense lane ownership across a forbidden 250ms boundary");
}

void extremeSignedTimesKeepChains() {
    auto source = chart();
    const int times[] = {std::numeric_limits<int>::min(),std::numeric_limits<int>::min()+500,
                         std::numeric_limits<int>::min()+1101,-1,499,1000,
                         std::numeric_limits<int>::max()-501,std::numeric_limits<int>::max()};
    for (int i = 0; i < 8; ++i) source.notes.push_back(tap("edge"+std::to_string(i),times[i],0));
    auto output = source;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,source,output,stats);
    require(!stats.reverted, "valid signed-time fixture must not overflow safety or window arithmetic");
}

void failedFinalAuditRestoresAllLanes() {
    auto source = chart(); source.notes.push_back(tap("source",1000,0));
    auto invalidE = source;
    invalidE.notes.push_back(hold("nk2-ln-source-unsafe",2000,0,3000));
    const auto before = invalidE;
    require(!keyconv::nk2::auditSafety(source,invalidE,10,500,true).passed(), "rollback fixture must be unsafe");
    const auto stats = spreadEFullField(source,invalidE);
    requireFixedEvents(before,invalidE);
    require(stats.reverted && stats.movedNotes == 0 && stats.movedGroups == 0 &&
                keyconv::exportOsu(before,10) == keyconv::exportOsu(invalidE,10),
            "failed source-LN safety audit must restore every completed-E lane");
}

void workBudgetExhaustionRemainsSafeAndDeterministic() {
    auto source = chart();
    for (int i = 0; i < 6000; ++i) {
        source.notes.push_back(tap("budget-"+std::to_string(i),1000+i*601,0));
    }
    auto output = source;
    const auto stats = spreadEFullField(source,output);
    requireValidTransform(source,source,output,stats);
    require(stats.budgetLimited && !stats.reverted && stats.safetyWork == 2000001 &&
                stats.movedNotes > 0 && stats.movedNotes < static_cast<int>(source.notes.size()),
            "work exhaustion must retain already verified moves and leave remaining E notes unchanged");
    auto repeated = source;
    const auto again = spreadEFullField(source,repeated);
    requireValidTransform(source,source,repeated,again);
    require(again.budgetLimited && !again.reverted &&
                std::tie(stats.movedNotes,stats.movedGroups,stats.acceptedMoves,stats.safetyWork,
                         stats.activeLanesBefore,stats.activeLanesAfter,stats.concentrationBefore,stats.concentrationAfter,
                         stats.splitLongChains,stats.phraseSwaps,stats.sourceConcentrationBefore,stats.sourceConcentrationAfter) ==
                std::tie(again.movedNotes,again.movedGroups,again.acceptedMoves,again.safetyWork,
                         again.activeLanesBefore,again.activeLanesAfter,again.concentrationBefore,again.concentrationAfter,
                         again.splitLongChains,again.phraseSwaps,again.sourceConcentrationBefore,again.sourceConcentrationAfter) &&
                keyconv::exportOsu(output,10) == keyconv::exportOsu(repeated,10),
            "the work limit must stop at the same safe state independently of wall-clock timing");
}

Chart sourceLnFixture() {
    auto source = chart(7);
    source.meta.targetKeyCount.reset();
    const int durations[] = {800,1250,2000};
    for (int i = 0; i < 64; ++i) {
        const int time = 1000+i*500;
        const auto id = "source-ln-"+std::to_string(i);
        source.notes.push_back(i%8 == 0 ? hold(id,time,i%7,time+durations[(i/8)%3]) : tap(id,time,i%7));
    }
    return source;
}

NK2Options eOptions() {
    NK2Options options;
    options.sourceKeyCount = 7; options.targetKeyCount = 10; options.mode = keyconv::nk2::Mode::Faithful;
    options.phraseSearch = options.phraseSupport = options.phraseDenseLn = options.phraseSourceLn = true;
    return options;
}

void ePlusPreservesRhythmAndGeneratedLns() {
    const auto source = sourceLnFixture();
    const auto sourceBytes = keyconv::exportOsu(source,7);
    auto options = eOptions();
    require(!options.eFullField, "E+ must be disabled by default");
    const auto baseline = keyconv::nk2::convertChart(source,options);
    options.eFullField = false;
    const auto disabled = keyconv::nk2::convertChart(source,options);
    require(!baseline.report.fullFieldTransform.has_value() &&
                keyconv::exportOsu(baseline.chart,10) == keyconv::exportOsu(disabled.chart,10) &&
                keyconv::nk2::reportToJson(baseline.report) == keyconv::nk2::reportToJson(disabled.report),
            "default and explicitly disabled E+ must preserve legacy E chart and report bytes");
    options.eFullField = true;
    const auto expanded = keyconv::nk2::convertChart(source,options);
    require(expanded.report.fullFieldTransform.has_value() && expanded.report.addedHoldNotes > 0 &&
                expanded.report.addedHoldNotes == baseline.report.addedHoldNotes &&
                expanded.report.addedNotes == baseline.report.addedNotes &&
                expanded.report.droppedNotes == baseline.report.droppedNotes,
            "E+ must reuse completed E notes, including generated LNs, without changing note accounting");
    requireValidTransform(source,baseline.chart,expanded.chart,*expanded.report.fullFieldTransform);
    require(expanded.report.independentSafety.has_value() && expanded.report.independentSafety->passed() &&
                expanded.report.independentSafety->sourceLnCoverageRequired &&
                expanded.report.independentSafety->generatedHoldsOutsideSourceLn == 0,
            "E+ must retain strict original-LN containment in its conversion report");
    require(keyconv::exportOsu(source,7) == sourceBytes, "E+ mutated the original chart");
    const auto repeated = keyconv::nk2::convertChart(source,options);
    require(keyconv::exportOsu(expanded.chart,10) == keyconv::exportOsu(repeated.chart,10) &&
                keyconv::nk2::reportToJson(expanded.report) == keyconv::nk2::reportToJson(repeated.report),
            "E+ output, metadata, counters, and diagnostic report must be deterministic");
}

void unsupportedOptionsReject() {
    const auto source = sourceLnFixture();
    for (int variant = 0; variant < 11; ++variant) {
        auto options = eOptions(); options.eFullField = true;
        switch (variant) {
        case 0: options.targetKeyCount = 7; break;
        case 1: options.mode = keyconv::nk2::Mode::Native; break;
        case 2: options.phraseSearch = false; break;
        case 3: options.phraseSupport = false; break;
        case 4: options.phraseDenseLn = false; break;
        case 5: options.phraseSourceLn = false; break;
        case 6: options.phraseSourceLnBoost = true; break;
        case 7: options.superSymmetry = true; break;
        case 8: options.sourceKeyCount = 0; break;
        case 9: options.sourceKeyCount = 19; break;
        case 10: options.mode = keyconv::nk2::Mode::Report; break;
        }
        bool rejected = false;
        try { (void)keyconv::nk2::convertChart(source,options); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected,"unsupported E+ option combination did not fail: "+std::to_string(variant));
    }
}

void eightKeyDestinationsAndBoundaries() {
    auto source = chart(4);
    for (int i = 0; i < 40; ++i) source.notes.push_back(tap("stream-"+std::to_string(i),i*250,0));
    auto output = source;
    const auto spread = spreadEFullField(source,output,8);
    requireValidTransform(source,source,output,spread,8);
    require(spread.splitLongChains > 0 && spread.sourceConcentrationAfter < 0.6 && spread.movedNotes > 0,
            "8K stream remained pinned despite available destinations");

    source = chart(8);
    for (int i = 0; i < 24; ++i) for (int lane = 0; lane < 8; ++lane)
        source.notes.push_back(tap(std::to_string(i)+"-"+std::to_string(lane),i*250,lane));
    output = source;
    const auto dense = spreadEFullField(source,output,8);
    requireValidTransform(source,source,output,dense,8);
    require(dense.movedNotes == 0 && dense.phraseSwaps == 0 && dense.activeLanesAfter == 8,
            "8K dense boundary escaped into unused 9th/10th storage lanes");

    auto invalid = source; invalid.notes.front().lane = 8;
    const auto before = keyconv::exportOsu(invalid,10);
    require(spreadEFullField(source,invalid,8).reverted && keyconv::exportOsu(invalid,10) == before,
            "8K accepted a 9th lane in its baseline or mutated invalid input");
    auto empty = chart();
    require(!spreadEFullField(empty,empty,8).reverted,"8K empty chart must be safe");
    require(spreadEFullField(empty,empty,18).reverted,"unsupported helper target must be rejected");

    for (int keys = 1; keys <= 18; ++keys) {
        auto native = chart(keys);
        for (int i = 0; i < 48; ++i) {
            const int time = i*700;
            native.notes.push_back(i%9 == 0 ? hold("n"+std::to_string(i),time,i%keys,time+600)
                                             : tap("n"+std::to_string(i),time,i%keys));
        }
        auto options = eOptions(); options.sourceKeyCount = keys; options.targetKeyCount = 8;
        const auto e = keyconv::nk2::convertChart(native,options);
        options.eFullField = true;
        const auto plus = keyconv::nk2::convertChart(native,options);
        requireValidTransform(native,e.chart,plus.chart,*plus.report.fullFieldTransform,8);
        require(plus.chart.meta.targetKeyCount == 8 && !plus.report.noOp &&
                plus.chart.meta.version->find("8K") != std::string::npos &&
                plus.report.independentSafety->passed(),"8K E+ metadata or independent safety failed");
        const auto repeat = keyconv::nk2::convertChart(native,options);
        require(keyconv::exportOsu(plus.chart,8) == keyconv::exportOsu(repeat.chart,8) &&
                keyconv::nk2::reportToJson(plus.report) == keyconv::nk2::reportToJson(repeat.report),
                "8K E+ conversion/report is nondeterministic");
    }
}

void sameKeyExplicitEPlusWritesARealRemix() {
    auto source = chart(10);
    source.meta.targetKeyCount.reset();
    source.meta.version.reset();
    for (int i = 0; i < 20; ++i) source.notes.push_back(tap("same-key-"+std::to_string(i),1000+i*600,0));
    auto options = eOptions();
    options.sourceKeyCount = 10;
    const auto unchanged = keyconv::nk2::convertChart(source,options);
    require(unchanged.report.noOp && !unchanged.report.chartMutated &&
                !unchanged.report.fullFieldTransform &&
                keyconv::exportOsu(unchanged.chart,10) == keyconv::exportOsu(source,10),
            "plain same-key E must retain its existing no-op behavior");
    options.eFullField = true;
    const auto remixed = keyconv::nk2::convertChart(source,options);
    require(!remixed.report.noOp && remixed.report.noOpReason.empty() && remixed.report.chartMutated &&
                remixed.report.fullFieldTransform && remixed.report.fullFieldTransform->movedNotes > 0 &&
                remixed.chart.meta.targetKeyCount == 10 && remixed.chart.meta.version &&
                remixed.chart.meta.version->find("-fullfield") != std::string::npos,
            "explicit same-key E+ must initialize output metadata and enable CLI chart writing");
    requireValidTransform(source,unchanged.chart,remixed.chart,*remixed.report.fullFieldTransform);
    require(remixed.report.addedNotes == 0 && remixed.report.droppedNotes == 0 &&
                remixed.report.independentSafety && remixed.report.independentSafety->passed(),
            "same-key E+ must relane without adding, dropping, or bypassing the safety gate");
    const auto repeated = keyconv::nk2::convertChart(source,options);
    require(keyconv::exportOsu(remixed.chart,10) == keyconv::exportOsu(repeated.chart,10) &&
                keyconv::nk2::reportToJson(remixed.report) == keyconv::nk2::reportToJson(repeated.report),
            "same-key E+ metadata and output must be deterministic");
}
}  // namespace

int main() {
    const std::vector<std::pair<std::string,void(*)()>> tests{
        {"movable notes reach full field",movableNotesReachFullField},
        {"repeat chains move together",repeatChainsMoveTogether},
        {"inclusive head and tail blockers",headAndTailBlockersStaySafe},
        {"empty sparse and no safe room",emptySparseAndNoSafeRoom},
        {"wide signed time arithmetic",extremeSignedTimesKeepChains},
        {"failed audit restores all lanes",failedFinalAuditRestoresAllLanes},
        {"deterministic work-budget exhaustion",workBudgetExhaustionRemainsSafeAndDeterministic},
        {"E+ rhythm LN safety and determinism",ePlusPreservesRhythmAndGeneratedLns},
        {"unsupported E+ configurations",unsupportedOptionsReject},
        {"same-key explicit E+ remix",sameKeyExplicitEPlusWritesARealRemix},
        {"all source key counts release long anchors",allSourceKeyCountsReleaseLongAnchors},
        {"balanced generated field does not hide source pinning",balancedGeneratedFieldDoesNotHideSourcePinning},
        {"dense boundary retains created-jack guard",denseBoundaryCannotBypassCreatedJackGuard},
        {"8K destinations metadata and safety",eightKeyDestinationsAndBoundaries},
    };
    try {
        for (const auto& [name,test] : tests) { test(); std::cout << "[pass] " << name << '\n'; }
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
    std::cout << tests.size() << " E+ full-field tests passed\n";
    return 0;
}
