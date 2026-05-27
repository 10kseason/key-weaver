# KeyWeaver Algorithm Lock v0.5.5

This document freezes the current KeyWeaver v0.5.5 high-key expansion behavior as the baseline algorithm.
Future tuning can still happen, but any behavior change to this contract must update this document, the
changelog, and the relevant regression tests in the same change.

## Locked Scope

- Primary target: `preserve-tap-plus` conversion into high-key targets, especially 7K to 10K.
- Target-10 conversions auto-load `profiles/keyweaver_10k_broad_style_v1.json` when the profile is bundled.
- The bundled broad profile is a KeyWeaver style profile from u_e and CircusGalop 10K references, excluding compact conversion variants such as `4K10C`, `5K7C`, and similar tags.
- The algorithm preserves the source chart skeleton first, then uses profile-guided coverage pressure to make 10K space feel alive.

## Fixed Profile Rule

Composer and repair logic must not use `chartSummary` for local fill decisions.

The local expansion budget must use the profile's 1000 ms density-bucket window features:

- `densityBuckets.low`
- `densityBuckets.mid`
- `densityBuckets.high`
- `densityBuckets.chordHeavy`
- `densityBuckets.jackRisk`

The root `desired*` fields may remain scorer/report fallbacks, but they are not the source of Composer or Repair local pressure when bucket data is present.
The current `lnHeavy` bucket is not part of the locked Composer rule because the broad profile's LN-heavy bucket currently mirrors the all-window bucket too closely.

## Budget Contract

- High-key `preserve-tap-plus` default target added ratio is `0.375`.
- `maxAddedNoteRatio` remains the hard global ceiling, defaulting to `0.45`.
- Adaptive Growth Budget is enabled only when a target profile is present, the policy is `preserve-tap-plus`, and the target key count is greater than the source key count.
- The adaptive window size comes from `profile.windowMs`, clamped to a safe range; the bundled v0.5.5 profile uses 1000 ms.
- A local window cannot exceed the global cap even if bucket pressure is high.

## Bucket Selection

For each adaptive window, KeyWeaver measures source-window features and selects one bucket in this priority order:

1. `jackRisk` when the window has profile-like same-lane repeat risk.
2. `chordHeavy` when chord-slice rate reaches the chord-heavy profile floor.
3. `low`, `mid`, or `high` from `densityCuts.lowMaxNps` and `densityCuts.midMaxNps`.
4. `all` only as a fallback when a specific bucket is missing.

The `jackRisk` feature must match the profile-builder definition: sort objects by time, then count only adjacent time-order pairs on the same lane with `0 < delta <= jackWindowMs`.
Do not count every same-lane pair inside a window; that overclassifies chord/hold-heavy sections as jack-risk and suppresses 10K fill.

## Fill Pressure

When a density bucket is selected:

- `activeLaneRate.p25` is the local minimum floor.
- `activeLaneRate.median` is the local target.
- `densityNps.median/p75/p90` shape density room instead of a fixed high-density throttle.
- `adjacentExpansion.median` raises adjacent-growth pressure.
- `chordHeavy` and `high` windows are allowed to stay more open than old fixed-density safety would allow.
- `jackRisk` windows stay conservative and keep created-jack prevention dominant.

This locks the intended behavior: low-density sections should not be overfilled, but mid/high/chord-heavy sections should not leave long dead 10K lanes when the profile says those lanes should be active.

## Anchor And Coverage Contract

The current baseline is Anchor-with-Coverage:

- Source 7K lane skeleton and gesture rails remain readable.
- 10K is treated as two 5K hand panels for 7K-to-10K phrase recomposition.
- Expansion-mode anchors are intentionally weaker than the older anchor-first behavior.
- Locally dead lanes receive fill pressure.
- Tap-plus may spend the second per-slice slot when global and local budgets allow it.
- Candidate additions still go through snap, collision, distance, LN conflict, and jack guards.

## Safety Invariants

The frozen algorithm must keep these invariants:

- No same-time collisions in accepted output.
- No long-note conflicts in accepted output.
- No newly created target jack groups from generated notes.
- No near-time or same-lane near conflicts from added notes.
- Generated notes remain deterministic.
- BMS-family inputs remain BMS-family outputs.
- 4K-8K BMS outputs use SP `#4K`-`#8K`; 9K defaults to `.pms`; 10K exports scratchless 2P channels.

## Locked Ray Smoke Baseline

For the Ray 7K-to-10K smoke chart:

```text
Input: D:\osu!\Songs\2558616 Kaguya(cvYuko Natsuyoshi) - ray (Cosmic Princess Kaguya! Version)\Kaguya(cv.Yuko Natsuyoshi) - ray (Cosmic Princess Kaguya! Version) (OsuJoa) [luna].osu
Profile: keyweaver_10k_broad_style_v1
Target added note ratio: 0.375
Adaptive budget average ratio: 0.377974
Added notes: 420
Created jacks: 0
Near-time conflicts: 0
LN conflicts: 0
K-likeness score: 81.4
```

This smoke result is not a universal required value for every chart. It is the reference symptom check for the v0.5.5 fix: 10K should no longer be starved by chart-summary-style budget collapse or overbroad jack-risk classification.

## Change Control

Changing any of the following requires an algorithm-lock update:

- Bucket selection order or thresholds.
- Any use of `chartSummary` in Composer or Repair local pressure.
- Global high-key added-ratio defaults.
- Anchor strength, dead-lane bonus, or per-slice fill-slot behavior.
- Safety guard relaxations.
- The bundled broad 10K profile.
- Ray smoke baseline expectations.

Required checks for changes:

```text
cmake --build build --config Release --target keyconv_tests KeyWeaver
build\keyconv_tests.exe
build\KeyWeaver.exe "<Ray luna .osu>" --source 7 --target 10 --dry-run --report build\ray_10k_density_bucket.json
.\scripts\package_release.ps1 -Version 0.5.5
```
