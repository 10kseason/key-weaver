# KeyWeaver Algorithm Lock v0.6.0

This document freezes the KeyWeaver v0.6.0 normal-mode algorithm contract.
Future tuning can still happen, but any behavior change to this contract must update this document,
the changelog, and the relevant regression tests in the same change.

This supersedes `docs/algorithm-lock-v0.5.5.md` as the current baseline. The v0.5.5 document remains
as historical context for the profile-guided Composer change.

## Locked Scope

- Primary target: high-key `preserve-tap-plus` conversion, especially 4K/7K/8K to 10K.
- Default high-key auto expansion is `auto-normal`, which maps to `preserve-tap-plus` when target keys are greater than source keys.
- Target-10 conversions auto-load `profiles/keyweaver_10k_broad_style_v1.json` when the profile is bundled.
- Preserve Convert is the strict no-added-note preset, with safe adjacent-lane drift for non-jack phrases.
- Stream transforms are deterministic special options: `superrandom` relanes every note and `full-jitter` offsets notes by 1-15 ms.

## Fixed Profile Rule

Composer and repair logic must not use `chartSummary` for local fill decisions when density buckets are present.

The local expansion budget must use the profile's 1000 ms density-bucket window features:

- `densityBuckets.low`
- `densityBuckets.mid`
- `densityBuckets.high`
- `densityBuckets.chordHeavy`
- `densityBuckets.jackRisk`

The root `desired*` fields may remain scorer/report fallbacks, but they are not the source of Composer or Repair local pressure when bucket data is present.

## Budget Contract

- `auto-low` / `preserve-tap-plus-low` target added-note ratio is `0.10`.
- `auto-normal` / `preserve-tap-plus` target added-note ratio is `0.15`.
- `auto-more` / `preserve-tap-plus-more` target added-note ratio is `0.20`.
- `maxAddedNoteRatio` remains the hard global ceiling, defaulting to `0.45`.
- Explicit `--max-added-ratio` still overrides the cap.
- Adaptive Growth Budget is enabled only when a target profile is present, the policy is tap-plus, and the target key count is greater than the source key count.
- The adaptive window size comes from `profile.windowMs`, clamped to a safe range; the bundled profile uses 1000 ms.
- A local window cannot exceed the global cap even if bucket pressure is high.

## Generated-Note Placement

On 8K+ high-key output, generated tap-plus notes must follow these priorities:

- Prefer source slices aligned to the 8th-note beat grid.
- Use 16th-beat slices as lower-priority fallback candidates.
- Suppress generated-note candidates for even-key 32nd-or-faster stair slices.
- Reduce fill pressure on the outer target lanes, especially lanes that would make both extremes into an alternating trill pair.
- Bias lane choice toward whole-target mirror symmetry.
- For even-key to even-key conversion, one-hand source slices keep generated additions inside the matching target hand; source slices that use both hands may use the full target range.

Target-10 has an additional density rule:

- Quarter-beat and 8th-beat source slices receive extra priority above the 8K+ baseline.
- The 10K rule may choose a quarter-beat candidate over the first baseline 8th-beat candidate when both are safe.

## Jack And Repeat Contract

- The default jack/repeat detection window is 500 ms.
- Long same-source-lane jack phrases should stay on one target lane in playable conversion when that is safe.
- Chord-embedded source jacks still count as jack intent and should remain preserved.
- Source-different target repeats are rejected during assignment, repair, expansion, and final sanitization when an alternative exists.
- Generated notes must not create no-source unwanted target jacks.

## LN Contract

- Generated tap-plus LNs are only allowed when anchored to a same-time source LN.
- Generated LN durations are limited to the local 16th-to-8th duration window.
- Longer source LN anchors should produce tap additions instead of cloned long generated holds where possible.
- Source taps are never converted into generated LNs.

## Safety Invariants

The frozen algorithm must keep these invariants:

- No same-time collisions in accepted output.
- No long-note conflicts in accepted output.
- No newly created target jack groups from generated notes.
- No near-time or same-lane near conflicts from added notes.
- Generated notes remain deterministic.
- BMS-family inputs remain BMS-family outputs.
- 4K-8K BMS outputs use SP `#4K`-`#8K`; 9K defaults to `.pms`; 10K exports scratchless 2P channels.
- Converted osu!mania difficulty markers distinguish expansion intensity and stream transforms, for example `KeyWeaver10K-sRan (more)` and `KeyWeaver10K-jitter (low)`.

## Locked Regression Coverage

The v0.6.0 contract is guarded by the unit tests named:

- `auto-more expansion reports larger budget`
- `high-key tap-plus prefers eighth-beat additions`
- `10K tap-plus boosts quarter-eighth density above 8K`
- `high-key extreme trill avoids both outer edges`
- `long source jack stays single lane playable`
- `chord-embedded long source jack stays single lane playable`
- `even-key fast 32nd stair suppresses additions`
- `even-key left-only additions stay left hand`
- `generated short hold clone ignores adjacent long hold length`
- `generated long hold clone is tapified`
- `500ms jack window detects slow source jack`
- `stream superrandom relanes every note`
- `full jitter offsets same-time chords`
- `difficulty name marks expansion and stream transform`

## Change Control

Changing any of the following requires an algorithm-lock update:

- High-key generated-note preset ratios.
- Bucket selection order or thresholds.
- Any use of `chartSummary` in Composer or Repair local pressure.
- 8th/16th/quarter-beat candidate priority.
- 32nd-or-faster stair suppression.
- Target-10 quarter/eighth density boost.
- Outer-lane, hand-zone, mirror-symmetry, or per-slice fill behavior.
- Jack/repeat detection defaults or generated-note jack guards.
- Generated LN duration and anchoring rules.
- Preserve Convert lane drift behavior.
- Stream transform semantics or naming markers.
- Safety guard relaxations.
- The bundled broad 10K profile.

Required checks for changes:

```text
cmake --build build --config Release --target keyconv_tests KeyWeaver keyconv keyconv_gui keyconv_public_header_smoke
build\keyconv_tests.exe
build\keyconv_public_header_smoke.exe
build\keyconv_gui.exe --smoke samples\simple_4k.osu dist\gui_smoke_060
.\scripts\package_release.ps1 -Version 0.6.0
```
