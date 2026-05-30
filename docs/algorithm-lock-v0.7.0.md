# KeyWeaver Algorithm Lock v0.7.0

This document freezes the KeyWeaver v0.7.0 auto-new-algorithm contract.
It supersedes `docs/algorithm-lock-v0.6.5.md` as the current baseline. The v0.6.5
document remains historical context for the auto-low whole-board high-key contract.

## Locked Scope

- Primary target: 7K-to-10K native-feel conversion, target-8 default density, plus inherited high-key tap-plus conversion.
- Omitted `--expansion-policy`, `--expansion-policy auto`, and `--expansion-policy auto-new-algorithm` mean the same default path.
- Same/lower target key counts still use `preserve`.
- Higher-key conversion still uses `preserve-tap-plus-low` by default, except target-8 higher-key conversion and 7K-to-10K.
- Target-8 higher-key conversion defaults to normal `preserve-tap-plus` density while explicit `auto-low` remains available.
- 7K-to-10K now defaults to native `dense-ln` with normal `preserve-tap-plus` density.
- `--native-10k off` restores the v0.6.5 whole-board 7K-to-10K fallback.
- Preserve Convert remains strict no-added-note conversion and does not enable native `dense-ln`.
- Target-10 conversions auto-load `profiles/keyweaver_10k_broad_style_v1.json` when bundled.

## Native 7K-To-10K Contract

- The default 7K-to-10K rail is `Native10KPreset::DenseLn`.
- The rail treats 10K as two 5K hands and routes the 7K middle column by hand trading.
- Native `dense-ln` may generate short LN bridge fills between same-hand overlapping LNs when collision, LN-conflict, distance, snap, and no-created-jack guards allow it.
- Native `dense-ln` uses a narrower density-control range: `auto-low` `0.12`, normal/default `0.15`, and `auto-more` `0.18`.
- `auto-normal` and `preserve-tap-plus` are the normal-density native rail.
- `auto-low` and `preserve-tap-plus-low` reduce native fill pressure.
- `auto-more` and `preserve-tap-plus-more` raise native fill pressure.

## Inherited v0.6.5 Contracts

Unless this document overrides them, v0.7.0 inherits the v0.6.5 contracts for:

- 8K+ high-key generated-note placement.
- Target-10 quarter/eighth density pressure.
- Whole-board assignment for 4K-to-5/6/7K and 8K+ conversion.
- 9K+ soft underuse/edge coverage.
- Jack/repeat preservation and no-created-jack safety.
- Generated LN anchoring and duration normalization.
- Adaptive Growth Budget with target-profile density buckets.
- Stream transforms and conversion difficulty markers.
- BMS-family input/output behavior.

## Safety Invariants

The frozen algorithm must keep these invariants:

- No same-time collisions in accepted output.
- No long-note conflicts in accepted output.
- No newly created target jack groups from generated notes.
- No near-time or same-lane near conflicts from added notes.
- Generated notes remain deterministic.
- Reports must expose `"algorithmVersion": "v0.7.0"`.
- Default 7K-to-10K reports must expose `"native10KPreset": "dense-ln"` and `"expansionPolicy": "preserve-tap-plus"`.
- Default target-8 higher-key reports must expose `"expansionPolicy": "preserve-tap-plus"`.
- Default non-target-8, non-7K-to-10K higher-key reports must keep `"expansionPolicy": "preserve-tap-plus-low"`.

## Change Control

Changing any of the following requires an algorithm-lock update:

- Default auto-new-algorithm routing.
- Native `dense-ln` preset selection or density ratios.
- `auto-low` / normal / `auto-more` expansion budgets.
- Generated LN bridge generation or duration limits.
- Hand-trading or within-hand lane candidate behavior.
- Any inherited v0.6.5 safety, assignment, jack, LN, stream, or profile-budget contract.

Required checks for changes:

```text
cmake --build build --config Release --target keyconv_tests KeyWeaver keyconv keyconv_gui keyconv_public_header_smoke
build\keyconv_tests.exe
build\keyconv_public_header_smoke.exe
build\keyconv_gui.exe --smoke samples\simple_4k.osu dist\gui_smoke_070_4k
build\keyconv_gui.exe --smoke samples\simple_7k_ln.osu dist\gui_smoke_070_7k
.\scripts\package_release.ps1 -Version 0.7.0
```
