#include "nk2/e_tap_boost.hpp"
#include "nk2/nk2_convert.hpp"
#include "nk2/safety_audit.hpp"
#include <keyconv/format/osu_exporter.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace {
using namespace keyconv;
using namespace keyconv::nk2;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Note tap(std::string id, int time, int lane) {
    Note n; n.id=std::move(id); n.time=time; n.lane=lane; n.sourceLane=lane; n.raw="raw:"+n.id; return n;
}
Chart fixture(int keys=4) {
    Chart c; c.meta.sourceKeyCount=keys; c.meta.targetKeyCount=10; c.meta.version="Native";
    TimingPoint t; t.beatLength=500; t.uninherited=true; c.timingPoints.push_back(t); return c;
}
NK2Options eOptions(int keys) {
    NK2Options o; o.sourceKeyCount=keys; o.targetKeyCount=10; o.mode=Mode::Faithful;
    o.phraseSearch=o.phraseSupport=o.phraseDenseLn=o.phraseSourceLn=o.eFullField=true; return o;
}
void verify(const Chart& source, const Chart& before, const Chart& after, const EFullFieldTapBoostStats& stats, int targetKeys=10) {
    const auto effective=auditSafety(source,after,targetKeys,500,true,&stats.newTapGuards);
    const auto strict=auditSafety(source,after,targetKeys,500,true);
    require(!stats.reverted && effective.passed(), "tap boost failed independent safety");
    require(strict.createdLaneRepeats==stats.strict500CreatedRepeats &&
            strict.createdLaneRepeats==effective.createdLaneRepeats+effective.tempoAllowedTapRepeats &&
            effective.tempoAllowedTapRepeats==stats.tempoAllowedTapRepeats,
            "strict and effective repeat accounting disagree");
    std::map<std::string,const Note*> old, output;
    std::set<int> sourceHeads;
    for (const auto& n : before.notes) old.emplace(n.id,&n);
    for (const auto& n : source.notes) if (old.count(n.id)) sourceHeads.insert(n.time);
    for (const auto& n : after.notes) require(output.emplace(n.id,&n).second,"duplicate output ID");
    for (const auto& [id,a] : old) {
        require(output.count(id),"existing E+ note dropped");
        const auto* b=output.at(id);
        require(std::tie(a->time,a->lane,a->type,a->endTime,a->raw,a->sourceLane)==
                std::tie(b->time,b->lane,b->type,b->endTime,b->raw,b->sourceLane),
                "existing E+ note placement, data or LN changed");
    }
    std::map<int,int> additions;
    int added=0;
    std::array<std::int64_t,10> addedLanes{};
    for (const auto& n : after.notes) if (!old.count(n.id)) {
        ++added; ++additions[n.time]; ++addedLanes[n.lane];
        require(stats.newTapGuards.count(n.id),"new tap lacks explicit repeat authorization");
        require(n.type==NoteType::Tap && !n.endTime && sourceHeads.count(n.time),
                "tap boost added a hold or invented a timestamp");
    }
    for (auto [time,count] : additions) {
        require(count<=3,"more than three new taps at one head");
        require(std::count_if(after.notes.begin(),after.notes.end(),[&](auto& n){return n.time==time;})<=8,
                "tap boost grew a chord beyond eight heads");
    }
    require(added==stats.addedTaps && after.notes.size()==before.notes.size()+added,"addition accounting mismatch");
    require(addedLanes==stats.addedLaneCounts && stats.newTapGuards.size()==static_cast<std::size_t>(added),
            "tap provenance or lane counters include existing notes");
    for (int lane=targetKeys;lane<10;++lane) require(addedLanes[lane]==0,"tap count escaped requested output field");
    std::set<std::string> originalIds;
    for (const auto& n : source.notes) originalIds.insert(n.id);
    std::map<int,int> windowOriginals, windowGenerated;
    int totalGenerated=0;
    for (const auto& n : after.notes) {
        if (originalIds.count(n.id)) ++windowOriginals[fullFieldTapWindow(n.time)];
        else { ++windowGenerated[fullFieldTapWindow(n.time)]; ++totalGenerated; }
    }
    if (added) require(totalGenerated<=stats.totalAddedBudget,"global generated budget exceeded");
    for (const auto& [time,count] : additions) {
        (void)count;
        const int window=fullFieldTapWindow(time);
        require(windowGenerated[window]<=fullFieldTapPhraseBudget(windowOriginals[window]),"local generated budget exceeded");
    }
    require(stats.candidates<=3*static_cast<std::int64_t>(sourceHeads.size()),"unbounded candidate iteration");
    if (added) require(std::is_sorted(after.notes.begin(),after.notes.end(),[](auto& a,auto& b){return a.time<b.time;}),
                       "boost output must stay chronological for export");
}

void substantialSafeAdditions() {
    auto source=fixture();
    for (int i=0;i<20;++i) source.notes.push_back(tap("n"+std::to_string(i),1000+i*700,i%4));
    auto output=source;
    const auto stats=addEFullFieldTaps(source,output);
    verify(source,source,output,stats);
    require(stats.addedTaps==15 && stats.totalAddedBudget==15,"spare chart should reach 75% added budget");
    auto repeat=source; const auto again=addEFullFieldTaps(source,repeat);
    require(exportOsu(output,10)==exportOsu(repeat,10) && again.addedTaps==stats.addedTaps &&
            again.candidates==stats.candidates,"tap boost is not deterministic");
}

void headAndTailBoundaries() {
    for (bool holds : {false,true}) {
        auto source=fixture(10);
        source.timingPoints.front().beatLength=1000;
        for (int lane=1;lane<10;++lane) {
            auto n=tap("block"+std::to_string(lane),0,lane);
            if (holds) { n.type=NoteType::Hold; n.endTime=1000; }
            source.notes.push_back(n);
        }
        const int boundary=holds?1000:500;
        source.notes.push_back(tap("anchor",boundary,0));
        auto output=source; const auto stats=addEFullFieldTaps(source,output);
        verify(source,source,output,stats);
        require(std::count_if(output.notes.begin(),output.notes.end(),[&](auto& n){return n.time==boundary;})==1,
                "inclusive 500ms guard or LN-tail blocker was bypassed");
    }
}

void tempoPolicyScopeAndBoundaries() {
    for (int boundary : {250,251}) {
        auto anchorSource=fixture(10);
        for (int lane=1;lane<10;++lane) anchorSource.notes.push_back(tap("block"+std::to_string(lane),0,lane));
        anchorSource.notes.push_back(tap("head",boundary,0));
        auto candidate=anchorSource; const auto placed=addEFullFieldTaps(anchorSource,candidate);
        verify(anchorSource,anchorSource,candidate,placed);
        require((boundary==250 && placed.addedTaps==0) || (boundary==251 && placed.addedTaps>0),
                "placement failed inclusive beat-guard boundary");
    }
    auto source=fixture(); source.notes.push_back(tap("original",0,0));
    auto output=source;
    output.notes.push_back(tap("nk2-beat-fullfield-tap-new",250,0));
    TapRepeatGuards guards{{output.notes.back().id,250}};
    require(!auditSafety(source,output,10,500,true,&guards).passed(),"exact beat guard must be rejected");
    output.notes.back().time=251;
    const auto allowed=auditSafety(source,output,10,500,true,&guards);
    require(allowed.passed() && allowed.tempoAllowedTapRepeats==1 &&
            auditSafety(source,output,10).createdLaneRepeats==1,"tempo exception must remain visible to strict audit");
    // Two newly added taps spanning a tempo change respect the slower side.
    source.notes.clear(); output.notes.front().id="nk2-beat-fullfield-tap-older";
    guards[output.notes.front().id]=400; output.notes.back().time=400;
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"tempo change ignored slower preceding tap");
    output.notes.back().time=401;
    require(auditSafety(source,output,10,500,false,&guards).passed(),"valid tempo transition was rejected");
    // IDs alone cannot relax originals, old generated notes or generated LNs.
    source=output; source.notes[0].sourceLane=0; source.notes[1].sourceLane=1;
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"original IDs acquired a new-tap exemption");
    source.notes.clear(); output.notes.front().id="nk2-beat-existing";
    output.notes.back().id="nk2-beat-other";
    guards={{output.notes.front().id,180},{output.notes.back().id,180}};
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"existing generated taps acquired exemption");
    output.notes.back().id="nk2-beat-fullfield-tap-hold";
    output.notes.back().type=NoteType::Hold; output.notes.back().endTime=500;
    guards={{output.notes.back().id,180}};
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"hold acquired tap exemption");
    output.notes.back().type=NoteType::Tap; output.notes.back().endTime.reset();
    for (int invalidGuard : {0,179,501}) {
        guards[output.notes.back().id]=invalidGuard;
        require(!auditSafety(source,output,10,500,false,&guards).passed(),"invalid guard accepted");
    }
    guards[output.notes.back().id]=180;
    output.notes.front().type=NoteType::Hold; output.notes.front().endTime=401;
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"tempo exception bypassed inclusive LN tail");
    output.notes.front().type=NoteType::Tap; output.notes.front().endTime.reset(); output.notes.back().time=0;
    require(!auditSafety(source,output,10,500,false,&guards).passed(),"tempo exception bypassed collision");
}

void finalPhrasesReceiveTheirBudget() {
    auto source=fixture(); source.timingPoints.front().beatLength=360;
    // Each phrase rounds two originals up to two possible generated taps.
    // Without reserving shares, the first 15 phrases exhaust the global 30.
    for (int phrase=0;phrase<20;++phrase) {
        source.notes.push_back(tap("a"+std::to_string(phrase),phrase*2000,0));
        source.notes.push_back(tap("b"+std::to_string(phrase),phrase*2000+700,1));
    }
    auto output=source; const auto stats=addEFullFieldTaps(source,output);
    verify(source,source,output,stats);
    require(stats.addedTaps==30,"phrase quota lost usable generated budget");
    std::set<int> boostedWindows;
    for (const auto& n : output.notes) if (stats.newTapGuards.count(n.id)) boostedWindows.insert(fullFieldTapWindow(n.time));
    require(boostedWindows.size()==20 && boostedWindows.count(19),"early phrases starved final phrase of additions");
}

void timingFallbackAndDenseLaneOpportunities() {
    auto source=fixture(); source.notes.push_back(tap("anchor",1000,0));
    const auto guardAtAnchor=[&](int expected) {
        auto output=source; const auto stats=addEFullFieldTaps(source,output);
        verify(source,source,output,stats);
        require(stats.addedTaps==1 && stats.minTapGuardMs==expected && stats.maxTapGuardMs==expected,
                "wrong tempo guard or fallback");
    };
    guardAtAnchor(250);
    TimingPoint point; point.time=500; point.uninherited=false; point.beatLength=-100;
    source.timingPoints.push_back(point); guardAtAnchor(250);
    point.uninherited=true;
    for (double beat : {0.0,-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        point.beatLength=beat; source.timingPoints.back()=point; guardAtAnchor(500);
    }
    point.beatLength=360.1; source.timingPoints.back()=point; guardAtAnchor(181);
    point.beatLength=1; source.timingPoints.back()=point; guardAtAnchor(180);
    point.beatLength=std::numeric_limits<double>::max(); source.timingPoints.back()=point; guardAtAnchor(500);
    source.timingPoints.clear(); guardAtAnchor(500);
    point.time=1001; point.beatLength=400; source.timingPoints.push_back(point); guardAtAnchor(500);
    point.time=1000; source.timingPoints.push_back(point); guardAtAnchor(200);
    point.beatLength=800; source.timingPoints.push_back(point); guardAtAnchor(400);

    source=fixture(); source.timingPoints.front().beatLength=320;
    for (int i=0;i<160;++i) source.notes.push_back(tap("stream"+std::to_string(i),i*80,i%4));
    auto base=source;
    for (std::size_t i=0;i<base.notes.size();++i) base.notes[i].lane=static_cast<int>(i%10);
    auto dense=base; const auto stats=addEFullFieldTaps(source,dense); verify(source,base,dense,stats);
    source.timingPoints.clear(); auto strict=base;
    const auto fallback=addEFullFieldTaps(source,strict); verify(source,base,strict,fallback);
    require(stats.addedTaps>fallback.addedTaps && stats.tempoAllowedTapRepeats>0,
            "beat-aware policy did not improve the synthetic stream's addition count");
    for (auto count : stats.addedLaneCounts) require(count>0,"a physical lane received no new taps despite opportunity");
    auto burstSource=fixture(); burstSource.timingPoints.front().beatLength=360;
    for (int onset=0;onset<100;++onset)
        for (int lane=0;lane<4;++lane)
            burstSource.notes.push_back(tap("burst"+std::to_string(onset)+"-"+std::to_string(lane),onset*100,lane));
    auto burst=burstSource; const auto burstStats=addEFullFieldTaps(burstSource,burst);
    verify(burstSource,burstSource,burst,burstStats);
    std::map<int,int> burstAdditions;
    for (const auto& n : burst.notes) if (burstStats.newTapGuards.count(n.id)) ++burstAdditions[fullFieldTapWindow(n.time)];
    require(burstAdditions.size()==5,"dense chord-stream fixture lost a phrase");
    for (auto [window,count] : burstAdditions) {
        (void)window;
        require(count==40,"dense chord stream did not reach its safe local cap");
    }
}

void emptyFullAndAlreadyAtBudget() {
    auto empty=fixture(), output=empty;
    verify(empty,empty,output,addEFullFieldTaps(empty,output));
    auto full=fixture(10);
    for (int lane=0;lane<10;++lane) full.notes.push_back(tap(std::to_string(lane),0,lane));
    output=full; const auto stats=addEFullFieldTaps(full,output);
    verify(full,full,output,stats); require(stats.addedTaps==0,"full chord must stay intact");
    auto source=fixture(); source.notes.push_back(tap("one",1000,0));
    auto baseline=source; baseline.notes.push_back(tap("nk2-beat-existing",1000,1));
    output=baseline; const auto capped=addEFullFieldTaps(source,output);
    verify(source,baseline,output,capped);
    require(capped.addedTaps==0 && exportOsu(baseline,10)==exportOsu(output,10),
            "existing generated notes must consume the total added budget");
}

void generatedHeadsAreNotNewRhythmAnchors() {
    auto source=fixture();
    for (int t : {1000,3000,5000}) source.notes.push_back(tap(std::to_string(t),t,0));
    auto baseline=source; baseline.notes.push_back(tap("nk2-beat-existing",2000,1));
    auto output=baseline; const auto stats=addEFullFieldTaps(source,output);
    verify(source,baseline,output,stats); require(stats.addedTaps>0,"fixture must exercise real additions");
    require(std::count_if(output.notes.begin(),output.notes.end(),[](auto& n){return n.time==2000;})==1,
            "generated-only onset became an extra chord anchor");
}

void extremeTimesAndInvalidInput() {
    auto source=fixture();
    for (int t : {std::numeric_limits<int>::min(),-1,0,std::numeric_limits<int>::max()})
        source.notes.push_back(tap(std::to_string(t),t, t==0?1:0));
    auto output=source; const auto stats=addEFullFieldTaps(source,output); verify(source,source,output,stats);
    require(fullFieldTapWindow(-1)==-1 && fullFieldTapWindow(-2000)==-1 && fullFieldTapWindow(0)==0,
            "negative phrase windows must use floor");
    auto invalid=source; invalid.notes.front().lane=10; const auto before=invalid;
    const auto rejected=addEFullFieldTaps(source,invalid);
    require(rejected.reverted && rejected.addedTaps==0 && exportOsu(before,10)==exportOsu(invalid,10),
            "invalid baseline must be rejected without mutation");
}

void allKeysAndGeneratedLnsStayUnchanged() {
    for (int target : {8,10}) for (int keys=1;keys<=18;++keys) {
        auto source=fixture(keys);
        for (int i=0;i<40;++i) {
            auto n=tap("s"+std::to_string(i),i*700,i%keys);
            if (i%8==0) { n.type=NoteType::Hold; n.endTime=n.time+600; }
            source.notes.push_back(n);
        }
        auto options=eOptions(keys);
        options.targetKeyCount=target;
        const auto base=convertChart(source,options);
        require(!base.report.fullFieldTapBoost,"tap boost must be opt-in");
        options.eFullFieldTapBoost=true;
        const auto boosted=convertChart(source,options);
        require(boosted.report.fullFieldTapBoost.has_value(),"tap boost report missing");
        verify(source,base.chart,boosted.chart,*boosted.report.fullFieldTapBoost,target);
        require(boosted.chart.meta.targetKeyCount==target && boosted.chart.meta.version->find(std::to_string(target)+"K")!=std::string::npos,
                "tap mode output metadata used the wrong key count");
        require(boosted.report.addedNotes==base.report.addedNotes+boosted.report.fullFieldTapBoost->addedTaps &&
                boosted.report.addedHoldNotes==base.report.addedHoldNotes && boosted.report.independentSafety->passed(),
                "conversion report must retain base additions/LNs and count appended taps exactly once");
        const auto repeated=convertChart(source,options);
        require(exportOsu(boosted.chart,target)==exportOsu(repeated.chart,target) &&
                reportToJson(boosted.report)==reportToJson(repeated.report),"boosted conversion/report is nondeterministic");
    }
    auto options=eOptions(4); options.eFullField=false; options.eFullFieldTapBoost=true;
    bool rejected=false;
    try { (void)convertChart(fixture(),options); } catch(const std::runtime_error&) { rejected=true; }
    require(rejected,"tap boost without E+ must be rejected");
}

void eightKeyTapFieldLimits() {
    auto source=fixture(); source.timingPoints.front().beatLength=320;
    for (int i=0;i<160;++i) source.notes.push_back(tap("stream"+std::to_string(i),i*80,i%4));
    auto base=source;
    for (std::size_t i=0;i<base.notes.size();++i) base.notes[i].lane=static_cast<int>(i%8);
    auto output=base;
    const auto stats=addEFullFieldTaps(source,output,8);
    verify(source,base,output,stats,8);
    require(stats.addedTaps>0 && stats.tempoAllowedTapRepeats>0,"8K dense stream did not gain beat-spaced taps");
    for (int lane=0;lane<8;++lane) require(stats.addedLaneCounts[lane]>0,"8K omitted an available physical lane");
    auto full=fixture(8);
    for (int lane=0;lane<8;++lane) full.notes.push_back(tap(std::to_string(lane),0,lane));
    output=full; const auto capped=addEFullFieldTaps(full,output,8);
    verify(full,full,output,capped,8);
    require(capped.addedTaps==0,"full 8K chord grew into a nonexistent 9th/10th lane");
    auto invalid=full; invalid.notes.front().lane=8;
    const auto before=exportOsu(invalid,10);
    require(addEFullFieldTaps(full,invalid,8).reverted && exportOsu(invalid,10)==before,
            "8K tap boost accepted invalid baseline or mutated it");
    auto empty=fixture();
    require(!addEFullFieldTaps(empty,empty,8).reverted,"8K empty tap boost failed");
    require(addEFullFieldTaps(empty,empty,18).reverted,"unsupported tap helper target accepted");
}
}  // namespace

int main() {
    try {
        substantialSafeAdditions(); headAndTailBoundaries(); emptyFullAndAlreadyAtBudget();
        generatedHeadsAreNotNewRhythmAnchors(); extremeTimesAndInvalidInput(); allKeysAndGeneratedLnsStayUnchanged();
        tempoPolicyScopeAndBoundaries(); timingFallbackAndDenseLaneOpportunities();
        finalPhrasesReceiveTheirBudget();
        eightKeyTapFieldLimits();
        std::cout<<"10 E+ tap-boost test groups passed\n";
    } catch(const std::exception& e) { std::cerr<<"[fail] "<<e.what()<<'\n'; return 1; }
}
