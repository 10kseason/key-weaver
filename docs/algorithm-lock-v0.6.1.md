# KeyWeaver Algorithm Lock v0.6.1

This document records the 10K planner contract layered on top of
`docs/algorithm-lock-v0.6.0.md`. The v0.6.0 document remains the frozen baseline
for generated-note, jack, LN, stream-transform, and broad safety behavior.

## Locked Scope

- Primary target: 7K-to-10K `playable` and `training` conversion.
- Default planner policy is `auto`.
- In `auto`, 7K-to-10K playable/training conversion uses the staged-native planner.
- `--ten-key-planner legacy` restores the previous direct dual-5K planner.
- `--ten-key-planner staged-7-9-10` explicitly selects the staged-native planner for supported conversions.
- `--ten-key-planner staged-7-14-10` explicitly selects the experimental mirror-compress planner for supported conversions.
- Unsupported source/target/style combinations report and behave as `legacy`.

## Staged Planner Contract

The staged-native planner must route 7K source lanes through a 9K scaffold before
projecting to 10K:

```text
7K source lane:  0 1 2 3 4 5 6
9K scaffold:     0 1 3 4 5 7 8
10K base lane:   0 1 3 4 6 8 9
center candidates for source lane 3: 3 4 5 6
```

Source lane 3 is the center bridge. Sparse center-lane phrases may move across
lanes 3-6, with lane 4 as the base anchor and lanes 5-6 available when balance
and no-created-jack safety prefer the right side of the split. Low-register
phrases should stay in lanes 0-4, and high-register phrases should stay in lanes
5-9 unless safety guards force a fallback.

## Mirror-Compress Experiment

The explicit `staged-7-14-10` planner treats each 7K source lane as a mirrored
14K pair, then compresses those virtual 14K anchors into 10K before assignment
scoring runs:

```text
7K source lane:       0   1   2   3   4   5   6
14K mirror pair:     0/13 1/12 2/11 3/10 4/9 5/8 6/7
10K compressed pair: 0/9  1/8  2/7  2/7  3/6 4/5 4/5
```

This policy is opt-in. It does not replace the `auto` planner. After the
compressed pair candidates are exposed, the normal balance, collision,
LN-conflict, distance, source-anchor, and no-created-jack rules decide the final
lane. Gesture Rail remains active, but mirror-compress opens gesture zones across
the full 10K field instead of reusing the legacy dual-5K phrase zone. Reports
expose this active policy as `staged-7-14-10`.

Because the raw 14K-to-10K compression gives panel-center lanes 3 and 8 fewer
direct source anchors than their neighboring lanes, the planner adds an
underuse bonus for those panel centers when they are below their local 5K-panel
average. This is a scoring pressure only; collision, LN, distance, gesture-jack,
and no-created-jack guards still take priority.

## Chord And Panel Contract

- Non-gesture low-register 7K chords stay inside the left 5K panel.
- Non-gesture high-register 7K chords stay inside the right 5K panel.
- Chords that include source lane 3 inherit the surrounding slice ownership:
  low/center-heavy slices stay left, high-heavy slices stay right, and true center
  bridge slices stay near lanes 3-6.
- Gesture Rail hints can still assign clear left-hand or right-hand voice ownership.
- Collision, LN conflict, no-created-jack, and distance safety checks remain higher
  priority than panel preference.

## Reporting And CLI

- CLI accepts `--ten-key-planner auto | legacy | staged-7-9-10 | staged-7-14-10`.
- Public API exposes `TenKeyPlannerPolicy`.
- JSON and text reports expose `quality.tenKeyPlanner`.
- The reported value is `staged-7-9-10` only when the staged planner is actually
  active; otherwise it is `legacy`.
- Target-K profiles measure 10K center/split behavior with `desiredCenterBridgeRate`,
  `desiredCenterSplitBalance`, `desiredSplitChordRate`, and matching density-bucket
  feature stats.
- JSON/text reports expose raw `centerBridgeRate`, `centerSplitBalance`,
  `splitChordRate` plus `centerBridgeScore`, `centerSplitBalanceScore`, and
  `splitChordScore`.

## Safety Invariants

The staged planner must preserve the v0.6.0 safety invariants:

- No same-time collisions in accepted output.
- No long-note conflicts in accepted output.
- No newly created target jack groups from generated notes.
- No source-different target repeat is accepted when a safe alternative exists.
- Existing 8K and non-10K candidate-lane behavior remains stable.

## Locked Regression Coverage

This contract is guarded by the unit tests named:

- `7K to 10K staged planner splits center bridge`
- `7K to 10K mirror-compress planner uses 14-lane pairs`
- `7K to 10K mirror-compress planner fills panel centers`
- `7K to 10K non-gesture chords stay in source panels`
- `10K non-jack source-lane anchor relaxes inside panel`
- `8K playable candidate radius remains stable`
- `public_header_smoke`

## Change Control

Changing any of the following requires this document, the changelog, and the
matching tests to be updated together:

- 7K -> 9K scaffold anchors.
- 9K -> 10K center split projection.
- 7K -> 14K mirror-pair anchors.
- 14K -> 10K mirror-compress projection.
- Mirror-compress full-field gesture zone behavior.
- Mirror-compress 5K panel-center underuse pressure.
- Center source-lane bridge zone.
- Low/high panel ownership rules.
- Chord panel ownership rules.
- Reported `tenKeyPlanner` semantics.
- CLI accepted planner policy names.
- Target-K center/split metric definitions or score weights.
- Any safety-priority relaxation around collisions, LN conflicts, distance guards,
  or no-created-jack handling.
