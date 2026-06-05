#include "core/gesture.hpp"

#include "core/mapping.hpp"
#include "core/repeat.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace keyconv {

namespace {

struct PhraseNote {
    std::size_t noteIndex = 0;
    std::string id;
    int sourceLane = 0;
    int time = 0;
};

struct FullFieldHandBalance {
    int leftNotes = 0;
    int rightNotes = 0;
};

int signOf(int value) {
    if (value > 0) {
        return 1;
    }
    if (value < 0) {
        return -1;
    }
    return 0;
}

double averageLane(const std::vector<PhraseNote>& notes) {
    if (notes.empty()) {
        return 0.0;
    }
    int total = 0;
    for (const auto& note : notes) {
        total += note.sourceLane;
    }
    return static_cast<double>(total) / static_cast<double>(notes.size());
}

bool useDualFiveSplit(int sourceKeyCount, int targetKeyCount) {
    return sourceKeyCount == 7 && targetKeyCount == 10;
}

std::pair<int, int> dualFiveZoneForPhrase(const std::vector<PhraseNote>& notes) {
    if (notes.empty()) {
        return {0, 4};
    }

    const double center = averageLane(notes);
    if (center <= 3.0) {
        return {0, 4};
    }
    return {5, 9};
}

PhraseRole phraseRoleFor(const std::vector<PhraseNote>& notes,
                         int sourceKeyCount,
                         int targetKeyCount,
                         bool fullTenKeyGestureZone) {
    if (fullTenKeyGestureZone || !useDualFiveSplit(sourceKeyCount, targetKeyCount) || notes.empty()) {
        return PhraseRole::Neutral;
    }

    const auto zone = dualFiveZoneForPhrase(notes);
    return zone.first < 5 ? PhraseRole::LeftHandVoice : PhraseRole::RightHandVoice;
}

std::pair<int, int> targetZoneFor(const std::vector<PhraseNote>& notes,
                                  int sourceKeyCount,
                                  int targetKeyCount,
                                  bool fullTenKeyGestureZone = false) {
    if (targetKeyCount <= 1) {
        return {0, 0};
    }
    if (notes.empty()) {
        return {0, targetKeyCount - 1};
    }
    if (fullTenKeyGestureZone) {
        return {0, targetKeyCount - 1};
    }
    if (useDualFiveSplit(sourceKeyCount, targetKeyCount)) {
        return dualFiveZoneForPhrase(notes);
    }

    const double center = averageLane(notes);
    const double sourceMid = static_cast<double>(std::max(0, sourceKeyCount - 1)) / 2.0;
    const double centerSlack = 0.75;
    if (std::abs(center - sourceMid) <= centerSlack) {
        return {0, targetKeyCount - 1};
    }

    const int mid = std::max(1, targetKeyCount / 2);
    if (center < sourceMid) {
        return {0, std::max(0, mid - 1)};
    }
    return {std::min(mid, targetKeyCount - 1), targetKeyCount - 1};
}

int laneInDualFiveZone(int sourceLane, int zoneStart, int zoneEnd, int targetKeyCount) {
    const bool leftZone = zoneStart < 5;
    const int sourceMin = leftZone ? 0 : 3;
    const int sourceMax = leftZone ? 3 : 6;
    const double t = static_cast<double>(clampInt(sourceLane, sourceMin, sourceMax) - sourceMin) /
                     static_cast<double>(std::max(1, sourceMax - sourceMin));
    const int lane = static_cast<int>(std::lround(static_cast<double>(zoneStart) +
                                                  t * static_cast<double>(zoneEnd - zoneStart)));
    return clampInt(lane, 0, targetKeyCount - 1);
}

int laneInZoneBySourcePosition(int sourceLane,
                               int sourceMin,
                               int sourceMax,
                               int zoneStart,
                               int zoneEnd,
                               int targetKeyCount) {
    if (targetKeyCount <= 1) {
        return 0;
    }
    if (zoneStart > zoneEnd) {
        return mapLaneDirect(sourceLane, sourceMax + 1, targetKeyCount);
    }
    if (sourceMax <= sourceMin || zoneEnd <= zoneStart) {
        return clampInt((zoneStart + zoneEnd) / 2, 0, targetKeyCount - 1);
    }

    const double t = static_cast<double>(sourceLane - sourceMin) / static_cast<double>(sourceMax - sourceMin);
    const int lane = static_cast<int>(std::lround(static_cast<double>(zoneStart) +
                                                  t * static_cast<double>(zoneEnd - zoneStart)));
    return clampInt(lane, 0, targetKeyCount - 1);
}

std::vector<PhraseNote> phraseNotesForToken(const Chart& chart,
                                            const std::vector<TimeSlice>& slices,
                                            const PatternToken& token) {
    std::vector<PhraseNote> notes;
    if (token.startSlice < 0 || token.endSlice < token.startSlice ||
        token.startSlice >= static_cast<int>(slices.size())) {
        return notes;
    }

    const int end = std::min(token.endSlice, static_cast<int>(slices.size()) - 1);
    for (int sliceIndex = token.startSlice; sliceIndex <= end; ++sliceIndex) {
        for (const auto noteIndex : slices[static_cast<std::size_t>(sliceIndex)].noteIndices) {
            if (noteIndex >= chart.notes.size()) {
                continue;
            }
            const auto& note = chart.notes[noteIndex];
            if (note.id.empty()) {
                continue;
            }
            PhraseNote phraseNote;
            phraseNote.noteIndex = noteIndex;
            phraseNote.id = note.id;
            phraseNote.sourceLane = note.sourceLane.value_or(note.lane);
            phraseNote.time = note.time;
            notes.push_back(std::move(phraseNote));
        }
    }
    return notes;
}

bool isGestureToken(PatternKind kind) {
    return kind == PatternKind::StairUp || kind == PatternKind::StairDown || kind == PatternKind::Trill ||
           kind == PatternKind::Jack;
}

void addStairHints(GestureRail& rail,
                   const std::vector<PhraseNote>& notes,
                   const PatternToken& token,
                   int motifId,
                   int sourceKeyCount,
                   int targetKeyCount,
                   bool fullTenKeyGestureZone) {
    if (notes.empty()) {
        return;
    }

    const auto [sourceMinIt, sourceMaxIt] = std::minmax_element(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
        return a.sourceLane < b.sourceLane;
    });
    const auto [zoneStart, zoneEnd] =
        targetZoneFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    const auto role = phraseRoleFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    for (const auto& note : notes) {
        GestureHint hint;
        hint.motifId = motifId;
        hint.kind = token.kind;
        hint.role = role;
        hint.preferredLane = useDualFiveSplit(sourceKeyCount, targetKeyCount) && !fullTenKeyGestureZone
                                  ? laneInDualFiveZone(note.sourceLane, zoneStart, zoneEnd, targetKeyCount)
                                  : laneInZoneBySourcePosition(note.sourceLane,
                                                              sourceMinIt->sourceLane,
                                                              sourceMaxIt->sourceLane,
                                                              zoneStart,
                                                              zoneEnd,
                                                              targetKeyCount);
        hint.zoneStart = zoneStart;
        hint.zoneEnd = zoneEnd;
        hint.direction = token.direction;
        rail.hintsByNoteId[note.id] = hint;
    }
}

void addTrillHints(GestureRail& rail,
                   const std::vector<PhraseNote>& notes,
                   int motifId,
                   int sourceKeyCount,
                   int targetKeyCount,
                   bool fullTenKeyGestureZone) {
    if (notes.size() < 2 || targetKeyCount <= 1) {
        return;
    }

    const auto [zoneStart, zoneEnd] =
        targetZoneFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    const auto role = phraseRoleFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    std::vector<int> sourceLanes;
    for (const auto& note : notes) {
        if (std::find(sourceLanes.begin(), sourceLanes.end(), note.sourceLane) == sourceLanes.end()) {
            sourceLanes.push_back(note.sourceLane);
        }
    }
    std::sort(sourceLanes.begin(), sourceLanes.end());
    if (sourceLanes.size() < 2) {
        return;
    }

    int first = clampInt(mapLaneDirect(sourceLanes.front(), sourceKeyCount, targetKeyCount), 0, targetKeyCount - 1);
    int second = clampInt(mapLaneDirect(sourceLanes.back(), sourceKeyCount, targetKeyCount), 0, targetKeyCount - 1);
    if (zoneStart <= zoneEnd) {
        first = clampInt(first, zoneStart, zoneEnd);
        second = clampInt(second, zoneStart, zoneEnd);
    }
    if (std::abs(second - first) > 2 || first == second) {
        const int center = clampInt(static_cast<int>(std::lround((static_cast<double>(first) + second) / 2.0)),
                                    0,
                                    targetKeyCount - 1);
        first = clampInt(center - 1, zoneStart, zoneEnd);
        second = clampInt(first + 1, zoneStart, zoneEnd);
        if (first == second) {
            first = clampInt(second - 1, 0, targetKeyCount - 1);
        }
    }
    if (first > second) {
        std::swap(first, second);
    }

    for (const auto& note : notes) {
        GestureHint hint;
        hint.motifId = motifId;
        hint.kind = PatternKind::Trill;
        hint.role = role;
        hint.preferredLane = note.sourceLane == sourceLanes.front() ? first : second;
        hint.zoneStart = std::min(first, second);
        hint.zoneEnd = std::max(first, second);
        rail.hintsByNoteId[note.id] = hint;
    }
}

void addJackHints(GestureRail& rail,
                  const std::vector<PhraseNote>& notes,
                  int motifId,
                  int sourceKeyCount,
                  int targetKeyCount,
                  bool fullTenKeyGestureZone) {
    if (notes.empty()) {
        return;
    }

    const int sourceLane = notes.front().sourceLane;
    const auto [zoneStart, zoneEnd] =
        targetZoneFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    const auto role = phraseRoleFor(notes, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    const int motifHitCount = static_cast<int>(notes.size());
    const int motifDurationMs = motifHitCount >= 2 ? notes.back().time - notes.front().time : 0;
    const bool longJack = motifHitCount >= 5;
    const int preferred = useDualFiveSplit(sourceKeyCount, targetKeyCount) && !fullTenKeyGestureZone
                              ? laneInDualFiveZone(sourceLane, zoneStart, zoneEnd, targetKeyCount)
                              : mapLaneDirect(sourceLane, sourceKeyCount, targetKeyCount);
    int jackZoneStart = clampInt(preferred, zoneStart, zoneEnd);
    int jackZoneEnd = jackZoneStart;
    if (!longJack && targetKeyCount > 1) {
        if (jackZoneStart + 1 <= zoneEnd) {
            jackZoneEnd = jackZoneStart + 1;
        } else if (jackZoneStart - 1 >= zoneStart) {
            --jackZoneStart;
        }
    }
    for (const auto& note : notes) {
        GestureHint hint;
        hint.motifId = motifId;
        hint.kind = PatternKind::Jack;
        hint.role = role;
        hint.preferredLane = clampInt(preferred, 0, targetKeyCount - 1);
        hint.zoneStart = jackZoneStart;
        hint.zoneEnd = jackZoneEnd;
        hint.motifHitCount = motifHitCount;
        hint.motifDurationMs = motifDurationMs;
        rail.hintsByNoteId[note.id] = hint;
    }
}

std::map<std::string, const Note*> sourceNotesById(const std::vector<Note>& notes) {
    std::map<std::string, const Note*> result;
    for (const auto& note : notes) {
        if (!note.id.empty()) {
            result[note.id] = &note;
        }
    }
    return result;
}

std::vector<PhraseNote> phraseNotesForJackGroup(const Chart& chart, const JackGroup& group) {
    const auto sourceById = sourceNotesById(chart.notes);
    std::vector<PhraseNote> notes;
    notes.reserve(group.noteIds.size());
    for (const auto& id : group.noteIds) {
        const auto found = sourceById.find(id);
        if (found == sourceById.end()) {
            continue;
        }
        PhraseNote phraseNote;
        phraseNote.id = found->second->id;
        phraseNote.sourceLane = found->second->sourceLane.value_or(found->second->lane);
        phraseNote.time = found->second->time;
        notes.push_back(std::move(phraseNote));
    }
    std::stable_sort(notes.begin(), notes.end(), [](const PhraseNote& lhs, const PhraseNote& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        return lhs.id < rhs.id;
    });
    return notes;
}

void addSourceJackGroupHints(GestureRail& rail,
                             const Chart& chart,
                             int sourceKeyCount,
                             int targetKeyCount,
                             int jackWindowMs,
                             int& motifId,
                             bool fullTenKeyGestureZone) {
    const auto groups = detectJackGroups(chart.notes, RepeatLaneMode::SourceLane, jackWindowMs);
    for (const auto& group : groups) {
        bool anyAlreadyHinted = false;
        for (const auto& id : group.noteIds) {
            if (rail.hintsByNoteId.count(id) > 0) {
                anyAlreadyHinted = true;
                break;
            }
        }
        if (anyAlreadyHinted) {
            continue;
        }

        auto notes = phraseNotesForJackGroup(chart, group);
        if (notes.size() < 3) {
            continue;
        }
        addJackHints(rail, notes, motifId++, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
    }
}

std::pair<int, int> fullFieldHandZone(bool leftHand) {
    return leftHand ? std::pair<int, int>{0, 4} : std::pair<int, int>{5, 9};
}

PhraseRole fullFieldRole(bool leftHand) {
    return leftHand ? PhraseRole::LeftHandVoice : PhraseRole::RightHandVoice;
}

int fullFieldPreferredLane(int sourceLane,
                           int sourceMin,
                           int sourceMax,
                           int zoneStart,
                           int zoneEnd,
                           int targetKeyCount) {
    return laneInZoneBySourcePosition(sourceLane,
                                      sourceMin,
                                      sourceMax,
                                      zoneStart,
                                      zoneEnd,
                                      targetKeyCount);
}

std::pair<int, int> sourceRangeForNotes(const std::vector<PhraseNote>& notes) {
    if (notes.empty()) {
        return {0, 0};
    }
    const auto [minIt, maxIt] = std::minmax_element(notes.begin(), notes.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.sourceLane < rhs.sourceLane;
    });
    return {minIt->sourceLane, maxIt->sourceLane};
}

std::vector<PhraseNote> sortedPhraseNotes(std::vector<PhraseNote> notes) {
    std::stable_sort(notes.begin(), notes.end(), [](const PhraseNote& lhs, const PhraseNote& rhs) {
        if (lhs.time != rhs.time) {
            return lhs.time < rhs.time;
        }
        if (lhs.sourceLane != rhs.sourceLane) {
            return lhs.sourceLane < rhs.sourceLane;
        }
        return lhs.id < rhs.id;
    });
    return notes;
}

bool fullFieldChordShouldSplit(const std::vector<PhraseNote>& notes, PatternKind kind) {
    if (kind == PatternKind::Chord || kind == PatternKind::AnchorLn || kind == PatternKind::ReleaseLn) {
        return true;
    }
    if (notes.size() < 2) {
        return false;
    }

    std::map<int, std::vector<int>> lanesByTime;
    for (const auto& note : notes) {
        lanesByTime[note.time].push_back(note.sourceLane);
    }
    for (auto& [time, lanes] : lanesByTime) {
        (void)time;
        if (lanes.size() < 2) {
            continue;
        }
        const auto [minIt, maxIt] = std::minmax_element(lanes.begin(), lanes.end());
        if (*maxIt - *minIt >= 3) {
            return true;
        }
    }
    return false;
}

bool fullFieldTokenShouldAlternateHands(PatternKind kind, std::size_t noteCount) {
    if (kind == PatternKind::Stream || kind == PatternKind::Burst) {
        return true;
    }
    if ((kind == PatternKind::StairUp || kind == PatternKind::StairDown) && noteCount >= 6) {
        return true;
    }
    return false;
}

bool chooseFullFieldPrimaryLeft(const FullFieldHandBalance& balance) {
    return balance.leftNotes <= balance.rightNotes;
}

void recordFullFieldHand(FullFieldHandBalance& balance, bool leftHand, int count = 1) {
    if (leftHand) {
        balance.leftNotes += count;
    } else {
        balance.rightNotes += count;
    }
}

void addFullFieldHint(GestureRail& rail,
                      const PhraseNote& note,
                      PatternKind kind,
                      int motifId,
                      bool leftHand,
                      int sourceKeyCount,
                      int targetKeyCount,
                      int direction,
                      int motifHitCount,
                      int motifDurationMs,
                      int sourceMin = 0,
                      int sourceMax = -1) {
    const auto [zoneStart, zoneEnd] = fullFieldHandZone(leftHand);
    if (sourceMax < sourceMin) {
        sourceMax = std::max(0, sourceKeyCount - 1);
    }
    GestureHint hint;
    hint.motifId = motifId;
    hint.kind = kind;
    hint.role = fullFieldRole(leftHand);
    hint.preferredLane =
        fullFieldPreferredLane(note.sourceLane, sourceMin, sourceMax, zoneStart, zoneEnd, targetKeyCount);
    hint.zoneStart = zoneStart;
    hint.zoneEnd = zoneEnd;
    hint.direction = direction;
    hint.motifHitCount = motifHitCount;
    hint.motifDurationMs = motifDurationMs;
    rail.hintsByNoteId[note.id] = hint;
}

void addFullFieldTokenHints(GestureRail& rail,
                            std::vector<PhraseNote> notes,
                            const PatternToken& token,
                            int motifId,
                            int sourceKeyCount,
                            int targetKeyCount,
                            FullFieldHandBalance& balance) {
    notes = sortedPhraseNotes(std::move(notes));
    if (notes.empty()) {
        return;
    }

    const bool primaryLeft = chooseFullFieldPrimaryLeft(balance);
    const int motifHitCount = static_cast<int>(notes.size());
    const int motifDurationMs = motifHitCount >= 2 ? notes.back().time - notes.front().time : 0;
    const auto [phraseSourceMin, phraseSourceMax] = sourceRangeForNotes(notes);
    if (token.kind == PatternKind::Jack) {
        for (const auto& note : notes) {
            addFullFieldHint(rail,
                             note,
                             token.kind,
                             motifId,
                             primaryLeft,
                             sourceKeyCount,
                             targetKeyCount,
                             token.direction,
                             motifHitCount,
                             motifDurationMs,
                             phraseSourceMin,
                             phraseSourceMax);
        }
        recordFullFieldHand(balance, primaryLeft, motifHitCount);
        return;
    }

    if (token.kind == PatternKind::Trill) {
        for (std::size_t index = 0; index < notes.size(); ++index) {
            const bool leftHand = (index % 2 == 0) ? primaryLeft : !primaryLeft;
            addFullFieldHint(rail,
                             notes[index],
                             token.kind,
                             motifId,
                             leftHand,
                             sourceKeyCount,
                             targetKeyCount,
                             token.direction,
                             motifHitCount,
                             motifDurationMs,
                             phraseSourceMin,
                             phraseSourceMax);
            recordFullFieldHand(balance, leftHand);
        }
        return;
    }

    if (fullFieldChordShouldSplit(notes, token.kind)) {
        const double sourceMid = static_cast<double>(std::max(0, sourceKeyCount - 1)) / 2.0;
        std::vector<PhraseNote> leftNotes;
        std::vector<PhraseNote> rightNotes;
        for (const auto& note : notes) {
            (static_cast<double>(note.sourceLane) <= sourceMid ? leftNotes : rightNotes).push_back(note);
        }
        const auto [leftSourceMin, leftSourceMax] = sourceRangeForNotes(leftNotes);
        const auto [rightSourceMin, rightSourceMax] = sourceRangeForNotes(rightNotes);
        for (const auto& note : notes) {
            const bool leftHand = static_cast<double>(note.sourceLane) <= sourceMid;
            addFullFieldHint(rail,
                             note,
                             token.kind,
                             motifId,
                             leftHand,
                             sourceKeyCount,
                             targetKeyCount,
                             token.direction,
                             motifHitCount,
                             motifDurationMs,
                             leftHand ? leftSourceMin : rightSourceMin,
                             leftHand ? leftSourceMax : rightSourceMax);
            recordFullFieldHand(balance, leftHand);
        }
        return;
    }

    if (fullFieldTokenShouldAlternateHands(token.kind, notes.size())) {
        for (std::size_t index = 0; index < notes.size(); ++index) {
            const bool leftHand = (index % 2 == 0) ? primaryLeft : !primaryLeft;
            addFullFieldHint(rail,
                             notes[index],
                             token.kind,
                             motifId,
                             leftHand,
                             sourceKeyCount,
                             targetKeyCount,
                             token.direction,
                             motifHitCount,
                             motifDurationMs,
                             phraseSourceMin,
                             phraseSourceMax);
            recordFullFieldHand(balance, leftHand);
        }
        return;
    }

    for (const auto& note : notes) {
        addFullFieldHint(rail,
                         note,
                         token.kind,
                         motifId,
                         primaryLeft,
                         sourceKeyCount,
                         targetKeyCount,
                         token.direction,
                         motifHitCount,
                         motifDurationMs,
                         phraseSourceMin,
                         phraseSourceMax);
    }
    recordFullFieldHand(balance, primaryLeft, motifHitCount);
}

void addFullFieldSourceJackGroupHints(GestureRail& rail,
                                      const Chart& chart,
                                      int sourceKeyCount,
                                      int targetKeyCount,
                                      int jackWindowMs,
                                      int& motifId,
                                      FullFieldHandBalance& balance) {
    const auto groups = detectJackGroups(chart.notes, RepeatLaneMode::SourceLane, jackWindowMs);
    for (const auto& group : groups) {
        bool anyAlreadyHinted = false;
        for (const auto& id : group.noteIds) {
            if (rail.hintsByNoteId.count(id) > 0) {
                anyAlreadyHinted = true;
                break;
            }
        }
        if (anyAlreadyHinted) {
            continue;
        }

        auto notes = sortedPhraseNotes(phraseNotesForJackGroup(chart, group));
        if (notes.size() < 3) {
            continue;
        }
        const bool primaryLeft = chooseFullFieldPrimaryLeft(balance);
        const int motifHitCount = static_cast<int>(notes.size());
        const int motifDurationMs = notes.back().time - notes.front().time;
        for (const auto& note : notes) {
            addFullFieldHint(rail,
                             note,
                             PatternKind::Jack,
                             motifId,
                             primaryLeft,
                             sourceKeyCount,
                             targetKeyCount,
                             0,
                             motifHitCount,
                             motifDurationMs);
        }
        recordFullFieldHand(balance, primaryLeft, motifHitCount);
        ++motifId;
    }
}

void addFullFieldFallbackHints(GestureRail& rail,
                               const Chart& chart,
                               int sourceKeyCount,
                               int targetKeyCount,
                               int& motifId,
                               FullFieldHandBalance& balance) {
    std::vector<PhraseNote> notes;
    notes.reserve(chart.notes.size());
    for (std::size_t index = 0; index < chart.notes.size(); ++index) {
        const auto& note = chart.notes[index];
        if (note.id.empty() || rail.hintsByNoteId.count(note.id) > 0) {
            continue;
        }
        PhraseNote phraseNote;
        phraseNote.noteIndex = index;
        phraseNote.id = note.id;
        phraseNote.sourceLane = note.sourceLane.value_or(note.lane);
        phraseNote.time = note.time;
        notes.push_back(std::move(phraseNote));
    }

    for (const auto& note : sortedPhraseNotes(std::move(notes))) {
        const bool primaryLeft = chooseFullFieldPrimaryLeft(balance);
        addFullFieldHint(rail,
                         note,
                         PatternKind::Single,
                         motifId++,
                         primaryLeft,
                         sourceKeyCount,
                         targetKeyCount,
                         0,
                         1,
                         0);
        recordFullFieldHand(balance, primaryLeft);
    }
}

std::map<std::string, Note> notesById(const std::vector<Note>& notes) {
    std::map<std::string, Note> result;
    for (const auto& note : notes) {
        if (!note.id.empty()) {
            result[note.id] = note;
        }
    }
    return result;
}

std::vector<Note> convertedNotesForToken(const Chart& original,
                                         const Chart& converted,
                                         const std::vector<TimeSlice>& slices,
                                         const PatternToken& token) {
    const auto convertedById = notesById(converted.notes);
    std::vector<Note> result;
    for (const auto& phraseNote : phraseNotesForToken(original, slices, token)) {
        const auto found = convertedById.find(phraseNote.id);
        if (found != convertedById.end()) {
            result.push_back(found->second);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.id < b.id;
    });
    return result;
}

std::vector<Note> convertedNotesForIds(const Chart& converted, const std::vector<std::string>& ids) {
    const auto convertedById = notesById(converted.notes);
    std::vector<Note> result;
    for (const auto& id : ids) {
        const auto found = convertedById.find(id);
        if (found != convertedById.end()) {
            result.push_back(found->second);
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.time != b.time) {
            return a.time < b.time;
        }
        return a.id < b.id;
    });
    return result;
}

bool targetZoneMatchesSource(const std::vector<PhraseNote>& sourceNotes,
                             const std::vector<Note>& targetNotes,
                             int sourceKeyCount,
                             int targetKeyCount) {
    if (sourceNotes.empty() || targetNotes.empty() || targetKeyCount <= 1) {
        return true;
    }
    const auto [zoneStart, zoneEnd] = targetZoneFor(sourceNotes, sourceKeyCount, targetKeyCount);
    if (zoneStart == 0 && zoneEnd == targetKeyCount - 1) {
        return true;
    }
    const double averageTarget =
        std::accumulate(targetNotes.begin(), targetNotes.end(), 0.0, [](double total, const Note& note) {
            return total + static_cast<double>(note.lane);
        }) /
        static_cast<double>(targetNotes.size());
    return averageTarget >= static_cast<double>(zoneStart) && averageTarget <= static_cast<double>(zoneEnd);
}

bool stairPreserved(const std::vector<Note>& targetNotes, int sourceDirection, int* directionFlips) {
    if (targetNotes.size() < 2) {
        return true;
    }
    int flips = 0;
    int flatSteps = 0;
    for (std::size_t i = 1; i < targetNotes.size(); ++i) {
        const int targetSign = signOf(targetNotes[i].lane - targetNotes[i - 1].lane);
        if (targetSign == 0) {
            ++flatSteps;
        } else if (sourceDirection != 0 && targetSign != sourceDirection) {
            ++flips;
        }
    }
    if (directionFlips != nullptr) {
        *directionFlips += flips;
    }
    return flips == 0 && flatSteps <= 1;
}

bool trillPreserved(const std::vector<Note>& targetNotes) {
    if (targetNotes.size() < 4) {
        return true;
    }
    std::set<int> unique;
    for (const auto& note : targetNotes) {
        unique.insert(note.lane);
    }
    if (unique.size() > 2) {
        return false;
    }
    if (unique.size() == 2 && (*unique.rbegin() - *unique.begin()) > 3) {
        return false;
    }
    for (std::size_t i = 2; i < targetNotes.size(); ++i) {
        if (targetNotes[i].lane != targetNotes[i - 2].lane) {
            return false;
        }
    }
    return targetNotes[0].lane != targetNotes[1].lane;
}

bool jackPreserved(const std::vector<Note>& targetNotes, bool longJack) {
    if (targetNotes.size() < 2) {
        return true;
    }
    std::set<int> unique;
    for (const auto& note : targetNotes) {
        unique.insert(note.lane);
    }
    if (longJack) {
        return unique.size() == 1;
    }
    return unique.size() <= 2 && (*unique.rbegin() - *unique.begin()) <= 1;
}

bool laneScattered(PatternKind kind, const std::vector<Note>& targetNotes) {
    if (targetNotes.empty()) {
        return false;
    }
    std::set<int> unique;
    for (const auto& note : targetNotes) {
        unique.insert(note.lane);
    }
    const int span = *unique.rbegin() - *unique.begin();
    if (kind == PatternKind::Trill) {
        return unique.size() > 2 || span > 3;
    }
    if (kind == PatternKind::Jack) {
        return unique.size() > 2 || span > 1;
    }
    return span > std::max(3, static_cast<int>(targetNotes.size()));
}

}  // namespace

GestureRail buildGestureRail(const Chart& chart,
                             int sourceKeyCount,
                             int targetKeyCount,
                             int sameTimeEpsilonMs,
                             int jackWindowMs,
                             bool enabled,
                             bool fullTenKeyGestureZone) {
    GestureRail rail;
    rail.enabled = enabled;
    if (!enabled || chart.notes.empty() || sourceKeyCount <= 0 || targetKeyCount <= 0) {
        return rail;
    }

    const auto slices = buildTimeSlices(chart, sourceKeyCount, sameTimeEpsilonMs);
    const auto tokens = detectPatternTokens(slices, jackWindowMs);
    int motifId = 0;
    for (const auto& token : tokens) {
        if (!isGestureToken(token.kind)) {
            continue;
        }
        const auto notes = phraseNotesForToken(chart, slices, token);
        if (notes.size() < 2) {
            continue;
        }
        if (token.kind == PatternKind::StairUp || token.kind == PatternKind::StairDown) {
            addStairHints(rail, notes, token, motifId, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
        } else if (token.kind == PatternKind::Trill) {
            addTrillHints(rail, notes, motifId, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
        } else if (token.kind == PatternKind::Jack) {
            addJackHints(rail, notes, motifId, sourceKeyCount, targetKeyCount, fullTenKeyGestureZone);
        }
        ++motifId;
    }

    addSourceJackGroupHints(rail, chart, sourceKeyCount, targetKeyCount, jackWindowMs, motifId, fullTenKeyGestureZone);

    return rail;
}

GestureRail buildFullFieldRail(const Chart& chart,
                               int sourceKeyCount,
                               int targetKeyCount,
                               int sameTimeEpsilonMs,
                               int jackWindowMs,
                               bool enabled) {
    GestureRail rail;
    rail.enabled = enabled;
    if (!enabled || chart.notes.empty() || sourceKeyCount <= 0 || targetKeyCount != 10) {
        return rail;
    }

    const auto slices = buildTimeSlices(chart, sourceKeyCount, sameTimeEpsilonMs);
    const auto tokens = detectPatternTokens(slices, jackWindowMs);
    int motifId = 0;
    FullFieldHandBalance balance;
    for (const auto& token : tokens) {
        if (token.kind == PatternKind::Single) {
            continue;
        }
        const auto notes = phraseNotesForToken(chart, slices, token);
        if (notes.empty()) {
            continue;
        }
        addFullFieldTokenHints(rail, notes, token, motifId++, sourceKeyCount, targetKeyCount, balance);
    }

    addFullFieldSourceJackGroupHints(rail, chart, sourceKeyCount, targetKeyCount, jackWindowMs, motifId, balance);
    addFullFieldFallbackHints(rail, chart, sourceKeyCount, targetKeyCount, motifId, balance);
    return rail;
}

const GestureHint* findGestureHint(const GestureRail* rail, const std::string& noteId) {
    if (rail == nullptr || !rail->enabled || noteId.empty()) {
        return nullptr;
    }
    const auto found = rail->hintsByNoteId.find(noteId);
    return found == rail->hintsByNoteId.end() ? nullptr : &found->second;
}

GestureReport evaluateGesturePreservation(const Chart& original,
                                          const Chart& converted,
                                          int sourceKeyCount,
                                          int targetKeyCount,
                                          int sameTimeEpsilonMs,
                                          bool gestureRailEnabled,
                                          int jackWindowMs) {
    GestureReport report;
    report.gestureRailEnabled = gestureRailEnabled;

    const auto slices = buildTimeSlices(original, sourceKeyCount, sameTimeEpsilonMs);
    const auto tokens = detectPatternTokens(slices, jackWindowMs);
    int preserved = 0;
    int detected = 0;
    std::set<std::string> evaluatedJackNoteIds;

    for (const auto& token : tokens) {
        if (!isGestureToken(token.kind)) {
            continue;
        }
        const auto sourceNotes = phraseNotesForToken(original, slices, token);
        const auto targetNotes = convertedNotesForToken(original, converted, slices, token);
        if (sourceNotes.size() < 2 || targetNotes.size() < 2) {
            continue;
        }

        if (!targetZoneMatchesSource(sourceNotes, targetNotes, sourceKeyCount, targetKeyCount)) {
            ++report.handZoneBreaks;
        }
        if (laneScattered(token.kind, targetNotes)) {
            ++report.motifLaneScatterCount;
        }

        bool ok = true;
        if (token.kind == PatternKind::StairUp || token.kind == PatternKind::StairDown) {
            ++report.detectedStairs;
            ok = stairPreserved(targetNotes, token.direction, &report.motifDirectionFlips);
            ok ? ++report.preservedStairs : ++report.brokenStairs;
        } else if (token.kind == PatternKind::Trill) {
            ++report.detectedTrills;
            ok = trillPreserved(targetNotes);
            ok ? ++report.preservedTrills : ++report.brokenTrills;
        } else if (token.kind == PatternKind::Jack) {
            ++report.detectedJacks;
            for (const auto& note : sourceNotes) {
                evaluatedJackNoteIds.insert(note.id);
            }
            ok = jackPreserved(targetNotes, sourceNotes.size() >= 5);
            ok ? ++report.preservedJacks : ++report.brokenJacks;
        }

        ++detected;
        if (ok) {
            ++preserved;
        }
    }

    const auto sourceJackGroups = detectJackGroups(original.notes, RepeatLaneMode::SourceLane, jackWindowMs);
    for (const auto& group : sourceJackGroups) {
        bool alreadyEvaluated = false;
        for (const auto& id : group.noteIds) {
            if (evaluatedJackNoteIds.count(id) > 0) {
                alreadyEvaluated = true;
                break;
            }
        }
        if (alreadyEvaluated) {
            continue;
        }

        const auto sourceNotes = phraseNotesForJackGroup(original, group);
        const auto targetNotes = convertedNotesForIds(converted, group.noteIds);
        if (sourceNotes.size() < 2 || targetNotes.size() < 2) {
            continue;
        }

        if (!targetZoneMatchesSource(sourceNotes, targetNotes, sourceKeyCount, targetKeyCount)) {
            ++report.handZoneBreaks;
        }
        if (laneScattered(PatternKind::Jack, targetNotes)) {
            ++report.motifLaneScatterCount;
        }

        ++report.detectedJacks;
        const bool ok = jackPreserved(targetNotes, group.hitCount >= 5);
        ok ? ++report.preservedJacks : ++report.brokenJacks;
        ++detected;
        if (ok) {
            ++preserved;
        }
    }

    report.gesturePreservationScore =
        detected == 0 ? 1.0 : static_cast<double>(preserved) / static_cast<double>(detected);
    return report;
}

}  // namespace keyconv
