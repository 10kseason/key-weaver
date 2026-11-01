# NK2 E+ Full-field

Status: experimental implementation, included in v1.4.0. The existing E preset
and its automatic adoption remain unchanged. E+ is an explicit 8K/10K comparison
mode: finish E first, then redistribute its existing notes across the field.
Algorithm v2 additionally addresses persistent source-lane ownership for every
supported source count (1K–18K to 8K/10K), without a 4K-specific mapping rule.
For the later request for more chord taps, use the separate
[E+ Full-field + taps option](nk2-e-tap-boost.md). The note-count preservation
contract below applies to the base redistribution; the optional tap pass keeps
that entire result and appends taps at retained source head times.

## Scope and success criterion

Question: can E use more lanes in short sections without changing its rhythm
or long notes? The observable criterion is increased mean active lanes and
reduced lane concentration in nonempty disjoint 2-second windows on movable
fixtures, with identical E note identities, count, order, head times, types,
LN tails, raw data and source-lane provenance. Only target lanes change.
V2 also checks original-source-to-target concentration: an even total field
must not hide originals pinned to a few lanes while generated notes fill the
rest. Movable long streams must change target lanes; dense boundaries that
cannot change ownership without a created jack must remain safe.

Safety is the stop condition: a rejected candidate stays in its E lane; a
failed final original-source audit restores every E lane. A failed initial E
audit prevents redistribution and remains subject to the existing CLI output
gate. No metric improvement establishes better playability.

## Use

Plain E+ is a CLI comparison recipe; the curated Windows GUI exposes the
separate **E+ - Full-field + taps** variant. Select Target **8** or **10** for
that GUI variant. Plain E+ also supports only these targets. Output filenames
and difficulty metadata contain `-fullfield-v2` to distinguish it from E and
the earlier local E+ v1 experiment. Existing comparison files are preserved.

CLI, including a separate optional hotspot report:

```text
build/KeyWeaver.exe input.osu --engine nk2 --target 10 --nk2-mode faithful --nk2-search phrase --nk2-phrase-support --nk2-phrase-dense-ln --nk2-phrase-source-ln --nk2-e-fullfield --report conversion.json --pattern-hotspot-report new-hotspots.json
```

Source key counts are detected through the existing osu/BMS parsers (1–18).
BMS-family input keeps BMS-family output. Single and batch conversion support
E+; the separate hotspot report remains single-input only. For automatic 7K
to 8K/10K E selection, `--engine nk2 --target 8 --nk2-e-fullfield` (or target 10) is sufficient.
Other inputs need the explicit E options above. F boost, legacy transform,
super-symmetry and stream transforms cannot be combined with E+.

Same-key 8K/10K E normally does nothing. Explicit E+ redistributes that same
population without adding notes. Already-converted inputs still trigger the
existing reconversion guard. Omit `--out` for an automatically chosen unused
filename; explicitly supplied output/report paths retain existing CLI behavior.

## Implementation and rationale

- The caller supplies the selected 8K/10K target to redistribution and the
  tap pass. Candidate destinations and safety gates use that count. Internal
  histogram storage retains capacity ten, with unused 8K cells zero; this
  preserves existing 10K arithmetic and iteration order. Metadata and optional
  lane reports expose only the selected output layout. See the
  [8K compatibility validation](nk2-e-tap-boost.md#8k-compatibility-validation-2026-11-01).
- `src/nk2/e_fullfield.*` is an internal post-pass over completed E. It does
  not alter E candidate generation, note budgets or source-LN containment.
- Consecutive E heads with gaps at most 500ms identify a repeat chain. Chains
  lasting at most 2 seconds remain indivisible, even across a window edge.
  Longer chains may change lanes at fixed 2-second boundaries; each segment's
  repeat shape stays together and every LN remains one complete note. This
  supersedes v1's whole-chain lock, which could anchor a source for most of a
  song. Long-jack continuity across these boundaries can therefore change.
- Moves must preserve collision, inclusive LN-tail and 500ms created-jack
  rules in both the destination and vacated lane. Generated notes never get
  the original-source repeat exemption. The final independent audit always
  uses the original chart, retaining generated-note provenance.
- The objective is `4 * gain2s + gain8s + gainGlobal + sourceGain`, where each gain is the
  reduction in squared lane shares; local gains sum over affected windows.
  `sourceGain` adds global concentration reduction for each retained original
  source lane plus twice its 8-second concentration reduction. Generated notes
  are excluded from this source statistic, even if they carry a source hint.
  A move cannot reduce mean 2-second active lanes or increase mean 2-second
  concentration (apart from the 1e-12 comparison tolerance). This does not
  guarantee all individual windows improve, all output lanes appear, or the
  8-second hotspot count decreases.
- After two group passes, a deterministic sweep may exchange all heads in a
  pair of lanes inside one 2-second window. It only accepts positive source
  gain and total objective gain. Such exchanges keep that window's active
  count and concentration unchanged, allowing redistribution even in an already
  balanced field. A pair touching a short repeat group spanning two windows is
  locked; both full affected lanes must pass safety before either changes.
- Windows align to 0ms using mathematical floor, including negative times.
  LN heads count once. Hold duration is not the distribution objective.
- Two group passes plus at most 45 lane-pair candidates per nonempty 2-second
  window share a limit of 2,000,000 lane-safety note visits.
  The report may count the one visit that detects exhaustion. Already accepted
  safe moves stay; later groups keep E placement. There is no wall-clock
  timeout or randomness. Equal scores prefer the closer lane, then lower lane.
  The pair sweep visits windows in time order and lane pairs in index order.
- The bound covers candidate safety visits, not initial/final audits or
  allocation/sorting. This is a small deterministic heuristic, not an optimal
  assignment solver. Extremely large charts still require profiling.

The optional `fullFieldTransform` report object records moved notes/groups,
accepted search moves (groups may move repeatedly; pair exchanges also count), work-limit/fallback states,
and before/after metrics. `activeLanes*` is the mean number of used lanes per
nonempty 2-second bin. `concentration*` is the mean of `sum((laneCount/N)^2)`
over those bins; lower is more even (0.125 for uniform 8K, 0.1 for 10K). It is not a gameplay
quality score. On rollback, moved counts are zero while attempted accepted
moves can remain in the report. When E+ is off this object is omitted, preserving
existing E JSON bytes. Hotspot sidecars identify E+ with `preset: "E+"`.
`algorithmVersion: 2` identifies the new behavior. `splitLongChains` counts
original E chains eligible for segmentation, not necessarily moved chains.
`phraseSwaps` counts accepted pair exchanges. `sourceConcentration*` averages
`sum((targetCount / retainedOriginalsFromThisSource)^2)` over represented
original source lanes; generated notes and dropped originals are excluded.
Lower means weaker source-to-target pinning, with 0.125 for uniform 8-lane use
and 0.1 for uniform 10-lane use.
Safety or the search budget may still leave some source placements fixed.

## Verification and playtest

```text
cmake -S . -B build-e-unpinned -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-e-unpinned
ctest --test-dir build-e-unpinned --output-on-failure
build-e-unpinned/keyconv_gui.exe --smoke samples/simple_7k_ln.osu build-e-unpinned/gui-smoke
```

Focused tests cover movable heads, original repeat chains, exact 500ms and LN
tail boundaries, empty/sparse/full-chord populations, signed time extremes,
failed audits, E note/LN preservation, determinism, same-key behavior, option
rejection and GUI/batch command routing. V2 adds direct and full-converter
coverage for all 1K–18K sources, balanced generated fields hiding fixed source
lanes, and a dense 10K boundary where changing ownership must be rejected. See the
[local validation note](nk2-e-fullfield-validation.md) for measured results.

Compare E and E+ at the same scroll speed, especially long one-hand sections,
LN releases, repeat chains and transitions. Wider lane use may change chord
shapes, hand assignment, stairs and perceived difficulty. Those preferences,
fatigue and musical phrasing require actual playtesting; concentration and
the safety audit cannot judge them. Base redistribution has no new density/LNs
or rhythm changes; the separate tap option is documented above. No automatic
adoption, whole-engine rewrite, release packaging or upload is included.
