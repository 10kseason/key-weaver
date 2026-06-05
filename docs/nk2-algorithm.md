# NK2 Algorithm

This document describes the current NK2 algorithm implementation in KeyWeaver.
It is a code-facing companion to `docs/nk2-design.md`: the design document
states the long-term direction, while this document explains what the current
prototype does, which invariants it preserves, how it ranks lanes, how it adds
support notes, and how to interpret the generated reports.

NK2 is experimental. Classic remains the default engine. NK2 is intended to
eventually become a phrase-aware, target-key-native converter, but the current
implementation is still a bounded prototype with explicit safety gates and
reporting hooks.

## Short Version

NK2 converts a chart by:

1. Building a base report from source intent and target layout.
2. Returning immediately for report-only mode or same-key no-op mode.
3. Sorting notes into deterministic time slices.
4. Classifying each note as a motif-aware placement case.
5. Ranking target lane candidates from native closeness, remix coverage,
   layout pressure, source anchoring, direction continuity, and motif rules.
6. Accepting the first candidate that passes same-time, LN, and no-new-jack
   gates.
7. Optionally adding support taps from LN ends, strong beats, and mirror events.
8. Recomputing safety, distribution, layout, provenance, and phrase-profile
   diagnostics for the final report.

The key idea is that NK2 does not only scale source lanes into target lanes. It
tries to preserve the source chart's readable intent while making the output
use the target key field more like an authored target-key chart.

## Current Implementation Scope

Current source files:

```text
src/nk2/
  intent_graph.hpp/.cpp
  layout_model.hpp/.cpp
  nk2_report.hpp/.cpp
  nk2_convert.hpp/.cpp
```

Current public entrypoints:

```cpp
keyconv::nk2::analyzeReportOnly(const Chart& chart, const NK2Options& options)
keyconv::nk2::convertChart(const Chart& chart, const NK2Options& options)
```

Current supported behavior:

- `--engine nk2 --nk2-mode report` is analysis-only and never mutates chart
  output.
- Same-key conversion is a no-op unless `--nk2-mode transform` is selected.
- Non-report NK2 conversion supports experimental 1K..10K source/target pairs.
- 7K -> 10K uses the dedicated `nk2-7k10k-panel-bridge-fullfield` prototype.
- Other non-same 1K..10K pairs use `nk2-generic-nk-relane-compress`.
- 4K -> 5K is a special generic branch that can add fill/support notes even in
  faithful mode.
- Support-note generation currently runs for 7K -> 10K and 4K -> 5K.
- Support notes are tap-only in the current milestone.

Current non-goals:

- NK2 is not the default engine.
- NK2 does not replace Classic policy locks.
- NK2 does not perform full beam search.
- NK2 does not retime source notes as a normal repair.
- NK2 does not synthesize generated LNs in the current milestone.
- NK2 does not yet use profile scoring to search multiple candidate charts.

## Core Contracts

NK2's safety and identity contracts are deliberately stronger than its style
ambitions.

Hard or near-hard contracts:

- Preserve source note timing.
- Preserve source LN start and end times.
- Keep output lanes inside the target key field.
- Avoid same-time same-lane collisions.
- Avoid active-LN body conflicts.
- Avoid created target jacks when a safe alternative exists.
- Preserve source jacks as source jacks instead of counting them as created
  target jacks.
- Drop only when the strict generic path cannot place a note safely.
- Keep generated support notes tap-only.
- Keep generated support notes traceable by ID and report provenance.

Style contracts:

- Keep source pattern identity visible.
- Let high-key output use more of the target field.
- Prefer target-native panel, bridge, and whole-field coverage when safe.
- Let neutral taps and LN anchors move more freely than source jacks, trills,
  stairs, and chords.
- Add support notes only from source-related anchors.

## Options

NK2 conversion is controlled by `NK2Options`.

```cpp
struct NK2Options {
    int sourceKeyCount = 0;
    int targetKeyCount = 0;
    Mode mode = Mode::Native;
    double nativeWeight = 0.5;
    double remixWeight = 0.5;
    LayoutWeights layoutWeights;
    int sameTimeEpsilonMs = 2;
};
```

Modes:

- `native`: default NK2 feel target. It balances source identity with target-key
  authorship and allows limited support notes where enabled.
- `faithful`: preservation-heavy mode. For normal NK2 paths it disables support
  notes; 4K -> 5K is the current exception because that branch exists to fill
  the added lane.
- `harder`: allows a higher support-note budget.
- `transform`: reserved for same-key transform behavior.
- `report`: analysis-only; no chart mutation.

Default blend:

```text
nativeWeight = 0.5
remixWeight  = 0.5
```

Default layout weights:

```text
panel     = 3
bridge    = 2
fullField = 6
```

These weights do not directly mean "put 3 notes here, 2 notes there, 6 notes
there." They define the relative pressure used by lane ranking and distribution
diagnostics.

## Pipeline

The current `convertChart()` flow is:

```text
input Chart + NK2Options
  -> buildBaseReport()
  -> if mode == report: return original chart + report
  -> if same-key no-op: fill safety/distribution report and return original chart
  -> check supported prototype
  -> set output target key count and NK2 difficulty marker
  -> clear output notes
  -> build deterministic source time slices
  -> for each slice:
       track occupied target lanes inside this slice
       classify each note motif
       rank target lane candidates
       accept a safe lane or drop/fallback depending on prototype
       update lane usage, placed notes, source-lane memory, motif counters
  -> sort converted notes
  -> set prototype name
  -> maybe apply support notes
  -> recompute distribution and safety metrics
  -> attach warnings for failed or degraded gates
  -> return converted chart + NK2 report
```

`analyzeReportOnly()` is a narrower path:

```text
input Chart + NK2Options
  -> buildBaseReport()
  -> add warning if the caller used analysis-only outside report mode
  -> return report
```

## Base Report

`buildBaseReport()` is the first step for both analysis and conversion. It
records:

- requested options
- target layout summary
- intent graph summary
- initial output note count
- same-key no-op status
- non-default native/remix weight warning

Same-key rule:

```text
if sourceKeyCount == targetKeyCount and mode != transform:
    noOp = true
```

This is intentional. Same-key `native`, `faithful`, and `harder` do not invent
changes. A same-key chart is already native for its key count unless the user
explicitly requests a transform.

## Intent Graph Summary

`intent_graph.*` currently builds a summary, not a full graph object. It still
captures the source features NK2 needs for reporting and support planning:

- source key count
- target key count
- total notes
- tap notes
- hold notes
- time slices
- chord slices
- jack motifs
- trill motifs
- stair motifs
- stream motifs
- LN anchors
- strong-beat anchors
- mirror support candidates
- recognizability anchors
- average local NPS

The intent pass sorts notes by:

```text
time, source lane, id
```

Then it builds slices with `sameTimeEpsilonMs`, default `2 ms`.

### Strong Beat Detection

Strong beat detection uses timing points. The current beat length defaults to
`500 ms` until an uninherited positive timing point is found. A time is treated
as a strong beat when its beat position is within `0.04` beats of the nearest
integer beat.

```text
beatPosition = (time - timingBase) / beatLength
isStrongBeat = abs(beatPosition - round(beatPosition)) <= 0.04
```

This is intentionally simple. The support-note generator uses strong beats as
musical anchors, not as a full rhythm parser.

### Motif Detection In The Intent Summary

The summary-level motif detector is lightweight:

- A jack motif is counted when consecutive sorted notes are within `500 ms` and
  share the same source lane.
- A trill motif is counted when a four-note single-lane window alternates
  between two source lanes.
- A stair motif is counted when a three-note single-lane window moves in one
  source-lane direction.
- A stream motif is counted when at least five single-note slices fit inside
  `1400 ms`.
- An LN anchor is counted when a slice includes a hold note.
- A chord slice is counted when a slice has more than one note.

These counters are diagnostics. They do not by themselves choose output lanes.
Placement-time motif classification is handled separately in `nk2_convert.cpp`.

## Target Layout Model

`layout_model.*` summarizes the target field.

For even targets at or above 8K:

```text
left panel  = lanes 0 .. targetK/2 - 1
right panel = lanes targetK/2 .. targetK - 1
```

For 10K:

```text
left panel  = lanes 0..4
right panel = lanes 5..9
bridge      = lanes 3..6
kind        = 10K panel-bridge-fullfield
```

For target key counts at or above 7K but not 10K:

```text
bridge = lanes targetK/2 - 1 .. targetK/2 + 1
```

For smaller targets, bridge scoring falls back toward whole-field behavior.

## Candidate Lane Pools

NK2 starts lane placement by building a deterministic candidate pool.

### Direct Lane

The direct lane is the rounded linear scale from source to target:

```text
directLane = round(sourceLane * (targetK - 1) / (sourceK - 1))
```

For degenerate 1K cases, the direct lane is `0`.

### Generic Pool

The generic candidate pool includes:

```text
direct
direct - 1
direct + 1
mirror of direct across the target field
every target lane
```

Duplicates and out-of-range lanes are removed.

### 7K -> 10K Pool

7K -> 10K uses a stronger hand-authored pool before falling back to the whole
field. Each source lane starts with its direct scaled lane, then adds local
panel or bridge candidates:

```text
source 0: direct, 1, 2
source 1: direct, 1, 3, 0
source 2: direct, 4, 2
source 3: direct, 4, 5, 3, 6
source 4: direct, 5, 7
source 5: direct, 7, 6, 9
source 6: direct, 8, 7
```

After those candidates, it adds the mirrored direct lane and then all 10 lanes.
This makes the ranking stage free to select under-used field lanes without
losing the preferred 7K-to-10K panel/bridge anchors.

## Placement Motifs

Placement-time motif classification uses a recent-note context, not only the
summary counters.

Motif kinds:

- `Jack`
- `Trill`
- `Stair`
- `Stream`
- `Chord`
- `LnAnchor`
- `Neutral`

The classifier receives:

- whether the current slice has exactly one note
- whether this note is a fast continuation of the previous single note
- whether the current source lane is continuing a source jack
- recent single-note source/target lane history

Classification order:

1. Source jack continuation becomes `Jack`.
2. Hold notes become `LnAnchor`.
3. Multi-note slices become `Chord`.
4. Non-fast single notes become `Neutral`.
5. Alternating recent single-note pairs become `Trill`.
6. Same-direction recent single-note movement becomes `Stair`.
7. Other fast single-note continuations become `Stream`.

The result controls scoring adjustments, motif placement counters, and generated
support provenance.

## Candidate Ranking

`rankedCandidates()` scores every lane candidate and sorts descending by score,
breaking ties by lower lane index.

The score is a weighted combination of:

- native closeness to the direct scaled lane
- remix/layout score
- lane coverage pressure
- panel-local coverage pressure
- whole-field under-use pressure
- direct anchor preference
- source-panel preference
- source jack preservation
- previous single-note direction continuity
- created-jack penalty
- motif-specific adjustment
- long-LN adjacent-copy preference

### Native Score

Native score is based on distance from the direct lane. Neutral taps and
stream-like taps are treated as freer anchors and can travel farther:

```text
distance = abs(candidateLane - directLane)
nativeDistance = freeOriginalTap ? 7.0 : 4.0
nativeDistance *= genericTargetFreedomMultiplier(targetK)
rawNativeScore = max(0, 1 - distance / nativeDistance)
```

Target 8K has reduced anchor contrast so it does not over-lock to direct lanes.

### Remix Score

Remix score combines:

- whether the lane belongs to the source-side panel
- whether the lane is in the bridge
- whether the lane improves target-lane coverage

The default weight interpretation is:

```text
panel : bridge : fullField = 3 : 2 : 6
```

In other words, whole-field use is usually the strongest style pressure, while
source-panel and bridge behavior still matter.

### Lane Coverage Need

Lane coverage pressure estimates whether a lane is under-used relative to the
desired layout distribution. It uses virtual totals so early notes do not swing
the score too aggressively.

When a lane is under-used, coverage need is positive. When it is over-used,
coverage need becomes negative. This allows the ranking stage to gradually pull
later notes into starved lanes without forcing unsafe placements.

### Panel Lane Need

For even panel targets, panel lane need checks only the local panel. This catches
cases where the whole chart appears balanced, but one lane inside a 5K side
panel is visually dead.

### Whole-Field Need

Whole-field need is a uniform-lane pressure. Generic high-key targets can boost
this pressure through `genericTargetFreedomMultiplier()`.

Current multipliers:

```text
target 8K: 1.35
target 5K: 1.55
other:     1.00
```

This is why the generic 7K -> 8K and 4K -> 5K tests expect wider field usage
instead of simple direct scaling.

### Anchor Lock Scale

Generic targets can also reduce direct-anchor locking through
`genericTargetAnchorLockMultiplier()`.

Current multiplier:

```text
target 8K: 0.62
other:     1.00
```

The final anchor lock scale is divided by the freedom multiplier. This keeps
freer targets from snapping every source note back to the direct lane.

### Source Jack Preservation

If a note continues a source jack, NK2 looks up the last placed target lane for
that source lane. The scoring then strongly prefers the same target lane:

```text
same source-jack lane: +2.0
other lane:            -1.0
```

This is how source jacks remain source jacks instead of becoming broken or
misreported as created target jacks.

### Direction Continuity

For fast single-note continuations, NK2 compares source-lane direction and
target-lane direction:

```text
same direction: +0.35
larger/equal target movement: additional +0.12
direction break: -0.50
```

Motif-specific scoring adds more precise trill/stair/stream behavior on top of
this generic direction continuity.

## Motif-Specific Scoring

Motif scoring is intentionally conservative. It can push candidates up or down,
but safety gates still decide whether a candidate can be accepted.

### Jack

Jack notes prefer the direct lane slightly and penalize non-direct lanes:

```text
direct: +0.25
other:  -0.20
```

Source jack lane memory adds a much stronger same-lane preference separately.

### Trill

Trills avoid same-lane collapse:

```text
targetDelta == 0: -1.30
targetDelta != 0: +0.65
compact movement <= 3 lanes: +0.25
wide movement: -0.20
source-panel lane: +0.10
```

The goal is not to pin a trill to a fixed two-lane pair. The goal is to preserve
alternation identity while allowing the target key field to breathe.

### Stair

Stairs preserve direction:

```text
same source/target direction: +0.75
target movement at least source movement: +0.30
direction break or flat target: -0.90
```

For high-key output this lets stairs widen, but not invert or collapse.

### Stream

Streams avoid repeated-lane flattening and reward continuous motion:

```text
targetDelta == 0: -0.80
targetDelta != 0: +0.25
compact movement <= 4 lanes: +0.15
very wide movement: -0.10
same source/target direction: +0.20
coverage need: +0.35 * laneCoverageNeed
```

Streams are freer than trills and stairs because they often benefit from target
field movement.

### Chord

Chords prefer source-panel readability:

```text
source-panel lane: +0.35
outside source panel: -0.35
center source lane into bridge: +0.20
```

Same-time collision gates still ensure two notes in one slice do not land on
the same target lane.

### LN Anchor

LN anchors preserve exact timing and duration, but their lane anchor is loose:

```text
direct lane: small bonus
layout under-use: meaningful bonus
panel lane need: meaningful bonus
outside source panel but under-used: small bonus
bridge lane: small bonus
```

Long holds reduce spread pressure through `longHoldSpreadScale = 0.25`. This
keeps long LNs from wandering too aggressively unless there is a strong adjacent
copy or coverage reason.

### Neutral

Neutral taps are the freest original notes:

```text
direct lane: tiny bonus
layout under-use: meaningful bonus
outside source panel but under-used: small bonus
```

This is how NK2 can make isolated original taps travel across a wider target
field while still leaving motifs more stable.

## Lane Acceptance Gates

After ranking, NK2 tries candidates in score order.

Hard gate 1: same-time collision

```text
candidate lane must not already be occupied inside this source slice
```

Hard gate 2: LN conflict

Reject when:

- the candidate starts inside an existing hold on the same lane
- the candidate hold overlaps an existing object on the same lane
- two holds overlap on the same lane

Hard gate 3: created target jack

Reject when:

- a recent placed note exists on the same target lane
- it is within the jack window
- the source lane is different

Current jack window for placement is `500 ms`.

### Dedicated 7K -> 10K Acceptance

The 7K -> 10K path uses `chooseLane()`.

It first tries candidates with all gates:

```text
no same-time collision
no LN conflict
no created target jack unless continuing a source jack
```

If none pass, it tries again with only collision and LN gates. If that still
fails, it falls back to the direct scaled lane.

This path prioritizes producing a full converted chart. Any resulting damage is
reported later through warnings and counters.

### Generic Strict Acceptance

The generic path uses `chooseLaneStrict()`.

It accepts only candidates that pass all gates. If no candidate passes, it
returns no lane and the source note is dropped.

This is the current lower-key and generic compression behavior: preserve what
can be represented safely, and count impossible overflow in `droppedNotes`.

## 7K -> 10K Prototype

Prototype name:

```text
nk2-7k10k-panel-bridge-fullfield
```

This path exists because 7K -> 10K stresses the NK2 design goals:

- source-lane identity must stay readable
- 10K wants two 5K panels
- center source material should use bridge lanes
- wide target field coverage matters
- source jacks, trills, stairs, and streams need different behavior
- support notes can make output feel more authored

Important 10K interpretations:

```text
left 5K panel: lanes 0..4
right 5K panel: lanes 5..9
bridge: lanes 3..6
whole field: lanes 0..9
```

The ranking model uses all three. It does not choose "panel mode" or
"full-field mode" as a separate pass. Instead, panel, bridge, and full-field
pressure all influence every candidate score.

Faithful 7K -> 10K keeps source note count unless placement fallback damage is
unavoidable. Native and harder can add support notes if safe candidates exist.

## Generic 1K..10K Prototype

Prototype name:

```text
nk2-generic-nk-relane-compress
```

This path covers non-same source/target pairs from 1K through 10K. It uses the
same candidate ranking model but strict lane acceptance.

For higher-key generic conversions, the prototype generally relanes only. If
the user selects native or harder and the branch does not support support-note
generation, the report warns:

```text
NK2 generic prototype currently relanes only; support-note generation is limited
to 4K to 5K and 7K to 10K.
```

For lower-key generic conversions, dropped notes represent objects that could
not be placed without collision, LN conflict, or created-jack damage.

## 4K -> 5K Fill Exception

4K -> 5K is the first generic fill branch. It intentionally adds safe support
notes even in faithful mode because the purpose of this branch is to occupy the
new fifth lane without damaging the source chart.

Special constants:

```text
global added ratio:         0.12
phrase added ratio:         0.30
minimum global budget:      8
minimum phrase budget:      3
maximum phrase budget:      12
support jack window:        240 ms
same-source support gap:    80 ms
```

The shorter support-jack window prevents medium-gap 4K material from being
treated as unsafe support repetition too aggressively.

## Support Notes

Support notes are generated after original notes are placed and sorted. They are
tap-only, source-related, budgeted, safety-gated, and provenance-tagged.

Support generation currently runs when:

```text
7K -> 10K
or
4K -> 5K
```

For other generic high-key paths, the converter may warn that support generation
is not enabled for that pair.

### Support Event Types

There are three support kinds:

- `Ln`
- `StrongBeat`
- `Mirror`

LN support event:

- emitted for a hold with an end time
- hold duration must be greater than `120 ms`
- event time is the LN end time
- anchor motif is `LnAnchor`

Strong-beat support event:

- emitted for placed notes that land on a strong beat
- anchor motif is recomputed from the placed chart
- event time is the anchor note time

Mirror support event:

- emitted as an independent strong-beat event when eligible
- target key count must be greater than source key count
- hold notes are skipped
- jack, trill, and LN-anchor motifs are skipped
- center source lane is skipped
- side-lane anchors prefer opposite-panel mirror behavior

### Support Budgets

Support has both a global budget and a phrase-local budget.

Global budget:

```text
report mode:      0
faithful mode:    0, except 4K -> 5K
native mode:      ceil(sourceNotes * 0.12), minimum 1
harder mode:      ceil(sourceNotes * 0.18), minimum 2
4K -> 5K:         ceil(sourceNotes * 0.12), minimum 8
```

Phrase windows are deterministic `2000 ms` buckets:

```text
phraseWindow = time / 2000
```

Default phrase budget:

```text
native: ceil(sourceNotesInWindow * 0.12), min 1, max 4
harder: ceil(sourceNotesInWindow * 0.18), min 2, max 6
```

4K -> 5K phrase budget:

```text
ceil(sourceNotesInWindow * 0.30), min 3, max 12
```

If the global budget is exhausted, the event is counted as a budget rejection.
If the phrase budget is exhausted, the event is counted as both a budget
rejection and a phrase-budget rejection.

### Support Lane Ranking

Support lanes are ranked separately from original note lanes.

Support ranking uses:

- remix layout score
- lane coverage need
- panel lane need
- mirror lane bonus
- opposite-panel bonus
- support-kind-specific bonuses

Mirror events add the mirror lane and its neighbors to the candidate pool.

Mirror lane:

```text
mirrorLane = targetK - 1 - anchorLane
```

Mirror support gets stronger opposite-panel pressure and a penalty for staying
inside the source-side panel.

LN support gets a small bridge bonus.

Strong-beat support gets a small source-panel bonus.

### Support Safety Gate

A support candidate is rejected if another note on the same lane:

- exists at the same time
- has an active hold body over the candidate time
- is within the guarded same-lane gap

Guarded gap:

```text
same source lane: default 120 ms, 4K -> 5K 80 ms
different source lane: default 500 ms, 4K -> 5K 240 ms
```

This combines no-collision, no-LN-conflict, and no-created-jack safety into a
single support candidate check.

### Support IDs

Generated support notes use IDs that begin with `nk2-`.

Current prefix pattern:

```text
nk2-<generator>-<anchorMotif>-<anchorId>-<time>
```

Examples:

```text
nk2-ln-ln-...
nk2-beat-chord-...
nk2-mirror-stream-...
```

The report also increments generated provenance counters by anchor motif:

- from jack
- from trill
- from stair
- from stream
- from chord
- from LN
- from neutral

This is useful when a generated note looks strange. The ID and report can show
whether it came from an LN end, a strong beat, or mirror support, and which
source motif justified it.

## Final Safety And Distribution Report

After conversion and support generation, NK2 recomputes final diagnostics from
the actual output chart.

### Placement Stats

`collectPlacementStats()` computes:

- lane distribution
- same-time same-lane collisions
- LN conflicts
- created target jacks
- preserved source jacks
- source anchor total
- source anchor matches

Generated notes are ignored by source-anchor diagnostics. This makes
`sourceAnchorScore` a measure of original-note direct anchoring only.

Source anchor match:

```text
note is not generated
and source lane is inside source key count
and output lane == directLane(sourceLane, sourceK, targetK)
```

`sourceAnchorScore` is:

```text
sourceAnchorMatches / sourceAnchorTotal
```

This is not a hard correctness target. A low score can be good if NK2 used
safe, readable target-native movement. It is a recognizability diagnostic beside
motif counters and layout scores.

### Jack Accounting

Final jack accounting sorts converted notes by time, lane, and id. Consecutive
same-lane notes inside the support jack window are counted as:

- `preservedSourceJacks` when source lanes match
- `createdJacks` when source lanes differ

The support jack window is usually `500 ms`, but 4K -> 5K uses `240 ms`.

## Layout Scores

NK2 reports several layout scores.

`panelScore`:

- left/right half balance
- returns 1 when both halves are equally used
- lower when one half dominates

`leftPanelScore` and `rightPanelScore`:

- local panel spread
- catches dead lanes inside each panel

`bridgeScore`:

- compares actual bridge usage with desired bridge share
- uses the active target bridge range

`fullFieldScore`:

- normalized lane entropy
- higher means more even whole-field use

`layoutCoverageScore`:

- compares actual lane distribution against the desired weighted layout
- uses panel, bridge, and full-field desired shares

These scores are diagnostics. They should be interpreted together with safety
counters and manual playtest feel.

## Phrase Profile Score

Phrase profile scoring is currently report-only. NK2 recomputes the same
`2000 ms` phrase windows after support generation and compares accepted support
notes against the local phrase budget.

Report fields:

- `phraseProfile.score`
- `phraseProfile.windows`
- `phraseProfile.overBudgetWindows`

This hook exists so future NK2 versions can search candidate variants against a
profile-aware phrase score. Current NK2 does not yet use it to choose among
multiple output candidates.

## Report Fields

NK2 reports are serialized by `reportToJson()` and `reportToText()`.

Top-level fields:

- engine
- mode
- chartMutated
- noOp
- noOpReason
- prototypeName
- weights
- layout
- intent
- placement
- support
- motifPlacements
- generatedProvenance
- layoutScores
- phraseProfile
- warnings

Important placement fields:

- `outputNotes`
- `addedNotes`
- `droppedNotes`
- `sameTimeCollisions`
- `longNoteConflicts`
- `createdJacks`
- `preservedSourceJacks`
- `sourceAnchorMatches`
- `sourceAnchorTotal`
- `sourceAnchorScore`
- `laneDistribution`

Important support fields:

- `lnCandidates`
- `lnAccepted`
- `lnRejected`
- `strongBeatCandidates`
- `strongBeatAccepted`
- `strongBeatRejected`
- `mirrorCandidates`
- `mirrorAccepted`
- `mirrorRejected`
- `rejectedByBudget`
- `rejectedByPhraseBudget`
- `phraseWindows`
- `rejectedBySafety`

Important motif placement fields:

- `jack`
- `trill`
- `stair`
- `stream`
- `chord`
- `ln`
- `neutral`

## Warnings

Warnings are emitted for conditions that are allowed in the current prototype
but should be visible:

- native/remix weights differ from the default 50/50 design target
- unsupported key-count pair was requested outside report mode
- generic higher-key conversion requested support generation that is not yet
  enabled for that pair
- faithful mode kept source note count because support notes were disabled
- 7K -> 10K support generation found no accepted safe candidates
- created target jacks remain in the output
- same-time collisions or LN conflicts remain in the output

Warnings are not a replacement for tests. They are report-surface indicators
for prototype degradation.

## CLI Surface

Relevant CLI options:

```text
--engine classic|nk2
--nk2-mode native|faithful|harder|transform|report
--nk2-native-weight <n>
--nk2-remix-weight <n>
--nk2-layout-weight-panel <n>
--nk2-layout-weight-bridge <n>
--nk2-layout-weight-fullfield <n>
```

Examples:

```bash
build/KeyWeaver.exe samples/simple_7k_ln.osu --source 7 --target 10 --engine nk2 --nk2-mode report --report dist/nk2_7k10k_intent.json
build/KeyWeaver.exe samples/simple_7k_ln.osu --source 7 --target 10 --engine nk2 --nk2-mode faithful --dry-run
build/KeyWeaver.exe samples/simple_7k_ln.osu --source 7 --target 10 --engine nk2 --nk2-mode native --out dist/simple_7k_10k_nk2.osu --report dist/nk2_7k10k.json
build/KeyWeaver.exe samples/simple_4k.osu --source 4 --target 5 --engine nk2 --nk2-mode faithful --report dist/nk2_4k5k.json
```

The output difficulty marker for NK2 is:

```text
KeyWeaverNK2-<target>K
```

If the existing version name already contains `KeyWeaver`, NK2 does not append a
second marker.

## GUI Surface

The GUI exposes NK2 as an experimental single-chart algorithm. Batch and matrix
flows remain NK1-only in the current milestone.

Expected GUI model:

```text
Algorithm: NK1 (Classic) / NK2 (Experimental)
NK2 modes: faithful / native / harder / transform
```

The GUI shells out to the CLI and reads the report, so NK2 GUI behavior should
stay consistent with the CLI options above.

## Tests And Acceptance

Current NK2 test coverage includes:

- report-only intent graph summary
- same-key no-op unless transform
- generic 4K -> 5K conversion
- generic 4K -> 5K whole-field spread
- generic 7K -> 8K conversion
- generic 7K -> 8K whole-field spread
- generic 7K -> 8K long-LN adjacent-copy placement
- generic 7K -> 4K compression
- 7K -> 10K prototype conversion
- 7K -> 10K source jack preservation
- 7K -> 10K support note gating
- 7K -> 10K mirror support
- 7K -> 10K phrase budget
- 7K -> 10K freer LN anchors
- 7K -> 10K freer original taps
- 7K -> 10K coverage pressure filling bridge gaps
- 7K -> 10K right-panel coverage pressure
- separated motif policies

Baseline automated gates:

```text
sameTimeCollisions == 0
longNoteConflicts == 0
createdJacks == 0
output lanes are inside target field
source LN durations are preserved exactly
same-key native/faithful/harder are no-op
report mode does not mutate charts
support accepted counters match added notes
support notes are tap-only
```

For documentation-only changes, a build is not usually needed. For NK2 behavior
changes, run the C++ tests and include real-chart smoke coverage when the change
affects support generation, key-count routing, or 7K -> 10K lane policy.

## How To Read An NK2 Report

Recommended order:

1. Check `chartMutated`, `noOp`, and `prototypeName`.
2. Check hard damage: collisions, LN conflicts, created jacks, and dropped
   notes.
3. Check support counts and rejection reasons.
4. Check lane distribution and layout scores.
5. Check motif placement counters.
6. Check source-anchor score only as a recognizability signal, not as a pass
   target.
7. Check warnings.
8. Inspect the chart manually if style or authored feel matters.

Example interpretation:

```text
addedNotes > 0
sameTimeCollisions == 0
longNoteConflicts == 0
createdJacks == 0
mirrorAccepted > 0
layoutCoverageScore increased
sourceAnchorScore decreased
```

This can be a good NK2 result: the converter added safe mirror/support material
and used the target field more freely, so direct source anchoring decreased.

Bad result examples:

```text
createdJacks > 0
sameTimeCollisions > 0
longNoteConflicts > 0
```

These indicate safety damage and should be treated as failures or prototype
warnings requiring repair.

## Design Tradeoffs

### Why Not Direct Scaling?

Direct scaling is stable, but high-key direct scaling often leaves target lanes
visually dead and makes output look converted. NK2 keeps direct scaling as a
candidate and diagnostic anchor, then lets layout pressure and motif scoring
move safe notes into more native target-key positions.

### Why Let Neutral Taps Move More Than Motifs?

Motifs carry pattern identity. A stair that changes direction, a trill that
collapses to one lane, or a source jack that splits randomly is easy to feel as
wrong. Isolated taps and stream-like taps can move more without destroying the
source chart's recognizable pattern.

### Why Are LN Anchors Freer?

The important LN contract is duration and conflict safety. A repeated LN can
often move into an under-used lane or bridge lane without losing its identity,
as long as the start/end timing and same-lane occupancy remain valid.

### Why Add Support Notes After Placement?

Support generation needs to know where original notes landed. Generating
support before placement would make mirror lanes, active-LN conflicts, lane
coverage, and phrase budgets less reliable.

### Why Phrase Budgets?

A global added-note ratio can still cluster too many generated notes in one
local section. Phrase budgets cap local density so support notes remain
authored-looking instead of appearing as bursty filler.

### Why Keep Source Anchor Score If Low Can Be Good?

Source anchor score is useful for detecting over-drift. It should not be used
alone because NK2 intentionally moves some original notes away from direct
scaled lanes. The useful question is whether the source pattern remains
recognizable after that movement.

## Known Gaps

The current NK2 implementation is not the final architecture promised by the
design document. Known gaps:

- Intent graph is still a summary, not a full object graph.
- There is no multi-candidate solver or beam search.
- Profile-guided phrase scoring is report-only.
- Same-key transform mode is recognized but not fully implemented as a mutation
  path.
- Support-note generation is enabled only for 7K -> 10K and 4K -> 5K.
- Support notes are tap-only.
- Generic higher-key pairs other than 4K -> 5K are mostly relane-only.
- Lower-key conversion drops impossible overflow instead of doing phrase-aware
  merge ranking.
- 7K -> 10K has a permissive fallback that can still produce reported damage if
  all safer candidates fail.
- Strong-beat detection is intentionally simple.
- Manual playtest is still required for authored-feel acceptance.

## Future Work

Natural next steps:

- Replace summary-only intent with a reusable `IntentGraph` object.
- Add a candidate solver that can compare multiple local placements before
  committing.
- Make support generation available for more higher-key pairs.
- Add generated-LN support only after tap support is consistently safe.
- Use profile-guided phrase scores to choose among candidate variants.
- Add stronger lower-key merge ranking before dropping source notes.
- Implement same-key transform mutation for `nk2-mode transform`.
- Add real-chart sample gates for NK2 support and layout regressions.
- Promote NK2 only after it beats Classic on representative real charts.

