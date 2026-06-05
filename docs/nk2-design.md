# NK2 Design

NK2 is the proposed second-generation conversion engine for KeyWeaver. It should
coexist with the current engine instead of replacing it in one step.

The current engine remains the stable Classic/reference path. NK2 is a new core
that can reuse existing parsers, exporters, reports, tests, and GUI plumbing
while changing the conversion model itself.

## Product Goal

NK2 should make converted charts look like they were authored for the target key
count, while still visibly preserving the source chart's pattern identity.

Primary feel target:

```text
Native target-key authorship: 50%
Remix-style expansion:        50%
```

This is not a faithful-only converter. It is also not a randomizer. The output
should feel manually arranged: readable source intent, target-key-native lane
use, tasteful mirrored expansion, and no unsafe generated junk.

## Scope

Target scope:

- any NK -> any NK
- osu!mania input/output
- BMS-family input/output
- target-profile support from the existing profile pipeline
- Classic engine kept available as a baseline

Implementation should still begin with a narrow milestone. The recommended
first milestone is `7K -> 10K`, because it stresses the most important NK2
ideas: panel logic, full-field usage, bridge handling, mirror expansion, and
source-pattern recognizability.

## Hard Priorities

Safety and identity outrank style pressure.

Priority order:

1. Preserve rhythm timing.
2. Preserve original LN duration.
3. Avoid collisions and LN conflicts.
4. Do not create new jacks.
5. Preserve source pattern recognizability.
6. Preserve hand feel.
7. Minimize note deletion.
8. Add target-key-native density and remix material.
9. Improve lane coverage and Target-K likeness.

When priorities conflict, NK2 should prefer a recognizable source pattern over a
perfectly smooth or naturally dense output.

## Architecture

Proposed module layout:

```text
src/nk2/
  nk2_convert.*
  intent_graph.*
  layout_model.*
  candidate_solver.*
  safety_solver.*
  feel_scorer.*
  profile_bridge.*
  nk2_report.*
```

Top-level pipeline:

```text
keyconv::Chart
  -> IntentGraph
  -> SourceLayoutAnalysis
  -> TargetLayoutModel
  -> CandidateField
  -> SafetySolver
  -> FeelScorer
  -> NK2ConvertResult
```

The Classic engine maps and repairs notes mostly as time slices. NK2 should
convert phrase intent first, then solve placements from that intent.

## Intent Graph

`IntentGraph` is the core abstraction. It should represent what the source chart
is trying to do, not only where notes currently are.

Suggested levels:

- object: tap, LN head/body/tail, generated support candidate
- slice: same-time or near-time chord group
- motif: jack, trill, stair, stream, burst, LN anchor, chord phrase
- phrase: local hand-feel unit over a time window
- section: density/energy region for budget and profile decisions

Each motif should carry at least:

- time range
- source lanes
- source hand/zone estimate
- directionality
- rhythm grid relation
- density
- LN pressure
- jack risk
- recognizability anchors
- allowed expansion modes

## Target Layout Model

NK2 must support any target key count, but layout behavior should be explicit.

For each target K, define:

- lane indices
- left/right or zone ownership when meaningful
- center/bridge lanes when meaningful
- full-field pressure lanes
- source-to-target anchor candidates
- mirror candidates
- lane capacity and local density budget
- chord width limits
- LN occupancy constraints

For 10K specifically, use all three interpretations together:

```text
left/right 5K panels: lanes 0-4 and 5-9
center bridge:        lanes 3-6
full field:           lanes 0-9
```

The scoring weight interpretation is:

```text
5K panel model : center bridge model : full-field model = 3 : 2 : 6
```

This means full-field movement should be strongest, but not at the cost of
destroying panel readability or bridge identity.

## Native Plus Remix Model

NK2 should explicitly balance two style forces:

- Native force: target-key-authored lane use, hand distribution, phrase shape,
  panel/full-field coverage, and profile likeness.
- Remix force: tasteful added material, opposite-hand mirror, strong-beat
  emphasis, larger physical motion, and LN support notes.

The default target blend is 50/50.

This should be implemented as scoring terms, not as two separate conversion
passes that fight each other.

Example score components:

```text
total =
  safetyHardGate
  + sourceRecognizability * highWeight
  + timingPreservation * highWeight
  + noNewJackScore * hardGate
  + handFeelScore
  + nativeLayoutScore * nativeWeight
  + remixEnhancementScore * remixWeight
  + profileLikenessScore
  - deletionPenalty
  - fakeRhythmPenalty
```

## Added Note Policy

Added notes are allowed for higher-key output, but they must be justified by
source intent.

Approved addition sources:

- opposite-hand mirror support
- LN-near support notes
- strong-beat and regular-grid emphasis
- low-density phrase expansion
- source motif continuation
- target-key-native hand fill when it does not hide the source pattern

Rules:

- Added notes must not create new jacks.
- Added notes must not create collisions or LN conflicts.
- Added notes should prefer source-related timings or strong musical grid
  timings.
- Added notes should not invent unrelated syncopation.
- Added-note budgets should preserve original difficulty by default.
- Harder/remix density should be an explicit mode, not the default.

## Lower-Key Conversion Policy

When target K is lower than source K, NK2 should avoid deletion first.

Preferred fallback order:

1. Relane while preserving timing.
2. Compress hand/zone ownership.
3. Merge equivalent same-time taps only when musically safe.
4. Drop the least intent-critical generated/support note.
5. Drop the least intent-critical source note only when impossible to keep.

Retiming should not be used as a normal fix. Rhythm timing preservation is a
core NK2 contract.

Current lower-key prototype behavior is a strict relane/compress pass. It does
not yet do phrase-aware merge ranking or playable roll/retime repair; it only
drops an overflow note after the legal target lanes fail the hard safety gates.

## Same-Key Conversion Policy

If source K equals target K and no transform is requested, NK2 should treat the
chart as already native and do nothing except produce a report.

Same-K conversion is meaningful mainly for explicit transform modes such as:

- SRandom-style lane transform
- Jitter-style timing transform
- future cleanup/report-only modes

Without such a mode, same-K input should be skipped or reported as no-op.

## Jack Policy

Source jacks should preserve their original feel as much as possible.

Rules:

- New jacks must not be created.
- Source jack groups should remain recognizable.
- Long or unsafe source jacks may be split only when the split still reads as
  the same phrase.
- Generated notes cannot join a target lane in a way that creates a no-source
  jack.
- Different source lanes should not become target repeated taps when a safe
  alternative exists.

Jack validation should remain a hard acceptance gate, not only a metric.

## Trill, Stair, And Stream Policy

Trills:

- Do not hard-lock trills to a fixed two-lane pair.
- Preserve alternation identity.
- Allow trills to breathe across nearby lanes or a compact cluster when the
  target K supports it.
- Avoid converting a trill into a same-lane repeat or unreadable scatter.

Stairs:

- Preserve direction and stair identity.
- For 7K -> 10K, stairs may widen and use the whole field.
- Wider stairs should still read as one continuous authored gesture.
- Do not flatten stairs into static lane anchors only for balance.
- In Super Symmetry mode, adjacent-lane source stairs should remain
  adjacent-lane target stairs when safety gates allow it.

Streams:

- Preserve the original stream feel.
- On 7K -> 10K, make the stream physically larger when useful.
- Maintain phrase continuity and source-lane identity.
- Use expansion to make the pattern feel target-native without hiding the
  original stream.

Super Symmetry mode:

- Lives in the same Stream selector as SuperRandom and Full Jitter.
- Is NK2-only.
- Preserves same-time mirrored source lanes as mirrored target lanes.
- Keeps gapless adjacent-lane stairs gapless instead of widening them for
  target-field coverage.
- Disables unpaired support-note generation so added taps do not break the
  requested symmetry.

## LN Policy

LN duration is a hard contract.

Rules:

- Preserve original LN start and end times.
- Do not shorten, lengthen, or tapify source LNs as a normal solution.
- Avoid collisions and LN conflicts by candidate rejection or relaning.
- Higher-key conversion may add LN support notes around source LNs.
- LN support notes must not collide with LN bodies or create new jacks.
- If the only possible solution would damage source LN duration, NK2 should
  reject that candidate and report the conflict.

## Difficulty Policy

Default difficulty target is source difficulty preservation.

For higher-key conversion, added material should improve target-key authorship
without making the chart meaningfully harder by default.

Suggested modes:

- `nk2-native`: 50/50 native/remix, source difficulty preserved.
- `nk2-faithful`: less added material, maximum pattern recognizability.
- `nk2-harder`: explicit density increase.
- `nk2-transform`: same-K SRandom/Jitter-style transforms.
- `nk2-report`: no output mutation, report only.

The default should be `nk2-native`.

## Profile Strategy

Keep the existing profile concept.

NK2 should use Target-K profiles for:

- lane coverage expectations
- density bucket expectations
- center/bridge behavior
- chord width tendencies
- hand balance tendencies
- LN-heavy and jack-risk windows

Profile search/building may need acceleration for large Songs folders. This is
separate from the conversion core. A future GPU-assisted profile scanner can be
added to the profile-building toolchain without making the NK2 converter depend
on GPU runtime.

## Report Metrics

NK2 reports should preserve existing safety metrics and add NK2-specific intent
metrics.

Required existing metrics:

- collisions
- LN conflicts
- near-time conflicts
- source/preserved/split/created jacks
- added notes and added ratio
- lane coverage
- lane entropy
- K-likeness

New NK2 metrics:

- source recognizability score
- native/remix blend score
- intent graph preservation score
- deletion pressure
- fake rhythm penalty
- mirror support count
- LN support count
- strong-beat support count
- panel score
- bridge score
- full-field score
- same-K no-op reason

## CLI And GUI Surface

CLI proposal:

```text
--engine classic|nk2
--nk2-mode native|faithful|harder|transform|report
--nk2-native-weight <n>
--nk2-remix-weight <n>
--nk2-layout-weight-panel <n>
--nk2-layout-weight-bridge <n>
--nk2-layout-weight-fullfield <n>
```

Default NK2 weights:

```text
native/remix = 50/50
10K panel/bridge/fullfield = 3/2/6
```

GUI proposal:

```text
Engine: Classic / NK2
NK2 mode: Native / Faithful / Harder / Transform / Report
```

Classic remains the default until NK2 has real-chart evidence.

## Implementation Milestones

Milestone 0: design-only

- add this document
- keep Classic unchanged

Milestone 1: skeleton

- add `src/nk2/` module stubs
- add `NK2Options`
- add report-only path that builds an intent graph and emits metrics
- no chart mutation yet

Milestone 1 implementation status: complete. `--engine nk2 --nk2-mode report`
builds an intent summary and target layout summary, writes optional JSON through
`--report`, and never writes converted chart output.

Milestone 2: 7K -> 10K placement prototype

- build target 10K layout model
- implement motif-aware candidate generation
- implement no-new-jack and LN-conflict hard gates
- produce converted chart under explicit `--engine nk2`

Current Milestone 2 status: implemented for `source=7,target=10` in non-report
NK2 modes. The prototype preserves LN end times, keeps source jacks on the same
target lane when possible, and reports lane distribution plus
collision/LN/jack safety counters.

Other non-same-K pairs from 1K through 10K now use a generic
`nk2-generic-nk-relane-compress` prototype. This path covers early checks such
as 4K->5K, 7K->8K, and 7K->4K. It preserves timing and surviving LN durations,
tries same-time local beam placement before the note-by-note fallback, tries all
legal target lanes before giving up, and records `droppedNotes` only when a note
cannot be placed or safely rolled without same-time collision, active-LN
conflict, or unrelated target-jack damage. Reports expose
`localSolverWindows`, `localSolverCandidates`, `localSolverFallbacks`, and
`lowerKeyRolledNotes` for this early solver path. The NK2 Stream selector also
supports `super-symmetry`, which reports `superSymmetryMirrorAnchors` and
`superSymmetryGaplessStairs` when it preserves mirrored same-time pairs or
adjacent-lane stair links.
The 4K->5K branch is the first generic fill-note exception: it can add tap-only
strong-beat and mirror support notes even in faithful mode, using phrase-local
budgets plus strict collision and LN gates. Its support-repeat guard is tuned
separately from the default 500 ms NK2 support guard: 4K->5K uses a 240 ms
support-jack window so dense 4K material can fill the fifth lane without treating
every medium-gap repeat as a newly created jack.

Milestone 3: NK2 native/remix scoring

- add native/remix scoring terms
- implement opposite-hand mirror support
- implement LN support notes
- implement strong-beat support notes
- validate source recognizability before density gains

Current Milestone 3 status: partially implemented for higher-key 1K..10K NK2
pairs plus the generic `source=4,target=5` fill branch. Higher-key `native` and
`harder` modes can add limited tap-only LN-head, LN-tail, strong-beat, and
mirror support notes, while faithful mode keeps source note count except for
4K->5K. Generic 4K->5K can add tap-only strong-beat and mirror support notes in
faithful mode to make the added fifth lane feel occupied. Every support
candidate is trialed against the converted
chart and accepted only if it does not increase same-time collisions, LN
conflicts, created target jacks, or unsafe target repeats. The 4K->5K faithful
fill target is about 12% added notes globally, with 2000 ms phrase windows capped
at 30% of local source density and clamped to 3-12 support notes. On the KKKK
regression chart this lands near the requested 250-note fill range while still
reporting zero overlap and zero 240 ms support-created jacks. JSON/text reports
expose LN, strong-beat, and mirror support candidate/accepted/rejected counts
plus budget and safety rejections.
Opposite-hand mirror support now has an independent support-event generator for
safe strong-beat anchors. It avoids LN anchors, source jacks, trills, and center
source lanes, then prefers the mirrored target lane or nearby opposite-panel
lanes through the same global budget, phrase budget, collision, LN-conflict, and
no-new-jack gates as other support notes. Placement and support lane ranking now
also include a layout-weighted coverage pressure derived from
the 10K `panel/bridge/full-field = 3/2/6` target distribution, so bridge lanes
that are safe but under-used can beat over-used direct anchors without relaxing
the no-collision, LN-conflict, or no-new-jack gates. Reports expose panel,
left/right panel spread, bridge, full-field, and layout coverage scores for
real-chart comparison. A smaller panel-internal coverage pressure also keeps
individual 5K-panel lanes from going dark when the global full-field score
already looks healthy. Candidate placement is now motif-aware: source jacks,
trills, stairs, streams, same-time chords, LN anchors, and neutral notes receive
separate scoring adjustments before the existing hard safety gates run. Reports
expose motif placement counters so regressions can show whether a bad section
came from jack preservation, trill alternation, stair widening, stream movement,
chord paneling, or LN anchoring.

Reports also expose a source-anchor diagnostic for original notes. `sourceAnchorTotal`
counts non-generated notes, `sourceAnchorMatches` counts notes that stayed on the
direct scaled source lane, and `sourceAnchorScore` is the normalized ratio. This
is not a hard pass/fail target for every NK2 iteration; it is a recognizability
signal used beside layout coverage and motif counters when judging whether the
original pattern is still readable.

LN anchors are intentionally looser than tap anchors in NK2. The source LN
duration is still preserved exactly, but LN lane ranking no longer strongly
locks to the direct scaled source lane or source-side panel. Repeated safe LN
anchors can drift into bridge, under-used field lanes, and even the opposite
panel when collision, active-LN, and no-created-jack gates allow it.

Neutral original tap anchors are also allowed to move more freely than the
source skeleton. NK2 keeps stricter motif rules for source jacks, trills,
stairs, and chords, but isolated or stream-like original taps receive reduced
direct-lane/source-panel bonuses and stronger full-field under-use pressure.
This makes side-lane source taps able to travel across the wider 10K field
without treating the movement as generated support.

Generated support notes now carry provenance in both their internal note IDs and
the NK2 report. Current prefixes include forms such as `nk2-ln-ln-*` and
`nk2-beat-chord-*`, where the first segment is the generator and the second
segment is the anchor motif. Reports expose generated provenance counters by
anchor motif so odd support notes can be traced back to LN, chord, stair,
stream, trill, jack, or neutral contexts.

Support generation now has a phrase-local density budget in addition to the
global added-note budget. NK2 divides support candidates into deterministic
2000 ms windows, estimates a native/harder cap from the original note count in
that window, and rejects overflow as phrase-budget pressure. Reports expose
`phraseWindows` and `rejectedByPhraseBudget` so clustered support can be tuned
without confusing it with collision or LN safety rejection.

Mirror support accepts/rejects are reported separately from LN head/tail and
strong-beat support. Generated mirror notes use IDs like `nk2-mirror-chord-*`,
again preserving the generator plus anchor motif for debugging.

The first profile-guided phrase scoring stub is report-only. NK2 recomputes the
same 2000 ms phrase windows after support generation, compares accepted support
notes against the local native/harder cap, and reports `phraseProfile.score`,
`phraseProfile.windows`, and `phraseProfile.overBudgetWindows`. Current behavior
does not yet search multiple profile candidates from this score; it makes the
future profile-guided search observable without changing Classic defaults.

Milestone 4: any NK generalization

- generalize layout models for 4K-10K first
- then extend toward wider/niche NK if needed
- include BMS-family smoke paths

Milestone 5: GUI integration

- add engine selector
- add NK2 mode selector
- display NK2 intent metrics in report preview

## Acceptance Gates

NK2 should not become the default until it beats Classic on real examples.

For NKNK/NK2 development builds, automated real-chart testing does not need to
judge the whole Songs folder or the full authored feel every time. A
deterministic 10% chart sample is enough for the regular safety gate, with known
problem charts pinned into the sample even if the hash would skip them.

Minimum automated gates:

- zero same-time collisions
- zero LN conflicts
- zero generated-note created jacks
- source LN duration preserved exactly
- same-K no-transform is no-op
- output lanes stay inside the target key field
- output remains playable enough to load and inspect; no obvious non-playable
  result such as invalid key count, impossible overlap, or broken export
- BMS-family input keeps BMS-family output

Regular NKNK safety smoke:

```text
sample size: deterministic 10% of candidate charts
always include: current hand-picked regression charts
check only: overlaps / LN conflicts / created generated jacks / non-playable output
do not require: perfect lane distribution, final native feel, or full manual playtest notes
```

Full style comparison is still useful before promoting NK2 to a default, but it
is a manual review gate rather than a normal automated test gate. Source
recognizability and target-native feel should be judged from representative
playtest samples, not from every chart on every iteration.

Suggested first comparative report:

```text
Classic full-field 7K -> 10K
NK2 native 7K -> 10K
NK2 faithful 7K -> 10K
NK2 harder 7K -> 10K
```

The comparison should include both metrics and manual playtest notes.

## Open Questions

These are the remaining design details that need real chart examples or manual
playtest feedback:

- What source patterns are allowed to receive opposite-hand mirror support?
- How aggressive can strong-beat emphasis be before it hides the source chart?
- How should NK2 choose between full-field movement and compact hand feel on
  very dense streams?
- Which generated LN support notes feel authored instead of artificial?
- What are the best reference charts for a native 10K target style profile?
