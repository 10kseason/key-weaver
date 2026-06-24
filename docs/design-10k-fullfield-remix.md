# 10K Full-Field Mirror-Remix Design Lock

This document locks the gated `--ten-k-fullfield-remix` experiment. It is a transform/remix mode, not a native-chart preservation mode.

## Gate

- [LOCKED] The mode is active only when `targetKeyCount == 10` and `tenKFullFieldRemix == true`.
- [LOCKED] Normal conversion behavior remains unchanged when the flag is off.
- [LOCKED] The global normal-mode `maxAddedNoteRatio = 0.45` contract is not changed.
- [LOCKED] No `GestureHint` or `GestureRail` field is added or repurposed.

## Budget

- [LOCKED] Full-field remix uses a mode-local total-note density ceiling of `1.6`.
- [LOCKED] The effective added-note target is `densityCeiling - 1.0`, defaulting to `0.60`.
- [LOCKED] The adaptive growth window remains 1000 ms by default.
- [FEEL] `tenKFullFieldRemixDensityCeiling` can be tuned if playtests show overfill or underfill.

## Rail

- [LOCKED] `buildFullFieldRail` reuses detected `PatternToken`s and writes ordinary `GestureHint` entries.
- [LOCKED] Left hand is lanes `0..4`; right hand is lanes `5..9`.
- [LOCKED] Primary short phrases trade hands by recent phrase timing instead of only count catch-up; stream/burst/stair phrases alternate hands inside the phrase so one token cannot collapse into one side of the 10K field.
- [LOCKED] Jacks keep their hand anchor and do not flip.
- [LOCKED] Trills split alternating hands and suppress echoes.
- [LOCKED] Wide chords split across hands and suppress echoes.
- [FEEL] Non-wide stream, stair, single, burst, and chord phrase feel is controlled by the rail's preferred lane and zone scoring.

## Echo Derivation

- [LOCKED] Echoes are derived after the primary lane is known.
- [LOCKED] Echoes are suppressed for `Jack`, `Trill`, `Chord`, `AnchorLn`, and `ReleaseLn`.
- [LOCKED] The echo zone is the opposite 5-lane hand zone.
- [LOCKED] The mirror lane is `targetKeyCount - 1 - primaryLane`.
- [LOCKED] Echo lane rotation is:

```cpp
int rotateWithinZone(int mirrorLane, int sliceIndex, int zoneStart, int zoneWidth, int phaseStep) {
    int base = mirrorLane - zoneStart;
    int off = (sliceIndex * phaseStep) % zoneWidth;
    int idx = (base + off) % zoneWidth;
    return zoneStart + idx;
}
```

- [LOCKED] For a 5-lane zone, `phaseStep` must be `2` or `3`.
- [FEEL] Default `phaseStep` is `2`.

## Pipeline

1. Detect pattern tokens with `detectPatternTokens`.
2. Build the full-field rail with `buildFullFieldRail`.
3. Optimize primary assignments with the existing greedy planner and local repair path.
4. Add derived mirror-remix echoes under the mode-local density ceiling.
5. Apply the existing compression, stream-transform, collision, distance, jack, and quality report checks.

## Required Evidence

- Deterministic same-input output.
- No created jacks.
- Density targets approximately `1.6x` on sparse eligible phrases.
- All 10 lanes are reachable over a phrase.
- Same-slice chords do not exceed per-hand 5-lane reach.
